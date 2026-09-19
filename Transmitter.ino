#include <Wire.h>
#include <math.h>
#include <SPI.h>
#include <LoRa.h>
#include <esp_task_wdt.h>

// ============================================================
//           POLESENTINEL TRANSMITTER (STABLE MASTER + GPS)
// ============================================================

// --- LoRa Pins ---
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 26

// --- Sensor Pins & Registers ---
#define SDA_PIN 21
#define SCL_PIN 22
#define ACS_PIN 34
#define MPU_ADDR 0x68

#define PWR_MGMT_1   0x6B
#define ACCEL_XOUT_H 0x3B
#define WHO_AM_I     0x75

#define SAMPLE_INTERVAL_MS 10
#define TRANSMIT_INTERVAL_MS 1000  // 1s interval prevents Gateway network bottlenecks

// --- Pole GPS (Fixed Installation Coordinates) ---
#define POLE_LAT 12.9675
#define POLE_LNG 80.0488

// --- IMU Variables ---
#define IMU_CALIBRATION_SAMPLES 200
float baseAX = 0, baseAY = 0, baseAZ = 0;
float baseAccelMagnitude = 0;
float filteredAX = 0, filteredAY = 0, filteredAZ = 0;

// --- Sensor Variables ---
#define ACS_CALIBRATION_SAMPLES 300
#define ACS_AVERAGE_SAMPLES 50 

float baseACS = 0;
float filteredACS = 0;

// --- Detection Thresholds ---
#define CRASH_ACCEL_THRESHOLD 25000
#define POSITION_CHANGE_ANGLE 8.0
#define TILT_CHANGE_ANGLE 4.0
#define VIBRATION_WARNING 1000
#define VIBRATION_SEVERE 2500

// --- Latched State Variables ---
bool crashDetected = false;
bool positionChanged = false;
bool tiltChanged = false;
bool electricalFault = false;
bool severeVibration = false;
bool sensorFault = false;
unsigned long tiltStartTime = 0;
#define PERSISTENCE_TIME_MS 1000

// ============================================================
// HELPER FUNCTIONS
// ============================================================
void writeRegister(byte reg, byte value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg); Wire.write(value);
  Wire.endTransmission();
}

byte readRegister(byte reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  Wire.requestFrom(MPU_ADDR, (byte)1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

bool readMPU(int16_t &ax, int16_t &ay, int16_t &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(MPU_ADDR, (byte)6);
  if (Wire.available() < 6) return false;
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  return true;
}

void calibrateSensors() {
  Serial.println("Calibrating IMU (Keep Still)...");
  long sumAX = 0, sumAY = 0, sumAZ = 0;
  int validSamples = 0;
  for (int i = 0; i < IMU_CALIBRATION_SAMPLES; i++) {
    int16_t ax, ay, az;
    if (readMPU(ax, ay, az)) {
      sumAX += ax; sumAY += ay; sumAZ += az; validSamples++;
    }
    delay(4);
  }
  if (validSamples == 0) { sensorFault = true; }
  else {
    baseAX = (float)sumAX / validSamples;
    baseAY = (float)sumAY / validSamples;
    baseAZ = (float)sumAZ / validSamples;
    baseAccelMagnitude = sqrt(baseAX * baseAX + baseAY * baseAY + baseAZ * baseAZ);
    filteredAX = baseAX; filteredAY = baseAY; filteredAZ = baseAZ;
  }

  Serial.println("Calibrating Current Sensor...");
  long sum = 0;
  for (int i = 0; i < ACS_CALIBRATION_SAMPLES; i++) {
    sum += analogRead(ACS_PIN);
    delay(2);
  }
  baseACS = (float)sum / ACS_CALIBRATION_SAMPLES;
}

float calculateAngleFromOriginal(float ax, float ay, float az) {
  float currentMagnitude = sqrt(ax * ax + ay * ay + az * az);
  if (currentMagnitude == 0 || baseAccelMagnitude == 0) return 0;
  float dot = ax * baseAX + ay * baseAY + az * baseAZ;
  float cosineAngle = dot / (currentMagnitude * baseAccelMagnitude);
  if (cosineAngle > 1.0) cosineAngle = 1.0;
  if (cosineAngle < -1.0) cosineAngle = -1.0;
  return acos(cosineAngle) * 180.0 / PI;
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // 1. Explicit Hardware Reset of LoRa
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW);
  delay(20);
  digitalWrite(LORA_RST, HIGH);
  delay(50);

  // 2. Initialize LoRa
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSPIFrequency(1E6); // Slow SPI for breadboard stability
  
  if (!LoRa.begin(433E6)) {
    Serial.println("❌ LoRa Init Failed! Check power stability.");
    while (1) { delay(500); }
  }
  LoRa.setTxPower(17);
  Serial.println("✅ LoRa Initialized Successfully!");

  // 3. Initialize I2C with hardware timeout
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(50); // Prevent bus freeze on MPU6050
  delay(50);
  writeRegister(PWR_MGMT_1, 0x00);
  delay(50);

  if (readRegister(WHO_AM_I) == 0xFF) sensorFault = true;

  analogReadResolution(12);
  calibrateSensors();

  // 4. Initialize Hardware Watchdog (3 second timeout) for ESP32 Core 3.x
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 3000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  Serial.println("📡 POLESENTINEL TRANSMITTING STABLY...");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  // Pet the watchdog to confirm we aren't frozen
  esp_task_wdt_reset(); 

  static unsigned long lastSample = 0;
  static unsigned long lastTransmit = 0;
  
  static float accelerationMagnitude = 0, vibrationMagnitude = 0, tiltAngle = 0, absDrop = 0;

  // --- SENSOR SAMPLING ---
  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();

    int16_t axRaw, ayRaw, azRaw;
    if (!readMPU(axRaw, ayRaw, azRaw)) {
      sensorFault = true;
    } else {
      sensorFault = false;
      float ax = axRaw, ay = ayRaw, az = azRaw;

      accelerationMagnitude = sqrt(ax * ax + ay * ay + az * az);
      filteredAX = 0.10f * ax + 0.90f * filteredAX;
      filteredAY = 0.10f * ay + 0.90f * filteredAY;
      filteredAZ = 0.10f * az + 0.90f * filteredAZ;

      vibrationMagnitude = sqrt(pow(ax - filteredAX, 2) + pow(ay - filteredAY, 2) + pow(az - filteredAZ, 2));
      tiltAngle = calculateAngleFromOriginal(ax, ay, az);

      // Fault Latching Logic
      if (accelerationMagnitude >= CRASH_ACCEL_THRESHOLD) crashDetected = true; 
      
      if (tiltAngle >= POSITION_CHANGE_ANGLE) {
        if (tiltStartTime == 0) tiltStartTime = millis();
        if (millis() - tiltStartTime >= PERSISTENCE_TIME_MS) positionChanged = true; 
      } else if (tiltAngle >= TILT_CHANGE_ANGLE && !positionChanged) {
        if (tiltStartTime == 0) tiltStartTime = millis();
        if (millis() - tiltStartTime >= PERSISTENCE_TIME_MS) tiltChanged = true;
      } else {
        tiltStartTime = 0; 
        tiltChanged = false; 
      }

      if (vibrationMagnitude >= VIBRATION_SEVERE) severeVibration = true;
      else if (vibrationMagnitude < VIBRATION_WARNING) severeVibration = false;
    }

    long acsSum = 0;
    for (int i = 0; i < ACS_AVERAGE_SAMPLES; i++) acsSum += analogRead(ACS_PIN);
    float acsAverage = (float)acsSum / ACS_AVERAGE_SAMPLES;
    absDrop = fabs(baseACS - acsAverage); 

    // Clamp microscopic sensor noise to 0.0
    if (absDrop < 25.0) {
      absDrop = 0.0; 
    }

    // *** THE FIX: DEBOUNCED & AUTO-RECOVERING ELECTRICAL FAULT ***
    static unsigned long elecStartTime = 0;
    
    // Increased threshold to 250 to ignore LoRa radio interference
    if (absDrop >= 250.0f) { 
      if (elecStartTime == 0) elecStartTime = millis();
      // Only trigger if the drop lasts for a full second
      if (millis() - elecStartTime >= 1000) {
        electricalFault = true; 
      }
    } else {
      elecStartTime = 0;
      // Auto-recover back to normal when the LED is plugged back in!
      electricalFault = false; 
    }
  }

  // --- LORA BROADCASTING ---
  if (millis() - lastTransmit >= TRANSMIT_INTERVAL_MS) {
    lastTransmit = millis();

    String overallStatus = "HEALTHY";
    if (sensorFault || crashDetected || positionChanged || electricalFault) overallStatus = "CRITICAL";
    else if (severeVibration || tiltChanged) overallStatus = "WARNING";

    // Build the payload with the GPS coordinates cleanly appended
    String payload = "SL-001,";
    payload += overallStatus + ",";
    payload += String(accelerationMagnitude, 0) + ",";
    payload += String(tiltAngle, 1) + ",";
    payload += String(absDrop, 1) + ",";
    payload += String(crashDetected ? 1 : 0) + ",";
    payload += String(positionChanged ? 1 : 0) + ",";
    payload += String(tiltChanged ? 1 : 0) + ",";
    payload += String(electricalFault ? 1 : 0) + ",";
    payload += String(severeVibration ? 1 : 0) + ",";
    payload += String(POLE_LAT, 4) + ","; 
    payload += String(POLE_LNG, 4);

    // Broadcast packet ASYNCHRONOUSLY to prevent loop blocking
    LoRa.beginPacket();
    LoRa.print(payload);
    LoRa.endPacket(true); // <--- 'true' means don't wait, keep running!

    Serial.println("📡 Sent: " + payload);
  }
}
