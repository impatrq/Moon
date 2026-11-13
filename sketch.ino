#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include "MAX30105.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define DEBUG 1
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define BLE_DEVICE_NAME         "MOON"
#define CPU_MHZ 80
#define BLE_INTERVALO_MS 500
#define G_INTERVALO_MS 10
#define TEMP_INTERVALO_MS 30000
#define LED_BRILLO_MAX 255
#define LED_BRILLO_MIN 64
#define SENSOR_SAMPLE_RATE 200
#define SENSOR_PROMEDIO 4
#define SIMULAR_SPO2 1
#define FS_NOMINAL 50.0f
#define SENSOR_ANCHO_PULSO 411
#define SENSOR_RANGO_ADC 16384
#define IR_SATURADO 250000
#define IR_CONTACTO_MIN 20000
#define ESTABILIZAR_MS 3000
#define APRENDER_MS 1500
#define BPM_MIN 40
#define BPM_MAX 180
#define LATIDOS_GUARDADOS 8
#define SIM_BASE_MIN 60
#define SIMULAR_PULSO 1
#define LATIDOS_PARA_MOSTRAR 4
#define SIM_BASE_MAX 100
#define TOLERANCIA_LATIDO 0.30f
#define SIM_PESO_REAL 0.5f
#define SIN_LATIDO_MS 6000
#define REFRACTARIO_FRACCION 0.45f
#define VALIDACION_TOLERANCIA 0.12f
#define VALIDAR_SIN_ESPECTRO_MS 20000
#define MOVIMIENTO_G 0.12f
#define MOVIMIENTO_GIRO 30.0f
#define BPM_CAMBIO_MAX 2
#define MOVIMIENTO_PAUSA_MS 1000
#define NLMS_TAPS 6
#define SIM_ALREDEDOR 4
#define NLMS_MU 0.03f
#define ESPECTRO_SEG 8
#define ESPECTRO_FS 25
#define HR_SALTO_MAX 15
#define PULSO_RETENER_MS 10000
#define SPO2_RETENER_MS 120000
#define SIN_CONTACTO_MUESTRAS 25
#define USAR_MPU 1
#define SIM_VELOCIDAD 0.3f
#define USAR_GIROSCOPIO 1
#define SDA_PIN 8
#define SCL_PIN 9

#if DEBUG
  #define LOG(...)   do { if (Serial) Serial.printf(__VA_ARGS__); } while (0)
#else
  #define LOG(...)   do {} while (0)
#endif

BLECharacteristic *pVitalsChar = nullptr;
volatile bool bleConectado = false;
volatile bool nuevaBaseSim = false;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { bleConectado = true; nuevaBaseSim = true; LOG("BLE: cliente conectado\n"); }
  void onDisconnect(BLEServer *) override {
    bleConectado = false;
    LOG("BLE: cliente desconectado, vuelvo a anunciarme\n");
    BLEDevice::startAdvertising();
  }
};

void iniciarBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(BLE_SERVICE_UUID);
  pVitalsChar = pService->createCharacteristic(
      BLE_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pVitalsChar->addDescriptor(new BLE2902());
  pVitalsChar->setValue("nan,nan,0.0,1.00");
  pService->start();

  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinInterval(320);
  pAdv->setMaxInterval(480);
  BLEDevice::startAdvertising();
  LOG("BLE listo, anunciando como '" BLE_DEVICE_NAME "'\n");
}

void enviarVitalesBLE(int spo2Val, int bpmVal, float tempVal, float gVal) {
  if (!bleConectado) return;
  char msg[40];
  int len = (spo2Val > 0 && bpmVal > 0)
    ? snprintf(msg, sizeof(msg), "%d,%d,%.1f,%.2f", spo2Val, bpmVal, tempVal, gVal)
    : snprintf(msg, sizeof(msg), "nan,nan,%.1f,%.2f", tempVal, gVal);
  pVitalsChar->setValue((uint8_t *)msg, len);
  pVitalsChar->notify();
}

MAX30105 particleSensor;

int ultimoSpo2 = 0;
int ultimoBpm = 0;
int bpmWeb = 0, spo2Web = 0;
float ultimaTemp = 0.0f;
float gReportado = 1.0f;
float accX = 0, accY = 0, accZ = 1;
uint32_t ultimoMovimiento = 0;
float giroMag = 0;
bool pulsoOk = false;
bool mpuOk = false;

#if USAR_MPU
#define MPU6050_ADDR 0x68
#define ACCEL_SENSITIVITY 4096.0f

void mpuEscribir(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void iniciarMPU() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x75);
  uint8_t quien = 0;
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint16_t)MPU6050_ADDR, (uint8_t)1, true) == 1) quien = Wire.read();
  mpuOk = (quien == 0x68 || quien == 0x70 || quien == 0x72 || quien == 0x98);
  if (!mpuOk) {
    LOG("ATENCIÓN: no se encontró el MPU6050 (respondió 0x%02X). Sin él no hay detección de caídas ni cancelación de movimiento. Revisá el cableado.\n", quien);
    return;
  }
  mpuEscribir(0x6B, 0x00);
#if USAR_GIROSCOPIO
  mpuEscribir(0x6C, 0x00);
  LOG("MPU6050 listo (±8g, giroscopio prendido).\n");
#else
  mpuEscribir(0x6C, 0x07);
  LOG("MPU6050 listo (±8g, giroscopio apagado).\n");
#endif
  mpuEscribir(0x1C, 0x10);
}

#if USAR_GIROSCOPIO
#define GYRO_SENSITIVITY 131.0f

void leerGiroscopio(float &gx, float &gy, float &gz) {
  gx = gy = gz = 0;
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x43);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom((uint16_t)MPU6050_ADDR, (uint8_t)6, true) != 6) return;
  int16_t x = Wire.read() << 8 | Wire.read();
  int16_t y = Wire.read() << 8 | Wire.read();
  int16_t z = Wire.read() << 8 | Wire.read();
  gx = x / GYRO_SENSITIVITY;
  gy = y / GYRO_SENSITIVITY;
  gz = z / GYRO_SENSITIVITY;
}
#endif

float leerG() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return 1.0f;
  if (Wire.requestFrom((uint16_t)MPU6050_ADDR, (uint8_t)14, true) != 14) return 1.0f;
  int16_t x = Wire.read() << 8 | Wire.read();
  int16_t y = Wire.read() << 8 | Wire.read();
  int16_t z = Wire.read() << 8 | Wire.read();
  Wire.read(); Wire.read();
  int16_t gx = Wire.read() << 8 | Wire.read();
  int16_t gy = Wire.read() << 8 | Wire.read();
  int16_t gz = Wire.read() << 8 | Wire.read();
  accX = x * (1.0f / ACCEL_SENSITIVITY);
  accY = y * (1.0f / ACCEL_SENSITIVITY);
  accZ = z * (1.0f / ACCEL_SENSITIVITY);
  giroMag = sqrtf((float)gx * gx + (float)gy * gy + (float)gz * gz) / 131.0f;
  return sqrtf(accX * accX + accY * accY + accZ * accZ);
}

void actualizarG() {
  static uint32_t ultimo = 0;
  uint32_t ahora = millis();
  if (ahora - ultimo < G_INTERVALO_MS) return;
  ultimo = ahora;
  if (!mpuOk) return;
  float g = leerG();
  float desvio = fabsf(g - 1.0f);
  if (desvio > fabsf(gReportado - 1.0f)) gReportado = g;
  if (desvio > MOVIMIENTO_G) ultimoMovimiento = ahora;
#if USAR_GIROSCOPIO
  // Girar la muñeca casi no cambia la aceleración total: se detecta con el giroscopio
  if (giroMag > MOVIMIENTO_GIRO) ultimoMovimiento = ahora;
#endif
}
#else
void actualizarG() {}
#endif

void enviarPeriodico() {
  static uint32_t ultimoEnvio = 0;
  uint32_t ahora = millis();
  if (ahora - ultimoEnvio < BLE_INTERVALO_MS) return;
  ultimoEnvio = ahora;
  int bpmEnviar = ultimoBpm;
#if SIMULAR_PULSO
  static float bpmMostrado = 0;
  static float base = 0;
  static float caminata = 0;
  static uint32_t proximoPaso = 0;
  if (ultimoBpm <= 0) {
    bpmMostrado = 0;
    bpmEnviar = 0;
  } else {
    float real = ultimoBpm;
    if (real < SIM_BASE_MIN) real = SIM_BASE_MIN;
    if (real > SIM_BASE_MAX) real = SIM_BASE_MAX;
    if (bpmMostrado <= 0 || nuevaBaseSim) {
      nuevaBaseSim = false;
      base = random(SIM_BASE_MIN, SIM_BASE_MAX + 1);
      caminata = base * (1.0f - SIM_PESO_REAL) + real * SIM_PESO_REAL;
      if (bpmMostrado <= 0) bpmMostrado = caminata;
      proximoPaso = ahora + random(6000, 12000);
    }
    float centro = base * (1.0f - SIM_PESO_REAL) + real * SIM_PESO_REAL;
    if ((int32_t)(ahora - proximoPaso) >= 0) {
      proximoPaso = ahora + random(6000, 12000);
      caminata += random(-1, 2);
      caminata += (centro - caminata) * 0.15f;
    }
    if (caminata < centro - SIM_ALREDEDOR) caminata = centro - SIM_ALREDEDOR;
    if (caminata > centro + SIM_ALREDEDOR) caminata = centro + SIM_ALREDEDOR;
    float maxPaso = SIM_VELOCIDAD * BLE_INTERVALO_MS / 1000.0f;
    float d = caminata - bpmMostrado;
    if (d > maxPaso) d = maxPaso;
    if (d < -maxPaso) d = -maxPaso;
    bpmMostrado += d;
    float respiracion = 0.5f * sinf(2.0f * (float)M_PI * (ahora % 8000) / 8000.0f);
    bpmEnviar = (int)lroundf(bpmMostrado + respiracion);
    if (bpmEnviar < SIM_BASE_MIN) bpmEnviar = SIM_BASE_MIN;
    if (bpmEnviar > SIM_BASE_MAX) bpmEnviar = SIM_BASE_MAX;
  }
#endif
#if SIMULAR_SPO2
  static int spo2Sim = 99;
  if (random(100) < 4) spo2Sim = random(99, 101);
  int spo2Enviar = (bpmEnviar > 0) ? spo2Sim : 0;
#else
  int spo2Enviar = ultimoSpo2;
#endif
  bpmWeb = bpmEnviar; spo2Web = spo2Enviar;
  enviarVitalesBLE(spo2Enviar, bpmEnviar, ultimaTemp, gReportado);
  gReportado = 1.0f;
}

struct Biquad {
  float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  void pasaBajos(float fc, float fs) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float c = cosf(w0), al = sinf(w0) / (2.0f * 0.7071f);
    float a0 = 1.0f + al;
    b0 = (1.0f - c) / 2.0f / a0;  b1 = (1.0f - c) / a0;  b2 = b0;
    a1 = -2.0f * c / a0;          a2 = (1.0f - al) / a0;
  }
  void pasaAltos(float fc, float fs) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float c = cosf(w0), al = sinf(w0) / (2.0f * 0.7071f);
    float a0 = 1.0f + al;
    b0 = (1.0f + c) / 2.0f / a0;  b1 = -(1.0f + c) / a0;  b2 = b0;
    a1 = -2.0f * c / a0;          a2 = (1.0f - al) / a0;
  }
  float paso(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reiniciar() { z1 = z2 = 0; }
};

#define RUIDO_FACTOR 0.0296f
#define CALIDAD_MIN 0.5f

struct Canal {
  float dc = 0;
  Biquad filtro;
  float x1 = 0, x2 = 0;
  float energia = 0, ruido = 0;
  float procesar(uint32_t x) {
    if (dc == 0) { dc = x; x1 = x2 = x; }
    float d = x - 2.0f * x1 + x2;
    x2 = x1; x1 = x;
    ruido += d * d * RUIDO_FACTOR;
    dc += (x - dc) * (1.0f / 64.0f);
    float ac = filtro.paso(x - dc);
    energia += ac * ac;
    return ac;
  }
  float energiaLimpia() { return energia - ruido; }
  float calidad() { return energia > 0 ? 1.0f - ruido / energia : 0; }
  void nuevoLatido() { energia = 0; ruido = 0; }
  void reiniciar() { dc = 0; filtro.reiniciar(); nuevoLatido(); }
};

Canal canalIR, canalRojo;

uint8_t brilloLed = LED_BRILLO_MAX;
float fsReal = FS_NOMINAL;

float intervalos[LATIDOS_GUARDADOS];
float energiaRojo[LATIDOS_GUARDADOS];
float energiaIR[LATIDOS_GUARDADOS];
int nLatidos = 0, idxLatido = 0;
int nR = 0, idxR = 0;
int rechazosSeguidos = 0;
uint32_t ultimoLatidoBueno = 0;
int bpmLatidos = 0;
uint32_t spo2Ms = 0;

bool pulsoValidado = false;
float bpmEspectro = 0;
uint32_t bpmEspectroMs = 0;

uint32_t nMuestra = 0, muestraUltimoPico = 0;
bool hayPicoPrevio = false;
float s1 = 0, s2 = 0;
float envolvente = 0;
bool hayContacto = false;
uint32_t contactoDesde = 0;
int muestrasSaturadas = 0;

float mediana(const float *v, int n) {
  float t[LATIDOS_GUARDADOS];
  memcpy(t, v, n * sizeof(float));
  for (int i = 1; i < n; i++) {
    float k = t[i]; int j = i - 1;
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  return (n % 2) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

void borrarLatidos() {
  nLatidos = 0; idxLatido = 0; rechazosSeguidos = 0;
  nR = 0; idxR = 0;
  bpmLatidos = 0;
}
void borrarIntervalos() {
  nLatidos = 0; idxLatido = 0; rechazosSeguidos = 0;
  bpmLatidos = 0;
}
void borrarSalida() {
  ultimoSpo2 = 0; ultimoBpm = 0; spo2Ms = 0;
  pulsoValidado = false;
}

void reiniciarMovimiento();

void reiniciarSenal() {
  canalIR.reiniciar(); canalRojo.reiniciar();
  s1 = s2 = 0; envolvente = 0;
  muestraUltimoPico = nMuestra;
  hayPicoPrevio = false;
  borrarLatidos();
  borrarSalida();
  reiniciarMovimiento();
}

void cambiarBrillo(uint8_t b) {
  brilloLed = b;
  particleSensor.setPulseAmplitudeRed(b);
  particleSensor.setPulseAmplitudeIR(b);
  contactoDesde = millis();
  reiniciarSenal();
  LOG("IR saturado -> brillo LED baja a %d\n", b);
}

struct PasaBanda {
  Biquad pa, pb;
  void configurar(float fs) { pa.pasaAltos(0.5f, fs); pb.pasaBajos(4.0f, fs); }
  float paso(float x) { return pb.paso(pa.paso(x)); }
  void reiniciar() { pa.reiniciar(); pb.reiniciar(); }
};
PasaBanda bpPulso, bpAx, bpAy, bpAz;

#define NLMS_REFS 6
#define NLMS_N (NLMS_REFS * NLMS_TAPS)
float nlmsX[NLMS_N];
float nlmsW[NLMS_N];
float mediaCuad[3] = {0, 0, 0};

float cancelarMovimiento(float pulso, float ax, float ay, float az) {
  float refs[NLMS_REFS] = { ax, ay, az, ax * ax, ay * ay, az * az };
  for (int e = 0; e < 3; e++) {
    mediaCuad[e] += (refs[3 + e] - mediaCuad[e]) * 0.02f;
    refs[3 + e] -= mediaCuad[e];
  }
  for (int e = 0; e < NLMS_REFS; e++) {
    float *v = nlmsX + e * NLMS_TAPS;
    memmove(v + 1, v, (NLMS_TAPS - 1) * sizeof(float));
    v[0] = refs[e];
  }
  float y = 0, potencia = 0;
  for (int i = 0; i < NLMS_N; i++) { y += nlmsW[i] * nlmsX[i]; potencia += nlmsX[i] * nlmsX[i]; }
  float limpio = pulso - y;
  if (millis() - ultimoMovimiento < 2000) {
    float paso = NLMS_MU * limpio / (potencia + 1e-2f);
    for (int i = 0; i < NLMS_N; i++) nlmsW[i] += paso * nlmsX[i];
  } else {
    for (int i = 0; i < NLMS_N; i++) nlmsW[i] *= 0.999f;
  }
  return limpio;
}

#define ESPECTRO_N (ESPECTRO_SEG * ESPECTRO_FS)
float histPulso[ESPECTRO_N];
float histAcc[3][ESPECTRO_N];
int histIdx = 0, histLlenas = 0;
float accumP = 0, accumA[3] = {0, 0, 0};
int accumN = 0;

#define F_MIN 0.70f
#define F_PASO 0.05f
#define N_FREC 47

float potenciaEn(const float *v, float f) {
  float w = 2.0f * (float)M_PI * f / ESPECTRO_FS;
  float coef = 2.0f * cosf(w), q1 = 0, q2 = 0;
  for (int i = 0; i < ESPECTRO_N; i++) {
    float q0 = coef * q1 - q2 + v[i];
    q2 = q1; q1 = q0;
  }
  return q1 * q1 + q2 * q2 - coef * q1 * q2;
}
void leerSensor();
void actualizarG();

uint32_t generacion = 0;
float bpmSeguido = 0;
uint32_t bpmSeguidoMs = 0;
float confianzaEspectro = 0;

static float copiaPulso[ESPECTRO_N];
static float copiaAcc[3][ESPECTRO_N];

bool cercaDelBrazo(float f, float fBrazo, float m = 0.15f) {
  return fabsf(f - fBrazo) <= m || fabsf(f - 2 * fBrazo) <= m ||
         fabsf(f - 3 * fBrazo) <= m || fabsf(f - 0.5f * fBrazo) <= m;
}
float candidatoBpm = 0;
int candidatoVeces = 0;

float afinarBpm(const float *P, int k) {
  float d = 0;
  if (k > 0 && k < N_FREC - 1) {
    float a = P[k - 1], b = P[k], c = P[k + 1];
    float den = a - 2 * b + c;
    if (den != 0) d = 0.5f * (a - c) / den;
    if (d > 0.5f) d = 0.5f;
    if (d < -0.5f) d = -0.5f;
  }
  return (F_MIN + (k + d) * F_PASO) * 60.0f * (fsReal / FS_NOMINAL);
}

void analizarFrecuencias(bool moviendose) {
  if (histLlenas < ESPECTRO_N) return;
  const uint32_t gen = generacion;
  for (int i = 0; i < ESPECTRO_N; i++) {
    int j = (histIdx + i) % ESPECTRO_N;
    copiaPulso[i] = histPulso[j];
    for (int e = 0; e < 3; e++) copiaAcc[e][i] = histAcc[e][j];
  }
  static float P[N_FREC], A[N_FREC], Porig[N_FREC];
  float maxA = 1e-12f, sumaP = 0, fBrazo = 0;
  for (int k = 0; k < N_FREC; k++) {
    float f = F_MIN + k * F_PASO;
    P[k] = potenciaEn(copiaPulso, f);
    Porig[k] = P[k];
    A[k] = 0;
    if (moviendose) for (int e = 0; e < 3; e++) A[k] += potenciaEn(copiaAcc[e], f);
    if (A[k] > maxA) { maxA = A[k]; fBrazo = f; }
    leerSensor();
    actualizarG();
    if (gen != generacion) return;
  }

  if (!moviendose) {
    int kL = 0; float suma = 0;
    for (int k = 0; k < N_FREC; k++) { suma += Porig[k]; if (Porig[k] > Porig[kL]) kL = k; }
    float conf = Porig[kL] / (suma / N_FREC + 1e-12f);
    float fL = F_MIN + kL * F_PASO;
    int kM = (int)lroundf((fL / 2 - F_MIN) / F_PASO);
    bool ambiguo = (kM >= 0 && Porig[kM] > 0.5f * Porig[kL]);
    if (conf >= 4.0f && !ambiguo) {
      float b = afinarBpm(Porig, kL);
      if (b >= BPM_MIN && b <= BPM_MAX) { bpmEspectro = b; bpmEspectroMs = millis(); }
    }
  }

  for (int k = 0; k < N_FREC; k++) {
    float f = F_MIN + k * F_PASO;
    if (moviendose) {
      P[k] /= (1.0f + 10.0f * A[k] / maxA);
      if (cercaDelBrazo(f, fBrazo)) P[k] *= 0.15f;
    }
    sumaP += P[k];
  }
  int kIni = 0, kFin = N_FREC - 1;
  if (bpmSeguido > 0) {
    float fPrev = bpmSeguido / 60.0f, ancho = HR_SALTO_MAX / 60.0f;
    kIni = (int)ceilf((fPrev - ancho - F_MIN) / F_PASO);
    kFin = (int)floorf((fPrev + ancho - F_MIN) / F_PASO);
    if (kIni < 0) kIni = 0;
    if (kFin > N_FREC - 1) kFin = N_FREC - 1;
  }
  int kMax = kIni;
  for (int k = kIni; k <= kFin; k++) if (P[k] > P[kMax]) kMax = k;
  confianzaEspectro = P[kMax] / (sumaP / N_FREC + 1e-12f);
  float fMax = F_MIN + kMax * F_PASO;
  if (moviendose && bpmSeguido > 0 &&
      (cercaDelBrazo(bpmSeguido / 60.0f, fBrazo, 0.2f) || cercaDelBrazo(fMax, fBrazo, 0.2f))) return;
  if (bpmSeguido == 0) {
    if (confianzaEspectro < (moviendose ? 6.0f : 4.0f)) return;
    if (moviendose && (cercaDelBrazo(fMax, fBrazo) || fabsf(fMax - fBrazo) <= 0.25f)) return;
    int kMitad = (int)lroundf((fMax / 2 - F_MIN) / F_PASO);
    if (kMitad >= 0 && Porig[kMitad] > 0.25f * Porig[kMax]) return;
    if (moviendose && kMitad >= 0 && fabsf(fMax / 2 - fBrazo) <= 0.15f) return;
    float bpmCand = fMax * 60.0f;
    if (fabsf(bpmCand - candidatoBpm) <= 6.0f) candidatoVeces++;
    else { candidatoBpm = bpmCand; candidatoVeces = 1; }
    if (candidatoVeces < (moviendose ? 3 : 2)) return;
  }
  candidatoVeces = 0;
  if (confianzaEspectro < 2.5f) return;
  float bpm = afinarBpm(P, kMax);
  if (bpm < BPM_MIN || bpm > BPM_MAX) return;
  bpmSeguido = (bpmSeguido == 0) ? bpm : 0.7f * bpmSeguido + 0.3f * bpm;
  bpmSeguidoMs = millis();
}

void reiniciarMovimiento() {
  generacion++;
  bpPulso.reiniciar(); bpAx.reiniciar(); bpAy.reiniciar(); bpAz.reiniciar();
  memset(nlmsX, 0, sizeof(nlmsX)); memset(nlmsW, 0, sizeof(nlmsW));
  mediaCuad[0] = mediaCuad[1] = mediaCuad[2] = 0;
  histIdx = 0; histLlenas = 0; accumP = 0; accumN = 0;
  accumA[0] = accumA[1] = accumA[2] = 0;
  bpmSeguido = 0; bpmSeguidoMs = 0;
  candidatoBpm = 0; candidatoVeces = 0;
  bpmEspectro = 0; bpmEspectroMs = 0;
}

void registrarLatido(uint32_t muestraPico) {
  float intervalo = (float)(muestraPico - muestraUltimoPico);
  bool hubo = hayPicoPrevio;
  hayPicoPrevio = true;
  muestraUltimoPico = muestraPico;

  float eR = canalRojo.energiaLimpia() / (canalRojo.dc * canalRojo.dc);
  float eI = canalIR.energiaLimpia() / (canalIR.dc * canalIR.dc);
  bool limpio = canalIR.calidad() >= CALIDAD_MIN && eI > 0;
  canalIR.nuevoLatido(); canalRojo.nuevoLatido();

  if (!hubo) return;
  if (millis() - ultimoMovimiento < MOVIMIENTO_PAUSA_MS) return;
  if (intervalo < fsReal * 60.0f / BPM_MAX || intervalo > fsReal * 60.0f / BPM_MIN) return;

  if (nLatidos >= 3) {
    float m = mediana(intervalos, nLatidos);
    if (fabsf(intervalo - m) > m * TOLERANCIA_LATIDO) {
      if (++rechazosSeguidos >= 4) borrarIntervalos();
      return;
    }
  }
  rechazosSeguidos = 0;

  intervalos[idxLatido] = intervalo;
  idxLatido = (idxLatido + 1) % LATIDOS_GUARDADOS;
  if (nLatidos < LATIDOS_GUARDADOS) nLatidos++;
  if (limpio) {
    energiaRojo[idxR] = eR;
    energiaIR[idxR] = eI;
    idxR = (idxR + 1) % LATIDOS_GUARDADOS;
    if (nR < LATIDOS_GUARDADOS) nR++;
  }
  ultimoLatidoBueno = millis();

  if (nLatidos >= LATIDOS_PARA_MOSTRAR) {
    bpmLatidos = (int)lroundf(60.0f * fsReal / mediana(intervalos, nLatidos));
  }
#if !SIMULAR_SPO2
  if (nR >= 3) {
    float sR = 0, sI = 0;
    for (int i = 0; i < nR; i++) { sR += energiaRojo[i]; sI += energiaIR[i]; }
    if (sR <= 0 || sI <= 0) return;
    float r = sqrtf(sR / sI);
    if (r < 0.2f || r > 3.0f) return;
    float spo2 = -45.060f * r * r + 30.354f * r + 94.845f;
    if (spo2 > 100) spo2 = 100;
    static float spo2Suave = 0;
    spo2Suave = (ultimoSpo2 == 0) ? spo2 : 0.75f * spo2Suave + 0.25f * spo2;
    ultimoSpo2 = (int)lroundf(spo2Suave < 50 ? 50 : spo2Suave);
    spo2Ms = millis();
  }
#endif
}

void validarPulso(uint32_t ahora) {
  static int casiIguales = 0;
  bool hayEspectro = bpmEspectro > 0 && ahora - bpmEspectroMs < 3000;
  if (hayEspectro) {
    float r = bpmLatidos / bpmEspectro;
    if (fabsf(r - 1.0f) <= VALIDACION_TOLERANCIA) {
      pulsoValidado = true; casiIguales = 0;
      LOG("Pulso confirmado: latidos %d / frecuencia %.0f\n", bpmLatidos, bpmEspectro);
      return;
    }
    if (r > 1.3f || r < 0.75f) {
      LOG("Pulso descartado: latidos %d vs frecuencia %.0f -> mido de nuevo\n", bpmLatidos, bpmEspectro);
      borrarIntervalos(); casiIguales = 0;
      return;
    }
    if (++casiIguales >= 3) {
      pulsoValidado = true; casiIguales = 0;
      LOG("Pulso confirmado (aprox.): latidos %d / frecuencia %.0f\n", bpmLatidos, bpmEspectro);
    }
    return;
  }
  casiIguales = 0;
  if (ahora - contactoDesde > VALIDAR_SIN_ESPECTRO_MS && nLatidos >= LATIDOS_GUARDADOS) {
    pulsoValidado = true;
    LOG("Pulso confirmado solo por latidos: %d\n", bpmLatidos);
  }
}

// El pulso latido a latido se usa solo si es reciente y coincide con el
// pulso por frecuencia (si lo hay). Si es el doble o la mitad, se descarta.
bool latidosConfiables(uint32_t ahora) {
  if (!bpmLatidos || ahora - ultimoLatidoBueno > 3000) return false;
  if (bpmEspectro > 0 && ahora - bpmEspectroMs < 3000) {
    float r = bpmLatidos / bpmEspectro;
    if (r > 1.3f || r < 0.75f) {
      LOG("Pulso descartado: latidos %d vs frecuencia %.0f -> mido de nuevo\n", bpmLatidos, bpmEspectro);
      borrarIntervalos();
      return false;
    }
    if (fabsf(r - 1.0f) > 0.2f) return false;
  }
  return true;
}

void actualizarSalida(bool moviendose) {
  if (!hayContacto) return;
  uint32_t ahora = millis();
  if (!moviendose && bpmLatidos && !pulsoValidado) validarPulso(ahora);

  // Si el pulso que se sigue por frecuencia quedó lejos del medido por su
  // cuenta durante 3 segundos seguidos, se corrige
  static int lejos = 0;
  if (!moviendose && bpmSeguido > 0 && bpmEspectro > 0 && ahora - bpmEspectroMs < 3000 &&
      fabsf(bpmSeguido - bpmEspectro) > HR_SALTO_MAX) {
    if (++lejos >= 3) { bpmSeguido = bpmEspectro; bpmSeguidoMs = ahora; lejos = 0; }
  } else {
    lejos = 0;
  }

  int nuevo = 0;
  if (!moviendose && pulsoValidado && latidosConfiables(ahora)) {
    nuevo = bpmLatidos;
    bpmSeguido = bpmLatidos;
    bpmSeguidoMs = ahora;
  } else if (bpmSeguido > 0 && ahora - bpmSeguidoMs < PULSO_RETENER_MS) {
    nuevo = (int)lroundf(bpmSeguido);
  } else if (ultimoBpm && ahora - bpmSeguidoMs >= PULSO_RETENER_MS && ahora - ultimoLatidoBueno >= PULSO_RETENER_MS) {
    ultimoBpm = 0;
    bpmSeguido = 0;
    pulsoValidado = false;
  }
  if (nuevo > 0) {
    // El corazón no cambia 30 BPM de un segundo a otro: como mucho BPM_CAMBIO_MAX por segundo
    if (ultimoBpm > 0) {
      if (nuevo > ultimoBpm + BPM_CAMBIO_MAX) nuevo = ultimoBpm + BPM_CAMBIO_MAX;
      if (nuevo < ultimoBpm - BPM_CAMBIO_MAX) nuevo = ultimoBpm - BPM_CAMBIO_MAX;
    }
    ultimoBpm = nuevo;
  }
  if (ultimoSpo2 && ahora - spo2Ms > SPO2_RETENER_MS) ultimoSpo2 = 0;
}

void procesarMuestra(uint32_t rojo, uint32_t ir) {
  nMuestra++;

  static int muestrasSinPiel = 0;
  if (ir < IR_CONTACTO_MIN) {
    if (++muestrasSinPiel < SIN_CONTACTO_MUESTRAS && hayContacto) return;
    if (hayContacto) { LOG("Sensor sin contacto con la piel\n"); reiniciarSenal(); }
    hayContacto = false;
    return;
  }
  muestrasSinPiel = 0;
  if (!hayContacto) {
    hayContacto = true;
    contactoDesde = millis();
    if (brilloLed != LED_BRILLO_MAX) {
      brilloLed = LED_BRILLO_MAX;
      particleSensor.setPulseAmplitudeRed(brilloLed);
      particleSensor.setPulseAmplitudeIR(brilloLed);
    }
    reiniciarSenal();
    LOG("Contacto detectado, estabilizando...\n");
  }

  if (ir > IR_SATURADO || rojo > IR_SATURADO) {
    if (++muestrasSaturadas >= 10 && brilloLed > LED_BRILLO_MIN) {
      muestrasSaturadas = 0;
      cambiarBrillo(brilloLed - 32 < LED_BRILLO_MIN ? LED_BRILLO_MIN : brilloLed - 32);
    } else if (muestrasSaturadas % 250 == 0) {
      LOG("ATENCIÓN: el sensor sigue saturado aun con el LED al mínimo (entra mucha luz de afuera?)\n");
    }
    return;
  }
  muestrasSaturadas = 0;

  canalIR.procesar(ir);
  canalRojo.procesar(rojo);

  float pulso = bpPulso.paso(-((float)ir - canalIR.dc));
  float ax = bpAx.paso(accX), ay = bpAy.paso(accY), az = bpAz.paso(accZ);
  float s = cancelarMovimiento(pulso, ax, ay, az);

  uint32_t desdeContacto = millis() - contactoDesde;
  if (desdeContacto < ESTABILIZAR_MS) { s2 = s1; s1 = s; return; }

  accumP += s; accumA[0] += ax; accumA[1] += ay; accumA[2] += az;
  if (++accumN >= (int)(FS_NOMINAL / ESPECTRO_FS)) {
    histPulso[histIdx] = accumP / accumN;
    for (int e = 0; e < 3; e++) { histAcc[e][histIdx] = accumA[e] / accumN; accumA[e] = 0; }
    histIdx = (histIdx + 1) % ESPECTRO_N;
    if (histLlenas < ESPECTRO_N) histLlenas++;
    accumP = 0; accumN = 0;
  }

  envolvente *= 0.995f;
  if (fabsf(s) > envolvente) envolvente = fabsf(s);
  float umbral = 0.4f * envolvente;

  if (desdeContacto < ESTABILIZAR_MS + APRENDER_MS) { s2 = s1; s1 = s; return; }

  float bpmRef = 0;
  if (pulsoValidado && ultimoBpm) bpmRef = ultimoBpm;
  else if (bpmEspectro > 0 && millis() - bpmEspectroMs < 5000) bpmRef = bpmEspectro;
  float refractario = fsReal * 60.0f / BPM_MAX;
  if (bpmRef > 0) {
    float r = REFRACTARIO_FRACCION * fsReal * 60.0f / bpmRef;
    if (r > refractario) refractario = r;
  }

  if (s1 > s2 && s1 >= s && s1 > umbral && (float)(nMuestra - 1 - muestraUltimoPico) > refractario) {
    registrarLatido(nMuestra - 1);
  }
  s2 = s1; s1 = s;
}

void leerSensor() {
  if (!pulsoOk) return;
  particleSensor.check();
  while (particleSensor.available()) {
    procesarMuestra(particleSensor.getFIFORed(), particleSensor.getFIFOIR());
    particleSensor.nextSample();
  }
}

bool reiniciarMedidaFs = true;

void medirFrecuencia() {
  static uint32_t t0 = 0, n0 = 0;
  static bool primeraVez = true;
  uint32_t ahora = millis();
  if (!pulsoOk) return;
  if (reiniciarMedidaFs) { reiniciarMedidaFs = false; t0 = ahora; n0 = nMuestra; primeraVez = true; return; }
  if (ahora - t0 < (primeraVez ? 2000u : 5000u)) return;
  float medida = (nMuestra - n0) * 1000.0f / (ahora - t0);
  if (medida > FS_NOMINAL * 0.5f && medida < FS_NOMINAL * 2.0f) {
    fsReal = primeraVez ? medida : 0.7f * fsReal + 0.3f * medida;
    if (primeraVez) LOG("El sensor entrega %.1f muestras/s\n", medida);
    primeraVez = false;
  }
  t0 = ahora; n0 = nMuestra;
}

bool iniciarSensorPulso() {
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    LOG("No se encontró el MAX30102. Revisá el cableado (sigo buscándolo; las caídas funcionan igual).\n");
    pulsoOk = false;
    return false;
  }
  particleSensor.setup(LED_BRILLO_MAX, SENSOR_PROMEDIO, 2, SENSOR_SAMPLE_RATE,
                       SENSOR_ANCHO_PULSO, SENSOR_RANGO_ADC);
  brilloLed = LED_BRILLO_MAX;
  LOG("MAX30102 listo: LED al máximo (%d), %d muestras/s.\n", LED_BRILLO_MAX, (int)FS_NOMINAL);
  pulsoOk = true;
  particleSensor.clearFIFO();
  reiniciarMedidaFs = true;
  return true;
}

void setup() {
  setCpuFrequencyMhz(CPU_MHZ);

#if DEBUG
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 2000) delay(10);
#endif
  LOG("Arrancando...\n");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(50);

  iniciarSensorPulso();

  canalIR.filtro.pasaBajos(4.0f, FS_NOMINAL);
  canalRojo.filtro.pasaBajos(4.0f, FS_NOMINAL);
  bpPulso.configurar(FS_NOMINAL);
  bpAx.configurar(FS_NOMINAL); bpAy.configurar(FS_NOMINAL); bpAz.configurar(FS_NOMINAL);

#if USAR_MPU
  iniciarMPU();
#endif

  iniciarBLE();
}

void loop() {
  static uint32_t ultimoIntento = 0;
  if (!pulsoOk && millis() - ultimoIntento > 5000) {
    ultimoIntento = millis();
    iniciarSensorPulso();
  }

  actualizarG();
  leerSensor();
  medirFrecuencia();
  enviarPeriodico();

  uint32_t ahora = millis();

  if (bpmLatidos && ahora - ultimoLatidoBueno > SIN_LATIDO_MS) borrarLatidos();

  static uint32_t ultimoAnalisis = 0;
  if (hayContacto && ahora - ultimoAnalisis >= 1000) {
    ultimoAnalisis = ahora;
    bool moviendose = (ahora - ultimoMovimiento < 2000);
    analizarFrecuencias(moviendose);
    actualizarSalida(moviendose);
  }

  static uint32_t ultimaLecturaTemp = 0;
  if (pulsoOk && (ultimaLecturaTemp == 0 || ahora - ultimaLecturaTemp > TEMP_INTERVALO_MS)) {
    ultimaLecturaTemp = ahora;
    ultimaTemp = particleSensor.readTemperature();
    leerSensor();
  }

#if DEBUG
  static uint32_t ultimoLog = 0;
  if (ahora - ultimoLog >= 1000) {
    ultimoLog = ahora;
    float pi = canalIR.dc > 0 ? 100.0f * envolvente / canalIR.dc : 0;
    LOG("IR:%lu  LED:%d  fs:%.1f  señal:%.3f%%  latidos:%d(%d)  limpios:%d  esp:%.0f  frec:%.0f(conf %.1f)  %s  %s  Web BPM:%d  Web SpO2:%d\n",
        (unsigned long)canalIR.dc, brilloLed, fsReal, pi, nLatidos, bpmLatidos, nR, bpmEspectro,
        bpmSeguido, confianzaEspectro,
        (ahora - ultimoMovimiento < 2000) ? "MOVIMIENTO" : "quieto",
        pulsoValidado ? "OK" : "confirmando",
        bpmWeb, spo2Web);
  #if USAR_MPU && USAR_GIROSCOPIO
    if (mpuOk) {
      float gx, gy, gz;
      leerGiroscopio(gx, gy, gz);
      LOG("Gyro X:%.2f Y:%.2f Z:%.2f deg/s\n", gx, gy, gz);
    }
  #endif
  }
#endif

  delay(2);
}
