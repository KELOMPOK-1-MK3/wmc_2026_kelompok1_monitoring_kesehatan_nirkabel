#!/usr/bin/env python3
"""Penerima data dari dua node nirkabel sekaligus.

Jalur 1: BLE  — Node 1 (pergelangan, MAX30102) mengirim detak jantung + SpO2.
Jalur 2: MQTT — Node 2 (dada, MLX90614) mengirim suhu tubuh.

Setiap paket dicatat waktu tibanya (t_terima) untuk menghitung:
  latency_ms  = (t_terima - t_kirim) * 1000
  jitter_ms   = |latency[i] - latency[i-1]|
  packet_loss = 1 - (jumlah seq unik / jumlah seq seharusnya)
  throughput  = jumlah paket diterima / durasi

Format payload dari node (JSON):
  {"node_id": "wrist01", "seq": 42, "t_kirim": 1699999999.123,
   "bpm": 72, "spo2": 97, "suhu": 36.5}

Cara pakai
----------
  # 1. Uji logika penerima + QoS tanpa perangkat keras (dua node tiruan):
  python receiver.py --source sim --durasi 60

  # 2. Pengukuran sesungguhnya:
  python receiver.py --source ble            # Node 1 lewat BLE
  python receiver.py --source mqtt --mqtt-host 192.168.1.10
  python receiver.py --source dua            # BLE dan MQTT sekaligus

Hasil tiap sesi: satu CSV di data/ (baris per paket) dan satu berkas
.ringkasan.json berisi keempat parameter QoS untuk sesi tersebut.
"""
from __future__ import annotations

import argparse
import asyncio
import csv
import json
import math
import random
import statistics
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

# ---------------------------------------------------------------------------
# Bagian 1: perhitungan QoS
# ---------------------------------------------------------------------------

KOLOM = [
    "node_id", "seq", "t_kirim", "t_terima", "latency_ms", "jitter_ms",
    "bpm", "spo2", "suhu", "hilang_sejak_paket_terakhir",
]


@dataclass
class Paket:
    node_id: str
    seq: int
    t_kirim: float
    t_terima: float
    nilai: dict[str, Any] = field(default_factory=dict)
    selisih: float = 0.0          # t_terima - t_kirim, apa adanya
    latency_ms: float = 0.0
    jitter_ms: float | None = None
    hilang: int = 0


class PelacakQos:
    """Mengumpulkan paket satu node dan menghitung empat parameter QoS.

    Penyelarasan jam (masalah utama pengukuran latency satu arah)
    -----------------------------------------------------------
    Jam node dan jam laptop penerima tidak sama, sehingga ``t_terima - t_kirim``
    bukan latency, melainkan latency ditambah selisih jam. Selisih jam itu
    diestimasi sebagai **nilai terkecil** dari ``t_terima - t_kirim`` sepanjang
    sesi: paket tercepat dianggap hampir tanpa antrean, sehingga selisihnya
    mendekati selisih jam murni.

    Konsekuensinya, angka latency yang dilaporkan adalah **delay relatif
    terhadap paket tercepat**, bukan delay absolut. Hal ini wajib disebutkan
    saat melaporkan hasil. Nilai selisih jam yang dipakai dicatat pada berkas
    ringkasan supaya dapat diperiksa.

    Menghitung paket hilang dari lompatan nomor urut, bukan dari perkiraan
    jumlah paket yang seharusnya dikirim.
    """

    def __init__(self, node_id: str, periode_kirim: float = 1.0) -> None:
        self.node_id = node_id
        self.periode_kirim = periode_kirim
        self.metode_offset = "minimum"
        self.offset_jam: float | None = None
        self.paket: list[Paket] = []
        self.seq_terakhir: int | None = None
        self.t_pertama = 0.0

    # -- penyelarasan jam --------------------------------------------------
    def perbarui_offset(self) -> bool:
        """Hitung ulang selisih jam. Kembalikan True bila nilainya berubah."""
        if self.metode_offset == "tanpa" or not self.paket:
            return False
        if self.metode_offset == "awal":
            baru = self.paket[0].selisih
        else:
            baru = min(p.selisih for p in self.paket)
        if self.offset_jam is not None and abs(baru - self.offset_jam) < 1e-9:
            return False
        self.offset_jam = baru
        return True

    def _hitung_latency(self, p: Paket) -> float:
        offset = self.offset_jam if self.metode_offset != "tanpa" else 0.0
        return max((p.selisih - (offset or 0.0)) * 1000.0, 0.0)

    def hitung_ulang(self) -> None:
        """Hitung ulang seluruh latency dan jitter setelah offset berubah."""
        jitter_sebelumnya = None
        for p in self.paket:
            p.latency_ms = self._hitung_latency(p)
            p.jitter_ms = (None if jitter_sebelumnya is None
                           else abs(p.latency_ms - jitter_sebelumnya))
            jitter_sebelumnya = p.latency_ms

    # -- penerimaan paket --------------------------------------------------
    def terima(self, node_id: str, seq: int, t_kirim: float,
               t_terima: float | None = None, nilai: dict | None = None) -> Paket:
        t_terima = time.time() if t_terima is None else t_terima

        hilang = 0
        if self.seq_terakhir is not None and seq > self.seq_terakhir + 1:
            hilang = seq - self.seq_terakhir - 1

        p = Paket(node_id=node_id, seq=seq, t_kirim=t_kirim, t_terima=t_terima,
                  nilai=nilai or {}, selisih=t_terima - t_kirim, hilang=hilang)
        if not self.paket:
            self.t_pertama = t_terima
        self.paket.append(p)
        self.seq_terakhir = seq if self.seq_terakhir is None else max(seq, self.seq_terakhir)

        # Bila offset berubah (ada paket yang lebih cepat), seluruh latency
        # dan jitter dihitung ulang; bila tidak, cukup paket terakhir.
        if self.perbarui_offset():
            self.hitung_ulang()
        else:
            p.latency_ms = self._hitung_latency(p)
            p.jitter_ms = (None if len(self.paket) < 2
                           else abs(p.latency_ms - self.paket[-2].latency_ms))
        return p

    # -- ringkasan ---------------------------------------------------------
    def ringkasan(self) -> dict[str, Any]:
        if not self.paket:
            return {"node_id": self.node_id, "paket_diterima": 0}

        lat = [p.latency_ms for p in self.paket]
        jit = [p.jitter_ms for p in self.paket if p.jitter_ms is not None]
        diterima = len(self.paket)
        hilang = sum(p.hilang for p in self.paket)
        seharusnya = diterima + hilang
        durasi = max(self.paket[-1].t_terima - self.t_pertama, 1e-9)

        return {
            "node_id": self.node_id,
            "paket_diterima": diterima,
            "paket_hilang": hilang,
            "durasi_detik": round(durasi, 2),
            "latency_rata_ms": round(statistics.fmean(lat), 3),
            "latency_min_ms": round(min(lat), 3),
            "latency_max_ms": round(max(lat), 3),
            "latency_stdev_ms": round(statistics.pstdev(lat), 3) if len(lat) > 1 else 0.0,
            "jitter_rata_ms": round(statistics.fmean(jit), 3) if jit else 0.0,
            "jitter_max_ms": round(max(jit), 3) if jit else 0.0,
            "packet_loss_persen": round(100.0 * hilang / seharusnya, 3) if seharusnya else 0.0,
            "throughput_paket_per_detik": round(diterima / durasi, 3),
            "metode_offset": self.metode_offset,
            "offset_jam_detik": (None if self.offset_jam is None
                                 else round(self.offset_jam, 6)),
            "catatan_latency": (
                "delay relatif terhadap paket tercepat (bukan delay absolut)"
                if self.metode_offset == "minimum" else
                "selisih jam diambil dari paket pertama (bukan delay absolut)"
                if self.metode_offset == "awal" else
                "jam node TIDAK diselaraskan — latency masih memuat selisih jam"),
        }

    def baris_csv(self) -> list[dict[str, Any]]:
        baris = []
        for p in self.paket:
            baris.append({
                "node_id": p.node_id,
                "seq": p.seq,
                "t_kirim": f"{p.t_kirim:.3f}",
                "t_terima": f"{p.t_terima:.3f}",
                "latency_ms": f"{p.latency_ms:.3f}",
                "jitter_ms": "" if p.jitter_ms is None else f"{p.jitter_ms:.3f}",
                "bpm": p.nilai.get("bpm", ""),
                "spo2": p.nilai.get("spo2", ""),
                "suhu": p.nilai.get("suhu", ""),
                "hilang_sejak_paket_terakhir": p.hilang,
            })
        return baris


# ---------------------------------------------------------------------------
# Bagian 2: penyimpanan hasil
# ---------------------------------------------------------------------------

def _folder_keluaran(out_dir: str) -> Path:
    """Cari folder data/ pada akar repository, bukan pada folder kerja."""
    p = Path(out_dir)
    if p.is_absolute():
        p.mkdir(parents=True, exist_ok=True)
        return p
    kandidat = [Path.cwd() / p, Path(__file__).resolve().parents[2] / p]
    for k in kandidat:
        if k.parent.exists():
            k.mkdir(parents=True, exist_ok=True)
            return k
    p.mkdir(parents=True, exist_ok=True)
    return p


def simpan_hasil(pelacak: dict[str, PelacakQos], folder: Path,
                 nama_sesi: str, catatan: dict | None = None) -> tuple[Path, Path]:
    csv_path = folder / f"{nama_sesi}.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=KOLOM)
        w.writeheader()
        for node in sorted(pelacak):
            w.writerows(pelacak[node].baris_csv())

    ringkasan = {
        "nama_sesi": nama_sesi,
        "waktu": datetime.now().isoformat(timespec="seconds"),
        "catatan": catatan or {},
        "per_node": {n: p.ringkasan() for n, p in sorted(pelacak.items())},
    }
    json_path = folder / f"{nama_sesi}.ringkasan.json"
    json_path.write_text(json.dumps(ringkasan, indent=2, ensure_ascii=False), "utf-8")
    return csv_path, json_path


# ---------------------------------------------------------------------------
# Bagian 3: sumber data — simulasi (tanpa perangkat keras)
# ---------------------------------------------------------------------------

class NodeTiruan:
    """Node tiruan yang meniru perilaku kanal nirkabel.

    Dipakai untuk menguji perhitungan QoS dan dashboard sebelum perangkat
    keras selesai dirakit. Parameter jejak delay dibuat menyerupai BLE
    (jitter kecil, andal) untuk Node 1 dan WiFi/MQTT (jitter lebih besar,
    sesekali hilang) untuk Node 2 — nilai di bawah adalah tiruan, bukan hasil
    pengukuran lapangan.
    """

    def __init__(self, node_id: str, periode: float, delay_ms: float,
                 jitter_ms: float, peluang_hilang: float = 0.0,
                 sensor: str = "bpm") -> None:
        self.node_id = node_id
        self.periode = periode
        self.delay_ms = delay_ms
        self.jitter_ms = jitter_ms
        self.peluang_hilang = peluang_hilang
        self.sensor = sensor
        self.seq = 0

    def nilai_sensor(self) -> dict[str, Any]:
        if self.sensor == "bpm":
            return {"bpm": random.randint(68, 84), "spo2": random.randint(96, 99)}
        return {"suhu": round(random.uniform(36.1, 37.0), 2)}

    async def jalankan(self, durasi: float,
                       kirim: Callable[[str, int, float, float, dict], None]) -> None:
        mulai = time.time()
        while time.time() - mulai < durasi:
            await asyncio.sleep(self.periode)
            self.seq += 1
            if random.random() < self.peluang_hilang:
                continue  # paket hilang di kanal, tidak pernah sampai
            t_kirim = time.time()
            delay = max(0.0, random.gauss(self.delay_ms, self.jitter_ms)) / 1000.0
            await asyncio.sleep(delay)
            kirim(self.node_id, self.seq, t_kirim, time.time(), self.nilai_sensor())


async def sumber_simulasi(durasi: float, pelacak: dict[str, PelacakQos],
                          metode: str = "minimum") -> None:
    def terima(node_id, seq, t_kirim, t_terima, nilai):
        p = pelacak.setdefault(node_id, PelacakQos(node_id))
        p.metode_offset = metode
        p.terima(node_id, seq, t_kirim, t_terima, nilai)

    nodes = [
        NodeTiruan("wrist01", 1.0, delay_ms=12, jitter_ms=4, sensor="bpm"),
        NodeTiruan("chest01", 2.0, delay_ms=35, jitter_ms=15,
                   peluang_hilang=0.03, sensor="suhu"),
    ]
    print(f"[sim] menjalankan {len(nodes)} node tiruan selama {durasi:.0f} detik …")
    await asyncio.gather(*(n.jalankan(durasi, terima) for n in nodes))


# ---------------------------------------------------------------------------
# Bagian 4: sumber data — perangkat sesungguhnya
# ---------------------------------------------------------------------------

SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


def _uraikan(payload: bytes) -> dict[str, Any] | None:
    try:
        data = json.loads(payload.decode("utf-8").strip())
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if "node_id" not in data or "seq" not in data:
        return None
    return data


def _catat(data: dict, pelacak: dict[str, PelacakQos], metode: str) -> None:
    node_id = str(data["node_id"])
    nilai = {k: v for k, v in data.items()
             if k not in ("node_id", "seq", "t_kirim")}
    p = pelacak.setdefault(node_id, PelacakQos(node_id))
    p.metode_offset = metode
    p.terima(node_id, int(data["seq"]), float(data["t_kirim"]),
             time.time(), nilai)
    print(f"  {node_id:8s} seq={int(data['seq']):5d} "
          f"latency={p.paket[-1].latency_ms:7.2f} ms  {nilai}")


async def sumber_ble(pelacak: dict[str, PelacakQos], metode: str,
                     kedaluwarsa: float) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("[BLE] pustaka bleak belum terpasang: pip install bleak")
        return

    print("[BLE] mencari Node 1 (wrist01) …")
    perangkat = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE_UUID.lower() in [str(u).lower() for u in ad.service_uuids],
        timeout=kedaluwarsa,
    )
    if perangkat is None:
        print("[BLE] Node 1 tidak ditemukan. Periksa daya node dan izin Bluetooth.")
        return

    def saat_diterima(_sender, data: bytearray) -> None:
        uraian = _uraikan(bytes(data))
        if uraian:
            _catat(uraian, pelacak, metode)

    async with BleakClient(perangkat) as klien:
        print(f"[BLE] tersambung ke {perangkat.address}")
        await klien.start_notify(CHAR_UUID, saat_diterima)
        while True:
            await asyncio.sleep(1.0)


async def sumber_mqtt(pelacak: dict[str, PelacakQos], metode: str,
                      host: str, port: int, topik: str) -> None:
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("[MQTT] pustaka paho-mqtt belum terpasang: pip install paho-mqtt")
        return

    klien = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    loop = asyncio.get_running_loop()

    def saat_pesan(_c, _u, pesan) -> None:
        uraian = _uraikan(pesan.payload)
        if uraian:
            loop.call_soon_threadsafe(_catat, uraian, pelacak, metode)

    klien.on_message = saat_pesan
    print(f"[MQTT] menyambung ke {host}:{port}, topik {topik} …")
    klien.connect(host, port, 60)
    klien.subscribe(topik)
    klien.loop_start()
    try:
        while True:
            await asyncio.sleep(1.0)
    finally:
        klien.loop_stop()
        klien.disconnect()


# ---------------------------------------------------------------------------
# Bagian 5: program utama
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Penerima data dua node nirkabel + perhitungan QoS 4 parameter")
    ap.add_argument("--source", choices=["sim", "ble", "mqtt", "dua"], default="sim",
                    help="sumber data: sim (tiruan), ble, mqtt, atau dua sekaligus")
    ap.add_argument("--out", default="data/", help="folder keluaran CSV")
    ap.add_argument("--sesi", default=None,
                    help="nama sesi (default: sesi-<skenario>-<tanggal>-<jam>)")
    ap.add_argument("--durasi", type=float, default=60.0,
                    help="lama sesi simulasi dalam detik")
    ap.add_argument("--mqtt-host", default="localhost")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--topik", default="klinik/#")
    ap.add_argument("--skenario", default="uji",
                    help="label skenario: 1m, 4m, 8m, penghalang, interferensi, putus, daya")
    ap.add_argument("--jarak-m", type=float, default=None)
    ap.add_argument("--metode-offset", choices=["minimum", "awal", "tanpa"],
                    default="minimum",
                    help="cara menyelaraskan jam node: minimum (paket tercepat), "
                         "awal (paket pertama), tanpa (tidak diselaraskan)")
    args = ap.parse_args()

    folder = _folder_keluaran(args.out)
    metode = args.metode_offset
    sekarang = datetime.now().strftime("%Y%m%d-%H%M")
    nama_sesi = args.sesi or f"sesi-{args.skenario}-{sekarang}"
    catatan = {"skenario": args.skenario, "jarak_m": args.jarak_m,
               "metode_offset": metode, "sumber": args.source}

    pelacak: dict[str, PelacakQos] = {}
    print(f"Sesi {nama_sesi} — sumber: {args.source} — keluaran: {folder}")

    try:
        if args.source == "sim":
            asyncio.run(sumber_simulasi(args.durasi, pelacak, metode))
        elif args.source == "ble":
            asyncio.run(sumber_ble(pelacak, metode, 20.0))
        elif args.source == "mqtt":
            asyncio.run(sumber_mqtt(pelacak, metode, args.mqtt_host,
                                    args.mqtt_port, args.topik))
        else:
            async def dua_jalur() -> None:
                await asyncio.gather(
                    sumber_ble(pelacak, metode, 20.0),
                    sumber_mqtt(pelacak, metode, args.mqtt_host,
                                args.mqtt_port, args.topik),
                )

            asyncio.run(dua_jalur())
    except KeyboardInterrupt:
        print("\nSesi dihentikan, menyimpan hasil …")

    if not pelacak:
        print("Tidak ada paket yang diterima — tidak ada berkas yang disimpan.")
        return

    csv_path, json_path = simpan_hasil(pelacak, folder, nama_sesi, catatan)
    print(f"\nCSV       : {csv_path}")
    print(f"Ringkasan : {json_path}\n")

    # Tabel ringkas untuk ditempel ke docs/pengujian.md
    print(f"{'node':8s} {'paket':>6s} {'hilang':>7s} {'latency rata':>13s} "
          f"{'jitter rata':>12s} {'loss':>7s} {'throughput':>11s}")
    for nama, p in sorted(pelacak.items()):
        s = p.ringkasan()
        print(f"{nama:8s} {s['paket_diterima']:6d} {s['paket_hilang']:7d} "
              f"{s['latency_rata_ms']:10.2f} ms {s['jitter_rata_ms']:9.2f} ms "
              f"{s['packet_loss_persen']:6.2f}% "
              f"{s['throughput_paket_per_detik']:8.2f} p/s")


if __name__ == "__main__":
    main()
