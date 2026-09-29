# Wearable Health Monitor — Dua Node Sensor Nirkabel untuk Pemantauan Detak Jantung, SpO2, dan Suhu Tubuh

**Mata kuliah:** Wireless / Mobile Computing (MKKL1029) · Semester 7
**Program Studi Ilmu Komputer — Fakultas Sains, Teknologi dan Ilmu Kesehatan**
**Universitas Bina Bangsa Getsempena**
**Dosen Pengampu:** Ahmad Mujahid Abdurrahman, S.Kom, M.T.

Dua node sensor nirkabel yang dikenakan pada tubuh: satu di pergelangan tangan untuk membaca detak jantung dan saturasi oksigen (SpO2) melalui sensor MAX30102, dan satu di dada atau ketiak untuk membaca suhu tubuh melalui sensor MLX90614. Node pertama mengirim data lewat BLE, node kedua lewat WiFi/MQTT, keduanya menuju laptop penerima yang menghitung empat parameter kualitas layanan — latency, throughput, packet loss, dan jitter — lalu menampilkannya pada dashboard. Proyek ini juga menguji keterbatasan kanal nirkabel pada berbagai jarak, keberadaan penghalang, interferensi, dan saat koneksi terputus.

## Anggota Kelompok

| No | Nama | NIM | Peran |
|---|---|---|---|
| 1 | Yogi Prasetya Sadewa | 23210060 | Ketua kelompok; perangkat lunak penerima, perhitungan QoS, dan integrasi BLE/MQTT |
| 2 | Asmarudin | 23210133 | Perakitan node 1 (ESP32 + MAX30102) dan kalibrasi pembacaan detak jantung |
| 3 | Deski Taiza | 23210003 | Perakitan node 2 (ESP32 + MLX90614) dan pengujian pembacaan suhu tubuh |
| 4 | Akhsanul Taqwim | 23210006 | Dashboard pemantauan dan visualisasi grafik secara langsung |
| 5 | Wira | 23210045 | Pelaksanaan pengujian lapangan: variasi jarak, penghalang, dan interferensi |
| 6 | Abadi | 23210004 | Pengujian daya tahan baterai dan pencatatan waktu pemakaian |
| 7 | Ferdyan Ardhani | 23210039 | Pengolahan data hasil pengukuran menjadi tabel dan grafik laporan |
| 8 | Muhammad Iqbal | 23210142 | Perbandingan hasil pengukuran dengan alat pembanding (oximeter dan termometer) |
| 9 | Meriandi Wahyu Kurniawan | [NIM] | Dokumentasi, README, dan pengelolaan repository |

> Kelompok berjumlah 9 orang; panduan menetapkan 4–5 orang sehingga jumlah ini
> dimintakan persetujuan dosen pada pertemuan ke-2. Setiap anggota melakukan
> commit dari akun masing-masing.

## Rencana Proyek

- Google Docs (dibagikan kepada dosen dengan akses komentar) — tautan: [MKKL1029 — Wearable Health Monitor](https://docs.google.com/document/d/1PLGa2ZfahxGsNL8aDKZaN4ubkPujqq-t2FL4Pyx6I9A/edit)
- Salinan di repository: [`docs/rencana-proyek.md`](docs/rencana-proyek.md)

## Cara Menjalankan

### Kebutuhan

- Python 3.10 atau lebih baru (penerima dan dashboard)
- Arduino IDE dengan pustaka ESP32, SparkFun MAX3010x, dan Adafruit MLX90614
- 2 unit ESP32 beserta sensor MAX30102 dan MLX90614
- Broker MQTT (Mosquitto) pada laptop penerima

### 1. Menyiapkan lingkungan penerima

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r src/receiver/requirements.txt
```

### 2. Menjalankan broker MQTT

```bash
mosquitto -c broker/mosquitto.conf
```

### 3. Mengunggah program ke kedua node

```bash
# Node 1 (pergelangan, BLE)
arduino-cli compile --upload -p /dev/ttyUSB0 firmware/node1_wrist

# Node 2 (dada, WiFi/MQTT)
arduino-cli compile --upload -p /dev/ttyUSB1 firmware/node2_suhu
```

### 4. Menjalankan penerima dan dashboard

```bash
pip install -r src/receiver/requirements.txt

# Uji cepat tanpa perangkat keras (dua node tiruan):
python src/receiver/receiver.py --source sim --durasi 60 --skenario uji

# Pengukuran sesungguhnya:
python src/receiver/receiver.py --source dua --mqtt-host 192.168.1.10

# Dashboard grafik (perlu: pip install streamlit):
streamlit run src/dashboard/app.py
```

> Hasil tiap sesi: satu CSV per paket dan satu `.ringkasan.json` berisi keempat
> parameter QoS pada folder `data/`. Angka dari mode `--source sim` adalah
> tiruan untuk menguji perhitungan, bukan hasil pengukuran — lihat
> [`docs/pengujian.md`](docs/pengujian.md).

## Struktur Repository

```
mkkl1029-health-monitor/
├── README.md
├── docs/
│   ├── rencana-proyek.md
│   ├── arsitektur.md
│   ├── pengujian.md
│   ├── diagrams/
│   └── MKKL1029-Wearable-Health-Monitor.docx
├── firmware/
│   ├── node1_wrist/     # ESP32 + MAX30102, kirim via BLE
│   └── node2_suhu/      # ESP32 + MLX90614, kirim via WiFi/MQTT
├── broker/              # konfigurasi Mosquitto
├── src/
│   ├── receiver/        # penerima BLE + MQTT dan perhitungan QoS
│   └── dashboard/       # tampilan grafik
└── data/                # hasil pengukuran (CSV per sesi)
```

## Dokumentasi

| Berkas | Isi |
|---|---|
| `docs/rencana-proyek.md` | Rencana proyek, target UTS dan UAS, pembagian kerja per minggu, risiko |
| `docs/arsitektur.md` | Penjelasan komponen, alur data, dan rumus perhitungan QoS |
| `docs/pengujian.md` | Skenario pengujian, cara menjalankan, dan catatan hasil |
| `docs/diagrams/` | Diagram arsitektur dan titik pengukuran |

## Pemenuhan Ketentuan Mata Kuliah

- **Perangkat nirkabel nyata**: dua node ESP32 yang benar-benar mengirim data melalui BLE dan WiFi.
- **Minimal dua topik mata kuliah**: sistem sensor, arsitektur nirkabel, standar IEEE 802.11 dan BLE, serta Quality of Service.
- **Minimal dua parameter QoS**: diukur empat sekaligus — latency, throughput, packet loss, dan jitter.
- **Keterbatasan nirkabel dibahas**: jarak, penghalang, interferensi 2,4 GHz, daya, dan koneksi terputus.

## Aturan Kerja Kelompok

- Commit dilakukan dari akun GitHub masing-masing anggota, bukan satu akun untuk seluruh kelompok.
- Pesan commit deskriptif dan menunjukkan kemajuan; dikerjakan minimal sekali per minggu per anggota.
- Pekerjaan bersama menggunakan branch dan pull request; pembagian tugas dicatat pada Issues.
- Kredensial, token, dan berkas `.env` tidak boleh masuk repository.

## Status

| Tahap | Target | Status |
|---|---|---|
| Pertemuan 2 | Rencana proyek, repository, undangan kolaborator | Selesai |
| Pertemuan 8 (UTS) | Kedua node menyala dan mengirim payload berisi waktu kirim serta nomor urut … | Belum dimulai |
| Pertemuan 16 (UAS) | Dua node sensor nirkabel yang berjalan penuh, dari pembacaan sensor sampai tampilan dashbo … | Belum dimulai |
