#include <ArduinoBLE.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <Arduino_Portenta_OTA.h>
#include "BlockDevice.h"
#include "MBRBlockDevice.h"
#include "LittleFileSystem.h"
#include <Ethernet.h>
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

// ---------------------------------------------------------------------------
// Provisioning state
// ---------------------------------------------------------------------------
String provisionBuffer   = "";
bool   provisionComplete = false;

// Global credentials — set during provisioning
String g_deviceId   = "";
String g_brokerHost = "";
int    g_brokerPort = 1883; // default to open debug port
String g_wifiSsid   = "ORBI74";
String g_wifiPass   = "huskymesa929";
String g_mqttUsername = "";
String g_mqttPassword = "";

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

  // Process provisioning data once fully received
  if (provisionComplete) {
    provisionComplete = false;
    processProvisioningData(provisionBuffer);
    provisionBuffer = "";
  }

  // Keep MQTT alive if connected — poll also delivers incoming messages
  if (mqttClient.connected()) {
    mqttClient.poll();
  }

  // Heartbeat LED
  digitalWrite(LED_D0, HIGH); delay(100);
  digitalWrite(LED_D0, LOW);  delay(100);
  digitalWrite(LED_D3, HIGH); delay(100);
  digitalWrite(LED_D3, LOW);  delay(100);
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

void onProvisionDataReceived(
  BLEDevice central,
  BLECharacteristic characteristic
) {
  int            len  = characteristic.valueLength();
  const uint8_t* data = characteristic.value();

  for (int i = 0; i < len; i++) {
    provisionBuffer += (char)data[i];
  }

  Serial.print("Received chunk: ");
  Serial.print(len);
  Serial.println(" bytes");

  // Check for END marker
  if (provisionBuffer.endsWith("END")) {
    provisionBuffer = provisionBuffer.substring(
      0,
      provisionBuffer.length() - 3
    );
    provisionComplete = true;
    Serial.println("All chunks received");

    uint8_t receivedStatus = STATUS_RECEIVED;
    statusChar.writeValue(receivedStatus);
  }
}

// ---------------------------------------------------------------------------
// Process provisioning payload — WiFi + broker connect + subscriptions
// ---------------------------------------------------------------------------
void processProvisioningData(String jsonData) {
  Serial.println("Processing provisioning data...");
  Serial.print("Payload size: ");
  Serial.print(jsonData.length());
  Serial.println(" bytes");

  uint8_t connectingStatus = STATUS_CONNECTING;
  statusChar.writeValue(connectingStatus);

  DynamicJsonDocument doc(512);
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

  Serial.println("── Provisioning credentials ─────────────");
  Serial.print("Device ID:   "); Serial.println(g_deviceId);
  Serial.print("Broker:      "); Serial.print(g_brokerHost);
  Serial.print(":"); Serial.println(g_brokerPort);
  Serial.print("WiFi SSID:   "); Serial.println(g_wifiSsid);
  Serial.println("Certificates received (not yet used for mTLS)");

  Serial.println("─────────────────────────────────────────");

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

  // ── Step 4: Publish test telemetry ──────────────────────────────────────
  String topic = "entgen/" + g_deviceId + "/telemetry";

  DynamicJsonDocument telDoc(512);
  telDoc["type"]           = "telemetry";
  telDoc["voltage_v"]      = 240.0;
  telDoc["current_a"]      = 18.5;
  telDoc["power_output_w"] = 4440;
  telDoc["power_input_w"]  = 4600;
  telDoc["power_factor"]   = 0.965;
  telDoc["frequency_hz"]   = 60.0;
  telDoc["temp_system_c"]  = 62.4;
  telDoc["temp_room_c"]    = 24.1;
  telDoc["gas_level_pct"]  = 87.0;
  telDoc["state"]          = "idle";
  telDoc["timestamp"]      = millis();

  JsonObject viCurve = telDoc.createNestedObject("vi_curve");
  JsonArray  viV     = viCurve.createNestedArray("voltage_samples");
  JsonArray  viI     = viCurve.createNestedArray("current_samples");
  viV.add(220.1); viV.add(225.3); viV.add(230.5); viV.add(235.2); viV.add(240.0);
  viI.add(10.2);  viI.add(12.5);  viI.add(15.1);  viI.add(17.3);  viI.add(18.5);

  JsonObject piCurve = telDoc.createNestedObject("pi_curve");
  JsonArray  piP     = piCurve.createNestedArray("power_samples");
  JsonArray  piI     = piCurve.createNestedArray("current_samples");
  piP.add(2040); piP.add(2550); piP.add(3100); piP.add(3550); piP.add(4440);
  piI.add(10.2); piI.add(12.5); piI.add(15.1); piI.add(17.3); piI.add(18.5);

  String telemetryJson;
  serializeJson(telDoc, telemetryJson);

  mqttClient.beginMessage(topic.c_str());
  mqttClient.print(telemetryJson);
  mqttClient.endMessage();

  Serial.print("Test telemetry published to: ");
  Serial.println(topic);

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

  FILE* f = fopen("/user/config.json", "w");
  if (f == nullptr) {
    Serial.println("Failed to open /user/config.json for writing");
    return;
  }

  size_t written = fwrite(
    jsonPayload.c_str(),
    1,
    jsonPayload.length(),
    f
  );
  fclose(f);

  if (written != jsonPayload.length()) {
    Serial.println("Warning: partial write to config.json");
  } else {
    Serial.println("Config saved to /user/config.json");
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