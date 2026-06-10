#include <Wire.h>
#include <MPU6050.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

MPU6050 mpu;
MAX30105 particleSensor;

// ── Umbrales MPU6050 ───────────────────────────────────────────
const float    FREEFALL_THRESHOLD  = 3.0f;
const uint32_t FREEFALL_MIN_MS     = 30;
const float    IMPACT_THRESHOLD    = 25.0f;
const uint32_t IMPACT_WINDOW_MS    = 500;
const uint32_t COOLDOWN_MS         = 3000;

// ── Umbrales MAX30102 ──────────────────────────────────────────
const float BPM_LOW        = 40.0f;
const float BPM_HIGH       = 150.0f;
const float SPO2_LOW       = 90.0f;
const uint32_t POST_FALL_MONITOR_MS = 15000;

// ── Botón de emergencia ────────────────────────────────────────
const uint8_t  BOTON_PIN         = 2;      // ← cambiá al GPIO que uses
const uint32_t BOTON_DEBOUNCE_MS = 50;     // anti-rebote
const uint32_t BOTON_HOLD_MS     = 1000;   // mantener 1s para confirmar
const uint32_t BOTON_COOLDOWN_MS = 10000;  // evita spam de alertas

// ── Buffers SpO2/BPM ──────────────────────────────────────────
#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int32_t  spo2Value      = 0;
int8_t   spo2Valid      = 0;
int32_t  heartRate      = 0;
int8_t   heartRateValid = 0;

// ── Variables de estado MPU6050 ────────────────────────────────
bool     inFreefall     = false;
uint32_t freefallStart  = 0;
bool     freefallValid  = false;
uint32_t freefallEnd    = 0;
uint32_t lastAlertTime  = 0;

// ── Variables de estado post-caída ────────────────────────────
bool     postFallMonitor = false;
uint32_t postFallStart   = 0;

// ── Variables de estado del botón ─────────────────────────────
bool     botonPresionado     = false;
uint32_t botonPressStart     = 0;
bool     botonHoldConfirmado = false;
uint32_t ultimaAlertaBoton   = 0;

// ── Índice circular MAX30102 ───────────────────────────────────
uint8_t  bufIndex    = 0;
bool     bufferLleno = false;

// ──────────────────────────────────────────────────────────────
void enviarAlertaEmergencia() {
  Serial.println("╔══════════════════════════════════╗");
  Serial.println("║      🚨 ALERTA DE EMERGENCIA      ║");
  Serial.println("╠══════════════════════════════════╣");
  Serial.println("║  El usuario presionó el botón    ║");
  Serial.println("║  de emergencia manualmente.      ║");
  Serial.println("║                                  ║");
  Serial.println("║  [TODO] Enviar SMS/notificación  ║");
  Serial.println("║  al contacto de emergencia.      ║");
  Serial.println("╠══════════════════════════════════╣");

  // Adjunta signos vitales al momento de la alerta
  if (heartRateValid) {
    Serial.printf("║  BPM:  %3d bpm                   ║\n", heartRate);
  } else {
    Serial.println("║  BPM:  calculando...             ║");
  }
  if (spo2Valid) {
    Serial.printf("║  SpO2: %3d%%                      ║\n", spo2Value);
  } else {
    Serial.println("║  SpO2: calculando...             ║");
  }

  Serial.println("╚══════════════════════════════════╝");
}

// ──────────────────────────────────────────────────────────────
// Lee el botón con anti-rebote y detección de pulsación larga.
// Dispara la alerta solo si se mantiene >= BOTON_HOLD_MS.
// ──────────────────────────────────────────────────────────────
void leerBotonEmergencia() {
  bool estadoActual = (digitalRead(BOTON_PIN) == LOW); // LOW = presionado (pull-up)
  uint32_t now = millis();

  if (estadoActual && !botonPresionado) {
    // Flanco de bajada: empieza la pulsación
    botonPresionado     = true;
    botonPressStart     = now;
    botonHoldConfirmado = false;

  } else if (estadoActual && botonPresionado && !botonHoldConfirmado) {
    // Sigue presionado: verificar si ya cumplió el tiempo de hold
    if ((now - botonPressStart) >= BOTON_HOLD_MS) {
      botonHoldConfirmado = true;

      // Verificar cooldown para no spamear alertas
      if ((now - ultimaAlertaBoton) >= BOTON_COOLDOWN_MS) {
        ultimaAlertaBoton = now;
        enviarAlertaEmergencia();
      } else {
        uint32_t restante = (BOTON_COOLDOWN_MS - (now - ultimaAlertaBoton)) / 1000;
        Serial.printf("[BOTON] Alerta ya enviada. Esperar %lu s.\n", restante);
      }
    } else {
      // Feedback de progreso cada 200ms mientras se mantiene
      static uint32_t ultimoFeedback = 0;
      if ((now - ultimoFeedback) >= 200) {
        ultimoFeedback = now;
        uint32_t transcurrido = now - botonPressStart;
        uint8_t  progreso     = (transcurrido * 100) / BOTON_HOLD_MS;
        Serial.printf("[BOTON] Manteniendo... %d%%\n", progreso);
      }
    }

  } else if (!estadoActual && botonPresionado) {
    // Flanco de subida: soltaron el botón
    uint32_t duracion = now - botonPressStart;

    if (duracion < BOTON_DEBOUNCE_MS) {
      // Ruido / rebote, ignorar
    } else if (!botonHoldConfirmado) {
      Serial.println("[BOTON] Pulsación corta ignorada. Mantener 1 segundo para enviar alerta.");
    }
    botonPresionado = false;
  }
}

// ──────────────────────────────────────────────────────────────
void actualizarMAX30102() {
  if (particleSensor.available() < 1) {
    particleSensor.check();
    return;
  }

  redBuffer[bufIndex] = particleSensor.getRed();
  irBuffer[bufIndex]  = particleSensor.getIR();
  particleSensor.nextSample();

  bufIndex++;
  if (bufIndex >= BUFFER_SIZE) {
    bufIndex     = 0;
    bufferLleno  = true;
  }

  if (bufferLleno && bufIndex == 0) {
    maxim_heart_rate_and_oxygen_saturation(
      irBuffer, BUFFER_SIZE, redBuffer,
      &spo2Value, &spo2Valid,
      &heartRate, &heartRateValid
    );
  }
}

// ──────────────────────────────────────────────────────────────
void evaluarSignosVitales(bool esPostCaida) {
  bool hayAlerta = false;

  if (heartRateValid) {
    float bpm = (float)heartRate;
    if (bpm < BPM_LOW || bpm > BPM_HIGH) {
      hayAlerta = true;
      Serial.printf("[ALERTA] BPM anormal: %.0f bpm\n", bpm);
    } else {
      Serial.printf("[INFO]   BPM: %.0f bpm\n", bpm);
    }
  } else {
    Serial.println("[INFO]   BPM: calculando...");
  }

  if (spo2Valid) {
    float spo2 = (float)spo2Value;
    if (spo2 < SPO2_LOW) {
      hayAlerta = true;
      Serial.printf("[ALERTA] SpO2 bajo: %.0f%%\n", spo2);
    } else {
      Serial.printf("[INFO]   SpO2: %.0f%%\n", spo2);
    }
  } else {
    Serial.println("[INFO]   SpO2: calculando...");
  }

  if (esPostCaida && hayAlerta) {
    Serial.println("[CRITICO] Caida detectada + signos vitales anormales. Requiere asistencia.");
  }
}

// ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin();

  // ── Botón ─────────────────────────────────────────────────
  pinMode(BOTON_PIN, INPUT_PULLUP);
  Serial.println("[OK] Boton de emergencia listo (GPIO " + String(BOTON_PIN) + ")");

  // ── MPU6050 ───────────────────────────────────────────────
  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("[ERROR] MPU6050 no encontrado.");
    while (true) delay(1000);
  }
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_8);
  mpu.setDLPFMode(MPU6050_DLPF_BW_20);
  Serial.println("[OK] MPU6050 listo");

  // ── MAX30102 ──────────────────────────────────────────────
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[ERROR] MAX30102 no encontrado.");
    while (true) delay(1000);
  }
  particleSensor.setup(60, 4, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x3C);
  particleSensor.setPulseAmplitudeIR(0x3C);
  Serial.println("[OK] MAX30102 listo");
  Serial.println("[OK] Sistema listo. Monitoreando...");
}

// ──────────────────────────────────────────────────────────────
void loop() {
  // ── Botón de emergencia (prioridad alta) ──────────────────
  leerBotonEmergencia();

  // ── Leer MPU6050 ──────────────────────────────────────────
  int16_t ax16, ay16, az16, gx, gy, gz;
  mpu.getMotion6(&ax16, &ay16, &az16, &gx, &gy, &gz);

  const float LSB_TO_G = 4096.0f;
  const float G        = 9.80665f;
  float ax  = (ax16 / LSB_TO_G) * G;
  float ay  = (ay16 / LSB_TO_G) * G;
  float az  = (az16 / LSB_TO_G) * G;
  float mag = sqrt(ax*ax + ay*ay + az*az);

  uint32_t now = millis();

  // ── FASE 1: caída libre ────────────────────────────────────
  if (mag < FREEFALL_THRESHOLD) {
    if (!inFreefall) {
      inFreefall    = true;
      freefallStart = now;
      freefallValid = false;
    } else if ((now - freefallStart) >= FREEFALL_MIN_MS) {
      freefallValid = true;
    }
  } else {
    if (inFreefall) freefallEnd = now;
    inFreefall = false;
  }

  // ── FASE 2: impacto post-caída ─────────────────────────────
  if (freefallValid && !inFreefall) {
    bool dentroDeVentana = (now - freefallEnd) <= IMPACT_WINDOW_MS;

    if (dentroDeVentana && mag > IMPACT_THRESHOLD) {
      if ((now - lastAlertTime) > COOLDOWN_MS) {
        Serial.println("──────────────────────────────");
        Serial.println("[CAIDA] Impacto detectado");
        Serial.printf( "  Magnitud: %.1f m/s²\n", mag);
        evaluarSignosVitales(true);
        postFallMonitor = true;
        postFallStart   = now;
        lastAlertTime   = now;
        Serial.println("──────────────────────────────");
      }
      freefallValid = false;
    } else if (!dentroDeVentana) {
      freefallValid = false;
    }
  }

  // ── FASE 3: monitoreo post-caída ───────────────────────────
  if (postFallMonitor) {
    uint32_t elapsed = now - postFallStart;

    static uint32_t ultimaEvaluacion = 0;
    if ((now - ultimaEvaluacion) >= 2000) {
      ultimaEvaluacion = now;
      Serial.printf("[POST-CAIDA] t+%lu s\n", elapsed / 1000);
      evaluarSignosVitales(true);
    }

    if (elapsed >= POST_FALL_MONITOR_MS) {
      postFallMonitor = false;
      Serial.println("[INFO] Fin de monitoreo post-caída.");
    }
  }

  // ── Leer MAX30102 ─────────────────────────────────────────
  actualizarMAX30102();

  delay(10); // proyecto


  delay(10); // proyecto

}