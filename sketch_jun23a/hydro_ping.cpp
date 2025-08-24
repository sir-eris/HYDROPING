//
//  hydro_ping.cpp
//  HydroPing
//
//  Created by Sir Eris on 6/23/25.
//

/* ---------- LIBRARIES ---------- */
#include <Wire.h>
#include <WiFi.h>
#include <Ticker.h>
#include <AsyncTCP.h>
#include <esp_sleep.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>


/* ---------- DEFINITIONS ---------- */
#define LIS3DH_ADDR 0x18
#define LIS3DH_INT1_PIN 2
#define LIS3DH_INT2_PIN 1
#define SDA_PIN 3
#define SCL_PIN 4
#define TOUCH_1 12


/* ---------- Object initializations ---------- */
AsyncWebServer server(80);
Ticker restartTicker;
Preferences prefs;
// pre-warm-up
String homeSSID, homePASS, userID, deviceToken;


/* ---------- Device modes and configurations ---------- */
bool deviceInitialized = false;
bool APModeActive = false;
unsigned long APStartMillis = 0;
// known at complie time
constexpr unsigned long long SETUP_TIMEOUT_MS = 10ULL * 60 * 1000; // 10 min setup mode
constexpr const char* HARDWARE_VERSION = "1.0";
constexpr const char* FIRMWARE_VERSION = "1.0";
constexpr const char* AP_SSID = "HydroPing-Wi-Fi";
constexpr const char* AP_PASS = "";


/* ---------- Persisit through deep sleep ---------- */
RTC_DATA_ATTR bool isDisconnected = false;
RTC_DATA_ATTR bool inSetupMode = false;
RTC_DATA_ATTR uint64_t deepSleepTimeOut = 12ULL * 60ULL * 60ULL * 1000000ULL;  // default: 12h


/* ---------- LIS3DH functions ---------- */
// I. writeRegister
// input (uint8_t, uint8_t): seletcted register, specific value
// output (void): use I2C to write a configuration to the chip at specific register
void writeRegister(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(LIS3DH_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// II. readRegister
// input (uint8_t): seletcted register
// output (void): use I2C to read a configuration from the chip at specific register
uint8_t readRegister(uint8_t reg) {
  Wire.beginTransmission(LIS3DH_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);  // Restart
  Wire.requestFrom(LIS3DH_ADDR, (uint8_t)1);
  return Wire.read();
}

// II. initLIS3DH
// input (): N/A
// output (void): initialize the chip
void initLIS3DH() {
  // writeRegister(0x20, 0x47); // CTRL_REG1: 50 Hz, XYZ enabled
  // writeRegister(0x21, 0x40); // CTRL_REG2: High-pass filter to click
  // writeRegister(0x22, 0x80); // CTRL_REG3: Click interrupt on INT1
  // writeRegister(0x23, 0x30); // CTRL_REG4: ±8g full scale for less sensitivity
  // writeRegister(0x24, 0x00); // CTRL_REG5: disable latching on INT2
  // writeRegister(0x32, 0x04); // CLICK_CFG: enable double tap on Z
  // writeRegister(0x33, 0x7F); // CLICK_THS: threshold
  // writeRegister(0x34, 0x10); // TIME_LIMIT: time between taps
  // writeRegister(0x35, 0x40); // TIME_LATENCY
  // writeRegister(0x36, 0xC0); // TIME_WINDOW

  writeRegister(0x20, 0x47);  // CTRL_REG1: 50Hz, XYZ enabled
  writeRegister(0x21, 0x10);  // CTRL_REG2: HPF disabled (simplify for now)
  writeRegister(0x22, 0x40);  // CTRL_REG3: route INT1 to INT1 pin
  writeRegister(0x23, 0x10);  // CTRL_REG4: ±4g scale, high resolution
  writeRegister(0x24, 0x08);  // CTRL_REG5: latch interrupt on INT1
  writeRegister(0x30, 0x2A);  // INT1_CFG: enable XH, YH, ZH
  writeRegister(0x32, 0x47);  // INT1_THS: threshold ~0.5g (adjust later)
  writeRegister(0x33, 0x05);  // INT1_DURATION: 1 count (20ms at 50Hz)
  (void)readRegister(0x31);
}


/* ---------- WiFi function ---------- */
// I. startAP
// input (): N/A
// output (void): activate setup mode, start async webserver, listen and process
// endpoints: /info, /connect
void startAP() {
  APModeActive = true;
  APStartMillis = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(1000);

  // respond with device hardware credentials
  server.on("/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(256);
    doc["deviceId"] = WiFi.softAPmacAddress();
    doc["hardwareVersion"] = HARDWARE_VERSION;
    doc["firmwareVersion"] = FIRMWARE_VERSION;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // receive and save STA wifi credentials
  server.on( "/connect", HTTP_POST,
    // onRequest: send initial response later in onBody
    [](AsyncWebServerRequest *request) {
      // We won't send response here because body not yet received
    },
    // onUpload (not used)
    NULL,
    // onBody
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      String body = String((char *)data).substring(0, len);
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, body);

      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      homeSSID = doc["ssid"].as<String>();
      homePASS = doc["password"].as<String>();
      userID = doc["userid"].as<String>();
      deviceToken = doc["devicetoken"].as<String>();

      if (homeSSID.isEmpty() || homePASS.isEmpty() || userID.isEmpty() || deviceToken.isEmpty()) {
        request->send(400, "application/json", "{\"error\":\"Missing complete credentials\"}");
        return;
      }

      prefs.begin("wifi", false);
      prefs.putString("ssid", homeSSID);
      prefs.putString("pass", homePASS);
      prefs.putString("userid", userID);
      prefs.putString("devicetoken", deviceToken);
      prefs.end();

      if (connectToWiFi()) {
        // reseting device mode
        isDisconnected = false;

        request->send(200, "application/json", "{\"message\":\"connected to wifi\"}");

        restartTicker.once(1, []() {
          deviceInitialized = true;
        });
        return;
      } else {
        request->send(400, "application/json", "{\"message\":\"connection failed try again\"}");
      }
      return;
    });

  server.begin();
}

// II. stopAP
// input (): N/A
// output (void): deactivate setup mode
void stopAP() {
  WiFi.softAPdisconnect(true);
  server.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(250);
  deviceInitialized = false,
  APModeActive = false;
  delay(500);
}

// III. connectToWiFi
// input (): N/A
// output (bool): try to connect to wifi and return success state
// NOTE: wifi mode set externally before calling this function
bool connectToWiFi() {
  prefs.begin("wifi", true);
  homeSSID = prefs.getString("ssid", "");
  homePASS = prefs.getString("pass", "");
  prefs.end();

  if (homeSSID.isEmpty() || homePASS.isEmpty()) return false;

  WiFi.begin(homeSSID.c_str(), homePASS.c_str());

  for (int i = 0; i < 20; ++i) {  // ≈10 s timeout
    if (WiFi.status() == WL_CONNECTED) {
      return true;
    }
    delay(500);
  }
  return false;
}


/* ---------- Backend data communication functions ---------- */
// I. readTouchAvg
// input (int): hardware touch pin number
// output (uint32_t): avg the capacitive level for some (samples) at some (5 microsecond) interval
uint32_t readTouchAvg(int pin, int samples = 8) {
  uint32_t total = 0;
  for (int i = 0; i < samples; i++, delay(5)) total += touchRead(pin);
  return total / samples;
}

// II. sendDataToDB
// input (String, unit32_t): hardware MacAddress, re
// output (void): send moisture value to backend, check and handle resposnse, call to perform any instructions in the response payload
void sendDataToDB(String macAddress, uint32_t moisture) {
  prefs.begin("wifi", true);
  deviceToken = prefs.getString("devicetoken", "");
  prefs.end();

  if (deviceToken.isEmpty()) {
    return;
  }

  HTTPClient http;
  http.begin("https://q15ur4emu9.execute-api.us-east-2.amazonaws.com/default/enterProbeReading");
  http.addHeader("Authorization", "Bearer " + deviceToken);
  http.addHeader("Content-Type", "application/json");
  String json = "{\"moisture\":" + String(moisture) + "}";
  int httpCode = http.POST(json);

  if (httpCode > 0) {
    String payload = http.getString();

    aggregareIntructions(payload);  // perform pre-define changes given as intructions in the response payload
  }

  http.end();
}

// III. aggregareIntructions
// input (String): API JSON response payload
// output (void): check payload, execute small snippets based on defined keys in the payload
void aggregareIntructions(String payload) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (!error) {
    // Check if the response contains specific key-action pairs
    if (doc.containsKey("deviceToken")) {  // deviceToken: renew JWT token
      String deviceToken = doc["deviceToken"];
      prefs.begin("wifi", false);
      prefs.putString("devicetoken", deviceToken);
      prefs.end();
    } else if (doc.containsKey("sleepTimeout")) {  // sleepTimeout: change deep sleep cycle timeout
      // expected in microseconds
      uint64_t newTimeout = doc["sleepTimeout"];

      if (newTimeout >= 60ULL * 60ULL * 1000000ULL && newTimeout <= 24ULL * 60ULL * 60ULL * 1000000ULL) {  // safety check: larger than 1 hrs, less than 24 hrs
        deepSleepTimeOut = newTimeout;
      }
    } else if (doc.containsKey("disconnected")) {  // disconnected: set device to disconnected and skip furthuer readings
      isDisconnected = true;
    } else if (doc.containsKey("deletedUser")) {  // deletedUser: the owner user of the device is deleted, set device to disconnected and skip furthuer readings
      isDisconnected = true;
    }
  }
}

// IV. scheduleNextSensorRead
// input (): N/A
// output (void): call readTouchAvg, set wifi mode to STA, connect to wifi, get hardware MacAddress, call sendDataToDB
void scheduleNextSensorRead() {
  if (isDisconnected) return;

  uint32_t moisture = readTouchAvg(TOUCH_1);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);
  if (connectToWiFi()) {
    String macAddress = WiFi.macAddress();
    sendDataToDB(macAddress, moisture);
  }
}


/* ---------- Deep sleep functions ---------- */
// I. scheduleNextSleep
// input (): N/A
// output (void): initiate deep sleep timer, allow interruption by specific hardware pins
void scheduleNextSleep() {
  esp_sleep_enable_ext0_wakeup((gpio_num_t)LIS3DH_INT1_PIN, 1);
  esp_sleep_enable_timer_wakeup(deepSleepTimeOut);
  esp_deep_sleep_start();
}


/* ---------- ESP32 STATIS FUNCTIONS ---------- */
// I. setup
// input (): N/A
// output (void): initialize hardware layers, communicate to backend & go back to sleep, allow setup mode loop cycle
void setup() {
  Serial.begin(115200);
  // while (!Serial) delay(10);
  // Serial.setDebugOutput(true);
  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN); // initialize I2C
  pinMode(LIS3DH_INT1_PIN, INPUT_PULLUP); // activate preferral
  delay(20);
  initLIS3DH(); // initialize preferral
  delay(20);

  // deep sleep interrupted, triggered by specififc pin
  bool wokeFromShake = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);
  
  // loop cycle allwed during setup mode
  if (wokeFromShake && !inSetupMode) {
    inSetupMode = true;
    startAP();
    delay(200);
    unsigned long startTime = millis();
    while (!deviceInitialized && millis() - startTime < SETUP_TIMEOUT_MS) {
      delay(100);
    }
    stopAP();
    inSetupMode = false;
    scheduleNextSleep();
  }

  // communicate to backend & go back to sleep
  scheduleNextSensorRead();
  scheduleNextSleep();
}

// II. loop
// input (): N/A
// output (void): N/A
void loop() {}
