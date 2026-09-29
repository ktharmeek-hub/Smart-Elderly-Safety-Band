#include <Wire.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// BLE Libraries
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// =====================================================
// ESP32-C3 SUPER MINI PIN CONFIGURATION
// =====================================================

// I2C: OLED + MPU6050
#define SDA_PIN 4
#define SCL_PIN 5

// MPU6050
#define MPU_ADDR 0x68

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Outputs - according to your current correct wiring
#define BUZZER_PIN 6
#define RED_LED_PIN 3
#define GREEN_LED_PIN 10
#define BLUE_LED_PIN 7

// Buttons - according to your current correct wiring
#define PANIC_BUTTON_PIN 1
#define SLEEP_CANCEL_BUTTON_PIN 0

// =====================================================
// OUTPUT SETTINGS
// =====================================================

#define LED_ON HIGH
#define LED_OFF LOW

// TMB12A05 active buzzer
#define BUZZER_ON HIGH
#define BUZZER_OFF LOW

// =====================================================
// TIME SETTINGS
// =====================================================

#define WARNING_TIME 10000UL
#define DEBOUNCE_TIME 50UL
#define PANIC_LONG_PRESS_TIME 2000UL

// =====================================================
// MPU6050 DETECTION SETTINGS - 1m DOWNWARD HAND-MOTION FALL DEMO
// =====================================================

// IMPORTANT:
// MPU6050 cannot measure exact falling distance directly.
// A hand-carried 1m downward movement is NOT a true free fall because
// your hand supports the sensor. So this final demo logic accepts either:
// 1) real low-g/free-fall period, OR
// 2) sustained downward low-g movement on the calibrated vertical axis,
// and then requires a clear impact/stop.
// Side movement is reduced by checking only the calibrated downward axis.

#define FREE_FALL_THRESHOLD 7500.0
#define IMPACT_THRESHOLD 20000.0
#define MOVEMENT_THRESHOLD 1000.0

// Downward 1m hand-motion fall demo settings.
// MPU6050 cannot truly measure position like a distance sensor.
// To avoid small fast movements being detected as falls, this version uses
// BOTH a time gate and an estimated downward distance gate.
// A movement is confirmed only when the estimated downward travel reaches 1m,
// then a clear stop/impact is detected.
#define DOWNWARD_START_DROP 1800.0
#define DOWNWARD_TOTAL_ACCEL_MAX 32000.0
#define DOWNWARD_MOVEMENT_THRESHOLD 1500.0

// Main user requirement:
// below 1m fast downward movement = ignore
// about 1m or more downward movement + impact = fall
#define REQUIRED_DOWNWARD_DISTANCE_M 1.00
#define MIN_DOWNWARD_TRAVEL_TIME 120UL

// Helps convert accelerometer readings into a practical demo distance.
// If 1m movement is not detected, increase to 1.4.
// If small movement is detected, decrease to 1.0 or increase MIN_DOWNWARD_TRAVEL_TIME.
#define DOWNWARD_DISTANCE_GAIN 1.8
#define DOWNWARD_ACCEL_NOISE_MPS2 0.80

// Demo purpose: 15 seconds inactivity.
// Final real application: change to 1800000UL for 30 minutes.
#define INACTIVITY_LIMIT 15000UL

// Real free-fall candidate time.
// Keep this stricter so tiny shakes do not trigger as free fall.
#define FREE_FALL_MIN_TIME 260UL

// After impact, wait shortly to confirm fall.
// No full stillness required, so patient can still move/crawl.
#define POST_FALL_CONFIRM_TIME 0UL

#define POST_FALL_CHECK_WINDOW 2500UL
#define FALL_COOLDOWN 5000UL

// Shake-only rejection settings.
// These do NOT use tilt rejection, because the hand demo may tilt slightly.
// They only make sure an impact/shake is accepted as fall only after
// real recent downward movement happened first.
#define IMPACT_AFTER_DOWNWARD_GRACE_TIME 450UL  // ms after last downward sample
#define MIN_VALID_DOWNWARD_SAMPLES 1           // sample count
#define DOWNWARD_CONTINUE_DROP 800.0            // MPU raw vertical accel drop
#define DOWNWARD_CONTINUE_MOVE 800.0            // MPU raw movement difference

// =====================================================
// ESTIMATED BATTERY SETTINGS
// =====================================================
// No extra hardware is connected to measure real LiPo voltage.
// Therefore, this is an ESTIMATED battery value based on runtime.
// For real battery %, add voltage divider to ESP32 ADC in future.

#define ESTIMATED_RUNTIME_MINUTES 120UL     // Approx 2 hours for small 0.93Wh LiPo
#define BATTERY_SEND_INTERVAL 15000UL       // Send battery estimate to app every 15 seconds
#define HOME_SCREEN_UPDATE_INTERVAL 1000UL  // Refresh OLED home screen every 1 second

// =====================================================
// PANIC BUTTON EVENT VALUES
// NOTE: using #define instead of enum avoids Arduino .ino prototype issue.
// =====================================================

#define PANIC_NONE 0
#define PANIC_SHORT_PRESS 1
#define PANIC_LONG_PRESS 2

// =====================================================
// BLE APP ALERT SETTINGS
// =====================================================

#define BLE_DEVICE_NAME "ElderlyBand"

#define SERVICE_UUID    "12345678-1234-1234-1234-1234567890ab"
#define ALERT_CHAR_UUID "abcd1234-5678-90ab-cdef-1234567890ab"

BLECharacteristic *alertCharacteristic = NULL;
bool appConnected = false;

// =====================================================
// SYSTEM STATES
// =====================================================

bool sleepMode = false;
bool warningActive = false;
bool emergencyActive = false;
bool oledConnected = false;
bool mpuConnected = false;

String warningReason = "";

unsigned long warningStartTime = 0;
int lastCountdownNumber = -1;

// =====================================================
// ESTIMATED BATTERY VARIABLES
// =====================================================

int estimatedBatteryPercent = 100;
unsigned long batteryStartTime = 0;
unsigned long lastBatterySendTime = 0;
unsigned long lastHomeScreenUpdateTime = 0;

// =====================================================
// MPU VARIABLES
// =====================================================

int16_t ax, ay, az;
int16_t gx, gy, gz;

int16_t lastAx = 0;
int16_t lastAy = 0;
int16_t lastAz = 0;

bool firstMPURead = true;

unsigned long lastMovementTime = 0;
unsigned long lastFallTime = 0;
unsigned long lastMPURetryTime = 0;
unsigned long lastMPUErrorPrint = 0;

bool fallCandidate = false;
bool impactSeen = false;
bool freeFallSeen = false;
bool downwardMotionSeen = false;

unsigned long freeFallStartTime = 0;
unsigned long downwardMotionStartTime = 0;
unsigned long fallCandidateStart = 0;
unsigned long impactTime = 0;
unsigned long downwardDurationAtImpact = 0;
unsigned long lastFallSampleTime = 0;
unsigned long lastValidDownwardMotionTime = 0;
int validDownwardSamples = 0;

float maxMovementDuringFall = 0;
float estimatedDownwardVelocityMps = 0.0;
float estimatedDownwardDistanceM = 0.0;
float maxEstimatedDownwardDistanceM = 0.0;

// Calibrated vertical axis.
// 0 = X, 1 = Y, 2 = Z. Sign makes resting gravity direction positive.
int verticalAxis = 2;
int verticalSign = 1;
float baselineVerticalAccel = 16384.0;

// =====================================================
// BUTTON VARIABLES
// =====================================================

bool lastPanicReading = HIGH;
bool stablePanicState = HIGH;
unsigned long lastPanicDebounceTime = 0;
unsigned long panicPressStartTime = 0;
bool panicLongPressSent = false;

bool lastSleepReading = HIGH;
bool stableSleepState = HIGH;
unsigned long lastSleepDebounceTime = 0;

// =====================================================
// FUNCTION DECLARATIONS
// =====================================================

void initBLE();
void sendAppAlert(String message);

bool initOLED();
void showStatusScreen(String mode, String line1, String line2);
void showWarningScreen(String reason, int secondsLeft);
void showEmergencyScreen();

void buzzerStart();
void buzzerStop();
void buzzerEmergencyLoud();

void setLEDs(bool redOn, bool greenOn, bool blueOn);
void updateNormalLEDs();
void allOutputsOff();

void resetSystem();

bool initMPU6050();
bool readMPU6050Raw();
int updateMPU6050(bool sleepModeNow);
void resetFallCandidate();
void calibrateVerticalAxis();
float getVerticalAccelInGravityDirection();

void startWarning(String reason);
void updateWarningCountdown();
void cancelWarning();
void confirmEmergency();
void updateEmergencyAlarm();
void stopEmergency();

void toggleSleepMode();

int readPanicButtonEvent();
bool readSleepCancelButtonPress();

void updateEstimatedBattery();
void sendEstimatedBatteryToApp();
void updateHomeScreenIfNeeded();

// =====================================================
// BLE CALLBACKS
// =====================================================

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    appConnected = true;
    Serial.println("BLE App connected");
  }

  void onDisconnect(BLEServer* pServer) {
    appConnected = false;
    Serial.println("BLE App disconnected");

    // Start advertising again so app can reconnect
    BLEDevice::startAdvertising();
  }
};

void initBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new MyServerCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);

  alertCharacteristic = service->createCharacteristic(
    ALERT_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  alertCharacteristic->addDescriptor(new BLE2902());
  alertCharacteristic->setValue("DEVICE_READY");

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();

  Serial.println("BLE started as ElderlyBand");
  Serial.println("Open Flutter app and connect to ElderlyBand");
}

void sendAppAlert(String message) {
  Serial.print("APP ALERT: ");
  Serial.println(message);

  if (alertCharacteristic == NULL) {
    return;
  }

  alertCharacteristic->setValue((uint8_t*)message.c_str(), message.length());

  if (appConnected) {
    alertCharacteristic->notify();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  batteryStartTime = millis();
  estimatedBatteryPercent = 100;
  lastBatterySendTime = 0;
  lastHomeScreenUpdateTime = 0;

  Serial.println();
  Serial.println("========================================");
  Serial.println("Smart Elderly Emergency Monitoring Band");
  Serial.println("ESP32-C3 + OLED + MPU6050 + TMB12A05");
  Serial.println("1m Downward Fall Detection + BLE App Alert");
  Serial.println("Battery Display Enabled");
  Serial.println("========================================");

  // Start Bluetooth/BLE
  initBLE();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);

  pinMode(PANIC_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SLEEP_CANCEL_BUTTON_PIN, INPUT_PULLUP);

  allOutputsOff();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);

  oledConnected = initOLED();
  mpuConnected = initMPU6050();

  resetSystem();

  Serial.println("Device ON");
  Serial.println("Normal Mode Started");
  Serial.println("1m downward fall detection enabled");
  Serial.println("Inactivity demo time = 15 seconds");
  Serial.println("BLE app alert enabled");
  Serial.println("Battery display enabled");

  sendAppAlert("DEVICE_STARTED");
  sendEstimatedBatteryToApp();

  showStatusScreen("NORMAL", "Device ON", "Monitoring...");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  updateEstimatedBattery();

  int panicEvent = readPanicButtonEvent();
  bool sleepCancelPressed = readSleepCancelButtonPress();

  if (panicEvent == PANIC_SHORT_PRESS) {
    Serial.println("Panic button short press");
  }

  if (panicEvent == PANIC_LONG_PRESS) {
    Serial.println("Panic button long press");
  }

  if (sleepCancelPressed) {
    Serial.println("Sleep/Cancel button pressed");
  }

  // ---------------- WARNING MODE ----------------
  if (warningActive) {
    if (panicEvent == PANIC_LONG_PRESS || sleepCancelPressed) {
      cancelWarning();
      return;
    }

    updateWarningCountdown();
    return;
  }

  // ---------------- EMERGENCY MODE ----------------
  if (emergencyActive) {
    if (panicEvent == PANIC_LONG_PRESS || sleepCancelPressed) {
      stopEmergency();
      return;
    }

    updateEmergencyAlarm();
    return;
  }

  // ---------------- NORMAL / SLEEP MODE ----------------
  if (sleepCancelPressed) {
    toggleSleepMode();
  }

  if (panicEvent == PANIC_SHORT_PRESS) {
    startWarning("PANIC BUTTON");
    return;
  }

  int mpuEvent = updateMPU6050(sleepMode);

  if (mpuEvent == 1) {
    startWarning("PANIC FALL ALERT");
    return;
  }

  if (mpuEvent == 2) {
    startWarning("INACTIVITY");
    return;
  }

  updateNormalLEDs();
  updateHomeScreenIfNeeded();
}

// =====================================================
// ESTIMATED BATTERY FUNCTIONS
// =====================================================

void updateEstimatedBattery() {
  unsigned long now = millis();

  unsigned long elapsedMinutes = (now - batteryStartTime) / 60000UL;

  int newPercent = 100 - (int)((elapsedMinutes * 100UL) / ESTIMATED_RUNTIME_MINUTES);

  if (newPercent > 100) newPercent = 100;
  if (newPercent < 0) newPercent = 0;

  estimatedBatteryPercent = newPercent;

  if (now - lastBatterySendTime >= BATTERY_SEND_INTERVAL) {
    lastBatterySendTime = now;
    sendEstimatedBatteryToApp();
  }
}

void sendEstimatedBatteryToApp() {
  sendAppAlert("BATTERY_EST:" + String(estimatedBatteryPercent));
}

void updateHomeScreenIfNeeded() {
  unsigned long now = millis();

  if (now - lastHomeScreenUpdateTime < HOME_SCREEN_UPDATE_INTERVAL) {
    return;
  }

  lastHomeScreenUpdateTime = now;

  if (warningActive || emergencyActive) {
    return;
  }

  if (sleepMode) {
    showStatusScreen("SLEEP", "Sleep Mode ON", "Inactivity OFF");
  } else {
    showStatusScreen("NORMAL", "Monitoring...", "Fall/Panic Active");
  }
}

// =====================================================
// OLED FUNCTIONS
// =====================================================

bool initOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED display failed");
    Serial.println("Check OLED VCC, GND, SDA GPIO4, SCL GPIO5");
    return false;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("Smart Elderly Band");
  display.println("Starting...");
  display.display();

  Serial.println("OLED display initialized successfully");
  delay(300);

  return true;
}

void showStatusScreen(String mode, String line1, String line2) {
  if (!oledConnected) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("ELDER MONITOR");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 14);
  display.print("Mode: ");
  display.println(mode);

  display.setCursor(0, 28);
  display.println(line1);

  display.setCursor(0, 40);
  display.println(line2);

  display.setCursor(0, 54);
  display.print("Battery: ");
  display.print(estimatedBatteryPercent);
  display.println("%");

  display.display();
}

void showWarningScreen(String reason, int secondsLeft) {
  if (!oledConnected) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("WARNING ALERT");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 14);
  display.print("Reason:");
  display.println(reason);

  display.setCursor(0, 30);
  display.print("Emergency in: ");
  display.print(secondsLeft);
  display.println("s");

  display.setCursor(0, 43);
  display.println("Cancel to stop");

  display.setCursor(0, 55);
  display.print("Bat: ");
  display.print(estimatedBatteryPercent);
  display.println("%");

  display.display();
}

void showEmergencyScreen() {
  if (!oledConnected) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println("EMERGENCY");

  display.setTextSize(1);
  display.setCursor(0, 26);
  display.println("BUZZER ON");
  display.println("Long press Panic");
  display.println("or Cancel to stop");

  display.setCursor(0, 55);
  display.print("Bat: ");
  display.print(estimatedBatteryPercent);
  display.println("%");

  display.display();
}

// =====================================================
// BUZZER FUNCTIONS
// =====================================================

void buzzerStart() {
  digitalWrite(BUZZER_PIN, BUZZER_ON);
}

void buzzerStop() {
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
}

void buzzerEmergencyLoud() {
  digitalWrite(BUZZER_PIN, BUZZER_ON);
}

// =====================================================
// LED FUNCTIONS
// =====================================================

void setLEDs(bool redOn, bool greenOn, bool blueOn) {
  digitalWrite(RED_LED_PIN, redOn ? LED_ON : LED_OFF);
  digitalWrite(GREEN_LED_PIN, greenOn ? LED_ON : LED_OFF);
  digitalWrite(BLUE_LED_PIN, blueOn ? LED_ON : LED_OFF);
}

void updateNormalLEDs() {
  buzzerStop();

  if (sleepMode) {
    setLEDs(false, false, true);   // Blue ON for sleep mode
  } else {
    setLEDs(false, true, false);   // Green ON for normal mode
  }
}

void allOutputsOff() {
  setLEDs(false, false, false);
  buzzerStop();
}

// =====================================================
// SYSTEM FUNCTIONS
// =====================================================

void resetSystem() {
  sleepMode = false;
  warningActive = false;
  emergencyActive = false;

  warningReason = "";
  lastCountdownNumber = -1;

  firstMPURead = true;

  resetFallCandidate();

  lastMovementTime = millis();

  allOutputsOff();
  updateNormalLEDs();
}

void resetFallCandidate() {
  fallCandidate = false;
  impactSeen = false;
  freeFallSeen = false;
  downwardMotionSeen = false;

  freeFallStartTime = 0;
  downwardMotionStartTime = 0;
  fallCandidateStart = 0;
  impactTime = 0;
  downwardDurationAtImpact = 0;
  lastFallSampleTime = 0;
  lastValidDownwardMotionTime = 0;
  validDownwardSamples = 0;
  maxMovementDuringFall = 0;
  estimatedDownwardVelocityMps = 0.0;
  estimatedDownwardDistanceM = 0.0;
  maxEstimatedDownwardDistanceM = 0.0;
}

// =====================================================
// MPU6050 FUNCTIONS
// =====================================================

bool initMPU6050() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  byte error = Wire.endTransmission(true);

  delay(100);

  if (error == 0) {
    Serial.println("MPU6050 initialized successfully");
    firstMPURead = true;
    return true;
  } else {
    Serial.println("MPU6050 initialization failed");
    Serial.println("Check MPU6050:");
    Serial.println("VCC -> 3V3");
    Serial.println("GND -> GND");
    Serial.println("SDA -> GPIO4");
    Serial.println("SCL -> GPIO5");

    showStatusScreen("ERROR", "MPU6050 Failed", "Check wiring");
    return false;
  }
}

void calibrateVerticalAxis() {
  int absX = abs(ax);
  int absY = abs(ay);
  int absZ = abs(az);

  if (absX >= absY && absX >= absZ) {
    verticalAxis = 0;
    verticalSign = (ax >= 0) ? 1 : -1;
    baselineVerticalAccel = absX;
  } else if (absY >= absX && absY >= absZ) {
    verticalAxis = 1;
    verticalSign = (ay >= 0) ? 1 : -1;
    baselineVerticalAccel = absY;
  } else {
    verticalAxis = 2;
    verticalSign = (az >= 0) ? 1 : -1;
    baselineVerticalAccel = absZ;
  }

  Serial.print("Vertical axis calibrated: ");
  if (verticalAxis == 0) Serial.print("X");
  if (verticalAxis == 1) Serial.print("Y");
  if (verticalAxis == 2) Serial.print("Z");
  Serial.print(" | Sign: ");
  Serial.print(verticalSign);
  Serial.print(" | Baseline: ");
  Serial.println(baselineVerticalAccel);
}

float getVerticalAccelInGravityDirection() {
  if (verticalAxis == 0) return verticalSign * (float)ax;
  if (verticalAxis == 1) return verticalSign * (float)ay;
  return verticalSign * (float)az;
}

bool readMPU6050Raw() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14, (uint8_t)true);

  if (Wire.available() < 14) {
    return false;
  }

  ax = Wire.read() << 8 | Wire.read();
  ay = Wire.read() << 8 | Wire.read();
  az = Wire.read() << 8 | Wire.read();

  // Skip temperature
  Wire.read();
  Wire.read();

  gx = Wire.read() << 8 | Wire.read();
  gy = Wire.read() << 8 | Wire.read();
  gz = Wire.read() << 8 | Wire.read();

  return true;
}

// Return values:
// 0 = no event
// 1 = fall detected
// 2 = inactivity detected
int updateMPU6050(bool sleepModeNow) {
  unsigned long now = millis();

  if (!mpuConnected) {
    if (now - lastMPURetryTime > 3000) {
      lastMPURetryTime = now;
      Serial.println("Retrying MPU6050 connection...");
      mpuConnected = initMPU6050();
    }

    return 0;
  }

  if (!readMPU6050Raw()) {
    mpuConnected = false;

    if (now - lastMPUErrorPrint > 2000) {
      Serial.println("MPU6050 read failed - check SDA/SCL/VCC/GND");
      showStatusScreen("ERROR", "MPU6050 read fail", "Check wiring");
      lastMPUErrorPrint = now;
    }

    return 0;
  }

  float accelMagnitude = sqrt(
    (float)ax * ax +
    (float)ay * ay +
    (float)az * az
  );

  if (firstMPURead) {
    lastAx = ax;
    lastAy = ay;
    lastAz = az;

    calibrateVerticalAxis();

    firstMPURead = false;
    lastMovementTime = now;

    Serial.println("MPU6050 first reading captured");
    return 0;
  }

  float movementChange = sqrt(
    (float)(ax - lastAx) * (ax - lastAx) +
    (float)(ay - lastAy) * (ay - lastAy) +
    (float)(az - lastAz) * (az - lastAz)
  );

  lastAx = ax;
  lastAy = ay;
  lastAz = az;

  float verticalAccel = getVerticalAccelInGravityDirection();
  float verticalDropSignal = baselineVerticalAccel - verticalAccel;

  bool freeFallDetected = accelMagnitude < FREE_FALL_THRESHOLD;

  // Only downward motion can start a fall candidate.
  // Side movement is mostly rejected because the calibrated gravity-axis value
  // must reduce clearly before candidate tracking starts.
  bool downwardStartDetected =
    verticalDropSignal > DOWNWARD_START_DROP &&
    accelMagnitude < DOWNWARD_TOTAL_ACCEL_MAX &&
    movementChange > DOWNWARD_MOVEMENT_THRESHOLD;

  bool impactDetected = accelMagnitude > IMPACT_THRESHOLD;

  // =====================================================
  // FINAL FALL LOGIC - STRICT 1m DOWNWARD APPROXIMATION
  //
  // User requirement implemented:
  // - Fast movement below about 1m should NOT be detected as fall.
  // - Fast downward movement about 1m or more from current position
  //   should be detected as fall only after a clear impact/stop.
  //
  // MPU6050 cannot directly measure exact distance without extra hardware.
  // Therefore, this code estimates downward travel by integrating the
  // downward acceleration along the calibrated gravity axis.
  // =====================================================

  if (!fallCandidate && now - lastFallTime > FALL_COOLDOWN) {
    // Path 1: real/near-real free fall.
    if (freeFallDetected) {
      if (freeFallStartTime == 0) {
        freeFallStartTime = now;
      }

      if (now - freeFallStartTime >= FREE_FALL_MIN_TIME) {
        fallCandidate = true;
        freeFallSeen = true;
        downwardMotionSeen = false;
        impactSeen = false;
        impactTime = 0;
        downwardDurationAtImpact = 0;

        // Start counting from first low-g moment.
        fallCandidateStart = freeFallStartTime;
        lastFallSampleTime = now;
        lastValidDownwardMotionTime = now;
        validDownwardSamples = 1;

        // Include the low-g time already seen before candidate confirmation.
        float initialFreeFallTimeSec = (now - freeFallStartTime) / 1000.0;
        estimatedDownwardVelocityMps = 9.81 * initialFreeFallTimeSec;
        estimatedDownwardDistanceM = 0.5 * 9.81 * initialFreeFallTimeSec * initialFreeFallTimeSec;
        maxEstimatedDownwardDistanceM = estimatedDownwardDistanceM;
        maxMovementDuringFall = movementChange;

        Serial.print("Free-fall candidate started | AccelMag: ");
        Serial.print(accelMagnitude);
        Serial.print(" | Move: ");
        Serial.println(movementChange);
      }
    } else {
      freeFallStartTime = 0;
    }

    // Path 2: hand-carried downward movement.
    // It only starts tracking. It will NOT confirm until the estimated
    // distance reaches REQUIRED_DOWNWARD_DISTANCE_M and impact is detected.
    if (!fallCandidate && downwardStartDetected) {
      fallCandidate = true;
      freeFallSeen = false;
      downwardMotionSeen = true;
      impactSeen = false;
      impactTime = 0;
      downwardDurationAtImpact = 0;

      downwardMotionStartTime = now;
      fallCandidateStart = now;
      lastFallSampleTime = now;
      lastValidDownwardMotionTime = now;
      validDownwardSamples = 1;
      estimatedDownwardVelocityMps = 0.0;
      estimatedDownwardDistanceM = 0.0;
      maxEstimatedDownwardDistanceM = 0.0;
      maxMovementDuringFall = movementChange;

      Serial.print("Downward tracking started | AccelMag: ");
      Serial.print(accelMagnitude);
      Serial.print(" | VerticalDrop: ");
      Serial.print(verticalDropSignal);
      Serial.print(" | Move: ");
      Serial.println(movementChange);
    }
  }

  if (fallCandidate) {
    // Detect real downward movement samples before impact.
    // This filters shake-only movement: shake can be impact only if
    // it happens after recent downward motion.
    bool validDownwardNow =
      verticalDropSignal > DOWNWARD_CONTINUE_DROP &&
      movementChange > DOWNWARD_CONTINUE_MOVE &&
      accelMagnitude < IMPACT_THRESHOLD;

    if (downwardMotionSeen && !freeFallSeen && validDownwardNow) {
      lastValidDownwardMotionTime = now;
      validDownwardSamples++;
    }

    if (movementChange > maxMovementDuringFall) {
      maxMovementDuringFall = movementChange;
    }

    // Estimate downward travel distance.
    // Positive verticalDropSignal means acceleration is in the downward direction
    // compared with the calibrated resting gravity value.
    if (lastFallSampleTime > 0) {
      float dt = (now - lastFallSampleTime) / 1000.0;

      // Protect against abnormal time gaps.
      if (dt < 0.0) dt = 0.0;
      if (dt > 0.08) dt = 0.08;

      bool integrateDownwardDistance = freeFallDetected || freeFallSeen ||
                                      (downwardMotionSeen && validDownwardNow);

      if (integrateDownwardDistance) {
        float downwardAccelMps2 = 0.0;

        if (freeFallDetected || freeFallSeen) {
          // Near free-fall gives close to g downward acceleration.
          downwardAccelMps2 = 9.81;
        } else {
          downwardAccelMps2 = (verticalDropSignal / baselineVerticalAccel) * 9.81;
        }

        if (downwardAccelMps2 < DOWNWARD_ACCEL_NOISE_MPS2) {
          downwardAccelMps2 = 0.0;
        }

        estimatedDownwardVelocityMps += downwardAccelMps2 * dt;

        if (estimatedDownwardVelocityMps < 0.0) {
          estimatedDownwardVelocityMps = 0.0;
        }

        estimatedDownwardDistanceM += estimatedDownwardVelocityMps * dt * DOWNWARD_DISTANCE_GAIN;

        if (estimatedDownwardDistanceM > maxEstimatedDownwardDistanceM) {
          maxEstimatedDownwardDistanceM = estimatedDownwardDistanceM;
        }
      } else {
        // If the movement is only shake and not downward, do not keep adding distance.
        // Slowly reduce stored velocity so continuous shaking cannot become fake 1m travel.
        estimatedDownwardVelocityMps *= 0.70;
      }
    }

    lastFallSampleTime = now;

    if (impactDetected && !impactSeen) {
      impactSeen = true;
      impactTime = now;
      downwardDurationAtImpact = now - fallCandidateStart;

      Serial.print("Impact detected | AccelMag: ");
      Serial.print(accelMagnitude);
      Serial.print(" | Time: ");
      Serial.print(downwardDurationAtImpact);
      Serial.print(" ms | EstDist: ");
      Serial.print(maxEstimatedDownwardDistanceM, 2);
      Serial.println(" m");

      bool enoughTime = downwardDurationAtImpact >= MIN_DOWNWARD_TRAVEL_TIME;
      bool enoughDistance = maxEstimatedDownwardDistanceM >= REQUIRED_DOWNWARD_DISTANCE_M;

      bool impactAfterRecentDownward =
        freeFallSeen ||
        (lastValidDownwardMotionTime > 0 &&
         now - lastValidDownwardMotionTime <= IMPACT_AFTER_DOWNWARD_GRACE_TIME);

      bool enoughDownwardSamples =
        freeFallSeen || validDownwardSamples >= MIN_VALID_DOWNWARD_SAMPLES;

      if (!enoughTime || !enoughDistance || !impactAfterRecentDownward || !enoughDownwardSamples) {
        Serial.print("Ignored: below 1m-like downward fall / shake-only impact | TimeOK=");
        Serial.print(enoughTime ? "YES" : "NO");
        Serial.print(" | DistOK=");
        Serial.print(enoughDistance ? "YES" : "NO");
        Serial.print(" | RecentDownward=");
        Serial.print(impactAfterRecentDownward ? "YES" : "NO");
        Serial.print(" | DownSamples=");
        Serial.println(validDownwardSamples);

        resetFallCandidate();
        return 0;
      }
    }

    bool impactConfirmed =
      impactSeen &&
      impactTime != 0 &&
      now - impactTime > POST_FALL_CONFIRM_TIME;

    if (impactConfirmed) {
      float finalDistance = maxEstimatedDownwardDistanceM;
      float finalMaxMove = maxMovementDuringFall;

      resetFallCandidate();
      lastFallTime = now;

      Serial.print("1m DOWNWARD FALL CONFIRMED | EstDist: ");
      Serial.print(finalDistance, 2);
      Serial.print(" m | MaxMove: ");
      Serial.println(finalMaxMove);

      return 1;
    }

    if (now - fallCandidateStart > POST_FALL_CHECK_WINDOW) {
      Serial.print("Movement ignored - no valid 1m downward impact | EstDist: ");
      Serial.print(maxEstimatedDownwardDistanceM, 2);
      Serial.println(" m");

      resetFallCandidate();
    }
  }

  // Inactivity detection
  // Slow movement resets inactivity because MOVEMENT_THRESHOLD is 1000.
  if (movementChange > MOVEMENT_THRESHOLD) {
    lastMovementTime = now;
  }

  // Sleep mode disables only inactivity.
  // Fall detection still works in sleep mode.
  if (!sleepModeNow) {
    if (now - lastMovementTime > INACTIVITY_LIMIT) {
      Serial.println("Inactivity detected");
      lastMovementTime = now;
      return 2;
    }
  }

  return 0;
}

// =====================================================
// WARNING / EMERGENCY FUNCTIONS
// =====================================================

void startWarning(String reason) {
  warningActive = true;
  emergencyActive = false;

  warningReason = reason;
  warningStartTime = millis();
  lastCountdownNumber = -1;

  setLEDs(false, false, false);
  buzzerStop();

  Serial.print("Warning started: ");
  Serial.println(warningReason);

  sendAppAlert("WARNING:" + warningReason);
}

void updateWarningCountdown() {
  unsigned long elapsed = millis() - warningStartTime;

  if (elapsed >= WARNING_TIME) {
    confirmEmergency();
    return;
  }

  int remainingSeconds = (WARNING_TIME - elapsed + 999) / 1000;

  if (remainingSeconds != lastCountdownNumber) {
    lastCountdownNumber = remainingSeconds;

    Serial.print("Warning: ");
    Serial.print(warningReason);
    Serial.print(" | Emergency in ");
    Serial.print(remainingSeconds);
    Serial.println(" seconds");

    showWarningScreen(warningReason, remainingSeconds);
  }

  // Red panic LED blink
  if ((millis() / 250) % 2 == 0) {
    setLEDs(true, false, false);
  } else {
    setLEDs(false, false, false);
  }

  // Warning buzzer beep-beep
  if ((millis() / 300) % 2 == 0) {
    buzzerStart();
  } else {
    buzzerStop();
  }
}

void cancelWarning() {
  String cancelledReason = warningReason;

  warningActive = false;
  emergencyActive = false;

  setLEDs(false, false, false);
  buzzerStop();

  lastMovementTime = millis();

  Serial.println("Warning cancelled");

  sendAppAlert("CANCELLED:" + cancelledReason);

  updateNormalLEDs();

  if (sleepMode) {
    showStatusScreen("SLEEP", "Warning cancelled", "Inactivity OFF");
  } else {
    showStatusScreen("NORMAL", "Warning cancelled", "Monitoring...");
  }
}

void confirmEmergency() {
  warningActive = false;
  emergencyActive = true;

  setLEDs(true, false, false);

  Serial.println("EMERGENCY CONFIRMED");

  sendAppAlert("EMERGENCY:" + warningReason);

  showEmergencyScreen();
}

void updateEmergencyAlarm() {
  setLEDs(true, false, false);
  buzzerEmergencyLoud();
}

void stopEmergency() {
  emergencyActive = false;
  warningActive = false;

  setLEDs(false, false, false);
  buzzerStop();

  lastMovementTime = millis();

  Serial.println("Emergency stopped");

  sendAppAlert("STOPPED:EMERGENCY");

  updateNormalLEDs();

  if (sleepMode) {
    showStatusScreen("SLEEP", "Emergency stopped", "Inactivity OFF");
  } else {
    showStatusScreen("NORMAL", "Emergency stopped", "Monitoring...");
  }
}

// =====================================================
// MODE FUNCTIONS
// =====================================================

void toggleSleepMode() {
  sleepMode = !sleepMode;

  if (sleepMode) {
    Serial.println("Sleep Mode ON");
    sendAppAlert("SLEEP_MODE_ON");
    showStatusScreen("SLEEP", "Sleep Mode ON", "Inactivity OFF");
  } else {
    Serial.println("Sleep Mode OFF");
    sendAppAlert("SLEEP_MODE_OFF");
    lastMovementTime = millis();
    showStatusScreen("NORMAL", "Sleep Mode OFF", "Monitoring...");
  }

  updateNormalLEDs();
}

// =====================================================
// BUTTON FUNCTIONS
// =====================================================

int readPanicButtonEvent() {
  bool reading = digitalRead(PANIC_BUTTON_PIN);

  if (reading != lastPanicReading) {
    lastPanicDebounceTime = millis();
  }

  if ((millis() - lastPanicDebounceTime) > DEBOUNCE_TIME) {
    if (reading != stablePanicState) {
      stablePanicState = reading;

      if (stablePanicState == LOW) {
        panicPressStartTime = millis();
        panicLongPressSent = false;
        lastPanicReading = reading;
        return PANIC_SHORT_PRESS;
      }

      if (stablePanicState == HIGH) {
        panicLongPressSent = false;
      }
    }
  }

  if (stablePanicState == LOW && !panicLongPressSent) {
    if (millis() - panicPressStartTime >= PANIC_LONG_PRESS_TIME) {
      panicLongPressSent = true;
      lastPanicReading = reading;
      return PANIC_LONG_PRESS;
    }
  }

  lastPanicReading = reading;
  return PANIC_NONE;
}

bool readSleepCancelButtonPress() {
  bool reading = digitalRead(SLEEP_CANCEL_BUTTON_PIN);

  if (reading != lastSleepReading) {
    lastSleepDebounceTime = millis();
  }

  if ((millis() - lastSleepDebounceTime) > DEBOUNCE_TIME) {
    if (reading != stableSleepState) {
      stableSleepState = reading;

      if (stableSleepState == LOW) {
        lastSleepReading = reading;
        return true;
      }
    }
  }

  lastSleepReading = reading;
  return false;
}
