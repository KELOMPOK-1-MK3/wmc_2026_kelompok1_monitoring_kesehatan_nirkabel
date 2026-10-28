# Arsip — Topik Lama (Tidak Dipakai Lagi)

Isi folder ini adalah pekerjaan pada topik **awal** mata kuliah MKKL1029:
*wearable health monitor* — dua node ESP32 dengan sensor MAX30102 (detak jantung,
SpO2) dan MLX90614 (suhu tubuh), dikirim lewat BLE dan WiFi/MQTT.

Topik itu tidak dilanjutkan karena kedua sensor tidak tersedia, lalu diganti dengan
**pemantauan QoS banyak perangkat memakai ESP32 sebagai gerbang** (lihat `../README.md`
dan `../docs/proposal/`).

| Berkas | Isi |
|---|---|
| `node1_wrist/` | Firmware node pergelangan (BLE + MAX30102) |
| `node2_suhu/` | Firmware node suhu (WiFi/MQTT + MLX90614) |
| `src-monitoring-kesehatan/` | Penerima BLE + MQTT, perhitungan QoS, dashboard Streamlit |
| `broker-mosquitto/` | Konfigurasi broker MQTT untuk topik lama |
| `MKKL1029-Wearable-Health-Monitor.docx` | Dokumen rencana topik lama |
| `diagram-arsitektur-mkkl1029.png` | Diagram arsitektur topik lama |

Disimpan, bukan dihapus, supaya jejak kerja sebelumnya tetap terlihat. Jangan dipakai
sebagai acuan penilaian topik sekarang.
