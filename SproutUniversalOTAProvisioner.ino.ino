#include <ArduinoBLE.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <Arduino_Portenta_OTA.h>

#include <Ethernet.h>

#if defined(ESP32)
  #include <LittleFS.h>          // ESP32's own LittleFS library — different API entirely
  #define CONFIG_FILE_PATH "/config.json"
  #define CONFIG_TEMP_PATH "/config.json.tmp"
#else
  // Opta / Mbed path — unchanged from the original sketch
  #include "BlockDevice.h"
  #include "MBRBlockDevice.h"
  #include "LittleFileSystem.h"
  #define CONFIG_FILE_PATH "/user/config.json"
  #define CONFIG_TEMP_PATH "/user/config.json.tmp"
#endif
// =============================================================================
// Entgen Controller v1.0.0 — BLE Provisioning + Enrollment + OTA Firmware
//
// Services:
//   fff0 — Entgen provisioning service
//
// Characteristics:
//   fff1 — Device info (read)
//   fff2 — Provisioning data (write)
//   fff3 — Status (notify)
//
// Status values:
//   0 — idle
//   1 — received
//   2 — connecting
//   3 — success
//   4 — failed
//
// MQTT topics subscribed after provisioning:
//   entgen/{device_id}/enroll        — full config payload from phone
//   entgen/{device_id}/ota/trigger   — OTA download + flash trigger
// =============================================================================

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------
BLEService provisioningService("fff0");

BLECharacteristic deviceInfoChar(
  "fff1",
  BLERead,
  47
);

BLECharacteristic provisionDataChar(
  "fff2",
  BLEWrite | BLEWriteWithoutResponse,
  512
);

BLECharacteristic statusChar(
  "fff3",
  BLERead | BLENotify | BLEIndicate,
  20
);

// ---------------------------------------------------------------------------
// WiFi + MQTT
// ---------------------------------------------------------------------------
WiFiClient    wifiClient;
MqttClient    mqttClient(wifiClient);

// ---------------------------------------------------------------------------
// Status constants
// ---------------------------------------------------------------------------
#define STATUS_IDLE        0
#define STATUS_RECEIVED    1
#define STATUS_CONNECTING  2
#define STATUS_SUCCESS     3
#define STATUS_FAILED      4

#define PROVISION_BUFFER_SIZE 8192
static char   provisionBuffer[PROVISION_BUFFER_SIZE];
static size_t provisionBufferLen = 0;
bool   provisionComplete = false;
// Device info genericization — near top of file:
#define DEVICE_ID_PREFIX "SPROUT"
#define FIRMWARE_VERSION "1.0.0"

// In setup(), replace the hardcoded string:
String deviceInfo = "{\"device_id\":\"" + String(DEVICE_ID_PREFIX) + "_0001\","
                    "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\"}";
// ---------------------------------------------------------------------------
// Provisioning state
// ---------------------------------------------------------------------------


// Global credentials — set during provisioning
String g_deviceId   = "";
String g_brokerHost = "";
int    g_brokerPort = 1883; // default to open debug port
String g_wifiSsid   = "ORBI74";
String g_wifiPass   = "huskymesa929";
String g_mqttUsername = "";
String g_mqttPassword = "";
String g_clientCert = "";
String g_clientKey = "";
String g_caCert = "";


// ---------------------------------------------------------------------------
// LittleFS — partition 4, mounted at /user
// ---------------------------------------------------------------------------
mbed::BlockDevice*     root = mbed::BlockDevice::get_default_instance();
mbed::MBRBlockDevice   userPartition(root, 4);
mbed::LittleFileSystem userFs("user");
bool                   fsMounted = false;

// ---------------------------------------------------------------------------
// BLE event handlers — status char subscribe/unsubscribe
// ---------------------------------------------------------------------------
void onStatusSubscribed(BLEDevice central, BLECharacteristic characteristic) {
  Serial.println("Phone subscribed to status notifications");
}

void onStatusUnsubscribed(BLEDevice central, BLECharacteristic characteristic) {
  Serial.println("Phone unsubscribed from status notifications");
}

// ---------------------------------------------------------------------------
// BLE advertising data
// ---------------------------------------------------------------------------
const uint8_t completeRawAdvertisingData[] = {
  0x02, 0x01, 0x06,
  0x09, 0xff, 0x01, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  pinMode(LED_D0, OUTPUT);
  pinMode(LED_D1, OUTPUT);
  pinMode(LED_D2, OUTPUT);
  pinMode(LED_D3, OUTPUT);

  Serial.begin(9600);
  while (!Serial);

  Serial.println("Entgen Controller v1.0.0 starting...");

  // ── Mount LittleFS partition 4 ──────────────────────────────────────────
  if (root->init() != 0) {
    Serial.println("QSPI init failed — was QSPIFormat run?");
  } else if (userFs.mount(&userPartition) != 0) {
    Serial.println("LittleFS mount failed — was QSPIFormat run with LittleFS?");
  } else {
    fsMounted = true;
    Serial.println("LittleFS mounted at /user");
  }

  if (!BLE.begin()) {
    Serial.println("BLE init failed");
    while (1);
  }

  // Device info characteristic
  String deviceInfo = "{\"device_id\":\"OPTA_0001\",\"firmware\":\"1.0.0\"}";
  deviceInfoChar.writeValue(
    (const uint8_t*)deviceInfo.c_str(),
    deviceInfo.length()
  );

  provisioningService.addCharacteristic(deviceInfoChar);
  provisioningService.addCharacteristic(provisionDataChar);
  provisioningService.addCharacteristic(statusChar);

  BLE.addService(provisioningService);

  uint8_t idleStatus = STATUS_IDLE;
  statusChar.writeValue(idleStatus);

  BLE.setEventHandler(BLEConnected,    onBLEConnected);
  BLE.setEventHandler(BLEDisconnected, onBLEDisconnected);
  statusChar.setEventHandler(BLESubscribed,   onStatusSubscribed);
  statusChar.setEventHandler(BLEUnsubscribed, onStatusUnsubscribed);
  provisionDataChar.setEventHandler(BLEWritten, onProvisionDataReceived);

  BLEAdvertisingData advData;
  advData.setRawData(
    completeRawAdvertisingData,
    sizeof(completeRawAdvertisingData)
  );
  BLE.setAdvertisingData(advData);

  BLEAdvertisingData scanData;
  scanData.setLocalName("EntGen Controller v1.0.0");
  BLE.setScanResponseData(scanData);

  BLE.advertise();
  Serial.println("Advertising — waiting for provisioning...");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  BLE.poll();

  if (provisionComplete) {
    provisionComplete = false;
    processProvisioningData(String(provisionBuffer));
    provisionBufferLen = 0;
  }

  if (mqttClient.connected()) {
    mqttClient.poll();
  }

  // Non-blocking heartbeat — replaces the old delay()-based version.
  // BLE.poll() now runs on every single loop iteration, not once every
  // 400ms+, so incoming chunks are drained as fast as they arrive.
  static unsigned long lastBlink = 0;
  static bool blinkState = false;
  if (millis() - lastBlink >= 500) {
    lastBlink = millis();
    blinkState = !blinkState;

    digitalWrite(LED_D3, blinkState);
  }
}

// ---------------------------------------------------------------------------
// BLE event handlers
// ---------------------------------------------------------------------------
void onBLEConnected(BLEDevice central) {
  Serial.print("BLE connected: ");
  Serial.println(central.address());
  digitalWrite(LED_D1, HIGH);
}

void onBLEDisconnected(BLEDevice central) {
  Serial.print("BLE disconnected: ");
  Serial.println(central.address());
  digitalWrite(LED_D1, LOW);
  BLE.advertise();
}

void onProvisionDataReceived(BLEDevice central, BLECharacteristic characteristic) {
  int            len  = characteristic.valueLength();
  const uint8_t* data = characteristic.value();

  // NOTHING GOES HERE — the old for-loop is deleted entirely, not modified.
  // memcpy below replaces it completely.

  if (provisionBufferLen + len >= PROVISION_BUFFER_SIZE) {
    Serial.println("[BLE] Provisioning payload exceeds buffer, rejecting.");
    provisionBufferLen = 0;
    return;
  }
  memcpy(provisionBuffer + provisionBufferLen, data, len);
  provisionBufferLen += len;
  provisionBuffer[provisionBufferLen] = '\0';

  Serial.print("Received chunk: ");
  Serial.print(len);
  Serial.println(" bytes");

  if (provisionBufferLen >= 3 &&
      strcmp(provisionBuffer + provisionBufferLen - 3, "END") == 0) {
    provisionBuffer[provisionBufferLen - 3] = '\0';
    provisionComplete = true;
    Serial.println("All chunks received");
    uint8_t receivedStatus = STATUS_RECEIVED;
    statusChar.writeValue(receivedStatus);
  }
}
// ADDED — new function, placed anywhere before processProvisioningData()
bool saveCertsToFlash(const String& deviceId, const String& clientCert, const String& clientKey, const String& caCert) {
  if (!fsMounted) {
    Serial.println("LittleFS not mounted — cannot save certs");
    return false;
  }

  DynamicJsonDocument doc(6144);
  doc["device_id"]   = deviceId;   // ADD — otherwise never persisted to config.json at all
  doc["client_cert"] = clientCert;
  doc["client_key"]  = clientKey;
  doc["ca_cert"]     = caCert;
  String out;
  serializeJson(doc, out);

  FILE* f = fopen("/user/config.json", "w");
  if (f == nullptr) {
    Serial.println("Failed to open /user/config.json for cert write");
    return false;
  }
  size_t written = fwrite(out.c_str(), 1, out.length(), f);
  fclose(f);

  if (written != out.length()) {
    Serial.println("Warning: partial write of cert seed file");
    return false;
  }
  Serial.println("Certs persisted to config.json (seed file, pre-enrollment).");
  return true;
}
// ---------------------------------------------------------------------------
// Process provisioning payload — WiFi + broker connect + subscriptions
// ---------------------------------------------------------------------------
void processProvisioningData(String jsonData) {
  Serial.println("Processing provisioning data...");
  Serial.print("Payload size: ");
  Serial.print(jsonData.length());
  Serial.println(" bytes");

  Serial.println("── RAW buffer before parse ──");
  Serial.println(jsonData);
  Serial.println("──────────────────────────────");

  uint8_t connectingStatus = STATUS_CONNECTING;
  statusChar.writeValue(connectingStatus);

  DynamicJsonDocument doc(8192);


  DeserializationError error = deserializeJson(doc, jsonData);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    uint8_t failedStatus = STATUS_FAILED;
    statusChar.writeValue(failedStatus);
    return;
  }

  // Extract credentials
  g_deviceId   = doc["device_id"].as<String>();
  g_brokerHost = doc["broker_ip"].as<String>();
  g_brokerPort = doc["broker_port"] | 1883;
  g_wifiSsid   = doc["wifi_ssid"].as<String>();
  g_wifiPass   = doc["wifi_pass"].as<String>();
  g_mqttUsername = doc["mqtt_username"].as<String>();
  g_mqttPassword = doc["mqtt_pass"].as<String>();

  g_clientCert = doc["client_cert"].as<String>();
  g_clientKey = doc["client_key"].as<String>();
  g_caCert = doc["ca_cert"].as<String>();

  Serial.println("── Provisioning credentials ─────────────");
  Serial.print("Device ID:   "); Serial.println(g_deviceId);
  Serial.print("Broker:      "); Serial.print(g_brokerHost);
  Serial.print(":"); Serial.println(g_brokerPort);
  Serial.print("WiFi SSID:   "); Serial.println(g_wifiSsid);
  Serial.println("Certificates parsed - will be persisted to flash before test connect.");

  Serial.println("─────────────────────────────────────────");
  // In processProvisioningData(), call it right after the parse block above:
  saveCertsToFlash(g_deviceId, g_clientCert, g_clientKey, g_caCert);

// TEMPORARY — confirm actual file contents, not just that the write claimed success
FILE* verifyF = fopen("/user/config.json", "r");
if (verifyF != nullptr) {
  char verifyBuf[4096];
  size_t n = fread(verifyBuf, 1, sizeof(verifyBuf) - 1, verifyF);
  fclose(verifyF);
  verifyBuf[n] = '\0';
  Serial.println("── config.json contents on disk ──");
  Serial.println(verifyBuf);
  Serial.println("───────────────────────────────────");
}
  // ── Step 1: Connect to WiFi ──────────────────────────────────────────────
  Serial.print("Connecting to WiFi: ");
  Serial.println(g_wifiSsid);

  WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());

  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 60) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed");
    uint8_t failedStatus = STATUS_FAILED;
    statusChar.writeValue(failedStatus);
    return;
  }

  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  

  // ── Step 2: Connect to MQTT broker on port 1883 (debug) ─────────────────
  Serial.print("Connecting to MQTT broker: ");
  Serial.print(g_brokerHost);
  Serial.print(":");
  Serial.println(g_brokerPort);
// Was: mqttClient.setUsernamePassword("mozzy", "l1gmagett1");
  mqttClient.setUsernamePassword(g_mqttUsername.c_str(), g_mqttPassword.c_str());
  mqttClient.setId(g_deviceId.c_str());

  // Register message callback BEFORE connecting so nothing is missed
  mqttClient.onMessage(onMqttMessage);

  if (!mqttClient.connect(g_brokerHost.c_str(), g_brokerPort)) {
    Serial.print("MQTT connection failed, error: ");
    Serial.println(mqttClient.connectError());
    uint8_t failedStatus = STATUS_FAILED;
    statusChar.writeValue(failedStatus);
    return;
  }

  Serial.println("MQTT broker connected");

  // ── Step 3: Subscribe to enrollment + OTA trigger topics ────────────────
  String enrollTopic = "entgen/" + g_deviceId + "/enroll";
  String otaTopic     = "entgen/" + g_deviceId + "/ota/trigger";

  mqttClient.subscribe(enrollTopic.c_str());
  mqttClient.subscribe(otaTopic.c_str());

  Serial.print("Subscribed to: "); Serial.println(enrollTopic);
  Serial.print("Subscribed to: "); Serial.println(otaTopic);

  mqttClient.beginMessage(("entgen/" + g_deviceId + "/provisioning_test").c_str());
mqttClient.print("ok");
mqttClient.endMessage();

  Serial.println("Provisioning test message published.");
  // ── Step 5: Notify Flutter app of success ───────────────────────────────
  uint8_t successStatus = STATUS_SUCCESS;
  statusChar.writeValue(successStatus);

  Serial.println("Provisioning complete — awaiting enrollment + OTA trigger");
  digitalWrite(LED_D2, HIGH);
}

// ---------------------------------------------------------------------------
// MQTT message router — enrollment save + OTA trigger
// ---------------------------------------------------------------------------
void onMqttMessage(int messageSize) {
  String topic = mqttClient.messageTopic();

  String payload = "";
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  Serial.print("MQTT message on: ");
  Serial.println(topic);

  if (topic.endsWith("/enroll")) {
    handleEnrollment(payload);
  } else if (topic.endsWith("/ota/trigger")) {
    handleOtaTrigger(payload);
  }
}

// ---------------------------------------------------------------------------
// Enrollment handler — write full config payload to LittleFS
// ---------------------------------------------------------------------------
void handleEnrollment(String jsonPayload) {
  Serial.println("Enrollment payload received");

  if (!fsMounted) {
    Serial.println("LittleFS not mounted — cannot save config");
    return;
  }

  // ADDED — read whatever's already on flash (certs from BLE
  // provisioning) BEFORE overwriting, so they aren't silently lost.
  DynamicJsonDocument existingDoc(8192);
  FILE* readF = fopen("/user/config.json", "r");
  if (readF != nullptr) {
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, readF);
    fclose(readF);
    buf[n] = '\0';
    deserializeJson(existingDoc, buf); // best-effort
  }

  DynamicJsonDocument newDoc(16384);
  DeserializationError err = deserializeJson(newDoc, jsonPayload);
  if (err) {
    Serial.print("Enrollment JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  // ADDED — carry certs forward since toEnrollmentJson() never includes them
  if (!existingDoc["device_id"].isNull())   newDoc["device_id"]   = existingDoc["device_id"];
  if (!existingDoc["client_cert"].isNull()) newDoc["client_cert"] = existingDoc["client_cert"];
  if (!existingDoc["client_key"].isNull())  newDoc["client_key"]  = existingDoc["client_key"];
  if (!existingDoc["ca_cert"].isNull())     newDoc["ca_cert"]     = existingDoc["ca_cert"];

  String merged;
  serializeJson(newDoc, merged);

  FILE* f = fopen("/user/config.json", "w");
  if (f == nullptr) {
    Serial.println("Failed to open /user/config.json for writing");
    return;
  }
  size_t written = fwrite(merged.c_str(), 1, merged.length(), f);
  fclose(f);

  if (written != merged.length()) {
    Serial.println("Warning: partial write to config.json");
  } else {
    Serial.println("Config saved to /user/config.json (certs preserved).");
  }
}

// ---------------------------------------------------------------------------
// OTA trigger handler — download, decompress, flash, reboot
// ---------------------------------------------------------------------------
void handleOtaTrigger(String jsonPayload) {
  Serial.println("OTA trigger received");
    {
    String otaStatusTopic = "entgen/" + g_deviceId + "/ota/status";
    StaticJsonDocument<128> statusDoc;
    statusDoc["status"]    = "flashing";
    statusDoc["device_id"] = g_deviceId;
    String statusPayload;
    serializeJson(statusDoc, statusPayload);

    mqttClient.beginMessage(otaStatusTopic.c_str(), true /* retain */);
    mqttClient.print(statusPayload);
    mqttClient.endMessage();
    Serial.println("Published OTA flashing status (retained)");
  }


  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, jsonPayload);

  if (error) {
    Serial.print("OTA trigger JSON parse error: ");
    Serial.println(error.c_str());
    return;
  }

  const char* url = doc["url"];
  if (url == nullptr) {
    Serial.println("OTA trigger missing url field");
    return;
  }

  Serial.print("Downloading OTA from: ");
  Serial.println(url);

  Arduino_Portenta_OTA_QSPI ota(QSPI_FLASH_FATFS_MBR, 2);

  if (!ota.isOtaCapable()) {
    Serial.println("Device is not OTA capable — check memory partitioning");
    return;
  }

  Arduino_Portenta_OTA::Error ota_err = Arduino_Portenta_OTA::Error::None;

  if ((ota_err = ota.begin()) != Arduino_Portenta_OTA::Error::None) {
    Serial.print("OTA begin failed, error: ");
    Serial.println((int)ota_err);
    return;
  }

  int downloaded = ota.download(url, false /* is_https */);
  if (downloaded <= 0) {
    Serial.println("OTA download failed");
    return;
  }

  Serial.println("Download complete — decompressing...");

  int decompressed = ota.decompress();
  if (decompressed < 0) {
    Serial.println("OTA decompress failed");
    return;
  }

  Serial.println("Decompress complete — applying update...");

  if ((ota_err = ota.update()) != Arduino_Portenta_OTA::Error::None) {
    Serial.print("OTA update failed, error: ");
    Serial.println((int)ota_err);
    return;
  }

  Serial.println("Update applied — rebooting into new firmware...");
  delay(500); // let serial flush
  ota.reset();
}