# Gerbang WiFi ESP32 — Pemantauan Kualitas Jaringan Nirkabel pada Banyak Perangkat

**Mata kuliah:** Wireless / Mobile Computing (MKKL1029) · Semester 7
**Program Studi Ilmu Komputer — Fakultas Sains, Teknologi dan Ilmu Kesehatan**
**Universitas Bina Bangsa Getsempena**
**Dosen Pengampu:** Ahmad Mujahid Abdurrahman, S.Kom, M.T.

Satu papan ESP32 dipakai sebagai gerbang sekaligus pencatat: ia tersambung ke hotspot
2,4 GHz yang menyediakan internet, lalu memancarkan hotspot sendiri untuk empat
perangkat yang diukur. Trafik keempat perangkat diteruskan lewat jalur uplink (NAPT),
sementara catatan pemakaian tiap perangkat dikirim ke laptop lewat **kabel USB** —
bukan lewat jaringan. Laptop hanya menampilkan dashboard dan menyimpan riwayat; ESP32
tidak menyimpan apa pun. Dari catatan itu kelompok mengukur empat parameter QoS
(latency, throughput, packet loss, jitter) pada jumlah perangkat yang berbeda-beda:
1, 2, lalu 4 perangkat aktif bersamaan.

> **Batas yang kita akui di muka.** Isi trafik terenkripsi (HTTPS/QUIC) tidak bisa
> dibaca, jadi yang dicatat hanya besar data, jumlah paket, dan nama domain dari DNS.
> Identifikasi kegiatan perangkat adalah perkiraan dari nama domain, bukan kepastian.
> ESP32 bukan pengganti router: throughput-nya terbatas sekitar 1–2 Mbps.

## Anggota Kelompok

| No | Nama | NIM | Bagian yang dikerjakan |
|---|---|---|---|
| 1 | Paris Mursidan Aufal | 23210125 | Penerima di laptop + basis data SQLite |
| 2 | Yogi Prasetya Sadewa | 23210060 | Ketua kelompok; penghitung QoS + penggabung data per perangkat |
| 3 | M. Sidiq Prasetio | 23210075 | Dashboard web + grafik langsung |
| 4 | Deski Taiza | 23210003 | Firmware mode ganda (hotspot + klien) + NAPT |
| 5 | Wira | 23210045 | Firmware pencatat pemakaian per perangkat + DNS |
| 6 | T. Zain Wardana | 23210001 | Pengujian QoS + laporan |

> Kelompok berjumlah 6 orang; setiap anggota mengerjakan satu bagian dan melakukan
> commit dari akun GitHub masing-masing.

## Proposal

- `docs/proposal/PROPOSAL WMC - Pemantauan QoS Multi Perangkat - Kelompok 1.docx` — versi yang dikumpulkan
- `docs/proposal/PROPOSAL WMC - Pemantauan QoS Multi Perangkat - Kelompok 1.pdf` — versi baca cepat
- `docs/rencana-proyek.md` — salinan isi rencana dalam bentuk markdown

## Cara Menjalankan

### 1. Unggah firmware gerbang

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --upload -p /dev/cu.usbserial-XXXX firmware/esp32-gateway
arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200
```

Sebelum mengunggah, isi `UPLINK_SSID`, `UPLINK_PASS`, `AP_SSID`, dan `AP_PASS` pada
bagian atas `firmware/esp32-gateway/esp32-gateway.ino`. Sandi asli tidak boleh masuk
repository.

### 2. Susun urutan pengujian

1. Nyalakan hotspot HP sumber internet pada pita **2,4 GHz** (ESP32 tidak mengenal 5 GHz).
2. Colok ESP32 ke laptop lewat USB-C, buka monitor serial.
3. Tunggu sampai muncul `# uplink ... ip ...` — artinya jalur internet siap.
4. Sambungkan perangkat uji (HP-2 sampai HP-5) ke hotspot `ESP32-QoS`.
5. Jalankan kegiatan yang diuji, amati baris `hb` dan `ev` pada monitor serial.

Contoh keluaran serial:

```
# uplink HP-1, ip 192.168.43.12, kanal 6
ev,connect,3c:5a:b4:11:22:33
hb,1000,1,-52,281992
hb,2000,1,-51,281976
```

Arti tiap baris:

| Baris | Isi |
|---|---|
| `# uplink ...` | Jalur internet siap, beserta IP yang diterima ESP32 |
| `ev,connect,<mac>` | Ada perangkat uji tersambung ke hotspot ESP32 |
| `ev,disconnect,<mac>` | Perangkat uji terlepas |
| `hb,<ms>,<klien>,<rssi>,<heap>` | Denyut tiap 1 detik: waktu sejak nyala, jumlah klien, kekuatan sinyal uplink (dBm), sisa memori |

## Struktur Repository

```
wmc_2026_kelompok1_monitoring_kesehatan_nirkabel/
├── README.md
├── docs/
│   ├── proposal/        # proposal yang dikumpulkan (docx + pdf)
│   ├── arsitektur.md    # komponen, alur data, rumus QoS
│   ├── pengujian.md     # skenario S1–S5 dan cara mencatat hasil
│   ├── rencana-proyek.md
│   └── diagrams/        # diagram arsitektur
├── firmware/
│   └── esp32-gateway/   # ESP32: klien + hotspot + NAPT + catatan lewat USB serial
├── arsip/               # kode topik lama (node sensor kesehatan) — disimpan, tidak dipakai
└── data/                # hasil pengukuran
```

## Dokumentasi

| Berkas | Isi |
|---|---|
| `docs/rencana-proyek.md` | Masalah, tujuan, arsitektur, target UTS dan UAS, pembagian kerja, risiko |
| `docs/arsitektur.md` | Komponen, alur data, rumus QoS, dan apa yang tidak bisa dideteksi |
| `docs/pengujian.md` | Skenario S1–S5, batas nilai yang diharapkan, cara mencatat hasil |
| `docs/diagrams/` | Diagram arsitektur sistem |

## Pemenuhan Ketentuan Mata Kuliah

- **Perangkat nirkabel nyata**: ESP32 bekerja pada dua peran radio sekaligus dan
  meneruskan trafik empat perangkat lewat WiFi 2,4 GHz.
- **Minimal dua topik mata kuliah**: arsitektur 802.11 (mode infrastruktur dan AP),
  NAT/NAPT, DNS, serta Quality of Service.
- **Minimal dua parameter QoS**: diukur empat — latency, throughput, packet loss, jitter.
- **Keterbatasan nirkabel dibahas**: bagi-bagi bandwidth, batas 4 klien per titik akses,
  pita 2,4 GHz yang padat, jarak dan penghalang, serta hotspot HP yang tidur saat kosong.

## Aturan Kerja Kelompok

- Commit dilakukan dari akun GitHub masing-masing anggota, bukan satu akun untuk seluruh kelompok.
- Pesan commit deskriptif dan menunjukkan kemajuan; dikerjakan minimal sekali per minggu per anggota.
- Pekerjaan bersama menggunakan branch dan pull request; pembagian tugas dicatat pada Issues.
- Kredensial, token, sandi hotspot, dan berkas `.env` tidak boleh masuk repository.

## Status

| Tahap | Target | Status |
|---|---|---|
| Pertemuan 2 | Rencana proyek, proposal, repository | Selesai |
| Pertemuan 8 (UTS) | ESP32 tersambung ke hotspot, empat perangkat mendapat internet lewat ESP32, catatan `hb`/`ev` terbaca di laptop lewat USB | Firmware mode ganda + NAPT selesai dan lolos kompilasi; pengujian dengan perangkat keras belum |
| Pertemuan 16 (UAS) | Penghitung pemakaian per perangkat, dashboard, dan hasil pengukuran S1–S5 | Belum dimulai |

Yang **sudah ada**:

- `firmware/esp32-gateway` — mode ganda (klien + hotspot), NAPT, sambung ulang otomatis,
  dan catatan serial `hb`/`ev`. Lolos kompilasi `arduino-cli` (`esp32:esp32` 3.3.12,
  881.364 byte / 67% ruang program).

Yang **belum ada** (jangan diklaim selesai):

- Penghitung byte dan paket **per perangkat** serta pembaca nama domain (DNS).
- Penerima di laptop yang membaca USB serial, menghitung QoS, dan menyimpan ke SQLite.
- Dashboard.
- Seluruh pengukuran S1–S5 — semua angka pada `docs/pengujian.md` masih kosong.

## Catatan Perubahan Arah Topik

Topik awal mata kuliah ini adalah *wearable health monitor* dua node (MAX30102 +
MLX90614). Topik itu tidak dilanjutkan karena kedua sensor tidak tersedia, lalu
diganti menjadi pemantauan QoS banyak perangkat dengan ESP32 sebagai gerbang. Kode,
dokumen, dan diagram topik lama dipindahkan ke `arsip/` — tidak dihapus, agar jejak
kerja sebelumnya tetap terlihat.
