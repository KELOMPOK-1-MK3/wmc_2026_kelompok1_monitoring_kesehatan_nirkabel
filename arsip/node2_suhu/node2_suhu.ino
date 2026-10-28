/*
 * Node 2 — Wearable di dada / ketiak.
 * ESP32 + MLX90614 (suhu tubuh tanpa sentuh) -> kirim via WiFi + MQTT.
 *
 * Payload JSON:
 *   {"node_id":"chest01","seq":N,"t_kirim":<epoch_ms>,"suhu":36.5}
 *
 * Pustaka yang dipasang di Arduino IDE:
 *   - "Adafruit MLX90614 Library"
 *   - "PubSubClient" (Nick O'Leary)
 * Papan: ESP32 Dev Module.
 *
 * Catatan waktu kirim (t_kirim):
 *   Node ini tersambung WiFi, jadi waktunya diambil dari NTP dan t_kirim
 *   adalah epoch milidetik. Bila NTP gagal, nilainya jatuh ke milidetik sejak
 *   menyala dan penerima menyelaraskan selisihnya pada awal sesi.
 */
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <Adafruit_MLX90614.h>
#include <PubSubClient.h>

// ---------------------------------------------------------------- pengaturan
static const char* NODE_ID = "chest01";
static const char* MQTT_TOPIC = "klinik/suhu/chest01";

// Isi sesuai jaringan pengujian. Jangan di-commit ke repository publik.
static const char* WIFI_SSID = "ISI_SSID";
static const char* WIFI_PASS = "ISI_SANDI";
static const char* MQTT_HOST = "192.168.1.10";   // laptop penerima / broker
static const int   MQTT_PORT = 1883;

// Interval kirim payload (ms). Dinaikkan saat uji daya tahan baterai.
static const unsigned long PERIODE_KIRIM_MS = 2000;
static const long OFFSET_ZONA_DETIK = 7 * 3600;  // WIB = UTC+7

// ------------------------------------------------------------------- sensor
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
WiFiClient klienWiFi;
PubSubClient mqtt(klienWiFi);

unsigned long seq = 0;
unsigned long terakhirSambungMQTT = 0;

// -------------------------------------------------------------------- waktu
uint64_t waktuKirimMs() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) == 0 && tv.tv_sec > 1600000000) {
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
  }
  return (uint64_t)millis();
}

// ------------------------------------------------------------------- jaringan
void sambungWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] menyambung");
  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] tersambung — IP %s\n", WiFi.localIP().toString().c_str());
    configTime(OFFSET_ZONA_DETIK, 0, "pool.ntp.org", "time.google.com");
  } else {
    Serial.println("[WiFi] gagal tersambung — mencoba lagi di putaran berikutnya");
  }
}

void sambungMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    sambungWiFi();
    return;
  }
  if (mqtt.connected()) return;
  if (millis() - terakhirSambungMQTT < 3000) return;  // jangan mencoba terlalu sering
  terakhirSambungMQTT = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(15);
  mqtt.setBufferSize(256);  // payload JSON lebih panjang dari 128 byte bawaan

  char idKlien[40];
  snprintf(idKlien, sizeof(idKlien), "%s-%04X", NODE_ID, (uint16_t)random(0xFFFF));
  Serial.printf("[MQTT] menyambung ke %s:%d sebagai %s …\n", MQTT_HOST, MQTT_PORT, idKlien);

  // Pesan terakhir (last will) supaya penerima tahu bila node mati mendadak.
  char topikStatus[64];
  snprintf(topikStatus, sizeof(topikStatus), "klinik/status/%s", NODE_ID);
  if (mqtt.connect(idKlien, topikStatus, 1, true, "offline")) {
    mqtt.publish(topikStatus, "online", true);
    Serial.println("[MQTT] tersambung");
  } else {
    Serial.printf("[MQTT] gagal (state=%d) — dicoba lagi\n", mqtt.state());
  }
}

// ------------------------------------------------------------------- kirim
void kirimPayload(float suhu) {
  if (!mqtt.connected()) {
    Serial.println("[MQTT] belum tersambung — payload tidak dikirim");
    return;
  }
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"node_id\":\"%s\",\"seq\":%lu,\"t_kirim\":%.3f,\"suhu\":%.2f}",
           NODE_ID, seq, waktuKirimMs() / 1000.0, suhu);
  if (mqtt.publish(MQTT_TOPIC, payload)) {
    seq++;
    Serial.printf("[MQTT] terkirim ke %s: %s\n", MQTT_TOPIC, payload);
  } else {
    Serial.println("[MQTT] gagal mengirim payload");
  }
}

// -------------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Node 2 (dada/ketiak) — ESP32 + MLX90614 ===");

  Wire.begin();

  // --- inisialisasi sensor dan cek koneksi I2C ---
  if (!mlx.begin()) {
    Serial.println("[I2C] MLX90614 tidak terdeteksi.");
    Serial.println("      Periksa SDA=21, SCL=22, VIN=3V3, dan alamat I2C 0x5A.");
    while (true) delay(1000);
  }
  Serial.println("[I2C] MLX90614 terdeteksi (0x5A)");

  sambungWiFi();
  sambungMQTT();
}

// --------------------------------------------------------------------- loop
void loop() {
  static unsigned long terakhirKirim = 0;

  if (!mqtt.connected()) {
    sambungMQTT();
  }
  mqtt.loop();

  float suhu = mlx.readObjectTempC();
  Serial.printf("[sensor] suhu=%.2f C (ambient %.2f C)\n",
                suhu, mlx.readAmbientTempC());

  if (millis() - terakhirKirim >= PERIODE_KIRIM_MS) {
    terakhirKirim = millis();
    // Pembacaan di luar rentang tubuh wajar (misal sensor belum menempel)
    // tetap dikirim apa adanya, tetapi ditandai pada keluaran serial.
    if (suhu < 30.0 || suhu > 42.0) {
      Serial.println("[sensor] peringatan: pembacaan di luar rentang tubuh — periksa pemasangan");
    }
    kirimPayload(suhu);
  }

  delay(200);
}
