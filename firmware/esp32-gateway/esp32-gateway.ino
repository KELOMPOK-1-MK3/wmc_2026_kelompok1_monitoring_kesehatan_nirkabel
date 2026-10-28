/*
 * esp32-gateway — gerbang WiFi + pencatat pemakaian per perangkat
 *
 * Peran papan ini (docs/arsitektur.md):
 *   - klien (STA)  : tersambung ke hotspot 2,4 GHz yang menyediakan internet
 *   - hotspot (AP) : menampung sampai 4 perangkat yang diukur
 *   - NAPT         : meneruskan trafik perangkat uji ke jalur uplink
 *   - pencatat     : menghitung byte/paket per perangkat, mencatat nama domain
 *                    dari pertanyaan DNS, dan mengukur waktu sambung ke internet
 *   - pengirim     : semua catatan dikirim ke laptop lewat kabel USB (serial),
 *                    bukan lewat jaringan. Papan tidak menyimpan riwayat.
 *
 * Cara menghitung: papan mendengarkan bingkai 802.11 di kanalnya sendiri
 * (mode promiscuous, hanya bingkai data). Untuk setiap perangkat yang dikenal,
 * dijumlahkan besar bingkai naik (dari perangkat) dan turun (ke perangkat),
 * jumlah bingkai, serta jumlah bingkai yang ditandai ulang (retry) — angka
 * retry inilah yang dipakai sebagai petunjuk packet loss di sisi radio.
 *
 * Batas yang perlu diketahui:
 *   - Angka byte dihitung dari bingkai 802.11 (bukan hanya isi IP), jadi
 *     beberapa persen lebih besar daripada hitungan di laptop.
 *   - Bingkai yang diulang ikut terhitung dua kali; itu disengaja, karena
 *     pengulangan itu memang memakan waktu udara.
 *   - Isi HTTPS/QUIC tidak dibaca. Yang terlihat hanya nama domain dari DNS
 *     biasa; aplikasi yang memakai DNS terenkripsi atau IP langsung tidak
 *     tercatat namanya.
 *
 * Format baris serial (CSV satu arah, 115200 baud):
 *   hb,<ms>,<jumlah_klien>,<rssi_uplink>,<heap>
 *   st,<mac>,<ip>,<rssi>                                  (tiap 1 detik per perangkat)
 *   tr,<mac>,<ip>,<byte_naik>,<byte_turun>,<bingkai_naik>,<bingkai_turun>,<bingkai_retry>
 *   dns,<mac>,<ip>,<nama_domain>                          (saat ada pertanyaan DNS)
 *   rtt,<ms>|rtt,-1                                       (tiap 2 detik, gagal = -1)
 *
 * Board  : ESP32 DevKit V1 (esp32:esp32:esp32), core 3.3.12
 */

#include <WiFi.h>
#include <esp_wifi.h>

// --- Kredensial --------------------------------------------------------------
// Sandi WiFi jangan ditulis di berkas ini (repository publik). Isi
// firmware/esp32-gateway/rahasia.h — berkas itu masuk .gitignore.
// Tanpa rahasia.h, papan tetap menyala sebagai hotspot dan mencatat bingkai,
// hanya saja jalur uplink tidak tersambung (rtt,-1).
#if __has_include("rahasia.h")
  #include "rahasia.h"
#endif

#ifndef UPLINK_SSID
  #define UPLINK_SSID "HP-1"
#endif
#ifndef UPLINK_PASS
  #define UPLINK_PASS "ganti-sandi-disini"
#endif
#ifndef AP_SSID
  #define AP_SSID "esp32-qos"
#endif
#ifndef AP_PASS
  #define AP_PASS "qos12345"
#endif

#define BATAS_KLIEN 4      // HP-2..HP-5; softAP bawaan sanggup 4 klien
#define MAKS_PERANGKAT 8   // ruang tabel (klien + sisa untuk sementara)
#define INTERVAL_LAPOR 1000
#define INTERVAL_RTT 2000
#define BATAS_RTT_MS 2000
// ---------------------------------------------------------------------------

struct Perangkat {
  uint8_t mac[6];
  uint32_t ip;
  uint32_t byte_naik, byte_turun;
  uint32_t bingkai_naik, bingkai_turun, bingkai_retry;
  uint32_t terakhir_lihat;
  bool dipakai;
};

static Perangkat tabel[MAKS_PERANGKAT];
static uint8_t mac_ap[6];
static uint32_t klien = 0;
static uint32_t terakhir_lapor = 0, terakhir_rtt = 0;
static portMUX_TYPE kunci = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------- utilitas --

static void kirim(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

static void mac_teks(const uint8_t *m, char *out) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static Perangkat *cari_perangkat(const uint8_t *mac) {
  for (int i = 0; i < MAKS_PERANGKAT; i++) {
    if (tabel[i].dipakai && memcmp(tabel[i].mac, mac, 6) == 0) return &tabel[i];
  }
  for (int i = 0; i < MAKS_PERANGKAT; i++) {
    if (!tabel[i].dipakai) {
      memset(&tabel[i], 0, sizeof(Perangkat));
      memcpy(tabel[i].mac, mac, 6);
      tabel[i].dipakai = true;
      return &tabel[i];
    }
  }
  return nullptr;  // tabel penuh
}

static bool dari_papan(const uint8_t *mac) { return memcmp(mac, mac_ap, 6) == 0; }

// ------------------------------------------------------------- baca DNS ----

// Ambil nama domain dari paket DNS (port 53). Hanya bentuk paling umum:
// satu pertanyaan, label biasa, tanpa kompresi.
static void catat_dns(const uint8_t *dns, int len, uint32_t ip_klien, const uint8_t *mac_klien) {
  if (len < 13) return;
  uint16_t qdcount = (dns[4] << 8) | dns[5];
  if (qdcount < 1) return;

  char nama[80];
  int pos = 12, out = 0;
  while (pos < len && out < (int)sizeof(nama) - 2) {
    uint8_t panjang = dns[pos++];
    if (panjang == 0) break;
    if (panjang > 63) return;             // label panjang = bukan nama biasa
    if (pos + panjang > len) return;
    if (out) nama[out++] = '.';
    for (int i = 0; i < panjang && out < (int)sizeof(nama) - 2; i++) {
      char c = (char)dns[pos + i];
      nama[out++] = (c > 32 && c < 127) ? c : '?';
    }
    pos += panjang;
  }
  if (out < 4) return;                    // terlalu pendek untuk nama domain
  nama[out] = 0;

  char teks_mac[18];
  mac_teks(mac_klien, teks_mac);
  kirim("dns,%s,%u.%u.%u.%u,%s", teks_mac, (ip_klien >> 24) & 0xFF, (ip_klien >> 16) & 0xFF,
        (ip_klien >> 8) & 0xFF, ip_klien & 0xFF, nama);
}

// --------------------------------------------------- hitung bingkai 802.11 --

static void hitung_bingkai(const uint8_t *f, int len, bool retry) {
  if (len < 24 + 8 + 20) return;
  if ((f[0] & 0x0C) != 0x08) return;          // hanya bingkai data

  bool qos = (f[0] & 0x80) != 0;               // QoS data: header lebih panjang
  int hdr = qos ? 26 : 24;
  if (len < hdr + 8 + 20) return;

  const uint8_t *llc = f + hdr;
  if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03)) return;
  if (!(llc[6] == 0x08 && llc[7] == 0x00)) return;   // IPv4 saja

  const uint8_t *ip = llc + 8;
  int ip_len = len - hdr - 8;
  if ((ip[0] >> 4) != 4) return;
  int hdr_ip = (ip[0] & 0x0F) * 4;
  if (hdr_ip < 20 || ip_len < hdr_ip) return;

  uint32_t sumber = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) | ((uint32_t)ip[14] << 8) | ip[15];
  uint32_t tujuan = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) | ((uint32_t)ip[18] << 8) | ip[19];

  const uint8_t *addr1 = f + 4;   // penerima di radio
  const uint8_t *addr2 = f + 10;  // pengirim di radio

  Perangkat *p = nullptr;
  bool naik = false;
  if (dari_papan(addr1) && !dari_papan(addr2)) {        // perangkat -> papan
    p = cari_perangkat(addr2);
    naik = true;
  } else if (dari_papan(addr2) && !dari_papan(addr1)) { // papan -> perangkat
    p = cari_perangkat(addr1);
  } else {
    return;                                      // bukan trafik perangkat kita
  }
  if (!p) return;

  portENTER_CRITICAL_ISR(&kunci);
  p->ip = naik ? sumber : tujuan;
  p->terakhir_lihat = millis();
  if (naik) {
    p->byte_naik += len;
    p->bingkai_naik++;
  } else {
    p->byte_turun += len;
    p->bingkai_turun++;
  }
  if (retry) p->bingkai_retry++;

  // Pertanyaan DNS dari perangkat: protokol UDP, port tujuan 53.
  if (naik && ip[9] == 17) {
    int off = hdr_ip;
    if (ip_len >= off + 8) {
      const uint8_t *udp = ip + off;
      uint16_t port_tujuan = (udp[2] << 8) | udp[3];
      int panjang_udp = (udp[4] << 8) | udp[5];
      if (port_tujuan == 53 && panjang_udp > 12 && ip_len >= off + 8 + 12) {
        catat_dns(udp + 8, min((int)panjang_udp, ip_len - off - 8), sumber, p->mac);
      }
    }
  }
  portEXIT_CRITICAL_ISR(&kunci);
}

static void pada_bingkai(void *buf, wifi_promiscuous_pkt_type_t tipe) {
  if (tipe != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  const uint8_t *f = pkt->payload;
  int len = pkt->rx_ctrl.sig_len - 4;              // buang FCS
  bool retry = (f[1] & 0x08) != 0;                 // bit retry pada frame control
  hitung_bingkai(f, len, retry);
}

// ------------------------------------------------------------------ RTT ----

// Waktu sambung TCP ke DNS publik: angka kasar kualitas jalur uplink.
static long ukur_rtt() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClient c;
  unsigned long t0 = millis();
  bool ok = c.connect("1.1.1.1", 53, BATAS_RTT_MS);
  long hasil = ok ? (long)(millis() - t0) : -1;
  c.stop();
  return hasil;
}

// ------------------------------------------------------------- peristiwa ----

static void pada_peristiwa(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_AP_STACONNECTED || ev == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    klien = WiFi.softAPgetStationNum();
    const uint8_t *m = (ev == ARDUINO_EVENT_WIFI_AP_STACONNECTED)
                           ? info.wifi_ap_staconnected.mac
                           : info.wifi_ap_stadisconnected.mac;
    char teks[18];
    mac_teks(m, teks);
    kirim("ev,%s,%s", ev == ARDUINO_EVENT_WIFI_AP_STACONNECTED ? "connect" : "disconnect", teks);
  }
}

// ---------------------------------------------------------------- setup ----

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("# esp32-gateway siap; format: hb, st, tr, dns, rtt, ev");

  WiFi.onEvent(pada_peristiwa);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, 6, 0, BATAS_KLIEN);
  WiFi.AP.enableNAPT(true);
  memcpy(mac_ap, WiFi.softAPmacAddress().c_str(), 6);

  WiFi.begin(UPLINK_SSID, UPLINK_PASS);
  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("# uplink %s, ip %s, kanal %d\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(), WiFi.channel());
  } else {
    Serial.println("# uplink gagal; perangkat uji belum punya internet");
  }

  // Dengarkan bingkai data di kanal sendiri untuk menghitung pemakaian.
  wifi_promiscuous_filter_t saring;
  saring.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&saring);
  esp_wifi_set_promiscuous_rx_cb(pada_bingkai);
  esp_wifi_set_promiscuous(true);
}

// ----------------------------------------------------------------- loop ----

void loop() {
  static unsigned long cek = 0;
  if (millis() - cek > 5000) {
    cek = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  if (millis() - terakhir_lapor >= INTERVAL_LAPOR) {
    terakhir_lapor = millis();

    // Daftar klien beserta kekuatan sinyal (dari sisi papan).
    wifi_sta_list_t daftar;
    if (esp_wifi_ap_get_sta_list(&daftar) == ESP_OK) {
      for (int i = 0; i < daftar.num; i++) {
        char teks[18];
        mac_teks(daftar.sta[i].mac, teks);
        Perangkat *p = cari_perangkat(daftar.sta[i].mac);
        uint32_t ip = p ? p->ip : 0;
        kirim("st,%s,%u.%u.%u.%u,%d", teks, (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
              ip & 0xFF, daftar.sta[i].rssi);
      }
    }

    portENTER_CRITICAL(&kunci);
    for (int i = 0; i < MAKS_PERANGKAT; i++) {
      if (!tabel[i].dipakai) continue;
      char teks[18];
      mac_teks(tabel[i].mac, teks);
      uint32_t ip = tabel[i].ip;
      kirim("tr,%s,%u.%u.%u.%u,%lu,%lu,%lu,%lu,%lu", teks, (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF, (unsigned long)tabel[i].byte_naik,
            (unsigned long)tabel[i].byte_turun, (unsigned long)tabel[i].bingkai_naik,
            (unsigned long)tabel[i].bingkai_turun, (unsigned long)tabel[i].bingkai_retry);
    }
    portEXIT_CRITICAL(&kunci);

    long rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    kirim("hb,%lu,%u,%ld,%u", millis(), klien, rssi, (unsigned)ESP.getFreeHeap());
  }

  if (millis() - terakhir_rtt >= INTERVAL_RTT) {
    terakhir_rtt = millis();
    kirim("rtt,%ld", ukur_rtt());
  }
}
