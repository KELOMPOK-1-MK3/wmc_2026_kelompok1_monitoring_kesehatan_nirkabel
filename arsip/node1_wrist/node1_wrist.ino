/*
 * Node 1 — Wearable di pergelangan tangan.
 * ESP32 + MAX30102 (detak jantung + SpO2) -> kirim via BLE.
 *
 * Payload JSON:
 *   {"node_id":"wrist01","seq":N,"t_kirim":<epoch_ms>,"bpm":..,"spo2":..}
 *
 * Pustaka yang dipasang di Arduino IDE:
 *   - "SparkFun MAX3010x Pulse and Proximity Sensor Library"
 * Papan: ESP32 Dev Module (Bluetooth bawaan ESP32, tanpa pustaka tambahan).
 *
 * Catatan waktu kirim (t_kirim):
 *   Bila NTP aktif (PAKAI_NTP = true) -> t_kirim adalah epoch milidetik sungguhan.
 *   Bila tidak -> t_kirim adalah milidetik sejak node menyala. Penerima
 *   menyelaraskan selisih jam pada awal setiap sesi (lihat docs/arsitektur.md),
 *   jadi latency tetap terhitung benar walau jam node tidak sama dengan laptop.
 */
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ---------------------------------------------------------------- pengaturan
static const char* NODE_ID = "wrist01";

// UUID gaya Nordic UART Service (NUS). Karakteristik TX di bawah ini yang
// di-subscribe oleh penerima (lihat SERVICE_UUID / CHAR_UUID di receiver.py).
#define SERVICE_UUID  "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_TX_UUID  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Interval kirim payload ke penerima (ms).
static const unsigned long PERIODE_KIRIM_MS = 1000;

// Interval hitung ulang detak jantung dan SpO2 (ms).
static const unsigned long PERIODE_HITUNG_MS = 2000;

// Setel true bila node juga tersambung WiFi untuk mengambil waktu NTP.
static const bool PAKAI_NTP = false;
static const char* WIFI_SSID = "ISI_SSID";
static const char* WIFI_PASS = "ISI_SANDI";
static const long OFFSET_ZONA_DETIK = 7 * 3600;  // WIB = UTC+7

// ------------------------------------------------------------------- sensor
MAX30105 sensor;

// Penyangga 100 sampel untuk perhitungan SpO2 (rasio RED/IR).
static const int PANJANG_PENYANGGA = 100;
uint32_t penyanggaIR[PANJANG_PENYANGGA];
uint32_t penyanggaRED[PANJANG_PENYANGGA];
int indeksPenyangga = 0;

// Deteksi detak untuk BPM.
static const byte UKURAN_RIWAYAT_DETAK = 4;
byte riwayatDetak[UKURAN_RIWAYAT_DETAK];
byte posisiDetak = 0;
long detakTerakhir = 0;
int detakPerMenit = 0;
float bpmHalus = 0.0;

unsigned long seq = 0;
bool tersambungBLE = false;
BLECharacteristic* karakteristikTX = nullptr;

// ---------------------------------------------------------------- urusan BLE
class CallbackServer : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    tersambungBLE = true;
    Serial.println("[BLE] penerima tersambung");
  }
  void onDisconnect(BLEServer* srv) override {
    tersambungBLE = false;
    Serial.println("[BLE] penerima terputus, menyalakan ulang iklan");
    // Penting: tanpa ini node tidak dapat ditemukan kembali setelah terputus.
    srv->startAdvertising();
  }
};

// -------------------------------------------------------------------- waktu
/** Waktu epoch dalam milidetik, atau milidetik sejak menyala bila NTP mati. */
uint64_t waktuKirimMs() {
  if (PAKAI_NTP) {
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) == 0 && tv.tv_sec > 1600000000) {
      return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
    }
  }
  return (uint64_t)millis();
}

void siapkanNTP() {
  if (!PAKAI_NTP) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] menyambung");
  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] gagal tersambung — t_kirim memakai waktu sejak menyala");
    return;
  }
  configTime(OFFSET_ZONA_DETIK, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[NTP] menyelaraskan waktu");
  struct tm info;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&info, 1000)) {
      Serial.println(" selesai");
      return;
    }
    Serial.print(".");
  }
  Serial.println(" gagal — t_kirim memakai waktu sejak menyala");
}

// ----------------------------------------------------------- hitung SpO2
/**
 * SpO2 dihitung dengan metode rasio-of-rasio RED/IR, pendekatan yang sama
 * dipakai contoh pustaka SparkFun. Hasilnya perkiraan, bukan angka medis —
 * karena itu pembacaan tetap dibandingkan dengan oximeter jari sebagai
 * pembanding (lihat docs/pengujian.md).
 */
double hitungSpO2() {
  double rataRED = 0, rataIR = 0;
  for (int i = 0; i < PANJANG_PENYANGGA; i++) {
    rataRED += penyanggaRED[i];
    rataIR += penyanggaIR[i];
  }
  rataRED /= PANJANG_PENYANGGA;
  rataIR /= PANJANG_PENYANGGA;

  double jumlahRED = 0, jumlahIR = 0;
  for (int i = 0; i < PANJANG_PENYANGGA; i++) {
    jumlahRED += (penyanggaRED[i] - rataRED) * (penyanggaRED[i] - rataRED);
    jumlahIR += (penyanggaIR[i] - rataIR) * (penyanggaIR[i] - rataIR);
  }
  double rmsRED = sqrt(jumlahRED / PANJANG_PENYANGGA);
  double rmsIR = sqrt(jumlahIR / PANJANG_PENYANGGA);
  double rasio = (rmsRED / rataRED) / (rmsIR / rataIR);

  // Kurva pendekatan dari rumus yang umum dipakai pada MAX3010x.
  double spo2 = -45.060 * rasio * rasio + 30.354 * rasio + 94.845;
  if (spo2 > 100.0) spo2 = 100.0;
  if (spo2 < 70.0) spo2 = 70.0;
  return spo2;
}

// ------------------------------------------------------------------- kirim
void kirimPayload(int bpm, double spo2) {
  if (!tersambungBLE || karakteristikTX == nullptr) {
    Serial.println("[BLE] belum ada penerima — payload tidak dikirim");
    return;
  }
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"node_id\":\"%s\",\"seq\":%lu,\"t_kirim\":%.3f,\"bpm\":%d,\"spo2\":%.1f}",
           NODE_ID, seq, waktuKirimMs() / 1000.0, bpm, spo2);
  karakteristikTX->setValue((uint8_t*)payload, strlen(payload));
  karakteristikTX->notify();
  seq++;
  Serial.printf("[BLE] terkirim: %s\n", payload);
}

// -------------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Node 1 (pergelangan) — ESP32 + MAX30102 ===");

  Wire.begin();

  // --- inisialisasi sensor dan cek koneksi I2C ---
  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[I2C] MAX30102 tidak terdeteksi.");
    Serial.println("      Periksa SDA=21, SCL=22, VIN=3V3, dan alamat I2C 0x57.");
    while (true) delay(1000);
  }
  Serial.println("[I2C] MAX30102 terdeteksi (0x57)");
  sensor.setup(60, 4, 2, 100, 411, 16384);  // 60 sampel/detik, jendela 411 us
  sensor.setPulseAmplitudeRed(0x0A);
  sensor.setPulseAmplitudeIR(0x0A);

  siapkanNTP();

  // --- aktifkan BLE ---
  BLEDevice::init("wearable-wrist01");
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new CallbackServer());

  BLEService* layanan = server->createService(SERVICE_UUID);
  karakteristikTX = layanan->createCharacteristic(
      CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  karakteristikTX->addDescriptor(new BLE2902());
  layanan->start();

  BLEAdvertising* iklan = BLEDevice::getAdvertising();
  iklan->addServiceUUID(SERVICE_UUID);
  iklan->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] iklan aktif sebagai 'wearable-wrist01' — menunggu penerima");
}

// --------------------------------------------------------------------- loop
void loop() {
  static unsigned long terakhirKirim = 0;
  static unsigned long terakhirHitung = 0;

  // Ambil satu sampel setiap putaran (sensor sudah diatur 60 sampel/detik).
  sensor.check();
  while (sensor.available()) {
    uint32_t ir = sensor.getFIFOIR();
    uint32_t red = sensor.getFIFORed();
    sensor.nextSample();

    penyanggaIR[indeksPenyangga] = ir;
    penyanggaRED[indeksPenyangga] = red;
    indeksPenyangga = (indeksPenyangga + 1) % PANJANG_PENYANGGA;

    if (ir < 50000) {
      // Jari belum menempel rapi — detak tidak dihitung dari sinyal lemah.
      detakTerakhir = 0;
      continue;
    }

    // --- deteksi detak untuk BPM ---
    if (checkForBeat(ir)) {
      long sekarang = millis();
      long selisih = sekarang - detakTerakhir;
      detakTerakhir = sekarang;
      float bpm = 60.0 / (selisih / 1000.0);
      if (bpm > 30 && bpm < 220) {
        bpmHalus = (bpmHalus == 0.0) ? bpm : 0.7 * bpmHalus + 0.3 * bpm;
        detakPerMenit = (int)(bpmHalus + 0.5);
        Serial.printf("[sensor] detak terdeteksi — bpm=%.1f\n", bpmHalus);
      }
    }
  }

  if (millis() - terakhirHitung >= PERIODE_HITUNG_MS && detakPerMenit > 0) {
    terakhirHitung = millis();
    double spo2 = hitungSpO2();
    Serial.printf("[sensor] bpm=%d  spo2=%.1f%%\n", detakPerMenit, spo2);
  }

  if (millis() - terakhirKirim >= PERIODE_KIRIM_MS) {
    terakhirKirim = millis();
    if (detakPerMenit > 0) {
      kirimPayload(detakPerMenit, hitungSpO2());
    }
  }
}
