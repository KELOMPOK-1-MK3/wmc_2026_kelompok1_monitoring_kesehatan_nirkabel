/*
 * esp32-gateway — gerbang WiFi + pencatat untuk pemantauan QoS banyak perangkat
 *
 * Peran papan ini (lihat docs/arsitektur.md):
 *   - klien (STA)  : tersambung ke hotspot 2,4 GHz yang menyediakan internet
 *   - hotspot (AP) : menampung maksimal 4 perangkat yang diukur
 *   - NAPT         : meneruskan trafik 4 perangkat itu ke jalur uplink
 *   - pencatat     : mengirim catatan ke laptop lewat kabel USB (serial), tanpa jaringan
 *
 * Yang dikirim ke laptop hanya baris CSV pendek, satu arah:
 *   hb,<ms_sejak_nyala>,<jumlah_klien>,<rssi_uplink>,<heap_bebas>
 *   ev,<connect|disconnect>,<mac_klien>
 *
 * Catatan jujur: penghitung byte per klien dan pembaca nama domain (DNS) BELUM
 * ada di versi ini. Baris `hb` sudah berguna untuk memantau jumlah klien dan
 * kualitas uplink, tetapi belum menjawab "berapa pemakaian tiap perangkat".
 *
 * Board  : ESP32 DevKit V1 (esp32:esp32:esp32), core 3.3.12
 * Serial : 115200 baud
 */

#include <WiFi.h>

// --- Ubah sesuai kondisi pengujian -------------------------------------------
// Hotspot sumber internet (2,4 GHz, WPA2). Jangan simpan sandi asli di repo.
static const char *UPLINK_SSID = "HP-1";
static const char *UPLINK_PASS = "ganti-sandi-disini";

// Hotspot yang dipancarkan papan ini untuk perangkat uji.
static const char *AP_SSID = "ESP32-QoS";
static const char *AP_PASS = "ganti-sandi-disini";

#define BATAS_KLIEN 4  // HP-2..HP-5; softAP bawaan sanggup 4 klien
#define INTERVAL_HB 1000
// ---------------------------------------------------------------------------

static unsigned long terakhir_hb = 0;
static unsigned int klien = 0;

static void kirim(const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

static void pada_peristiwa(WiFiEvent_t ev, WiFiEventInfo_t info) {
  const uint8_t *m = nullptr;
  const char *jenis = nullptr;

  if (ev == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    m = info.wifi_ap_staconnected.mac;
    jenis = "connect";
    klien = WiFi.softAPgetStationNum();
  } else if (ev == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    m = info.wifi_ap_stadisconnected.mac;
    jenis = "disconnect";
    klien = WiFi.softAPgetStationNum();
  } else {
    return;
  }

  kirim("ev,%s,%02x:%02x:%02x:%02x:%02x:%02x", jenis, m[0], m[1], m[2], m[3], m[4], m[5]);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.onEvent(pada_peristiwa);

  // AP + STA sekaligus: satu radio, dua peran.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, 6, 0, BATAS_KLIEN);
  WiFi.AP.enableNAPT(true);  // klien AP ikut keluar lewat uplink STA

  WiFi.begin(UPLINK_SSID, UPLINK_PASS);
  kirim("hb,0,0,0,%u", (unsigned)ESP.getFreeHeap());

  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("# uplink %s, ip %s, kanal %d\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.channel());
  } else {
    Serial.println("# uplink gagal, perangkat uji belum punya internet");
  }
}

void loop() {
  // Sambung ulang uplink kalau putus (hotspot HP sering tidur).
  static unsigned long cek = 0;
  if (millis() - cek > 5000) {
    cek = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  if (millis() - terakhir_hb >= INTERVAL_HB) {
    terakhir_hb = millis();
    long rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    kirim("hb,%lu,%u,%ld,%u", terakhir_hb, klien, rssi, (unsigned)ESP.getFreeHeap());
  }
}
