#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <WiFi.h>

// ---------------- WiFi ----------------
const char* ssid = "TU_RED_WIFI";
const char* password = "TU_CONTRASEÑA";

// ---------------- MAX30102 ----------------
MAX30105 particleSensor;

#define BUFFER_SIZE 100

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int32_t bufferLength = BUFFER_SIZE;
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// Para promediar y estabilizar
#define NUM_READINGS 5
int32_t bpmReadings[NUM_READINGS];
int32_t spo2Readings[NUM_READINGS];
int readingIndex = 0;
bool bufferFilled = false;

// ---------------- MPU6050 ----------------

#define SDA_PIN 8
#define SCL_PIN 9
#define MPU6050_ADDR 0x68
#define ACCEL_SENSITIVITY 16384.0f
#define GYRO_SENSITIVITY  131.0f

void leerMPU6050(float &ax, float &ay, float &az,
                  float &gx, float &gy, float &gz,
                  float &tempMPU) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 14, true);

  int16_t rawAX = Wire.read() << 8 | Wire.read();
  int16_t rawAY = Wire.read() << 8 | Wire.read();
  int16_t rawAZ = Wire.read() << 8 | Wire.read();
  int16_t rawTemp = Wire.read() << 8 | Wire.read();
  int16_t rawGX = Wire.read() << 8 | Wire.read();
  int16_t rawGY = Wire.read() << 8 | Wire.read();
  int16_t rawGZ = Wire.read() << 8 | Wire.read();

  ax = rawAX / ACCEL_SENSITIVITY;
  ay = rawAY / ACCEL_SENSITIVITY;
  az = rawAZ / ACCEL_SENSITIVITY;

  gx = rawGX / GYRO_SENSITIVITY;
  gy = rawGY / GYRO_SENSITIVITY;
  gz = rawGZ / GYRO_SENSITIVITY;

  tempMPU = rawTemp / 340.0 + 36.53;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  // Bus I2C compartido por ambos sensores
  Wire.begin(SDA_PIN, SCL_PIN);

  // ---- Inicializar MAX30102 ----
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("No se encontró el MAX30102. Revisá el cableado.");
    while (1);
  }
  Serial.println("MAX30102 detectado correctamente.");

  byte ledBrightness = 30;
  byte sampleAverage = 8; // más promediado interno = menos ruido
  byte ledMode = 2;
  int sampleRate = 100;
  int pulseWidth = 411;
  int adcRange = 4096;

  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);

  // ---- Inicializar MPU6050 ----
  // Nota: particleSensor.begin() ya deja el bus a 400kHz (I2C_SPEED_FAST),
  // velocidad totalmente compatible con el MPU6050.
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B); // registro de power management
  Wire.write(0x00); // sacarlo de sleep mode
  Wire.endTransmission();

  Serial.println("MPU6050 listo. Leyendo datos...");
}

void loop() {
  // ---- Llenar buffer del MAX30102 ----
  for (int i = 0; i < bufferLength; i++) {
    while (particleSensor.available() == false)
      particleSensor.check();

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }

  maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer,
                                          &spo2, &validSPO2, &heartRate, &validHeartRate);

  float temperature = particleSensor.readTemperature();

  // ---- Leer MPU6050 (una vez por cada ciclo de 100 muestras) ----
  float ax, ay, az, gx, gy, gz, tempMPU;
  leerMPU6050(ax, ay, az, gx, gy, gz, tempMPU);

  // ---- Procesar y mostrar datos del MAX30102 ----
  if (validHeartRate && validSPO2 && heartRate > 40 && heartRate < 180 && spo2 >= 70 && spo2 <= 100) {
    bpmReadings[readingIndex] = heartRate;
    spo2Readings[readingIndex] = spo2;
    readingIndex = (readingIndex + 1) % NUM_READINGS;
    if (readingIndex == 0) bufferFilled = true;

    if (bufferFilled) {
      long bpmSum = 0, spo2Sum = 0;
      for (int i = 0; i < NUM_READINGS; i++) {
        bpmSum += bpmReadings[i];
        spo2Sum += spo2Readings[i];
      }
      Serial.print("BPM promedio: ");
      Serial.print(bpmSum / NUM_READINGS);
      Serial.print("  SpO2 promedio: ");
      Serial.print(spo2Sum / NUM_READINGS);
      Serial.print("%  Temp MAX30102: ");
      Serial.print(temperature, 1);
      Serial.println(" C");
    } else {
      Serial.println("Estabilizando lectura de pulsioximetro...");
    }
  } else {
    Serial.println("Señal no válida - mantené el dedo quieto y firme");
  }

  // ---- Mostrar datos del MPU6050 ----
  Serial.printf("Accel X:%.2f Y:%.2f Z:%.2f g | Gyro X:%.2f Y:%.2f Z:%.2f deg/s | Temp MPU:%.1fC\n",
                ax, ay, az, gx, gy, gz, tempMPU);

  Serial.println("--------------------------------------------------");
}
