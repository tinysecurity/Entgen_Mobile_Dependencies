#ifndef SPROUT_API_H
#define SPROUT_API_H

// -- Runtime Constants --
#define SOCKET_BUFFER_SIZE 4096
#define STATIC_IN_BUFFER_SIZE 2096
#define STATIC_OUT_BUFFER_SIZE 1024
#define ENABLE_DEBUG
#define ENABLE_ERROR_STRING

#define MAX_SSID_LEN        32
#define MAX_PASSWORD_LEN    64
#define MAX_USERNAME_LEN    32
#define MAX_NAME_LEN        32
#define MAX_TOPIC_LEN       64
#define MAX_STR_LEN         600
#define MAX_TOPICS          16
#define MAX_CERT_LEN        2048
#define CONFIG_FILE_PATH    "/fs/config.json"
#define CONFIG_TEMP_PATH    "/fs/config.json.tmp"
#define CONFIG_READ_BUFFER_SIZE 64000
#define BROKER_PORT         8883

#include <Arduino.h>
#include <WiFi.h>
#include <BlockDevice.h>
#include <LittleFileSystem.h>
#include <MBRBlockDevice.h>
#include <FATFileSystem.h>
#include <SPI.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <mbed.h>
#include <ESP_SSLClient.h>
#include <WiFiUdp.h>

using namespace mbed;

// -- Data model — moved from the sketch, since every method signature
// below needs these types to exist before the class declaration.
enum class TopicType : uint8_t { TOPIC_BOOL, TOPIC_FLOAT32, TOPIC_STR, TOPIC_UNKNOWN };
enum class TopicArray : uint8_t { NONE, STR, FLOAT, BOOL };

struct TopicLookupResult {
  TopicArray arrayType = TopicArray::NONE;
  int        index     = -1;
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
  void        (*onReceive)(TopicEntryStr &entry) = nullptr;
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
  void        (*onReceive)(TopicEntryFloat &entry) = nullptr;
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
  void        (*onReceive)(TopicEntryBool &entry) = nullptr;
};

struct EnrollmentConfig {
  char            deviceId[MAX_NAME_LEN];
  char            wifiSSID[MAX_SSID_LEN];
  char            wifiPassword[MAX_PASSWORD_LEN];
  char            mqttUsername[MAX_USERNAME_LEN];
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
  char            clientCertPem[MAX_CERT_LEN];
  char            clientKeyPem[MAX_CERT_LEN];
  char            caCertPem[MAX_CERT_LEN];
};

class Sprout_API {
public:
  // -- Lifecycle --
  Sprout_API(String devType);              // was "Sprout(...)" — name must match class
  void   initSprout();                     // partitionCheck + fs mount + loadConfigFromFlash + resolveSproutAccessorIndices
  bool   connectWiFi();                    // renamed from setNetworkParams — that name implied config, not the connect action
  bool connectMQTT();
  void publishOtaCompleteStatus();
  void   poll();                           // NEW — replaces loop()'s body: WiFi/MQTT reconnect checks, mqttClient.poll(),
                                            // publishUpdatedOutputs, reconfig-flag handling. Odyssey's loop() becomes:
                                            //   sprout.poll(); userLoop();

  // -- Config load/save/validate --
  bool loadConfigFromFlash(EnrollmentConfig &cfg);
  bool saveConfigToFlash(const EnrollmentConfig &cfg);
  bool validateConfig(const EnrollmentConfig &cfg);          // MISSING from your draft — validateSingleWill + checkPubSubOverlap wrapper
  bool validateSingleWill(const EnrollmentConfig &cfg, int &willTypeOut, int &willIndexOut); // MISSING
  void checkPubSubOverlap(const EnrollmentConfig &cfg);
  bool partitionCheck();
  unsigned long getNtpTime();

  // -- JSON helpers --
  bool copyJsonStringField(JsonVariantConst obj, const char* key, char* dest, size_t destSize, bool required);
  bool copyJsonIPField(JsonVariantConst obj, const char* key, IPAddress &dest, bool required);
  bool copyJsonMacField(JsonVariantConst obj, const char* key, byte* dest);
  TopicType   parseTopicType(const char* s);                 // MISSING — needed by loadTopicsFromJson
  const char* topicTypeToString(TopicType t);
  bool parseCommonTopicFields(JsonObjectConst entry, char* nameOut, char* topicOut, bool &pubOut, bool &subOut, bool &willOut);
  bool parseStrTopicEntry(JsonObjectConst entry, TopicEntryStr &out);
  bool parseFloatTopicEntry(JsonObjectConst entry, TopicEntryFloat &out);
  bool parseBoolTopicEntry(JsonObjectConst entry, TopicEntryBool &out);
  bool loadTopicsFromJson(JsonVariantConst topicsField, EnrollmentConfig &cfg); // fixed signature — original only took cfg, but needs the field
  JsonObject writeCommonTopicFields(JsonArray &arr, const char* name, const char* topic, TopicType type, bool pub, bool sub, bool will);

  // -- Topic lookup — MISSING from your draft, needed by onMqttMessage/reconfig --
  TopicLookupResult findTopicByTopicString(const String &topic);
// header:
TopicLookupResult findTopicByNameAnywhere(const EnrollmentConfig &cfg, const char* name);
  int findIndexByName(const TopicEntryStr* arr, int count, const char* name);
  int findIndexByName(const TopicEntryFloat* arr, int count, const char* name);
  int findIndexByName(const TopicEntryBool* arr, int count, const char* name);

  // -- MQTT publish/subscribe — MISSING, this is the whole point of the library --
  void applyTopicSubscriptions();
  void publishUpdatedOutputs(EnrollmentConfig &cfg);
  bool publishStrTopic(TopicEntryStr &t);
  bool publishFloatTopic(TopicEntryFloat &t);
  bool publishBoolTopic(TopicEntryBool &t);

  // -- Reconfiguration — MISSING --
  bool isReservedTopicName(const char* name);
  bool applyTopLevelPatch(JsonVariantConst patch, EnrollmentConfig &scratch);
  bool applyTopicRemovals(JsonVariantConst patch, EnrollmentConfig &scratch);
  bool applyTopicPatch(JsonVariantConst patch, EnrollmentConfig &scratch);
  bool processReconfiguration(const char* payload);

  // -- Sprout accessors — MISSING. These are what userLoop() actually calls;
  // without them exposed, the library can't replace the sketch's globals. --
  void resolveSproutAccessorIndices();
  bool        sproutButton1();
  bool        sproutButton2();
  float       sproutInputNum1();
  float       sproutInputNum2();
  const char* sproutInputStr();
  bool sproutButton1IsNew();
  bool sproutButton2IsNew();
  bool sproutInputNum1IsNew();
  bool sproutInputNum2IsNew();
  bool sproutInputStrIsNew();
  void sproutButton1Ack();
  void sproutButton2Ack();
  void sproutInputNum1Ack();
  void sproutInputNum2Ack();
  void sproutInputStrAck();
  void sproutSetOutputStatus1(bool v);
  void sproutSetOutputStatus2(bool v);
  void sproutSetOutputNum1(float v);
  void sproutSetOutputNum2(float v);
  bool sproutSetOutputStr(const char* v);
  bool sproutSetDeviceStatus(const char* v);

  // -- Direct config access for the user sketch (e.g. printing debug lengths) --
  EnrollmentConfig registration;   // MISSING as a member — was a free global; every
                                    // method above needs to read/write ONE shared
                                    // instance per device, which only works as a
                                    // member, not a parameter passed in each time.

private:
  BlockDevice*      bd;
LittleFileSystem  fs;
  // -- Device objects — MISSING as members. These were free globals in the
  // sketch; a library needs its own client instances owned by the class,
  // not relying on the sketch to declare them. --
  WiFiClient       wifiClient;
  ESP_SSLClient    sslClient;
  MqttClient       mqttClient;
  String           deviceType;      // from constructor — reserved for the
                                     // ESP32/Opta filesystem-abstraction branch
  EnrollmentConfig reconfigScratch;

  bool             pendingWifiReconnect = false;
  bool             pendingMqttReconnect = false;

  int idxDeviceStatus = -1, idxReconfiguration = -1;
  int idxButton1 = -1, idxButton2 = -1;
  int idxInputNum1 = -1, idxInputNum2 = -1, idxInputStr = -1;
  int idxOutStatus1 = -1, idxOutStatus2 = -1;
  int idxOutNum1 = -1, idxOutNum2 = -1, idxOutStr = -1;

  static void onMqttMessageThunk(int messageSize);  // MISSING — MqttClient's
                                                      // onMessage() takes a
                                                      // free function pointer,
                                                      // not a member function;
                                                      // needs a static thunk +
                                                      // a static instance
                                                      // pointer to dispatch
                                                      // back into the real
                                                      // (non-static) handler.
  static Sprout_API* _instance;
  void onMqttMessage(int messageSize);
};

#endif