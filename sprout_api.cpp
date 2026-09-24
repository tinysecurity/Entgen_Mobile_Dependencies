#include "sprout_api.h"

//-------------------------------
// ---- SPROUT LIFECYCLE------
//------------------------------
Sprout_API::Sprout_API(String devType): mqttClient(sslClient), fs("fs"){
  bd = BlockDevice::get_default_instance();
  deviceType = devType;
  _instance = this;

}

void Sprout_API::initSprout(){


    Serial.println("Program has begun.");

    // Check that the filesystem has been installed and partitioned
    // If the filesystem is not found, halt and direct the user to run the partitioning sketch.
   if (!partitionCheck()) {
      Serial.println("FATAL: Filesystem not found or improperly partitioned.");
      Serial.println("Halting -- run QSPIFormat from Examples folder to partition the filesystem.");
      while (1);
    }

    // Mount the real filesystem for actual use. 
    if (bd->init() != BD_ERROR_OK) {
      Serial.println("FATAL: Could not reinitialize QSPI flash after partition check.");
      while (1);
    }
    static MBRBlockDevice userDataPartition(bd, 4);
    if (fs.mount(&userDataPartition) != 0) {
      Serial.println("FATAL: Could not mount user data filesystem.");
      while (1);
    }

    // Load the configuration file from memory
    if (!loadConfigFromFlash(registration)) {
      Serial.println("FATAL: No valid configuration found on flash.");
      Serial.println("This device may not have completed commissioning.");
      Serial.println("Halting -- recommission before retrying.");
      while (1);
    }
    Serial.print("[DEBUG] caCertPem length: "); Serial.println(strlen(registration.caCertPem));
    Serial.print("[DEBUG] clientCertPem length: "); Serial.println(strlen(registration.clientCertPem));
    Serial.print("[DEBUG] clientKeyPem length: "); Serial.println(strlen(registration.clientKeyPem));

    // Resolve the 11 base Sprout accessor indices by name -- must happen
    // after load succeeds, before subscribe/publish touch the topic tables.
    resolveSproutAccessorIndices();
    WiFi.config(registration.localIP_wifi, registration.dns, registration.gateway, registration.subnet);
    mqttClient.setTxPayloadSize(600);
}


bool Sprout_API::connectWiFi(){
    WiFi.disconnect();
    WiFi.end();
    delay(1000);
    Serial.print("Connecting to WiFi");
    WiFi.begin(registration.wifiSSID, registration.wifiPassword);

    int attempts = 0;
    const int MAX_ATTEMPTS = 10;

    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      attempts++;

      if (attempts >= MAX_ATTEMPTS) {
        Serial.println();
        Serial.print("WiFi connection failed after ");
        Serial.print(MAX_ATTEMPTS / 2);
        Serial.println(" seconds. Status code: ");
        switch (WiFi.status()) {
          case WL_DISCONNECTED:
            Serial.println("  WL_DISCONNECTED — check SSID and password");
            break;
          case WL_CONNECTION_LOST:
            Serial.println("  WL_CONNECTION_LOST — signal may be too weak");
            break;
          case WL_CONNECT_FAILED:
            Serial.println("  WL_CONNECT_FAILED — authentication failed, check password");
            break;
          case WL_NO_SSID_AVAIL:
            Serial.println("  WL_NO_SSID_AVAIL — network not found, check SSID");
            break;
          case WL_IDLE_STATUS:
            Serial.println("  WL_IDLE_STATUS — WiFi module not responding");
            break;
          default:
            Serial.print("  Unknown status code: ");
            Serial.println(WiFi.status());
            break;
        }
        Serial.println("Giving up for now -- will retry on next loop() pass.");
        return false;   // bounded -- always returns, never halts
      }
    }

    Serial.println();
    Serial.print("WiFi connected to: ");
    Serial.println(registration.wifiSSID);

    if (WiFi.localIP() == registration.localIP_wifi) {
      Serial.print("Static IP configured successfully. IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.print("Warning: IP mismatch. Got: ");
      Serial.println(WiFi.localIP());
      Serial.print("Expected: ");
      Serial.println(registration.localIP_wifi);
      Serial.println("Continuing with assigned IP — update localIP in sketch if needed.");
    }
    return true;

}

bool Sprout_API::connectMQTT(){

      // ADD — configure TLS before mqttClient.connect() runs; MqttClient's
    // own connect(ip, port) call takes no cert arguments, so sslClient
    // must already be fully configured by the time that call happens.
    sslClient.setClient(&wifiClient, true);
    sslClient.setDebugLevel(3);   // TEMPORARY — remove once connection succeeds

    sslClient.setCACert(registration.caCertPem);
    sslClient.setCertificate(registration.clientCertPem);
    sslClient.setPrivateKey(registration.clientKeyPem);
    // connectMQTT() — add right after the TLS setup block, before the connect loop:
    mqttClient.setUsernamePassword(registration.mqttUsername, registration.brokerPassword);
    // Capability-gated time source, not macro-gated (matches the earlier
    // decision to avoid guessing board-identifying macros). WiFi.getTime()
    // is confirmed to exist on the Opta's WiFi.h; if it ever returns a
    // sane value elsewhere too, this still works correctly.
// connectMQTT() — replace the WiFi.getTime() block with this:

  unsigned long now = getNtpTime();
  int timeAttempts = 0;
  while (now == 0 && timeAttempts < 5) {
    delay(500);
    now = getNtpTime();
    timeAttempts++;
  }

  Serial.print("[MQTT] NTP time after ");
  Serial.print(timeAttempts);
  Serial.print(" attempts: ");
  Serial.println(now);

if (now > 0) {
  sslClient.setX509Time(now);
} else {
  Serial.println("[MQTT] Could not obtain NTP time — TLS handshake likely to fail cert validity check.");
}
    int willType, willIndex;
    validateSingleWill(registration, willType, willIndex); // config already validated at load; this just locates it

    if (willIndex >= 0) {
      switch (willType) {
        case 0: // STR
          mqttClient.beginWill(registration.topicsStr[willIndex].topic, true, 1);
          mqttClient.print("offline");
          mqttClient.endWill();
          break;
        case 1: // FLOAT32
          mqttClient.beginWill(registration.topicsFloat[willIndex].topic, true, 1);
          mqttClient.print(0);
          mqttClient.endWill();
          break;
        case 2: // BOOL
          mqttClient.beginWill(registration.topicsBool[willIndex].topic, true, 1);
          mqttClient.print("false");
          mqttClient.endWill();
          break;
      }
    }
    Serial.print("[MQTT] Connecting on port: ");
    Serial.println(BROKER_PORT);

    Serial.print("Connecting to MQTT broker");
    int attempts = 0;
    const int MAX_ATTEMPTS = 10;

    while (!mqttClient.connect(registration.brokerIP, BROKER_PORT)) {
      int error = mqttClient.connectError();

            // ADD — get the SSL layer's own specific error, not just ArduinoMqttClient's generic -2
      char sslErrDesc[128];
      int sslErr = sslClient.getLastSSLError(sslErrDesc, sizeof(sslErrDesc));
      Serial.print("[SSL] Last SSL error code: ");
      Serial.print(sslErr);
      Serial.print(" — ");
      Serial.println(sslErrDesc);

      Serial.println();
      Serial.print("Connection attempt ");
      Serial.print(attempts + 1);
      Serial.print(" failed. Error code: ");
      Serial.print(error);
      Serial.print(" — ");
      switch (error) {
        case -2: Serial.println("Connection refused"); break;
        case -1: Serial.println("Connection timeout");  break;
        case  1: Serial.println("Unacceptable protocol version"); break;
        case  2: Serial.println("Client ID rejected"); break;
        case  3: Serial.println("Server unavailable"); break;
        case  4: Serial.println("Bad username or password"); break;
        case  5: Serial.println("Not authorized"); break;
        default: Serial.println("Unknown error"); break;
      }

      attempts++;
      if (attempts >= MAX_ATTEMPTS) {
        Serial.println("Max attempts reached -- will retry on next loop() pass.");
        return false;
      }
      delay(500);
    }
    Serial.println();
    Serial.println("MQTT connected.");

    if (willIndex >= 0 && willType == 0) {
      mqttClient.beginMessage(registration.topicsStr[willIndex].topic, true);
      mqttClient.print("online");
      mqttClient.endMessage();
      Serial.println("Announced presence by setting status to online");
    }
    return true;
}

// cpp:
void Sprout_API::publishOtaCompleteStatus(){
  String otaStatusTopic = "entgen/" + String(registration.deviceId) + "/ota/status";
  StaticJsonDocument<128> statusDoc;
  statusDoc["status"]    = "complete";
  statusDoc["device_id"] = registration.deviceId;
  String statusPayload;
  serializeJson(statusDoc, statusPayload);

  mqttClient.beginMessage(otaStatusTopic.c_str(), true /* retain */);
  mqttClient.print(statusPayload);
  mqttClient.endMessage();
  Serial.println("Published OTA complete status (retained).");
}

void Sprout_API::poll(){
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();   // return value not otherwise needed here -- next check below covers it
    }
    if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
      if (connectMQTT()) {
        // Subscriptions don't survive a broker reconnect -- must be redone
        // every time a new MQTT session is established, not just at boot.
        applyTopicSubscriptions();
      }
    }
    mqttClient.poll();
    publishUpdatedOutputs(registration);
    // -- Reconfiguration  --------------------
    if (registration.topicsStr[idxReconfiguration].update_flag) {
      registration.topicsStr[idxReconfiguration].update_flag = false;
      processReconfiguration(registration.topicsStr[idxReconfiguration].current_value);
    }
    if (pendingWifiReconnect) {
      pendingWifiReconnect = false;
      Serial.println("[RECONFIG] Applying new WiFi settings...");
      WiFi.config(registration.localIP_wifi, registration.dns, registration.gateway, registration.subnet);
      connectWiFi();
    }
    if (pendingMqttReconnect) {
      pendingMqttReconnect = false;
      Serial.println("[RECONFIG] Applying new MQTT/VTN settings...");
      mqttClient.stop();  // force a clean disconnect before reconnecting with new broker/credentials
      if (connectMQTT()) {
        applyTopicSubscriptions();
      }
    }
}


//---------------------------------------------
// ----- Config Load Save Validate-------
//--------------------------------------------
bool Sprout_API::loadConfigFromFlash(EnrollmentConfig &cfg){
  FILE* f = fopen(CONFIG_FILE_PATH, "r");
  if (f == nullptr) {
    Serial.print("[CONFIG] Could not open ");
    Serial.println(CONFIG_FILE_PATH);
    return false;
  }

  static char buffer[CONFIG_READ_BUFFER_SIZE];
  size_t bytesRead = fread(buffer, 1, CONFIG_READ_BUFFER_SIZE - 1, f);
  fclose(f);

  if (bytesRead == 0) {
    Serial.println("[CONFIG] Config file is empty or unreadable.");
    return false;
  }
  buffer[bytesRead] = '\0';

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, buffer, bytesRead);
  if (err) {
    Serial.print("[CONFIG] JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  bool ok = true;
  ok &= copyJsonStringField(doc, "device_id",     cfg.deviceId,     MAX_NAME_LEN,     true);
  ok &= copyJsonStringField(doc, "wifi_ssid",     cfg.wifiSSID,     MAX_SSID_LEN,     true);
  ok &= copyJsonStringField(doc, "wifi_password", cfg.wifiPassword, MAX_PASSWORD_LEN, true);
  ok &= copyJsonStringField(doc, "mqtt_username", cfg.mqttUsername, MAX_USERNAME_LEN, true);
  ok &= copyJsonStringField(doc, "broker_password", cfg.brokerPassword, MAX_PASSWORD_LEN, true);

  ok &= copyJsonMacField(doc, "mac", cfg.mac);

  ok &= copyJsonIPField(doc, "ip",        cfg.localIP_wifi, true);
  ok &= copyJsonIPField(doc, "gateway",   cfg.gateway,      true);
  ok &= copyJsonIPField(doc, "subnet",    cfg.subnet,       true);
  ok &= copyJsonIPField(doc, "dns",       cfg.dns,          true);
  ok &= copyJsonIPField(doc, "broker_ip", cfg.brokerIP,     true);
  ok &= copyJsonStringField(doc, "client_cert", cfg.clientCertPem, MAX_CERT_LEN, true);
  ok &= copyJsonStringField(doc, "client_key",  cfg.clientKeyPem,  MAX_CERT_LEN, true);
  ok &= copyJsonStringField(doc, "ca_cert",     cfg.caCertPem,     MAX_CERT_LEN, true);

  JsonVariantConst topicsField = doc["topics"];
  if (topicsField.isNull()) {
    Serial.println("[CONFIG] Missing 'topics' section, rejecting config.");
    ok = false;
  } else {
    ok &= loadTopicsFromJson(topicsField, cfg);
  }

  if (!ok) {
    Serial.println("[CONFIG] One or more fields failed validation, rejecting config.");
    return false;
  }

  if (!validateConfig(cfg)) {
    return false;
  }

  Serial.println("[CONFIG] Configuration loaded successfully.");
  return true;
}
    bool Sprout_API::saveConfigToFlash(const EnrollmentConfig &cfg){

      if (!validateConfig(cfg)) {
        Serial.println("[CONFIG] Refusing to save an invalid configuration.");
        return false;
      }

      JsonDocument doc;
      doc["device_id"]       = cfg.deviceId; 
      doc["wifi_ssid"]       = cfg.wifiSSID;
      doc["wifi_password"]   = cfg.wifiPassword;
      // saveConfigToFlash(), alongside the existing field writes:
      doc["mqtt_username"] = cfg.mqttUsername;
      doc["broker_password"] = cfg.brokerPassword;

      doc["ip"]        = cfg.localIP_wifi.toString();
      doc["gateway"]   = cfg.gateway.toString();
      doc["subnet"]    = cfg.subnet.toString();
      doc["dns"]       = cfg.dns.toString();
      doc["broker_ip"] = cfg.brokerIP.toString();
      doc["client_cert"] = cfg.clientCertPem;
      doc["client_key"]  = cfg.clientKeyPem;
      doc["ca_cert"]     = cfg.caCertPem;
      JsonArray macArr = doc["mac"].to<JsonArray>();
      for (int i = 0; i < 6; i++) {
        macArr.add(cfg.mac[i]);
      }

      JsonArray topicsArr = doc["topics"].to<JsonArray>();

      for (int i = 0; i < cfg.topicsStrCount; i++) {
        const TopicEntryStr &t = cfg.topicsStr[i];
        JsonObject entryObj = writeCommonTopicFields(topicsArr, t.name, t.topic, t.data_type, t.pub_flag, t.sub_flag, t.is_will);
        entryObj["init_value"] = t.init_value;
      }
      for (int i = 0; i < cfg.topicsFloatCount; i++) {
        const TopicEntryFloat &t = cfg.topicsFloat[i];
        JsonObject entryObj = writeCommonTopicFields(topicsArr, t.name, t.topic, t.data_type, t.pub_flag, t.sub_flag, t.is_will);
        entryObj["init_value"] = t.init_value;
      }
      for (int i = 0; i < cfg.topicsBoolCount; i++) {
        const TopicEntryBool &t = cfg.topicsBool[i];
        JsonObject entryObj = writeCommonTopicFields(topicsArr, t.name, t.topic, t.data_type, t.pub_flag, t.sub_flag, t.is_will);
        entryObj["init_value"] = t.init_value;
      }

      FILE* f = fopen(CONFIG_TEMP_PATH, "w");
      if (f == nullptr) {
        Serial.print("[CONFIG] Could not open temp file for writing: ");
        Serial.println(CONFIG_TEMP_PATH);
        return false;
      }

      static char buffer[CONFIG_READ_BUFFER_SIZE];
      size_t written = serializeJson(doc, buffer, CONFIG_READ_BUFFER_SIZE);
      if (written == 0 || written >= CONFIG_READ_BUFFER_SIZE) {
        Serial.println("[CONFIG] Serialized config too large for buffer, aborting save.");
        fclose(f);
        remove(CONFIG_TEMP_PATH);
        return false;
      }

      size_t writtenToFile = fwrite(buffer, 1, written, f);
      fclose(f);

      if (writtenToFile != written) {
        Serial.println("[CONFIG] Incomplete write to temp file, aborting save.");
        remove(CONFIG_TEMP_PATH);
        return false;
      }

      if (rename(CONFIG_TEMP_PATH, CONFIG_FILE_PATH) != 0) {
        Serial.println("[CONFIG] Failed to rename temp file into place.");
        remove(CONFIG_TEMP_PATH);
        return false;
      }

      Serial.println("[CONFIG] Configuration saved successfully.");
    return true;
  }
  
  bool Sprout_API::validateConfig(const EnrollmentConfig &cfg){
      int willType, willIndex;
      if (!validateSingleWill(cfg, willType, willIndex)) return false;
      checkPubSubOverlap(cfg); // advisory only -- logs a warning, never rejects
      // future rules go here
      return true;
  } 
          // MISSING from your draft — validateSingleWill + checkPubSubOverlap wrapper
  bool Sprout_API::validateSingleWill(const EnrollmentConfig &cfg, int &willTypeOut, int &willIndexOut){
    int willCount = 0;
    willTypeOut  = -1;
    willIndexOut = -1;

    for (int i = 0; i < cfg.topicsStrCount; i++) {
      if (cfg.topicsStr[i].is_will) {
        willCount++;
        willTypeOut  = 0;
        willIndexOut = i;
      }
    }
    for (int i = 0; i < cfg.topicsFloatCount; i++) {
      if (cfg.topicsFloat[i].is_will) {
        willCount++;
        willTypeOut  = 1;
        willIndexOut = i;
      }
    }
    for (int i = 0; i < cfg.topicsBoolCount; i++) {
      if (cfg.topicsBool[i].is_will) {
        willCount++;
        willTypeOut  = 2;
        willIndexOut = i;
      }
    }

    if (willCount > 1) {
      Serial.print("[CONFIG] More than one topic marked is_will (");
      Serial.print(willCount);
      Serial.println(" found) -- rejecting config.");
      return false;
    }

    return true;
  }


  void Sprout_API::checkPubSubOverlap(const EnrollmentConfig &cfg){
    for (int i = 0; i < cfg.topicsStrCount; i++) {
      if (cfg.topicsStr[i].pub_flag && cfg.topicsStr[i].sub_flag) {
        Serial.print("[CONFIG] Warning: topic '");
        Serial.print(cfg.topicsStr[i].name);
        Serial.println("' has both pub_flag and sub_flag set -- confirm this is intentional.");
      }
    }
    for (int i = 0; i < cfg.topicsFloatCount; i++) {
      if (cfg.topicsFloat[i].pub_flag && cfg.topicsFloat[i].sub_flag) {
        Serial.print("[CONFIG] Warning: topic '");
        Serial.print(cfg.topicsFloat[i].name);
        Serial.println("' has both pub_flag and sub_flag set -- confirm this is intentional.");
      }
    }
    for (int i = 0; i < cfg.topicsBoolCount; i++) {
      if (cfg.topicsBool[i].pub_flag && cfg.topicsBool[i].sub_flag) {
        Serial.print("[CONFIG] Warning: topic '");
        Serial.print(cfg.topicsBool[i].name);
        Serial.println("' has both pub_flag and sub_flag set -- confirm this is intentional.");
      }
    }
  }

  bool Sprout_API::partitionCheck(){
    if (bd->init() != BD_ERROR_OK) {
      return false;  // Can't even talk to the flash
    }
    
    bool ok = true;

    // Check Partition 1: WiFi firmware and certs
    MBRBlockDevice p1(bd, 1);
    FATFileSystem fs1("wlan_chk");
    if (fs1.mount(&p1) == 0) {
      fs1.unmount();
    } else {
      ok = false;
    };

    // Check Partition 2: OTA
    MBRBlockDevice p2(bd, 2);
    FATFileSystem fs2("ota_chk");
    if (fs2.mount(&p2) == 0) {
      fs2.unmount();
    } else {
      ok = false;
    };

    // Check Partition 4: User data
    MBRBlockDevice p4(bd, 4);
    LittleFileSystem fs4lfs("user_chk_lfs");
    if (fs4lfs.mount(&p4) == 0) {
      fs4lfs.unmount();
    } else {
      FATFileSystem fs4fat("user_chk_fat");
      if (fs4fat.mount(&p4) == 0) {
        fs4fat.unmount();
      } else {
        ok = false;
      };
    };

    bd->deinit();
    return ok;
  }

  unsigned long Sprout_API::getNtpTime(){
    WiFiUDP ntpUdp;
    const char* ntpServer = "pool.ntp.org";
    const int NTP_PACKET_SIZE = 48;
    byte packetBuffer[NTP_PACKET_SIZE];

    if (!ntpUdp.begin(2390)) {  // arbitrary local port
      Serial.println("[NTP] Could not open UDP socket.");
      return 0;
    }

    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;            // Stratum
    packetBuffer[2] = 6;            // Polling interval
    packetBuffer[3] = 0xEC;         // Peer clock precision
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;

    ntpUdp.beginPacket(ntpServer, 123);
    ntpUdp.write(packetBuffer, NTP_PACKET_SIZE);
    ntpUdp.endPacket();

    int attempts = 0;
    while (!ntpUdp.parsePacket() && attempts < 20) {
      delay(100);
      attempts++;
    }

    if (attempts >= 20) {
      Serial.println("[NTP] No response from server.");
      ntpUdp.stop();
      return 0;
    }

    ntpUdp.read(packetBuffer, NTP_PACKET_SIZE);
    ntpUdp.stop();

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord  = word(packetBuffer[42], packetBuffer[43]);
    unsigned long secsSince1900 = (highWord << 16) | lowWord;

    const unsigned long seventyYears = 2208988800UL;  // NTP epoch (1900) -> Unix epoch (1970)
    return secsSince1900 - seventyYears;
  }
//--------------------------------
  // -- JSON helpers --
  //--------------------------------
  bool Sprout_API::copyJsonStringField(JsonVariantConst obj, const char* key, char* dest, size_t destSize, bool required){
        if (obj[key].isNull()) {
      if (required) {
        Serial.print("[CONFIG] Missing required field: ");
        Serial.println(key);
        return false;
      };
      dest[0] = '\0';
      return true;
    }

    const char* value = obj[key];
    if (value == nullptr) {
      Serial.print("[CONFIG] Field is not a string: ");
      Serial.println(key);
      return false;
    };

    size_t len = strlen(value);
    if (len >= destSize) {
      Serial.print("[CONFIG] Field too long, rejecting config: ");
      Serial.print(key);
      Serial.print(" (");
      Serial.print(len);
      Serial.print(" chars, max ");
      Serial.print(destSize - 1);
      Serial.println(")");
      return false;
    };

    strcpy(dest, value);  // length already validated above
    return true;
  }

  bool Sprout_API::copyJsonIPField(JsonVariantConst obj, const char* key, IPAddress &dest, bool required){
        if (obj[key].isNull()) {
      if (required) {
        Serial.print("[CONFIG] Missing required IP field: ");
        Serial.println(key);
        return false;
      };
      return true;
    };
    const char* value = obj[key];
    if (value == nullptr || !dest.fromString(value)) {
      Serial.print("[CONFIG] Invalid IP address for field: ");
      Serial.println(key);
      return false;
    };
    return true;
  }

  bool Sprout_API::copyJsonMacField(JsonVariantConst obj, const char* key, byte* dest){
        JsonArrayConst arr = obj[key].as<JsonArrayConst>();
    if (arr.isNull() || arr.size() != 6) {
      Serial.print("[CONFIG] Missing or malformed MAC address field: ");
      Serial.println(key);
      return false;
    };
    int i = 0;
    for (JsonVariantConst v : arr) {
      dest[i++] = (byte)v.as<int>();
    }
    return true;
  }

  TopicType Sprout_API::parseTopicType(const char* s){
    if (strcmp(s, "BOOL")    == 0) return TopicType::TOPIC_BOOL;
    if (strcmp(s, "FLOAT32") == 0) return TopicType::TOPIC_FLOAT32;
    if (strcmp(s, "STR")     == 0) return TopicType::TOPIC_STR;
    return TopicType::TOPIC_UNKNOWN;
  }              // MISSING — needed by loadTopicsFromJson

  const char* Sprout_API::topicTypeToString(TopicType t){
        switch (t) {
      case TopicType::TOPIC_BOOL:    return "BOOL";
      case TopicType::TOPIC_FLOAT32: return "FLOAT32";
      case TopicType::TOPIC_STR:     return "STR";
      default:                       return "UNKNOWN";
    }
  }
  bool Sprout_API::parseCommonTopicFields(JsonObjectConst entry, char* nameOut, char* topicOut, bool &pubOut, bool &subOut, bool &willOut){
    bool ok = true;
    ok &= copyJsonStringField(entry, "name",  nameOut,  MAX_NAME_LEN,  true);
    ok &= copyJsonStringField(entry, "topic", topicOut, MAX_TOPIC_LEN, true);

    JsonVariantConst pubField  = entry["pub_flag"];
    JsonVariantConst subField  = entry["sub_flag"];
    JsonVariantConst willField = entry["is_will"];

    if (pubField.isNull() || subField.isNull()) {
      Serial.println("[CONFIG] Topic entry missing pub_flag or sub_flag.");
      return false;
    }

    pubOut  = pubField.as<bool>();
    subOut  = subField.as<bool>();
    willOut = willField.isNull() ? false : willField.as<bool>(); // absent -> not a will topic

    return ok;
  }

  bool Sprout_API::parseStrTopicEntry(JsonObjectConst entry, TopicEntryStr &out){
        bool ok = parseCommonTopicFields(entry, out.name, out.topic, out.pub_flag, out.sub_flag, out.is_will);

    const char* initVal = entry["init_value"];
    if (initVal == nullptr) {
      Serial.println("[CONFIG] STR topic missing init_value.");
      return false;
    }
    size_t initLen = strlen(initVal);
    if (initLen >= MAX_STR_LEN) {
      Serial.print("[CONFIG] STR topic init_value too long (");
      Serial.print(initLen);
      Serial.print(" chars, max ");
      Serial.print(MAX_STR_LEN - 1);
      Serial.println("), rejecting.");
      return false;
    }
    strcpy(out.init_value, initVal);
    strcpy(out.current_value, out.init_value); // Initialize current value to initial value

    out.data_type   = TopicType::TOPIC_STR;
    out.update_flag = false; // runtime state -- always starts clean on load, never read from JSON

    return ok;
  }
  bool Sprout_API::parseFloatTopicEntry(JsonObjectConst entry, TopicEntryFloat &out){
      bool ok = parseCommonTopicFields(entry, out.name, out.topic, out.pub_flag, out.sub_flag, out.is_will);

  JsonVariantConst initField = entry["init_value"];
  bool parsedOk = false;
  float value = 0.0f;

  if (initField.is<float>()) {
    value = initField.as<float>();
    parsedOk = true;
  } else if (initField.is<const char*>()) {
    value = atof(initField.as<const char*>());
    parsedOk = true; // atof returns 0.0 on genuine garbage too, but the alternative (strtod + errno checking) is more code for little real benefit here
  }

  if (!parsedOk) {
    Serial.println("[CONFIG] FLOAT32 topic missing or invalid init_value.");
    return false;
  }

  out.init_value = value;
  out.current_value = out.init_value;
  out.data_type   = TopicType::TOPIC_FLOAT32;
  out.update_flag = false;
  return ok;
  }

  bool Sprout_API::parseBoolTopicEntry(JsonObjectConst entry, TopicEntryBool &out){
      bool ok = parseCommonTopicFields(entry, out.name, out.topic, out.pub_flag, out.sub_flag, out.is_will);

  JsonVariantConst initField = entry["init_value"];
  bool parsedOk = false;
  bool value = false;

  if (initField.is<bool>()) {
    value = initField.as<bool>();
    parsedOk = true;
  } else if (initField.is<const char*>()) {
    const char* s = initField.as<const char*>();
    if (strcmp(s, "true") == 0)  { value = true;  parsedOk = true; }
    if (strcmp(s, "false") == 0) { value = false; parsedOk = true; }
  }

  if (!parsedOk) {
    Serial.println("[CONFIG] BOOL topic missing or invalid init_value.");
    return false;
  }

  out.init_value = value;
  out.current_value = out.init_value;
  out.data_type   = TopicType::TOPIC_BOOL;
  out.update_flag = false;
  return ok;
  }
  bool Sprout_API::loadTopicsFromJson(JsonVariantConst topicsField, EnrollmentConfig &cfg){
        if (!topicsField.is<JsonArrayConst>()) {
      Serial.println("[CONFIG] 'topics' must be an array.");
      return false;
    }

    cfg.topicsStrCount = 0;
    cfg.topicsFloatCount = 0;
    cfg.topicsBoolCount = 0;

    bool ok = true;
    for (JsonObjectConst entry : topicsField.as<JsonArrayConst>()) {
      const char* typeStr = entry["data_type"];
      if (!typeStr) {
        Serial.println("[CONFIG] Topic entry missing data_type, rejecting.");
        ok = false;
        continue;
      }
      TopicType type = parseTopicType(typeStr);

      switch (type) {
        case TopicType::TOPIC_STR:
          if (cfg.topicsStrCount >= MAX_TOPICS) {
            Serial.println("[CONFIG] Too many STR topics, rejecting.");
            ok = false;
            break;
          }
          ok &= parseStrTopicEntry(entry, cfg.topicsStr[cfg.topicsStrCount++]);
          break;

        case TopicType::TOPIC_FLOAT32:
          if (cfg.topicsFloatCount >= MAX_TOPICS) {
            Serial.println("[CONFIG] Too many FLOAT32 topics, rejecting.");
            ok = false;
            break;
          }
          ok &= parseFloatTopicEntry(entry, cfg.topicsFloat[cfg.topicsFloatCount++]);
          break;

        case TopicType::TOPIC_BOOL:
          if (cfg.topicsBoolCount >= MAX_TOPICS) {
            Serial.println("[CONFIG] Too many BOOL topics, rejecting.");
            ok = false;
            break;
          }
          ok &= parseBoolTopicEntry(entry, cfg.topicsBool[cfg.topicsBoolCount++]);
          break;

        default:
          Serial.print("[CONFIG] Unrecognized data_type: ");
          Serial.println(typeStr);
          ok = false;
      }
    }

    return ok;
  } // fixed signature — original only took cfg, but needs the field

  JsonObject Sprout_API::writeCommonTopicFields(JsonArray &arr, const char* name, const char* topic, TopicType type, bool pub, bool sub, bool will){
    JsonObject entryObj = arr.add<JsonObject>();
    entryObj["name"]      = name;
    entryObj["topic"]     = topic;
    entryObj["data_type"] = topicTypeToString(type);
    entryObj["pub_flag"]  = pub;
    entryObj["sub_flag"]  = sub;
    entryObj["is_will"]   = will;
    return entryObj;
  }


//-----------------------------------------
//-------TOPIC LOOKUP----------
//----------------------------------------
  TopicLookupResult Sprout_API::findTopicByTopicString(const String &topic){
    for (int i = 0; i < registration.topicsStrCount; i++)
      if (topic == registration.topicsStr[i].topic) return { TopicArray::STR, i };

    for (int i = 0; i < registration.topicsFloatCount; i++)
      if (topic == registration.topicsFloat[i].topic) return { TopicArray::FLOAT, i };

    for (int i = 0; i < registration.topicsBoolCount; i++)
      if (topic == registration.topicsBool[i].topic) return { TopicArray::BOOL, i };

    return {}; // arrayType stays NONE, index stays -1
  }

// cpp — restore the cfg parameter, use it instead of registration:
TopicLookupResult Sprout_API::findTopicByNameAnywhere(const EnrollmentConfig &cfg, const char* name){
  int idx = findIndexByName(cfg.topicsStr, cfg.topicsStrCount, name);
  if (idx >= 0) return { TopicArray::STR, idx };

  idx = findIndexByName(cfg.topicsFloat, cfg.topicsFloatCount, name);
  if (idx >= 0) return { TopicArray::FLOAT, idx };

  idx = findIndexByName(cfg.topicsBool, cfg.topicsBoolCount, name);
  if (idx >= 0) return { TopicArray::BOOL, idx };

  return {};
}

  int Sprout_API::findIndexByName(const TopicEntryStr* arr, int count, const char* name){
    for (int i = 0; i < count; i++) if (strcmp(arr[i].name, name) == 0) return i;
    return -1;
  }

  int Sprout_API::findIndexByName(const TopicEntryFloat* arr, int count, const char* name){
    for (int i = 0; i < count; i++) if (strcmp(arr[i].name, name) == 0) return i;
    return -1;
  }

  int Sprout_API::findIndexByName(const TopicEntryBool* arr, int count, const char* name){
    for (int i = 0; i < count; i++) if (strcmp(arr[i].name, name) == 0) return i;
    return -1;
  }

//------------------------------------
//-------MQTT Pub Sub-----------
//------------------------------------

  void Sprout_API::applyTopicSubscriptions(){
    bool ok = true;

    for (int i = 0; i < registration.topicsStrCount; i++) {
      if (registration.topicsStr[i].sub_flag) {
        if (!mqttClient.subscribe(registration.topicsStr[i].topic, 1)) {
          Serial.print("[MQTT] Failed to subscribe: ");
          Serial.println(registration.topicsStr[i].topic);
          ok = false;
        }
      }
    }

    for (int i = 0; i < registration.topicsFloatCount; i++) {
      if (registration.topicsFloat[i].sub_flag) {
        if (!mqttClient.subscribe(registration.topicsFloat[i].topic, 1)) {
          Serial.print("[MQTT] Failed to subscribe: ");
          Serial.println(registration.topicsFloat[i].topic);
          ok = false;
        }
      }
    }

    for (int i = 0; i < registration.topicsBoolCount; i++) {
      if (registration.topicsBool[i].sub_flag) {
        if (!mqttClient.subscribe(registration.topicsBool[i].topic, 1)) {
          Serial.print("[MQTT] Failed to subscribe: ");
          Serial.println(registration.topicsBool[i].topic);
          ok = false;
        }
      }
    }

    mqttClient.onMessage(onMqttMessageThunk);
    Serial.print("[MQTT] Subscriptions applied");
    Serial.println(ok ? "." : " (with errors).");
  }

  void Sprout_API::publishUpdatedOutputs(EnrollmentConfig &cfg){
    for (int i = 0; i < cfg.topicsStrCount; i++)
      if (cfg.topicsStr[i].pub_flag && cfg.topicsStr[i].update_flag) publishStrTopic(cfg.topicsStr[i]);

    for (int i = 0; i < cfg.topicsFloatCount; i++)
      if (cfg.topicsFloat[i].pub_flag && cfg.topicsFloat[i].update_flag) publishFloatTopic(cfg.topicsFloat[i]);

    for (int i = 0; i < cfg.topicsBoolCount; i++)
      if (cfg.topicsBool[i].pub_flag && cfg.topicsBool[i].update_flag) publishBoolTopic(cfg.topicsBool[i]);

  }

  bool Sprout_API::publishStrTopic(TopicEntryStr &t){
    mqttClient.beginMessage(t.topic, false, 1);
    mqttClient.print(t.current_value);   // was init_value -- publish the live value, not the boot default
    int result = mqttClient.endMessage();
    if (result == 1) t.update_flag = false;
    return result == 1;
  }

  bool Sprout_API::publishFloatTopic(TopicEntryFloat &t){
    mqttClient.beginMessage(t.topic, false, 1);
    mqttClient.print(t.current_value, 4);
    int result = mqttClient.endMessage();
    if (result == 1) t.update_flag = false;
    return result == 1;
  }

  bool Sprout_API::publishBoolTopic(TopicEntryBool &t){
    mqttClient.beginMessage(t.topic, false, 1);
    mqttClient.print(t.current_value ? "true" : "false");
    int result = mqttClient.endMessage();
    if (result == 1) t.update_flag = false;
    return result == 1;
  }

//---------------------------------------
//------RECONFIG-------------------
//------------------------------------------

  bool Sprout_API::isReservedTopicName(const char* name){
    static const char* reserved[] = {
      "device_status", "reconfiguration",
      "input_button1", "input_button2", "input_num1", "input_num2", "input_str",
      "output_status1", "output_status2", "output_num1", "output_num2", "output_str"
    };
    for (const char* r : reserved) {
      if (strcmp(name, r) == 0) return true;
    }
    return false;
  }

  bool Sprout_API::applyTopLevelPatch(JsonVariantConst patch, EnrollmentConfig &scratch){
    bool ok = true;
    if (!patch["wifi_ssid"].isNull())
      ok &= copyJsonStringField(patch, "wifi_ssid", scratch.wifiSSID, MAX_SSID_LEN, true);
    if (!patch["wifi_password"].isNull())
      ok &= copyJsonStringField(patch, "wifi_password", scratch.wifiPassword, MAX_PASSWORD_LEN, true);
    if (!patch["broker_password"].isNull())
      ok &= copyJsonStringField(patch, "broker_password", scratch.brokerPassword, MAX_PASSWORD_LEN, true);
    if (!patch["mac"].isNull())
      ok &= copyJsonMacField(patch, "mac", scratch.mac);
    if (!patch["ip"].isNull())
      ok &= copyJsonIPField(patch, "ip", scratch.localIP_wifi, true);
    if (!patch["gateway"].isNull())
      ok &= copyJsonIPField(patch, "gateway", scratch.gateway, true);
    if (!patch["subnet"].isNull())
      ok &= copyJsonIPField(patch, "subnet", scratch.subnet, true);
    if (!patch["dns"].isNull())
      ok &= copyJsonIPField(patch, "dns", scratch.dns, true);
    if (!patch["broker_ip"].isNull())
      ok &= copyJsonIPField(patch, "broker_ip", scratch.brokerIP, true);
    return ok;
  }

  bool Sprout_API::applyTopicRemovals(JsonVariantConst patch, EnrollmentConfig &scratch){
    JsonVariantConst removeField = patch["remove_topics"];
    if (removeField.isNull()) return true; // no removals requested -- not an error

    if (!removeField.is<JsonArrayConst>()) {
      Serial.println("[RECONFIG] 'remove_topics' must be an array.");
      return false;
    }

    for (JsonVariantConst nameVar : removeField.as<JsonArrayConst>()) {
      const char* name = nameVar.as<const char*>();
      if (!name) continue;

      if (isReservedTopicName(name)) {
        Serial.print("[RECONFIG] Refusing to remove essential topic: ");
        Serial.println(name);
        return false; // hard fail -- exactly the error you asked for
      }

      bool found = false;
      // Str
      for (int i = 0; i < scratch.topicsStrCount; i++) {
        if (strcmp(scratch.topicsStr[i].name, name) == 0) {
          for (int j = i; j < scratch.topicsStrCount - 1; j++) scratch.topicsStr[j] = scratch.topicsStr[j + 1];
          scratch.topicsStrCount--;
          found = true;
          break;
        }
      }
      // Float
      if (!found) {
        for (int i = 0; i < scratch.topicsFloatCount; i++) {
          if (strcmp(scratch.topicsFloat[i].name, name) == 0) {
            for (int j = i; j < scratch.topicsFloatCount - 1; j++) scratch.topicsFloat[j] = scratch.topicsFloat[j + 1];
            scratch.topicsFloatCount--;
            found = true;
            break;
          }
        }
      }
      // Bool
      if (!found) {
        for (int i = 0; i < scratch.topicsBoolCount; i++) {
          if (strcmp(scratch.topicsBool[i].name, name) == 0) {
            for (int j = i; j < scratch.topicsBoolCount - 1; j++) scratch.topicsBool[j] = scratch.topicsBool[j + 1];
            scratch.topicsBoolCount--;
            found = true;
            break;
          }
        }
      }

      if (!found) {
        Serial.print("[RECONFIG] Warning: remove_topics named a topic that doesn't exist: ");
        Serial.println(name);
        // not a failure -- asking to remove something already absent is harmless
      }
    }
    return true;
  }

  bool Sprout_API::applyTopicPatch(JsonVariantConst patch, EnrollmentConfig &scratch){
    JsonVariantConst topicsField = patch["topics"];
    if (topicsField.isNull()) return true;

    if (!topicsField.is<JsonArrayConst>()) {
      Serial.println("[RECONFIG] 'topics' must be an array.");
      return false;
    }

    for (JsonObjectConst entry : topicsField.as<JsonArrayConst>()) {
      const char* name = entry["name"];
      if (!name) {
        Serial.println("[RECONFIG] Topic patch entry missing name, rejecting.");
        return false;
      }
      if (isReservedTopicName(name)) {
        Serial.print("[RECONFIG] Refusing to modify essential topic via patch: ");
        Serial.println(name);
        return false;
      }

      const char* typeStr = entry["data_type"];
      if (!typeStr) {
        Serial.println("[RECONFIG] Topic patch entry missing data_type, rejecting.");
        return false;
      }
      TopicType type = parseTopicType(typeStr);
      if (type == TopicType::TOPIC_UNKNOWN) {
        Serial.print("[RECONFIG] Unrecognized data_type in patch: ");
        Serial.println(typeStr);
        return false;
      }

      TopicArray expectedArray =
        (type == TopicType::TOPIC_STR)     ? TopicArray::STR :
        (type == TopicType::TOPIC_FLOAT32) ? TopicArray::FLOAT :
                                              TopicArray::BOOL;

      TopicLookupResult existing = findTopicByNameAnywhere(scratch, name);
      if (existing.arrayType != TopicArray::NONE && existing.arrayType != expectedArray) {
        Serial.print("[RECONFIG] Topic '");
        Serial.print(name);
        Serial.println("' already exists with a different data_type -- remove it first, then add it with the new type, rejecting patch.");
        return false;
      }

      switch (type) {
        case TopicType::TOPIC_STR: {
          int idx = (existing.arrayType == TopicArray::STR) ? existing.index : -1;
          bool isNew = (idx < 0);
          if (isNew) {
            if (scratch.topicsStrCount >= MAX_TOPICS) {
              Serial.println("[RECONFIG] Too many STR topics, rejecting patch.");
              return false;
            }
            idx = scratch.topicsStrCount++;
          }
          TopicEntryStr temp;
          if (!parseStrTopicEntry(entry, temp)) return false;
          char preservedCurrent[MAX_STR_LEN];
          strcpy(preservedCurrent, scratch.topicsStr[idx].current_value);
          scratch.topicsStr[idx] = temp;
          if (!isNew) strcpy(scratch.topicsStr[idx].current_value, preservedCurrent);
          break;
        }
        case TopicType::TOPIC_FLOAT32: {
          int idx = (existing.arrayType == TopicArray::FLOAT) ? existing.index : -1;
          bool isNew = (idx < 0);
          if (isNew) {
            if (scratch.topicsFloatCount >= MAX_TOPICS) {
              Serial.println("[RECONFIG] Too many FLOAT32 topics, rejecting patch.");
              return false;
            }
            idx = scratch.topicsFloatCount++;
          }
          TopicEntryFloat temp;
          if (!parseFloatTopicEntry(entry, temp)) return false;
          float preservedCurrent = scratch.topicsFloat[idx].current_value;
          scratch.topicsFloat[idx] = temp;
          if (!isNew) scratch.topicsFloat[idx].current_value = preservedCurrent;
          break;
        }
        case TopicType::TOPIC_BOOL: {
          int idx = (existing.arrayType == TopicArray::BOOL) ? existing.index : -1;
          bool isNew = (idx < 0);
          if (isNew) {
            if (scratch.topicsBoolCount >= MAX_TOPICS) {
              Serial.println("[RECONFIG] Too many BOOL topics, rejecting patch.");
              return false;
            }
            idx = scratch.topicsBoolCount++;
          }
          TopicEntryBool temp;
          if (!parseBoolTopicEntry(entry, temp)) return false;
          bool preservedCurrent = scratch.topicsBool[idx].current_value;
          scratch.topicsBool[idx] = temp;
          if (!isNew) scratch.topicsBool[idx].current_value = preservedCurrent;
          break;
        }
        default:
          return false; // unreachable -- TOPIC_UNKNOWN already rejected above
      }
    }
    return true;
  }

  bool Sprout_API::processReconfiguration(const char* payload){
    JsonDocument patchDoc;
    DeserializationError err = deserializeJson(patchDoc, payload);
    if (err) {
      Serial.print("[RECONFIG] Invalid JSON, ignoring: ");
      Serial.println(err.c_str());
      return false;
    }

    reconfigScratch = registration; // full copy -- validated in isolation, never touches the live struct directly

    bool ok = true;
    ok &= applyTopLevelPatch(patchDoc, reconfigScratch);
    ok &= applyTopicRemovals(patchDoc, reconfigScratch);
    ok &= applyTopicPatch(patchDoc, reconfigScratch);

    if (!ok || !validateConfig(reconfigScratch)) {
      Serial.println("[RECONFIG] *** Reconfiguration REJECTED -- reverting to last known good configuration. ***");
      return false; // reconfigScratch discarded, registration untouched
    }

    bool wifiChanged =
      strcmp(registration.wifiSSID, reconfigScratch.wifiSSID) != 0 ||
      strcmp(registration.wifiPassword, reconfigScratch.wifiPassword) != 0 ||
      registration.localIP_wifi != reconfigScratch.localIP_wifi ||
      registration.gateway      != reconfigScratch.gateway ||
      registration.subnet       != reconfigScratch.subnet ||
      registration.dns          != reconfigScratch.dns;

    bool mqttChanged =
      registration.brokerIP != reconfigScratch.brokerIP ||
      strcmp(registration.brokerPassword, reconfigScratch.brokerPassword) != 0;

    registration = reconfigScratch; // commit

    if (!saveConfigToFlash(registration)) {
      Serial.println("[RECONFIG] *** Save to flash FAILED -- reverting live config to last known good. ***");
      loadConfigFromFlash(registration); // reload whatever's still on disk -- guaranteed to match pre-patch state
      resolveSproutAccessorIndices();
      return false;
    }

    resolveSproutAccessorIndices(); // topic set may have changed -- indices must be re-resolved
    pendingWifiReconnect |= wifiChanged;
    pendingMqttReconnect |= mqttChanged;

    Serial.println("[RECONFIG] Reconfiguration applied and saved successfully.");
    return true;
  }


  //-----------------------------------------------
  //--------SPROUT ACCESSORS------------
  //----------------------------------------------

  void Sprout_API::resolveSproutAccessorIndices(){
    idxDeviceStatus     = findIndexByName(registration.topicsStr,   registration.topicsStrCount,   "device_status");
    idxReconfiguration  = findIndexByName(registration.topicsStr,   registration.topicsStrCount,   "reconfiguration");
    idxButton1          = findIndexByName(registration.topicsBool,  registration.topicsBoolCount,  "input_button1");
    idxButton2          = findIndexByName(registration.topicsBool,  registration.topicsBoolCount,  "input_button2");
    idxInputNum1        = findIndexByName(registration.topicsFloat, registration.topicsFloatCount, "input_num1");
    idxInputNum2        = findIndexByName(registration.topicsFloat, registration.topicsFloatCount, "input_num2");
    idxInputStr         = findIndexByName(registration.topicsStr,   registration.topicsStrCount,   "input_str");
    idxOutStatus1       = findIndexByName(registration.topicsBool,  registration.topicsBoolCount,  "output_status1");
    idxOutStatus2       = findIndexByName(registration.topicsBool,  registration.topicsBoolCount,  "output_status2");
    idxOutNum1          = findIndexByName(registration.topicsFloat, registration.topicsFloatCount, "output_num1");
    idxOutNum2          = findIndexByName(registration.topicsFloat, registration.topicsFloatCount, "output_num2");
    idxOutStr           = findIndexByName(registration.topicsStr,   registration.topicsStrCount,   "output_str");

    if (idxDeviceStatus < 0 || idxReconfiguration < 0 || idxButton1 < 0 || idxButton2 < 0 ||
        idxInputNum1 < 0 || idxInputNum2 < 0 || idxInputStr < 0 || idxOutStatus1 < 0 ||
        idxOutStatus2 < 0 || idxOutNum1 < 0 || idxOutNum2 < 0 || idxOutStr < 0) {
      Serial.println("FATAL: one or more base Sprout topics not found by name in loaded configuration.");
      Serial.println("Halting -- this configuration file is missing required topic(s).");
      while (1);
    }
  }

  bool Sprout_API::sproutButton1(){
    return registration.topicsBool[idxButton1].current_value;
  }

  bool Sprout_API::sproutButton2(){
    return registration.topicsBool[idxButton2].current_value; 
  }
  float Sprout_API::sproutInputNum1(){
    return registration.topicsFloat[idxInputNum1].current_value;
  }

  float Sprout_API::sproutInputNum2(){
    return registration.topicsFloat[idxInputNum2].current_value;
  }

  const char* Sprout_API::sproutInputStr(){
    return registration.topicsStr[idxInputStr].current_value;
  }

  bool Sprout_API::sproutButton1IsNew(){
    return registration.topicsBool[idxButton1].update_flag;
  }
  bool Sprout_API::sproutButton2IsNew(){
    return registration.topicsBool[idxButton2].update_flag;
  }

  bool Sprout_API::sproutInputNum1IsNew(){
    return registration.topicsFloat[idxInputNum1].update_flag;
  }

  bool Sprout_API::sproutInputNum2IsNew(){
    return registration.topicsFloat[idxInputNum2].update_flag;
  }

  bool Sprout_API::sproutInputStrIsNew(){
    return registration.topicsStr[idxInputStr].update_flag;
  }

  void Sprout_API::sproutButton1Ack(){
    registration.topicsBool[idxButton1].update_flag = false; 
  }

  void Sprout_API::sproutButton2Ack(){
     registration.topicsBool[idxButton2].update_flag = false;
  }
  void Sprout_API::sproutInputNum1Ack(){
    registration.topicsFloat[idxInputNum1].update_flag = false;

  }
    
  void Sprout_API::sproutInputNum2Ack(){
    registration.topicsFloat[idxInputNum2].update_flag = false;
  }

  void Sprout_API::sproutInputStrAck(){
    registration.topicsStr[idxInputStr].update_flag = false;
  }

  void Sprout_API::sproutSetOutputStatus1(bool v){
    registration.topicsBool[idxOutStatus1].current_value = v;
    registration.topicsBool[idxOutStatus1].update_flag = true;
  }

  void Sprout_API::sproutSetOutputStatus2(bool v){
    registration.topicsBool[idxOutStatus2].current_value = v;
    registration.topicsBool[idxOutStatus2].update_flag = true;
  }

  void Sprout_API::sproutSetOutputNum1(float v){
    registration.topicsFloat[idxOutNum1].current_value = v;
    registration.topicsFloat[idxOutNum1].update_flag = true;
  }
    
  void Sprout_API::sproutSetOutputNum2(float v){
    registration.topicsFloat[idxOutNum2].current_value = v;
    registration.topicsFloat[idxOutNum2].update_flag = true;
  }

  bool Sprout_API::sproutSetOutputStr(const char* v){
    size_t len = strlen(v);
    if (len >= MAX_STR_LEN) {
      Serial.println("[SPROUT] output_str value too long, ignoring set.");
      return false;
    }
    strcpy(registration.topicsStr[idxOutStr].current_value, v);
    registration.topicsStr[idxOutStr].update_flag = true;
    return true;
  }

  bool Sprout_API::sproutSetDeviceStatus(const char* v){
    size_t len = strlen(v);
    if (len >= MAX_STR_LEN) {
      Serial.println("[SPROUT] device_status value too long, ignoring set.");
      return false;
    }
    strcpy(registration.topicsStr[idxDeviceStatus].current_value, v);
    registration.topicsStr[idxDeviceStatus].update_flag = true;
    return true;

  }

  //---------------------------------------------------
//-------------PRIVATE HANDLERS AND SUCH--------------
//-------------------------------------------------------

  void Sprout_API::onMqttMessageThunk(int messageSize){
    if (_instance != nullptr) {
      _instance->onMqttMessage(messageSize);  // hands off to the real, non-static handler
    }
  }

  Sprout_API* Sprout_API::_instance = nullptr;


  void Sprout_API::onMqttMessage(int messageSize){
    String topic = mqttClient.messageTopic();

    String payloadStr;
    payloadStr.reserve(messageSize);
    while (mqttClient.available()) {
      payloadStr += (char)mqttClient.read();
    }

    Serial.print("[MQTT] Message on "); Serial.print(topic);
    Serial.print(" ("); Serial.print(messageSize); Serial.println(" bytes)");

    TopicLookupResult found = findTopicByTopicString(topic);

    switch (found.arrayType) {
      case TopicArray::STR: {
        TopicEntryStr &t = registration.topicsStr[found.index];
        if (payloadStr.length() >= MAX_STR_LEN) {
          Serial.print("[MQTT] Payload too long, dropping message on: ");
          Serial.println(topic);
          return;
        }
        strcpy(t.current_value, payloadStr.c_str());

        if (t.pub_flag && t.sub_flag) {
          if (t.onReceive) {
            t.onReceive(t); // callback decides whether to set update_flag
          } else {
            Serial.print("[MQTT] Warning: pub+sub topic '");
            Serial.print(t.name);
            Serial.println("' has no registered handler -- value stored, not republished.");
          }
        } else {
          t.update_flag = true;
        }
        break;
      }

      case TopicArray::FLOAT: {
        TopicEntryFloat &t = registration.topicsFloat[found.index];
        t.current_value = payloadStr.toFloat();

        if (t.pub_flag && t.sub_flag) {
          if (t.onReceive) {
            t.onReceive(t);
          } else {
            Serial.print("[MQTT] Warning: pub+sub topic '");
            Serial.print(t.name);
            Serial.println("' has no registered handler -- value stored, not republished.");
          }
        } else {
          t.update_flag = true;
        }
        break;
      }

      case TopicArray::BOOL: {
        TopicEntryBool &t = registration.topicsBool[found.index];
        t.current_value = (payloadStr == "true" || payloadStr == "1");

        if (t.pub_flag && t.sub_flag) {
          if (t.onReceive) {
            t.onReceive(t);
          } else {
            Serial.print("[MQTT] Warning: pub+sub topic '");
            Serial.print(t.name);
            Serial.println("' has no registered handler -- value stored, not republished.");
          }
        } else {
          t.update_flag = true;
        }
        break;
      }

      default:
        Serial.print("[MQTT] No matching topic entry for: ");
        Serial.println(topic);
    }
  }
