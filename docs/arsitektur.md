# Arsitektur Sistem

![Diagram arsitektur](diagrams/arsitektur-gateway-esp32.png)

*Gambar 1. Empat perangkat uji melewati ESP32; catatan pemakaian dikirim ke laptop lewat kabel USB.*

## Peran Tiap Bagian

| Bagian | Peran | Catatan |
|---|---|---|
| HP-1 | Hotspot 2,4 GHz penyedia internet | Jalur, bukan objek ukur. Dipilih 2,4 GHz karena ESP32 tidak mengenal 5 GHz |
| ESP32 | Gerbang + pencatat | Mode ganda: klien (uplink) sekaligus hotspot (untuk perangkat uji), ditambah NAPT |
| HP-2 … HP-5 | Perangkat yang diukur | Tersambung ke hotspot ESP32, bukan ke HP-1 — kalau ke HP-1, trafiknya tidak lewat ESP32 dan tidak ada yang bisa diukur |
| Kabel USB-C | Jalur catatan ke laptop | Telemetri lewat serial, bukan jaringan; laptop tidak perlu ikut WiFi mana pun |
| Laptop | Penerima + dashboard + penyimpanan | Satu-satunya tempat data disimpan |

## Alur Data

1. Perangkat uji mengirim permintaan (mis. membuka media sosial).
2. Permintaan masuk ke hotspot ESP32.
3. ESP32 meneruskannya lewat jalur uplink ke HP-1 dengan NAPT, sambil mencatat: perangkat mana, berapa byte naik dan turun, berapa paket, sambungan aktif, dan nama domain yang ditanyakan lewat DNS.
4. Catatan itu dikirim ke laptop sebagai baris CSV pendek lewat kabel USB.
5. Penerima menambah cap waktu terima, menghitung parameter QoS, menyimpan ke SQLite, dan meneruskannya ke dashboard.
6. ESP32 tidak menyimpan riwayat; kalau kabel dicabut, catatan berhenti. Riwayat hanya ada di laptop.

## Rumus

| Parameter | Rumus | Satuan |
|---|---|---|
| Latency | `t_terima − t_kirim` (dengan koreksi selisih jam) | milidetik |
| Jitter | `abs(latency[i] − latency[i−1])` | milidetik |
| Packet loss | `1 − (seq unik diterima / seq seharusnya)` | persen |
| Throughput | `jumlah byte diterima / durasi` | bit per detik |

Catatan latency: jam ESP32 dan jam laptop tidak sama, jadi selisih jam diestimasi
dari nilai terkecil `t_terima − t_kirim` sepanjang sesi. Angka yang dilaporkan adalah
**delay relatif terhadap paket tercepat**, bukan delay absolut, dan itu wajib
disebutkan saat melaporkan hasil. Nilai selisih jam yang dipakai ditulis ke berkas
ringkasan setiap sesi.

## Protokol dan Lapisan

| Lapisan | Yang dipakai | Keterangan |
|---|---|---|
| Fisik / MAC | IEEE 802.11 b/g/n, pita 2,4 GHz, kanal 6 | Satu radio dipakai untuk dua peran (klien + AP), jadi throughput terbagi |
| Jaringan | IPv4, NAT/NAPT | `WiFi.AP.enableNAPT(true)` pada ESP32 core 3.x |
| Nama | DNS | Hanya nama domain yang terlihat; isi permintaan tidak |
| Aplikasi | HTTP/HTTPS, QUIC | Isi terenkripsi — tidak dibaca, hanya volumenya yang dihitung |
| Telemetri | Serial USB (115200 baud), CSV satu arah | Tidak memakai jaringan, tidak memakai broker |

## Yang Bisa dan Tidak Bisa Diketahui

| Pertanyaan | Bisa? | Sebab |
|---|---|---|
| Berapa byte naik/turun tiap perangkat | Bisa | Dihitung per alamat IP/MAC di ESP32 |
| Berapa paket dan sambungan aktif tiap perangkat | Bisa | Sama, per alamat |
| Situs/aplikasi apa yang dibuka | Sebagian | Hanya nama domain dari DNS. Isi HTTPS/QUIC terenkripsi; aplikasi bisa memakai IP langsung atau DNS terenkripsi |
| Kekuatan sinyal tiap perangkat | Sebagian | RSSI klien AP tidak disediakan API Arduino; diukur lewat cara lain atau dicatat manual |
| Kecepatan tiap perangkat | Bisa, terbatas | Batas atas ±1–2 Mbps total karena satu radio dan setengah-duplex |

## Batas Perangkat Keras

- SoftAP bawaan ESP32 sanggup 4 klien. Skenario lima perangkat uji butuh pengaturan ulang batas ini.
- Satu radio untuk dua peran: setiap detik yang dipakai memancarkan hotspot tidak dipakai menerima dari uplink, sehingga throughput lebih rendah daripada angka teoretis.
- Hotspot HP sering mematikan diri saat tidak ada trafik; firmware menyambung ulang tiap 5 detik.
- Banyak hotspot HP mengisolasi klien, sehingga perangkat uji tidak dapat saling terhubung. Untuk pengukuran ini tidak masalah karena jalur data hanya menuju internet.
