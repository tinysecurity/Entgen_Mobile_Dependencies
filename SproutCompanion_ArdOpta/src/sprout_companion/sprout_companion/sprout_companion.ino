  // ============================================================
  // Entwise Sprout Companion Sketch  v0.1
  //
  // This sketch handles the MQTT/WiFi/configuration plumbing for an
  // Arduino Opta running Sprout, so the end user can write their own
  // control logic without needing to understand networking, JSON
  // parsing, or MQTT internals.
  //
  // On boot, this sketch:
  //   - Verifies the QSPI flash has been partitioned (see QSPIFormat.ino)
  //   - Loads WiFi credentials, MQTT broker settings, and the device's
  //     topic table from a JSON configuration file in flash
  //   - Connects to WiFi and the MQTT broker, publishing a Last Will
  //     and Testament on whichever topic the config marks as such
  //   - Subscribes to all topics flagged for subscription in the config
  //
  // During normal operation, this sketch:
  //   - Services incoming MQTT messages, storing each topic's latest
  //     value for the user's code to read
  //   - Publishes any output values the user's code has changed since
  //     the last pass
  //   - Listens for reconfiguration messages (from the companion phone
  //     app) that can update WiFi/broker credentials or add, update, and
  //     remove custom topics at runtime -- validated against a scratch
  //     copy of the configuration and rejected without effect if invalid
  //
  // The 11 base Sprout I/O topics are exposed to user code through simple
  // accessor functions (sproutButton1(), sproutSetOutputNum1(), etc.) --
  // see userLoop() below for where to add your own logic. These topics,
  // along with the reconfiguration topic itself, are protected from being
  // renamed or removed by any reconfiguration message.
  //
  // Everything here is scaffolding: no application-specific control logic
  // lives in this file. Site-specific behavior belongs in userLoop().
  // ============================================================

  #include <WiFi.h>
  #include <BlockDevice.h>
  #include <LittleFileSystem.h>
  #include <MBRBlockDevice.h>
  #include <SPI.h>
  #include <ArduinoMqttClient.h>
  #include <ArduinoJson.h>
  #include <mbed.h>

  using namespace mbed;

  // -- Configuration Section --------------------

  // Filesystem
  BlockDevice*      bd = BlockDevice::get_default_instance();
  LittleFileSystem  fs("fs");

  // Enrollment Configuration Parameters

  #define MAX_SSID_LEN        32
  #define MAX_PASSWORD_LEN    64

  #define CONFIG_FILE_PATH "/fs/config.json"
  #define CONFIG_TEMP_PATH "/fs/config.json.tmp"
  #define CONFIG_READ_BUFFER_SIZE 64000   // generous margin above expected file size

  // This structure defines the MQTT topics to which this device publishes and subscribes.

  #define MAX_NAME_LEN 32
  #define MAX_TOPIC_LEN 64
  #define MAX_STR_LEN 512
  #define MAX_TOPICS 64

  enum class TopicType : uint8_t {
    TOPIC_BOOL,
    TOPIC_FLOAT32,
    TOPIC_STR,
    TOPIC_UNKNOWN   // sentinel for "didn't match anything" -- see parse function below
  };

  struct TopicEntryStr {
    char        name[MAX_NAME_LEN];
    char        topic[MAX_TOPIC_LEN];
    TopicType   data_type;
    char        init_value[MAX_STR_LEN];
    char        current_value[MAX_STR_LEN];
    bool        pub_flag;
    bool        sub_flag;
    bool        is_will;
    bool        update_flag = false;
    void        (*onReceive)(TopicEntryStr &entry) = nullptr; // user-supplied, only consulted for pub+sub topics
  };

  struct TopicEntryFloat {
    char        name[MAX_NAME_LEN];
    char        topic[MAX_TOPIC_LEN];
    TopicType   data_type;
    float       init_value;
    float       current_value;
    bool        pub_flag;
    bool        sub_flag;
    bool        is_will;
    bool        update_flag = false;
    void        (*onReceive)(TopicEntryFloat &entry) = nullptr; // user-supplied, only consulted for pub+sub topics
  };

  struct TopicEntryBool {
    char        name[MAX_NAME_LEN];
    char        topic[MAX_TOPIC_LEN];
    TopicType   data_type;
    bool        init_value;
    bool        current_value;
    bool        pub_flag;
    bool        sub_flag;
    bool        is_will;
    bool        update_flag = false;
    void        (*onReceive)(TopicEntryBool &entry) = nullptr; // user-supplied, only consulted for pub+sub topics
  };

  // This structure defines the enrollment configuration parameters.
  // These include wifi credentials, the local wifi IP address, 
  // and the MQTT topics used by the device.

  struct EnrollmentConfig {
    char            wifiSSID[MAX_SSID_LEN];
    char            wifiPassword[MAX_PASSWORD_LEN];

    byte            mac[6];
    IPAddress       localIP_wifi;
    IPAddress       gateway;
    IPAddress       subnet;
    IPAddress       dns;
    IPAddress       brokerIP;
    char            brokerPassword[MAX_PASSWORD_LEN];

    TopicEntryStr   topicsStr[MAX_TOPICS];
    int             topicsStrCount;
    TopicEntryFloat topicsFloat[MAX_TOPICS];
    int             topicsFloatCount;
    TopicEntryBool  topicsBool[MAX_TOPICS];
    int             topicsBoolCount;
  };

  EnrollmentConfig registration;

  // Network configuration

  // WiFi Configuration
  WiFiClient wifiClient;

  bool pendingWifiReconnect  = false;

  // MQTT Configuration
  const int   BROKER_PORT = 1883;
  MqttClient  mqttClient(wifiClient);

  bool pendingMqttReconnect  = false;

  // MQTT Topic Parsing Configuration

  enum class TopicArray : uint8_t { NONE, STR, FLOAT, BOOL };

  struct TopicLookupResult {
    TopicArray arrayType = TopicArray::NONE;
    int        index     = -1;
  };

  // Sprout Companion UI accessor configuration

  // Cached indices into registration.topicsBool/topicsFloat/topicsStr, resolved once
  // by name during initialization. -1 means "not found" -- see resolveSproutAccessorIndices().
  static int idxDeviceStatus = -1; // STR,   pub
  static int idxReconfiguration = -1; // STR,   sub
  static int idxButton1      = -1; // BOOL,  sub
  static int idxButton2      = -1; // BOOL,  sub
  static int idxInputNum1    = -1; // FLOAT, sub
  static int idxInputNum2    = -1; // FLOAT, sub
  static int idxInputStr     = -1; // STR,   sub
  static int idxOutStatus1   = -1; // BOOL,  pub
  static int idxOutStatus2   = -1; // BOOL,  pub
  static int idxOutNum1      = -1; // FLOAT, pub
  static int idxOutNum2      = -1; // FLOAT, pub
  static int idxOutStr       = -1; // STR,   pub

  // Reconfiguration Configuration

  static EnrollmentConfig reconfigScratch; 

  // -- Setup Section ----------------------------
  // Code here runs once

  void setup() {
    Serial.begin(115200);
    while (!Serial);
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

    // Resolve the 11 base Sprout accessor indices by name -- must happen
    // after load succeeds, before subscribe/publish touch the topic tables.
    resolveSproutAccessorIndices(registration);

    WiFi.config(registration.localIP_wifi, registration.dns, registration.gateway, registration.subnet);
    if (!connectWiFi()) {
      Serial.println("WiFi not connected at boot -- will keep retrying from loop().");
    }

    if (connectMQTT()) {
      applyTopicSubscriptions(registration);
    } else {
      Serial.println("MQTT not connected at boot -- will keep retrying from loop().");
    }
  }


  // --Main Program Loop-------------------------
  // Code here runs continuously

  void loop() {
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();   // return value not otherwise needed here -- next check below covers it
    }

    if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
      if (connectMQTT()) {
        // Subscriptions don't survive a broker reconnect -- must be redone
        // every time a new MQTT session is established, not just at boot.
        applyTopicSubscriptions(registration);
      }
    }

    mqttClient.poll();

    // USER CODE
    // Write your code in the function call at the bottom of the sketch.
    // Update this call with any return values and inputs that you add.
    userLoop();

    // Publish anything the user's code (or an incoming pub+sub message with
    // a registered onReceive handler) has marked dirty since the last pass.
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
        applyTopicSubscriptions(registration);
      }
    }
  }

  // -- Filesystem Functions --------------------

  // Verify that flash storage has been partitioned 
  bool partitionCheck() {
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
    }

    // Check Partition 2: OTA
    MBRBlockDevice p2(bd, 2);
    FATFileSystem fs2("ota_chk");
    if (fs2.mount(&p2) == 0) {
      fs2.unmount();
    } else {
      ok = false;
    }

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
      }
    }

    bd->deinit();
    return ok;
  }

  // -- JSON Helper Functions -------------------

  // Copies a JSON string field into a fixed-size destination buffer.
  // Rejects (rather than silently truncates) values that don't fit.
  bool copyJsonStringField(JsonVariantConst obj, const char* key,
                            char* dest, size_t destSize, bool required) {
    if (obj[key].isNull()) {
      if (required) {
        Serial.print("[CONFIG] Missing required field: ");
        Serial.println(key);
        return false;
      }
      dest[0] = '\0';
      return true;
    }

    const char* value = obj[key];
    if (value == nullptr) {
      Serial.print("[CONFIG] Field is not a string: ");
      Serial.println(key);
      return false;
    }

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
    }

    strcpy(dest, value);  // length already validated above
    return true;
  }

  // Parses an IPAddress from a dotted-string JSON field, e.g. "10.0.0.101".
  bool copyJsonIPField(JsonVariantConst obj, const char* key, IPAddress &dest, bool required) {
    if (obj[key].isNull()) {
      if (required) {
        Serial.print("[CONFIG] Missing required IP field: ");
        Serial.println(key);
        return false;
      }
      return true;
    }
    const char* value = obj[key];
    if (value == nullptr || !dest.fromString(value)) {
      Serial.print("[CONFIG] Invalid IP address for field: ");
      Serial.println(key);
      return false;
    }
    return true;
  }

  // Parses the MAC address from a JSON array of 6 integers, e.g. [0xDE, 0xAD, ...].
  bool copyJsonMacField(JsonVariantConst obj, const char* key, byte* dest) {
    JsonArrayConst arr = obj[key].as<JsonArrayConst>();
    if (arr.isNull() || arr.size() != 6) {
      Serial.print("[CONFIG] Missing or malformed MAC address field: ");
      Serial.println(key);
      return false;
    }
    int i = 0;
    for (JsonVariantConst v : arr) {
      dest[i++] = (byte)v.as<int>();
    }
    return true;
  }

  TopicType parseTopicType(const char* s) {
    if (strcmp(s, "BOOL")    == 0) return TopicType::TOPIC_BOOL;
    if (strcmp(s, "FLOAT32") == 0) return TopicType::TOPIC_FLOAT32;
    if (strcmp(s, "STR")     == 0) return TopicType::TOPIC_STR;
    return TopicType::TOPIC_UNKNOWN;
  }

  // Shared fields common to all three topic entry types.
  bool parseCommonTopicFields(JsonObjectConst entry, char* nameOut, char* topicOut,
                              bool &pubOut, bool &subOut, bool &willOut) {
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

  bool parseStrTopicEntry(JsonObjectConst entry, TopicEntryStr &out) {
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

  bool parseFloatTopicEntry(JsonObjectConst entry, TopicEntryFloat &out) {
    bool ok = parseCommonTopicFields(entry, out.name, out.topic, out.pub_flag, out.sub_flag, out.is_will);

    JsonVariantConst initField = entry["init_value"];
    if (initField.isNull() || !initField.is<float>()) {
      Serial.println("[CONFIG] FLOAT32 topic missing or invalid init_value.");
      return false;
    }
    out.init_value = initField.as<float>();
    out.current_value = out.init_value;

    out.data_type   = TopicType::TOPIC_FLOAT32;
    out.update_flag = false;

    return ok;
  }

  bool parseBoolTopicEntry(JsonObjectConst entry, TopicEntryBool &out) {
    bool ok = parseCommonTopicFields(entry, out.name, out.topic, out.pub_flag, out.sub_flag, out.is_will);

    JsonVariantConst initField = entry["init_value"];
    if (initField.isNull() || !initField.is<bool>()) {
      Serial.println("[CONFIG] BOOL topic missing or invalid init_value.");
      return false;
    }
    out.init_value = initField.as<bool>();
    out.current_value = out.init_value;

    out.data_type   = TopicType::TOPIC_BOOL;
    out.update_flag = false;

    return ok;
  }

  // Parses the topics arrays from the JSON configuration files
  bool loadTopicsFromJson(JsonVariantConst topicsField, EnrollmentConfig &cfg) {
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
  }

  // Converts the topic type enum in the configuration structure to strings to save back to the configuration file.
  const char* topicTypeToString(TopicType t) {
    switch (t) {
      case TopicType::TOPIC_BOOL:    return "BOOL";
      case TopicType::TOPIC_FLOAT32: return "FLOAT32";
      case TopicType::TOPIC_STR:     return "STR";
      default:                       return "UNKNOWN";
    }
  }

  // Writes the common fields in a topic entry to memory.
  // Since init_value has different data types depending on the kind of topic, 
  // it has to be handled in a separate command.
  JsonObject writeCommonTopicFields(JsonArray &arr, const char* name, const char* topic,
                                    TopicType type, bool pub, bool sub, bool will) {
    JsonObject entryObj = arr.add<JsonObject>();
    entryObj["name"]      = name;
    entryObj["topic"]     = topic;
    entryObj["data_type"] = topicTypeToString(type);
    entryObj["pub_flag"]  = pub;
    entryObj["sub_flag"]  = sub;
    entryObj["is_will"]   = will;
    return entryObj;
  }

  // -- Enrollment Functions --------------------

  // Enrollment file validation functions

  // Scans all three topic arrays and confirms at most one topic is marked
  // is_will. Returns false (and rejects the config) if more than one is found.
  // willTypeOut / willIndexOut are only meaningful when this returns true:
  //   willTypeOut:  0 = topicsStr, 1 = topicsFloat, 2 = topicsBool, -1 = no will topic defined
  //   willIndexOut: index into the corresponding array, -1 if no will topic defined
  bool validateSingleWill(const EnrollmentConfig &cfg, int &willTypeOut, int &willIndexOut) {
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

  // Warns (does not reject) if any topic has both pub_flag and sub_flag set.
  // This is sometimes intentional (e.g. a writable setpoint that echoes back),
  // but it's also exactly the shape of a feedback-loop bug, so it's worth
  // surfacing every time rather than silently allowing it.
  void checkPubSubOverlap(const EnrollmentConfig &cfg) {
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

  bool validateConfig(const EnrollmentConfig &cfg) {
    int willType, willIndex;
    if (!validateSingleWill(cfg, willType, willIndex)) return false;
    checkPubSubOverlap(cfg); // advisory only -- logs a warning, never rejects
    // future rules go here
    return true;
  }

  // Load configuration file
  // Reads persisted config from LittleFS into the given struct.
  bool loadConfigFromFlash(EnrollmentConfig &cfg) {
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
    ok &= copyJsonStringField(doc, "wifi_ssid",     cfg.wifiSSID,     MAX_SSID_LEN,     true);
    ok &= copyJsonStringField(doc, "wifi_password", cfg.wifiPassword, MAX_PASSWORD_LEN, true);
    ok &= copyJsonStringField(doc, "broker_password", cfg.brokerPassword, MAX_PASSWORD_LEN, true);

    ok &= copyJsonMacField(doc, "mac", cfg.mac);

    // IPAddress -> dotted-decimal string
    ok &= copyJsonIPField(doc, "ip",        cfg.localIP_wifi, true);
    ok &= copyJsonIPField(doc, "gateway",   cfg.gateway,      true);
    ok &= copyJsonIPField(doc, "subnet",    cfg.subnet,       true);
    ok &= copyJsonIPField(doc, "dns",       cfg.dns,          true);
    ok &= copyJsonIPField(doc, "broker_ip", cfg.brokerIP,     true);

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
      return false; // validateConfig's sub-checks already log the specific reason
    }

    Serial.println("[CONFIG] Configuration loaded successfully.");
    return true;
  }

  // Save configuration file
  bool saveConfigToFlash(const EnrollmentConfig &cfg) {
    if (!validateConfig(cfg)) {
      Serial.println("[CONFIG] Refusing to save an invalid configuration.");
      return false;
    }

    JsonDocument doc;

    doc["wifi_ssid"]       = cfg.wifiSSID;
    doc["wifi_password"]   = cfg.wifiPassword;
    doc["broker_password"] = cfg.brokerPassword;

    doc["ip"]        = cfg.localIP_wifi.toString();
    doc["gateway"]   = cfg.gateway.toString();
    doc["subnet"]    = cfg.subnet.toString();
    doc["dns"]       = cfg.dns.toString();
    doc["broker_ip"] = cfg.brokerIP.toString();

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

  // -- WiFi and Ethernet Functions -------------

  // WiFi connection
  // Makes 10 attempts to connect to the WiFi.
  // Provides troubleshooting for the connection if it fails.
  bool connectWiFi() {
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

  // -- MQTT Functions --------------------------

  bool connectMQTT() {
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

    Serial.print("Connecting to MQTT broker");
    int attempts = 0;
    const int MAX_ATTEMPTS = 10;

    while (!mqttClient.connect(registration.brokerIP, BROKER_PORT)) {
      int error = mqttClient.connectError();
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

  // MQTT Subscription function
  void applyTopicSubscriptions(EnrollmentConfig &cfg) {
    bool ok = true;

    for (int i = 0; i < cfg.topicsStrCount; i++) {
      if (cfg.topicsStr[i].sub_flag) {
        if (!mqttClient.subscribe(cfg.topicsStr[i].topic, 1)) {
          Serial.print("[MQTT] Failed to subscribe: ");
          Serial.println(cfg.topicsStr[i].topic);
          ok = false;
        }
      }
    }

    for (int i = 0; i < cfg.topicsFloatCount; i++) {
      if (cfg.topicsFloat[i].sub_flag) {
        if (!mqttClient.subscribe(cfg.topicsFloat[i].topic, 1)) {
          Serial.print("[MQTT] Failed to subscribe: ");
          Serial.println(cfg.topicsFloat[i].topic);
          ok = false;
        }
      }
    }

    for (int i = 0; i < cfg.topicsBoolCount; i++) {
      if (cfg.topicsBool[i].sub_flag) {
        if (!mqttClient.subscribe(cfg.topicsBool[i].topic, 1)) {
          Serial.print("[MQTT] Failed to subscribe: ");
          Serial.println(cfg.topicsBool[i].topic);
          ok = false;
        }
      }
    }

    mqttClient.onMessage(onMqttMessage);
    Serial.print("[MQTT] Subscriptions applied");
    Serial.println(ok ? "." : " (with errors).");
  }

  // MQTT Message handler functions

  // Topic lookup
  TopicLookupResult findTopicByTopicString(EnrollmentConfig &cfg, const String &topic) {
    for (int i = 0; i < cfg.topicsStrCount; i++)
      if (topic == cfg.topicsStr[i].topic) return { TopicArray::STR, i };

    for (int i = 0; i < cfg.topicsFloatCount; i++)
      if (topic == cfg.topicsFloat[i].topic) return { TopicArray::FLOAT, i };

    for (int i = 0; i < cfg.topicsBoolCount; i++)
      if (topic == cfg.topicsBool[i].topic) return { TopicArray::BOOL, i };

    return {}; // arrayType stays NONE, index stays -1
  }

  // Searches all three topic arrays for a given name, regardless of type.
  // Used to catch a patch that tries to introduce/update a name that already
  // exists under a *different* type than the patch declares.
TopicLookupResult findTopicByNameAnywhere(const EnrollmentConfig &cfg, const char* name) {
    int idx = findIndexByName(cfg.topicsStr, cfg.topicsStrCount, name);
    if (idx >= 0) return { TopicArray::STR, idx };

    idx = findIndexByName(cfg.topicsFloat, cfg.topicsFloatCount, name);
    if (idx >= 0) return { TopicArray::FLOAT, idx };

    idx = findIndexByName(cfg.topicsBool, cfg.topicsBoolCount, name);
    if (idx >= 0) return { TopicArray::BOOL, idx };

    return {}; // arrayType stays NONE
  }

  // On Message handler
  void onMqttMessage(int messageSize) {
    String topic = mqttClient.messageTopic();

    String payloadStr;
    payloadStr.reserve(messageSize);
    while (mqttClient.available()) {
      payloadStr += (char)mqttClient.read();
    }

    Serial.print("[MQTT] Message on "); Serial.print(topic);
    Serial.print(" ("); Serial.print(messageSize); Serial.println(" bytes)");

    TopicLookupResult found = findTopicByTopicString(registration, topic);

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

  // MQTT Publish Handlers
  bool publishStrTopic(TopicEntryStr &t) {
    mqttClient.beginMessage(t.topic, false, 1);
    mqttClient.print(t.current_value);   // was init_value -- publish the live value, not the boot default
    int result = mqttClient.endMessage();
    if (result == 1) t.update_flag = false;
    return result == 1;
  }

  bool publishFloatTopic(TopicEntryFloat &t) {
    mqttClient.beginMessage(t.topic, false, 1);
    mqttClient.print(t.current_value, 4);
    int result = mqttClient.endMessage();
    if (result == 1) t.update_flag = false;
    return result == 1;
  }

  bool publishBoolTopic(TopicEntryBool &t) {
    mqttClient.beginMessage(t.topic, false, 1);
    mqttClient.print(t.current_value ? "true" : "false");
    int result = mqttClient.endMessage();
    if (result == 1) t.update_flag = false;
    return result == 1;
  }

  void publishUpdatedOutputs(EnrollmentConfig &cfg) {
    for (int i = 0; i < cfg.topicsStrCount; i++)
      if (cfg.topicsStr[i].pub_flag && cfg.topicsStr[i].update_flag) publishStrTopic(cfg.topicsStr[i]);

    for (int i = 0; i < cfg.topicsFloatCount; i++)
      if (cfg.topicsFloat[i].pub_flag && cfg.topicsFloat[i].update_flag) publishFloatTopic(cfg.topicsFloat[i]);

    for (int i = 0; i < cfg.topicsBoolCount; i++)
      if (cfg.topicsBool[i].pub_flag && cfg.topicsBool[i].update_flag) publishBoolTopic(cfg.topicsBool[i]);
  }

  // Sprout Accessor Functions

  // Topic Finder Function
  // Overloaded by array type -- same function name, compiler picks the right
  // one based on the argument type. Not templates, just three plain functions
  // that happen to share a name.
  int findIndexByName(const TopicEntryBool* arr, int count, const char* name) {
    for (int i = 0; i < count; i++) if (strcmp(arr[i].name, name) == 0) return i;
    return -1;
  }
  int findIndexByName(const TopicEntryFloat* arr, int count, const char* name) {
    for (int i = 0; i < count; i++) if (strcmp(arr[i].name, name) == 0) return i;
    return -1;
  }
  int findIndexByName(const TopicEntryStr* arr, int count, const char* name) {
    for (int i = 0; i < count; i++) if (strcmp(arr[i].name, name) == 0) return i;
    return -1;
  }

  // Resolves accessor base indices by calling findIndexByName
  void resolveSproutAccessorIndices(EnrollmentConfig &cfg) {
    idxDeviceStatus     = findIndexByName(cfg.topicsStr,   cfg.topicsStrCount,   "device_status");
    idxReconfiguration  = findIndexByName(cfg.topicsStr,   cfg.topicsStrCount,   "reconfiguration");
    idxButton1          = findIndexByName(cfg.topicsBool,  cfg.topicsBoolCount,  "input_button1");
    idxButton2          = findIndexByName(cfg.topicsBool,  cfg.topicsBoolCount,  "input_button2");
    idxInputNum1        = findIndexByName(cfg.topicsFloat, cfg.topicsFloatCount, "input_num1");
    idxInputNum2        = findIndexByName(cfg.topicsFloat, cfg.topicsFloatCount, "input_num2");
    idxInputStr         = findIndexByName(cfg.topicsStr,   cfg.topicsStrCount,   "input_str");
    idxOutStatus1       = findIndexByName(cfg.topicsBool,  cfg.topicsBoolCount,  "output_status1");
    idxOutStatus2       = findIndexByName(cfg.topicsBool,  cfg.topicsBoolCount,  "output_status2");
    idxOutNum1          = findIndexByName(cfg.topicsFloat, cfg.topicsFloatCount, "output_num1");
    idxOutNum2          = findIndexByName(cfg.topicsFloat, cfg.topicsFloatCount, "output_num2");
    idxOutStr           = findIndexByName(cfg.topicsStr,   cfg.topicsStrCount,   "output_str");

    if (idxDeviceStatus < 0 || idxReconfiguration < 0 || idxButton1 < 0 || idxButton2 < 0 ||
        idxInputNum1 < 0 || idxInputNum2 < 0 || idxInputStr < 0 || idxOutStatus1 < 0 ||
        idxOutStatus2 < 0 || idxOutNum1 < 0 || idxOutNum2 < 0 || idxOutStr < 0) {
      Serial.println("FATAL: one or more base Sprout topics not found by name in loaded configuration.");
      Serial.println("Halting -- this configuration file is missing required topic(s).");
      while (1);
    }
  }

  // -- Read accessors --
  bool        sproutButton1()   { return registration.topicsBool[idxButton1].current_value; }
  bool        sproutButton2()   { return registration.topicsBool[idxButton2].current_value; }
  float       sproutInputNum1() { return registration.topicsFloat[idxInputNum1].current_value; }
  float       sproutInputNum2() { return registration.topicsFloat[idxInputNum2].current_value; }
  const char* sproutInputStr()  { return registration.topicsStr[idxInputStr].current_value; }

  // -- Write accessors -- set current_value AND update_flag, so the next
  // publishDirtyOutputs() pass actually sends it.
  void sproutSetOutputStatus1(bool v) {
    registration.topicsBool[idxOutStatus1].current_value = v;
    registration.topicsBool[idxOutStatus1].update_flag = true;
  }
  void sproutSetOutputStatus2(bool v) {
    registration.topicsBool[idxOutStatus2].current_value = v;
    registration.topicsBool[idxOutStatus2].update_flag = true;
  }
  void sproutSetOutputNum1(float v) {
    registration.topicsFloat[idxOutNum1].current_value = v;
    registration.topicsFloat[idxOutNum1].update_flag = true;
  }
  void sproutSetOutputNum2(float v) {
    registration.topicsFloat[idxOutNum2].current_value = v;
    registration.topicsFloat[idxOutNum2].update_flag = true;
  }
  bool sproutSetOutputStr(const char* v) {
    size_t len = strlen(v);
    if (len >= MAX_STR_LEN) {
      Serial.println("[SPROUT] output_str value too long, ignoring set.");
      return false;
    }
    strcpy(registration.topicsStr[idxOutStr].current_value, v);
    registration.topicsStr[idxOutStr].update_flag = true;
    return true;
  }
  bool sproutSetDeviceStatus(const char* v) {
    size_t len = strlen(v);
    if (len >= MAX_STR_LEN) {
      Serial.println("[SPROUT] device_status value too long, ignoring set.");
      return false;
    }
    strcpy(registration.topicsStr[idxDeviceStatus].current_value, v);
    registration.topicsStr[idxDeviceStatus].update_flag = true;
    return true;
  }

  // -- Reconfiguration Functions ---------------

  // Used to protect essential topics from being changed, such as the reconfiguration topic itself
  bool isReservedTopicName(const char* name) {
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

  // Updates the top level fields, such as WiFi and MQTT credentials
  // If these are not present in the reconfiguration message, the old value is retained. 
  // This function does not allow deletion of these fields.
  bool applyTopLevelPatch(JsonVariantConst patch, EnrollmentConfig &scratch) {
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

  // Remove a topic. 
  // Prevents removal of the previously identified reserved topics.

  bool applyTopicRemovals(JsonVariantConst patch, EnrollmentConfig &scratch) {
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

  // Add or update a topic
  bool applyTopicPatch(JsonVariantConst patch, EnrollmentConfig &scratch) {
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

  bool processReconfiguration(const char* payload) {
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
      resolveSproutAccessorIndices(registration);
      return false;
    }

    resolveSproutAccessorIndices(registration); // topic set may have changed -- indices must be re-resolved
    pendingWifiReconnect |= wifiChanged;
    pendingMqttReconnect |= mqttChanged;

    Serial.println("[RECONFIG] Reconfiguration applied and saved successfully.");
    return true;
  }

  // ============================================================
  // USER CODE
  // ============================================================
  // Read inputs:   sproutButton1(), sproutButton2(), sproutInputNum1(),
  //                sproutInputNum2(), sproutInputStr()
  // Write outputs: sproutSetOutputStatus1(bool), sproutSetOutputStatus2(bool),
  //                sproutSetOutputNum1(float), sproutSetOutputNum2(float),
  //                sproutSetOutputStr(const char*)
  //
  // Runs once per loop() pass, right after MQTT messages are received and
  // before outputs are published, so anything you set here goes out this
  // same pass. Avoid delay() or long blocking calls here -- they'll stall
  // MQTT servicing and WiFi reconnect logic for the whole device, not just
  // your own code.
  void userLoop() {
    // Starter example -- replace with your own logic:
    if (sproutButton1()) {
      sproutSetOutputStatus1(true);
    }
  }