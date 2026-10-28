#!/usr/bin/env python3
"""Dashboard pemantauan tanda vital dan hasil pengukuran QoS.

Membaca berkas CSV hasil sesi dari folder ``data/`` lalu menyajikannya sebagai
grafik detak jantung, SpO2, dan suhu tubuh, ditambah tabel ringkasan keempat
parameter QoS per sesi.

Cara pakai
----------
  pip install pandas matplotlib
  streamlit run src/dashboard/app.py          # dashboard grafik (utama)

  python src/dashboard/app.py                 # laporan teks di terminal
  python src/dashboard/app.py --plot          # menyimpan PNG ke docs/grafik/

Sumber data: berkas ``sesi-*.csv`` dan ``sesi-*.ringkasan.json`` yang ditulis
oleh ``src/receiver/receiver.py``. Kolom yang dibaca: node_id, seq, t_kirim,
t_terima, latency_ms, jitter_ms, bpm, spo2, suhu.
"""
from __future__ import annotations

import argparse
import json
from datetime import datetime
from pathlib import Path

import pandas as pd

# ---------------------------------------------------------------------------
# Pencarian berkas
# ---------------------------------------------------------------------------

AKAR = Path(__file__).resolve().parents[2]


def folder_data(eksplisit: str | None = None) -> Path:
    if eksplisit:
        return Path(eksplisit).expanduser()
    kandidat = [AKAR / "data", Path.cwd() / "data", Path.cwd()]
    for k in kandidat:
        if k.is_dir() and list(k.glob("sesi-*.csv")):
            return k
    return kandidat[0]


def muat_sesi(folder: Path) -> dict[str, pd.DataFrame]:
    """Muat semua berkas ``sesi-*.csv`` menjadi {nama sesi: DataFrame}."""
    hasil: dict[str, pd.DataFrame] = {}
    for berkas in sorted(folder.glob("sesi-*.csv")):
        try:
            df = pd.read_csv(berkas)
        except (OSError, pd.errors.EmptyDataError, pd.errors.ParserError):
            continue
        if df.empty:
            continue
        df["t_terima"] = pd.to_numeric(df["t_terima"], errors="coerce")
        df["t_kirim"] = pd.to_numeric(df["t_kirim"], errors="coerce")
        for kolom in ("latency_ms", "jitter_ms", "bpm", "spo2", "suhu"):
            if kolom in df.columns:
                df[kolom] = pd.to_numeric(df[kolom], errors="coerce")
        df["waktu"] = pd.to_datetime(df["t_terima"], unit="s")
        df["detik_ke"] = df["t_terima"] - df["t_terima"].min()
        hasil[berkas.stem] = df
    return hasil


def muat_ringkasan(folder: Path) -> pd.DataFrame:
    """Gabungkan seluruh berkas ``*.ringkasan.json`` menjadi satu tabel QoS."""
    baris = []
    for berkas in sorted(folder.glob("sesi-*.ringkasan.json")):
        try:
            data = json.loads(berkas.read_text("utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        catatan = data.get("catatan", {}) or {}
        for node_id, s in (data.get("per_node") or {}).items():
            if not s.get("paket_diterima"):
                continue
            baris.append({
                "sesi": data.get("nama_sesi", berkas.stem),
                "node": node_id,
                "paket": s["paket_diterima"],
                "latency_ms": s["latency_rata_ms"],
                "jitter_ms": s["jitter_rata_ms"],
                "loss_%": s["packet_loss_persen"],
                "throughput_paket/s": s["throughput_paket_per_detik"],
                "skenario": catatan.get("skenario", "-"),
                "jarak_m": catatan.get("jarak_m"),
            })
    return pd.DataFrame(baris)


# ---------------------------------------------------------------------------
# Laporan teks
# ---------------------------------------------------------------------------

def laporan_teks(folder: Path) -> None:
    sesi = muat_sesi(folder)
    ringkasan = muat_ringkasan(folder)

    if not sesi:
        print(f"Belum ada berkas sesi-*.csv di {folder}.")
        print("Jalankan dulu: python src/receiver/receiver.py --source sim --durasi 60")
        return

    print(f"Folder data : {folder}")
    print(f"Jumlah sesi : {len(sesi)}\n")

    for nama, df in sesi.items():
        node = sorted(df["node_id"].dropna().unique())
        print(f"=== {nama} — {len(df)} paket — node: {', '.join(map(str, node))}")
        rentang = df["waktu"].max() - df["waktu"].min()
        print(f"    rentang waktu    : {rentang}")
        for kolom, label, satuan in (("bpm", "detak jantung", "bpm"),
                                     ("spo2", "SpO2", "%"),
                                     ("suhu", "suhu tubuh", "C")):
            if kolom in df.columns and df[kolom].notna().any():
                s = df[kolom].dropna()
                print(f"    {label:14s} : rata {s.mean():6.2f} {satuan} "
                      f"(min {s.min():.2f}, maks {s.max():.2f}, n={len(s)})")
        if "latency_ms" in df.columns and df["latency_ms"].notna().any():
            print(f"    latency          : rata {df['latency_ms'].mean():6.2f} ms "
                  f"(maks {df['latency_ms'].max():.2f})")
        print()

    if not ringkasan.empty:
        print("=== Ringkasan QoS per sesi dan node")
        with pd.option_context("display.width", 160, "display.max_columns", 20):
            print(ringkasan.to_string(index=False))


def simpan_grafik(folder: Path) -> list[Path]:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    tujuan = AKAR / "docs" / "grafik"
    tujuan.mkdir(parents=True, exist_ok=True)
    dibuat: list[Path] = []

    sesi = muat_sesi(folder)
    for nama, df in sesi.items():
        tanda_vital = [k for k in ("bpm", "spo2", "suhu") if k in df.columns
                       and df[k].notna().any()]
        jumlah_panel = len(tanda_vital) + 1
        if jumlah_panel == 1:
            continue
        fig, sumbu = plt.subplots(jumlah_panel, 1, figsize=(10, 2.6 * jumlah_panel),
                                  sharex=True)
        for ax, kolom in zip(sumbu, tanda_vital):
            for node_id, grup in df.groupby("node_id"):
                ax.plot(grup["detik_ke"], grup[kolom], marker="o", ms=3, label=str(node_id))
            ax.set_ylabel(kolom)
            ax.grid(alpha=0.3)
            ax.legend(fontsize=8)
        sumbu[0].set_title(f"Tanda vital — {nama}")
        sumbu[-1].plot(df["detik_ke"], df["latency_ms"], color="crimson", marker="o", ms=3)
        sumbu[-1].set_ylabel("latency (ms)")
        sumbu[-1].set_xlabel("detik sejak paket pertama")
        sumbu[-1].grid(alpha=0.3)
        fig.tight_layout()
        keluar = tujuan / f"tanda-vital-{nama}.png"
        fig.savefig(keluar, dpi=130)
        plt.close(fig)
        dibuat.append(keluar)
    return dibuat


# ---------------------------------------------------------------------------
# Dashboard Streamlit
# ---------------------------------------------------------------------------

def jalankan_streamlit(folder: Path) -> None:
    import streamlit as st
    import matplotlib.pyplot as plt

    st.set_page_config(page_title="Pemantauan Tanda Vital + QoS", layout="wide")
    st.title("Pemantauan Tanda Vital Nirkabel + Pengukuran QoS")
    st.caption("MKKL1029 — dua node sensor (pergelangan dan dada) melalui BLE dan MQTT")

    sesi = muat_sesi(folder)
    ringkasan = muat_ringkasan(folder)

    if not sesi:
        st.warning(f"Belum ada berkas sesi-*.csv di {folder}.")
        st.code("python src/receiver/receiver.py --source sim --durasi 60", language="bash")
        return

    if st.button("Muat ulang data"):
        st.rerun()

    pilih = st.selectbox("Sesi pengukuran", sorted(sesi))
    df = sesi[pilih]

    kolom = st.columns(4)
    for col, (nama, kolom_data, satuan) in zip(kolom, [
        ("Detak jantung", "bpm", "bpm"),
        ("SpO2", "spo2", "%"),
        ("Suhu tubuh", "suhu", "C"),
        ("Latency rata-rata", "latency_ms", "ms"),
    ]):
        if kolom_data in df.columns and df[kolom_data].notna().any():
            col.metric(nama, f"{df[kolom_data].mean():.2f} {satuan}")
        else:
            col.metric(nama, "—")

    st.subheader("Grafik tanda vital")
    grafik = [k for k in ("bpm", "spo2", "suhu") if k in df.columns and df[k].notna().any()]
    if grafik:
        fig, sumbu = plt.subplots(len(grafik) + 1, 1, figsize=(10, 2.4 * (len(grafik) + 1)),
                                  sharex=True)
        for ax, kolom in zip(sumbu, grafik):
            for node_id, grup in df.groupby("node_id"):
                ax.plot(grup["detik_ke"], grup[kolom], marker="o", ms=3, label=str(node_id))
            ax.set_ylabel(kolom)
            ax.grid(alpha=0.3)
            ax.legend(fontsize=8)
        sumbu[-1].plot(df["detik_ke"], df["latency_ms"], color="crimson", marker="o", ms=3)
        sumbu[-1].set_ylabel("latency (ms)")
        sumbu[-1].set_xlabel("detik sejak paket pertama")
        sumbu[-1].grid(alpha=0.3)
        fig.tight_layout()
        st.pyplot(fig)

    st.subheader("Jitter per paket")
    if "jitter_ms" in df.columns:
        st.line_chart(df[["detik_ke", "jitter_ms"]].dropna().set_index("detik_ke"))

    st.subheader("Ringkasan QoS seluruh sesi")
    if ringkasan.empty:
        st.info("Belum ada berkas ringkasan JSON.")
    else:
        st.dataframe(ringkasan, use_container_width=True)
        st.caption("Tabel ini yang ditempel ke docs/pengujian.md.")


# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description="Dashboard tanda vital dan hasil QoS")
    ap.add_argument("--data", default=None, help="folder berisi sesi-*.csv")
    ap.add_argument("--plot", action="store_true",
                    help="simpan grafik PNG ke docs/grafik/ lalu keluar")
    args = ap.parse_args()
    folder = folder_data(args.data)

    if args.plot:
        dibuat = simpan_grafik(folder)
        if dibuat:
            print("Grafik disimpan:")
            for p in dibuat:
                print(f"  {p}")
        else:
            print("Belum ada sesi yang bisa digambarkan.")
        return

    # Dijalankan lewat `streamlit run` -> pakai mode dashboard.
    try:
        from streamlit.runtime import exists as _streamlit_aktif
        if _streamlit_aktif():
            jalankan_streamlit(folder)
            return
    except ImportError:
        pass

    laporan_teks(folder)
    print(f"\nUntuk dashboard grafik: pip install streamlit lalu\n"
          f"  streamlit run {Path(__file__).name}")
    print(f"Untuk menyimpan grafik PNG: python {Path(__file__).name} --plot")


if __name__ == "__main__":
    main()
