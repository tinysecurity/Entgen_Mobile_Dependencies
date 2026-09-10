  // ============================================================
  // Entwise Sprout's Odyssey Sketch  v0.1
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
  #include "FATFileSystem.h"
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

  // -- [USER CONFIGURATION] ---------------------

  // Command Parser

  // Command Limits
  #define MAX_INPUT_LEN   140   // Maximum number of characters in the input string
  #define MAX_TOKENS      8     // Maximum number of tokens we'll bother finding (to save memory)
  #define MAX_TOKEN_LEN   16    // Maximum number of characters in one word

  // Vocabulary
  enum Verb {
    VERB_NONE = 0,
    VERB_GO,
    VERB_USE,
    VERB_SLASH,
    VERB_DODGE,
    VERB_SAY,
    VERB_HELP,
    VERB_LOOK
  };
  
  enum Target {
    TARGET_NONE = 0,
    // directions
    TARGET_NORTH,
    TARGET_SOUTH,
    TARGET_EAST,
    TARGET_WEST,
    // items and interactables
    TARGET_CROOK,
    TARGET_LEVER,
    TARGET_PANEL,
    TARGET_BIRD,
    TARGET_CYCLOPS
  };

  enum Phrase {
    PHRASE_NONE = 0,
    PHRASE_MYPLUK,
    PHRASE_FRIEND,
    PHRASE_BLUE
  };

  // Lookup tables - match strings to the enum vocabularies we previously defined
  struct VerbEntry {
    const char* word;
    Verb verb;
  };

  struct TargetEntry {
    const char* word;
    Target target;
  };

  struct PhraseEntry {
    const char* word;
    Phrase phrase;
  };

  const VerbEntry VERB_TABLE[] = {
    { "go",     VERB_GO},
    { "use",    VERB_USE},
    { "slash",  VERB_SLASH},
    { "dodge",  VERB_DODGE},
    { "say",    VERB_SAY},
    { "look",   VERB_LOOK},
    { "help",   VERB_HELP}
  };
  const int VERB_TABLE_SIZE = sizeof(VERB_TABLE) / sizeof(VERB_TABLE[0]);

  const TargetEntry TARGET_TABLE[] = {
    { "north",   TARGET_NORTH},
    { "south",   TARGET_SOUTH },
    { "east",    TARGET_EAST },
    { "west",    TARGET_WEST },
    { "crook",   TARGET_CROOK },
    { "lever",   TARGET_LEVER },
    { "panel",   TARGET_PANEL },
    { "bird",    TARGET_BIRD },
    { "cyclops", TARGET_CYCLOPS }
  };
  const int TARGET_TABLE_SIZE = sizeof(TARGET_TABLE) / sizeof(TARGET_TABLE[0]);

  const PhraseEntry PHRASE_TABLE[] = {
    { "mypluk",     PHRASE_MYPLUK},
    { "friend",     PHRASE_FRIEND},
    { "blue",       PHRASE_BLUE}
  };
  const int PHRASE_TABLE_SIZE = sizeof(PHRASE_TABLE) / sizeof(PHRASE_TABLE[0]);

  // Parse - passes over the tokens, classifies them into {verb, target, phrase} components.
  struct ParsedCommand {
    Verb verb;
    Target target;
    Phrase phrase;
    bool too_many;      // indicates that the user sent more than one verb, target, or phrase, so command cannot be understood
  };

  // State Machine

  // Inventory and state flags, defined so they can be packed into one uint16
  #define FLAG_HAS_CROOK            (1 << 0)
  #define FLAG_HAS_BIRD             (1 << 1)
  #define FLAG_PANEL_SOLVED         (1 << 2)
  #define FLAG_SHEEP_DEFEATED       (1 << 3)
  #define FLAG_RIDDLE_SOLVED        (1 << 4)
  #define FLAG_CIPHER_SOLVED        (1 << 5)
  #define FLAG_DOOR_OPENED          (1 << 6)

  // Room and State Identifiers

  // RoomId groups states with a shared physical location.
  enum RoomId {
    ROOM_BEACH,
    ROOM_FOREST,
    ROOM_CLIFF,
    ROOM_CAVE,
    ROOM_MAZE_1,
    ROOM_MAZE_2,
    ROOM_MAZE_3,
    ROOM_MAZE_4,
    ROOM_MAZE_5,
    ROOM_MAZE_6,
    ROOM_MAZE_7,
    ROOM_MAZE_8,
    ROOM_MAZE_9
  };

  // StateId enumerates every distinct state the player can occupy.
  // This enum's order MUST exactly match the STATES[] array
  // defined below, as StateId is used as a direct index into that array in order to save memory.
  enum StateId {
    STATE_BEACH,
    STATE_FOREST,
    STATE_CLIFF,
    STATE_CLIFF_EMPTY,
    STATE_CAVE,
    STATE_CAVE_EMPTY,
    STATE_MAZE_1,
    STATE_MAZE_2,
    STATE_MAZE_3,
    STATE_MAZE_4,
    STATE_MAZE_4_EMPTY,
    STATE_MAZE_5,
    STATE_MAZE_5_EMPTY,
    STATE_MAZE_6,
    STATE_MAZE_7,
    STATE_MAZE_8,
    STATE_MAZE_8_BIRD,
    STATE_MAZE_9,
    STATE_MAZE_9_EMPTY,
    STATE_MAZE_9_NO_BIRD,
    STATE_END_LEAVE,
    STATE_END_FRIENDS_SOLVE,
    STATE_END_FRIENDS_BIRD,
    STATE_COUNT // sentinel, always last in the list, gives array size
  };

  // -- Combat Function -----------------------------------------

  enum CombatPhase {
    COMBAT_INACTIVE,          // Not yet engaged or already defeated
    COMBAT_AWAITING_DEFENSE,  // waiting on DODGE
    COMBAT_AWAITING_ATTACK   // dodge succeeded, waiting on SLASH
  };

  const uint8_t SHEEP_DEFENSE_PATTERNS[3] = {
    90,
    53,
    226
  };

  const uint8_t SHEEP_ATTACK_PATTERNS[3] = {
    150,
    105,
    195
  };

  // Sheep fight flavor text
  const char* SHEEP_DEFENSE_FLAVOR[3] = {
    "The sheep darts in to nip at Sprout from the left.\n"
    "Sprout reads the attack as a 90.\n",
    "The sheep snaps at Sprout in an uneven strike pattern.\n" 
    "Sprout hastily recognizes attack 53!\n",
    "The sheep rears, readying a heavy overhead slam.\n"
    "Sprout winces, knowing that the 226 attack hurts to parry...\n"
    "though it's worse to get hit.\n"
  };

  const char* SHEEP_ATTACK_FLAVOR[3] = {
    "The sheep staggers, exposing itself low along its right side.\n"
    "The sheep's vulnerability reads as 150 to Sprout's expert eye.",
    "The sheep stumbles forward, frantically flailing to keep its feet.\n" 
    "In the chaos, Sprout spots opportunities all along 105.\n",
    "The sheep's attack left it overextended,\n"
    "leaving its upper body wide open along lane 195.\n"
  };

  const char* SHEEP_MISS_TEXT =
    "Sprout's blow just bounces off the sheep's fluffy wool.\n"
    "It seems to be unaffected.\n";

  const char* SHEEP_HIT_TEXT = 
    "Sprout smacks the sheep firmly on the nose with the CROOK.\n"
    "The sheep pauses, stunned. It then lets out an apologetic bleat\n"
    "and steps out of Sprout's way.";

  // StateCommand / State
  //
  // A StateCommand either:
  //   (a) has handler == NULL -> it's a simple transition. The dispatcher
  //       sets current_state = next_state and auto-prints that state's
  //       description. Use this for plain movement and anything with no
  //       side effects beyond "you are now somewhere else."
  //   (b) has handler != NULL -> the handler owns EVERYTHING: setting
  //       current_state (if it changes at all), setting flags, and
  //       calling sproutSetOutputStr() with whatever text is appropriate.
  //       The dispatcher will NOT auto-print a description after calling
  //       a handler. Use this for combat, puzzles, riddles, item pickups,
  //       or any transition that needs custom text or logic.
  //
  // condition gates whether the command is usable at all right now (e.g.
  // "use lever" requires FLAG_HAS_CROOK). Pass a function that always
  // returns true if there's no precondition.

  // Game State structure

  struct GameState {
    StateId     current_state;
    uint16_t    flags;

    // Sheep combat states
    CombatPhase combat_phase;
    uint8_t     sheep_defense_pattern;
    uint8_t     sheep_attack_pattern;
  };

  GameState game_state = { STATE_BEACH, 0, COMBAT_INACTIVE, 0, 0 };

  struct StateCommand {
    Verb      verb;
    Target    target;
    bool      (*condition)(GameState*);
    StateId   next_state;                 // used only when handler == NULL
    void      (*handler)(GameState*, ParsedCommand);     // NULL for simple transitions
  };

  struct State {
    RoomId              room;                       
    const char*         description;      // constant string - the room or state text
    const StateCommand* commands;
    uint8_t             command_count;
  };

  // Print state description prototype

  void print_current_description(GameState* state);

  // Condition helper prototypes

  bool always_allowed(GameState* state);
  bool crook_not_yet_taken(GameState* state);
  bool crook_taken(GameState* state);
  bool door_open(GameState* state);
  bool panel_not_solved(GameState* state);
  bool panel_solved(GameState* state);
  bool sheep_not_defeated(GameState* state);
  bool sheep_defeated(GameState* state);
  bool riddle_not_solved(GameState* state);
  bool riddle_solved(GameState* state);
  bool cipher_not_solved(GameState* state);
  bool cipher_solved(GameState* state);
  bool sheep_awaiting_defense(GameState* s);
  bool sheep_awaiting_attack(GameState* s);

  // Combat prototypes

  void cmd_sheep_dodge(GameState* state, ParsedCommand cmd);
  void cmd_sheep_slash(GameState* state, ParsedCommand cmd);

  // -- Wait-for-Acknowledgement --------------------------------

  typedef void (*AckCallback)(GameState*);

  bool        awaiting_ack        = false;
  AckCallback pending_ack_action  = NULL;

  const char* MSG_WAITING_FOR_ACK = "Awaiting Acknowledgement. Press A to continue.";

  // -- Game States and Descriptions ----------------------------

  // Generic replies (constants)

  const char* MSG_TOO_MANY        = "One thing at a time - try a single command.\n";
  const char* MSG_UNRECOGNIZED    = "I don't understand that. Type HELP to get a list of valid commands.\n";
  const char* MSG_NOT_HERE        = "That doesn't work here.\n";
  const char* MSG_MISSING_AUX     = "You don't have what you need to do that yet.\n";
  const char* MSG_HELP            = 
    "Commands: GO, USE, SLASH, DODGE, SAY, HELP, LOOK\n"
    "Add a direction to GO like NORTH to move that direction.\n"
    "The USE commands picks up or interacts with objects in the room.\n"
    "SLASH to attack, hitting if you find a hole in the defense.\n"
    "Enter an integer 0 - 7 in NumInput1 to pick an attack direction.\n"
    "DODGE negates an attack. Enter your defense in NumInput2.\n"
    "Enter an integer between 0 and 255.\n"
    "SAY is how Sprout speaks. Specify to whom he speaks.\n"
    "HELP lists valid commands.\n"
    "LOOK tells you what Sprout sees in the current room.\n";

  // Beach State

  const StateCommand beach_commands[] = {
    { VERB_GO, TARGET_EAST, always_allowed, STATE_FOREST, NULL }
  };
  const uint8_t beach_commands_count = sizeof(beach_commands) / sizeof(beach_commands[0]);

  const char* BEACH_DESCRIPTION = 
    "Sprout finds himself on a sandy beach.\n"
    "His boat rests in the shallows where he had tied it in place.\n"
    "The sails are tattered and there are cracks in the timbers.\n"
    "He needs to find materials to fix the craft, and some rich\n"
    "soil and fresh water to feed himself.\n"
    "He looks at the forest rising in the EAST.\n"
    "Perhaps he can find supplies there?\n";

  // Forest State

  void cliff_handler(GameState* state, ParsedCommand cmd);
  void cave_handler(GameState* state, ParsedCommand cmd);

  const StateCommand forest_commands[] = {
    { VERB_GO, TARGET_WEST, always_allowed, STATE_BEACH, NULL },
    { VERB_GO, TARGET_NORTH, always_allowed, STATE_CLIFF, cliff_handler },
    { VERB_GO, TARGET_EAST, always_allowed, STATE_CAVE, cave_handler }
  };
  const uint8_t forest_commands_count = sizeof(forest_commands) / sizeof(forest_commands[0]);

  const char* FOREST_DESCRIPTION =
    "Sprout pauses in the forest where the path splits.\n"
    "He paushes to catch his breath. The path had been steep.\n"
    "He idly considers what kind of creature had left this trail.\n"
    "To the WEST, the trail drops down to the sandy beach.\n"
    "To the EAST, it continues up the mountain side.\n"
    "The path splits to the NORTH, rising more gently towards a gap\n"
    "in the canopy.\n";

  // Cliff State

  void crook_handler(GameState* state, ParsedCommand cmd);

  const StateCommand cliff_commands[] = {
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_FOREST, NULL },
    { VERB_USE, TARGET_CROOK, crook_not_yet_taken, STATE_CLIFF, crook_handler }
  };
  const uint8_t cliff_commands_count = sizeof(cliff_commands) / sizeof(cliff_commands[0]);

  const char* CLIFF_DESCRIPTION =
    "Sprout emerges from the forest into a grassy meadow\n"
    "at the top of a cliff overlooking the sea.\n"
    "Wind rustles the grass, cooling his vegetable skin.\n"
    "There is a large boulder in the middle of the meadow.\n"
    "Strangely, a long CROOK, taller than Sprout himself,\n"
    "rests against it. Apparently someone lives here!\n"
    "Sprout glances at the forest to the SOUTH,\n"
    "speculating about who it might be.\n";
  
  const char* CROOK_COLLECTION_DESCRIPTION = 
    "Sprout hefts the CROOK. It's a bit awkward to carry,\n"
    "but he's sure it will come in handy.\n"
    "He finally manages to settle it against one shoulder.\n"
    "It will be comfortable enough there.\n";

  // Cliff Empty State

  const StateCommand cliff_empty_commands[] = {
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_FOREST, NULL }
  };
  const uint8_t cliff_empty_commands_count = sizeof(cliff_empty_commands) / sizeof(cliff_empty_commands[0]);

  const char* CLIFF_EMPTY_DESCRIPTION =
    "Sprout sits on the boulder in the clearing overlooking the sea.\n"
    "It feels good to rest and enjoy the beauty of the sky.\n"
    "After a few minutes he looks back to the forest in the SOUTH.\n"
    "He should be getting on with his adventure.\n";

  // Cave State

  void lever_handler(GameState* state, ParsedCommand cmd);

  const StateCommand cave_commands[] = {
    { VERB_GO, TARGET_WEST, always_allowed, STATE_FOREST, NULL },
    { VERB_USE, TARGET_LEVER, crook_taken, STATE_CAVE, lever_handler }
  };
  const uint8_t cave_commands_count = sizeof(cave_commands) / sizeof(cave_commands[0]);

  const char* CAVE_DESCRIPTION =
    "Sprout enters a cave. Not a nasty, dripping cave,\n"
    "but a homely one, with piles of grass in the corners\n"
    "where some creature had clearly been laying down.\n"
    "Light filters in from the entrance to the forest in the WEST.\n"
    "To the EAST a large door is set into the back of the cave.\n"
    "There is a LEVER next to the door, high out of Sprout's reach.\n";
  
  const char* DOOR_OPENING_DESCRIPTION = 
    "Sprout holds the CROOK at one end and reeeaches for the LEVER.\n"
    "After a couple of failed attempts, he manages to hook it.\n"
    "With a mighty tug, he pulls the lever down!\n"
    "There is a click, and the door swings open.\n";

  // Cave Empty State - the player has opened the door in the cave

  void maze_4_handler(GameState* state, ParsedCommand cmd);

  const StateCommand cave_empty_commands[] = {
    { VERB_GO, TARGET_WEST, always_allowed, STATE_FOREST, NULL },
    { VERB_GO, TARGET_EAST, door_open, STATE_MAZE_4, maze_4_handler }
  };
  const uint8_t cave_empty_commands_count = sizeof(cave_empty_commands) / sizeof(cave_empty_commands[0]);

  const char* CAVE_EMPTY_DESCRIPTION =
    "Torchlight flickers from the door in the EAST wall of the cave.\n"
    "Daylight filters in from the entrance in the WEST.\n"
    "Whomever lives here must live inside the mountain!\n"
    "Sprout tingles with excitement to meet them.\n";

  // Maze 1 State

  const StateCommand maze_1_commands[] = {
    { VERB_GO, TARGET_NORTH, always_allowed, STATE_MAZE_4, maze_4_handler },
    { VERB_GO, TARGET_EAST, always_allowed, STATE_MAZE_2, NULL }
  };
  const uint8_t maze_1_commands_count = sizeof(maze_1_commands) / sizeof(maze_1_commands[0]);

  const char* MAZE_1_DESCRIPTION = 
    "A large stove dominates this room,\n"
    "connected to a natural chimney in the rock."
    "A corridor slopes gently up to the NORTH,\n"
    "and there is a drop-off into a pool of water to the EAST.\n";

  // Maze 2 State

  const StateCommand maze_2_commands[] = {
    { VERB_GO, TARGET_WEST, always_allowed, STATE_MAZE_1, NULL }
  };
  const uint8_t maze_2_commands_count = sizeof(maze_2_commands) / sizeof(maze_2_commands[0]);

  const char* MAZE_2_DESCRIPTION = 
    "Sprout dangles his root-feet into the pool and drinks deeply.\n"
    "Ah, so refreshing!\n"
    "Satisfied, he rises and turns back towards the kitchen\n"
    "in the WEST.\n";
  
  // Maze 3 State

  const StateCommand maze_3_commands[] = {
    { VERB_GO, TARGET_NORTH, always_allowed, STATE_MAZE_6, NULL }
  };
  const uint8_t maze_3_commands_count = sizeof(maze_3_commands) / sizeof(maze_3_commands[0]);

  const char* MAZE_3_DESCRIPTION = 
    "This room is painted in the blue colors of the sea and skies.\n"
    "White puffy clouds float above the water.\n"
    "The waves sparkle with glints of silver at their crests.\n"
    "Seagulls are painted in places where the painter made a mistake.\n"
    "The only exit is a door to the NORTH.";
  
  // Maze 4 State
  
  void maze_5_handler(GameState* state, ParsedCommand cmd);
  void panel_handler(GameState* state, ParsedCommand cmd);

  const StateCommand maze_4_commands[] = {
    { VERB_GO, TARGET_WEST, door_open, STATE_CAVE, cave_handler },
    { VERB_GO, TARGET_NORTH, always_allowed, STATE_MAZE_7, NULL },
    { VERB_GO, TARGET_EAST, panel_solved, STATE_MAZE_5, maze_5_handler },
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_MAZE_1, NULL },
    { VERB_USE, TARGET_PANEL, panel_not_solved, STATE_MAZE_4_EMPTY, panel_handler }
  };
  const uint8_t maze_4_commands_count = sizeof(maze_4_commands) / sizeof(maze_4_commands[0]);

  const char* MAZE_4_DESCRIPTION = 
    "The entryway has three exits:\n"
    "the door back to the cave in the WEST,\n"
    "a corridor sloping gently SOUTH,\n"
    "and an arch to another room in the NORTH.\n"
    "The wall in the EAST has a puzzle on a clay PANEL.\n"
    "If 4 is 2 and 1 and 12 is 3 and 2 and 1, what is 27?\n"
    "Enter our answer in NumInput1 and NumInput2, then USE the PANEL.\n";

  const char* PANEL_SOLVED_DESCRIPTION =
    "Sprout drags his branch finger through the clay,\n"
    "scribing 3 and 1, the prime factors of 27.\n"
    "He owes old Euclid an apology.\n"
    "He was certain he'd never use this in the real world!\n"
    "A moment later there is a rumbling of gears,\n"
    "and the wall slides away.\n";

  const char* PANEL_WRONG_DESCRIPTION = 
    "Sprout drags his branch finger through the clay,\n"
    "scribing his answers, but nothing happens.\n"
    "He frowns.\n"
    "If only he'd listened more to Euclid, his math teacher!\n";

  // Maze 4 Empty State

  const StateCommand maze_4_empty_commands[] = {
    { VERB_GO, TARGET_WEST, door_open, STATE_CAVE, cave_handler },
    { VERB_GO, TARGET_NORTH, always_allowed, STATE_MAZE_7, NULL },
    { VERB_GO, TARGET_EAST, panel_solved, STATE_MAZE_5, maze_5_handler },
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_MAZE_1, NULL }
  };
  const uint8_t maze_4_empty_commands_count = sizeof(maze_4_empty_commands) / sizeof(maze_4_empty_commands[0]);

  const char* MAZE_4_EMPTY_DESCRIPTION = 
    "The entryway has four exits.\n"
    "There is a door back to the cave in the WEST,\n"
    "a corridor sloping gently SOUTH,\n"
    "and an arch to another room in the NORTH.\n"
    "The wall in the EAST has slid open, revealing a long hallway.\n";

  // Maze 5 State

  const StateCommand maze_5_commands[] = {
    { VERB_GO, TARGET_WEST, panel_solved, STATE_MAZE_4, maze_4_handler },
    { VERB_GO, TARGET_EAST, sheep_defeated, STATE_MAZE_6, NULL },
    { VERB_DODGE, TARGET_NONE, sheep_awaiting_defense, STATE_MAZE_5, cmd_sheep_dodge },
    { VERB_SLASH, TARGET_NONE, sheep_awaiting_attack,  STATE_MAZE_5, cmd_sheep_slash }
  };
  const uint8_t maze_5_commands_count = sizeof(maze_5_commands) / sizeof(maze_5_commands[0]);

  const char* MAZE_5_DESCRIPTION = 
    "The hallway extends to the EAST and the WEST.\n"
    "Sprout takes a turn around a corner and comes face to face\n"
    "with a large four-footed crature covered with white wool.\n"
    "It fixes him with a glare from strange square-pupiled eyes.\n"
    "Sprout's sap races through his capillaries.\n"
    "It's a sheep! And sheep love to eat tender sprouts.\n"
    "He readies the CROOK as hunger blooms in the sheep's eyes.\n"
    "The world blurs around him.\n";

  // Maze 5 Empty State

  const StateCommand maze_5_empty_commands[] = {
    { VERB_GO, TARGET_WEST, panel_solved, STATE_MAZE_4, maze_4_handler },
    { VERB_GO, TARGET_EAST, sheep_defeated, STATE_MAZE_6, NULL }
  };
  const uint8_t maze_5_empty_commands_count = sizeof(maze_5_empty_commands) / sizeof(maze_5_empty_commands[0]);

  const char* MAZE_5_EMPTY_DESCRIPTION = 
    "The hallway extends to the EAST and the WEST.\n"
    "An embarrased sheep presses itself against the wall.\n"
    "It avoids looking Sprout in the eyes as he passes by.\n";

  // Maze 6 State

  void maze_9_handler(GameState* state, ParsedCommand cmd);

  const StateCommand maze_6_commands[] = {
    { VERB_GO, TARGET_NORTH, always_allowed, STATE_MAZE_9, maze_9_handler },
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_MAZE_3, NULL }
  };
  const uint8_t maze_6_commands_count = sizeof(maze_6_commands) / sizeof(maze_6_commands[0]);

  const char* MAZE_6_DESCRIPTION = 
    "This room contains a large loom.\n"
    "It has a half-finished project on it: a blanket showing a sunset.\n"
    "There are piles of wool in baskets next to it.\n"
    "Sprout runs a branch finger along the seat. Hmm, it is dusty.\n"
    "There is a hallway going WEST,\n" 
    "a door to the SOUTH, and a passage to the NORTH.\n";

  // Maze 7 State

  const StateCommand maze_7_commands[] = {
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_MAZE_4, maze_4_handler }
  };
  const uint8_t maze_7_commands_count = sizeof(maze_7_commands) / sizeof(maze_7_commands[0]);

  const char* MAZE_7_DESCRIPTION = 
    "This room seems to be a cozy den.\n"
    "There's a couple of reclining couches around a short table.\n"
    "A few scrolls are tucked into a case on one wall.\n"
    "Sprout pulls one out and reads the title.\n"
    "Ipykz vm aol Tlkpaalyhulhu\n"
    "Sprout frowns. This is not a language he's familiar with.\n"
    "At least the picture of the seagull is interesting.\n"
    "An arch in the SOUTH leads to the entryway.\n";

  // Maze 8 State

  void maze_8_handler(GameState* state, ParsedCommand cmd);
  void cipher_handler(GameState* state, ParsedCommand cmd);

  const StateCommand maze_8_commands[] = {
    { VERB_GO, TARGET_EAST, always_allowed, STATE_END_LEAVE, NULL },
    { VERB_SAY, TARGET_CYCLOPS, always_allowed, STATE_END_FRIENDS_SOLVE, cipher_handler }
  };
  const uint8_t maze_8_commands_count = sizeof(maze_8_commands) / sizeof(maze_8_commands[0]);

  const char* MAZE_8_DESCRIPTION = 
    "Sprout enters the room and meets a massive cyclops!\n"
    "She is reading through a thick monocle, but stands\n"
    "when she notices that she has a visitor.\n"
    "\'Olssv! P yhylsf nla nblzaz.\n"
    "Zwlhr, Mypluk, huk il dlsjvtl!\'\n"
    "The cyclops looks at Sprout expectantly.\n"
    "Sprout shifts nervously, looking back at the door to the EAST.\n";

  const char* INCORRECT_CIPHER = 
    "The cyclops's eye narrows in confusion.\n"
    "She does not seem to understand Sprout's words,\n"
    "or perhaps they are not what she expected.\n";

  const char* CORRECT_CIPHER = 
    "The cyclops's face lights up!\n"
    "\'Nylha! P ht hivba av ohcl kpuuly.\n"
    "Dvbsk fvb sprl zvtl zald\?\'\n"
    "She frowns, looking Sprout over.\n"
    "\'Zpssf tl, vm jvbyzl uva.\n"
    "Fvb hyl h wshua!\n"
    "Sla tl nla fvb h upjl wva vm tvpza kpya.\'\n";

  // Maze 8 Bird State

  void translation_handler(GameState* state, ParsedCommand cmd);

  const StateCommand maze_8_bird_commands[] = {
    { VERB_GO, TARGET_EAST, always_allowed, STATE_END_LEAVE, NULL },
    { VERB_SAY, TARGET_CYCLOPS, always_allowed, STATE_END_FRIENDS_BIRD, translation_handler }
  };
  const uint8_t maze_8_bird_commands_count = sizeof(maze_8_bird_commands) / sizeof(maze_8_bird_commands[0]);

  const char* MAZE_8_BIRD_DESCRIPTION = 
    "Sprout enters the room and meets a massive cyclops!\n"
    "She is reading through a thick monocle, but stands\n"
    "when she notices that she has a visitor.\n"
    "\'Olssv! P yhylsf nla nblzaz.\n"
    "Zwlhr, Mypluk, huk il dlsjvtl!\'\n"
    "\'She asked if you are a friend,\' the bird helpfully supplies.\n"
    "Both the bird and the cyclops look at Sprout expectently.\n"
    "Sprout shifts nervously, looking back at the door to the EAST.\n";

  const char* INCORRECT_TRANSLATION = 
    "The bird sighs, which is an impressive thing to hear a bird do.\n"
    "\'No, that's not what you need to say.\n"
    "She said \"speak friend\".\'\n";

  const char* CORRECT_TRANSLATION = 
    "The cyclops's face lights up!\n"
    "\'Nylha! P ht hivba av ohcl kpuuly.\n"
    "Dvbsk fvb sprl zvtl zald\?\'\n"
    "The bird chirps back:\n"
    "\'Vm jvbyzl uva, ol pz h wshua.\'\n"
    "The bird looks at Sprout.\n"
    "\'She was going to feed you a stew,\n" 
    "which is not appropriate at all.\n" 
    "Umm...what do plants eat?\'\n";

  // Maze 9 State

  void riddle_handler(GameState* state, ParsedCommand cmd);

  const StateCommand maze_9_commands[] = {
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_MAZE_6, NULL },
    { VERB_GO, TARGET_WEST, riddle_solved, STATE_MAZE_8, maze_8_handler },
    { VERB_SAY, TARGET_BIRD, always_allowed, STATE_MAZE_9_EMPTY, riddle_handler }
  };
  const uint8_t maze_9_commands_count = sizeof(maze_9_commands) / sizeof(maze_9_commands[0]);

  const char* MAZE_9_DESCRIPTION = 
    "Sprout walks into what seems to be a solarium.\n"
    "A clever assortment of mirrors directs light into the room\n"
    "through the sheer rock.\n"
    "There is a passage going SOUTH and a closed door to the WEST.\n"
    "Plants fill the space, and Sprout feels a warm contentment\n"
    "when the sun falls on his leaf.\n"
    "A bird, previously hidden in the leaves, suddenly chirps up.\n"
    "\'What do you call it when a bird is happy, but a person is sad\?\'\n";

  const char* INCORRECT_RIDDLE = 
    "\'No, that's not the answer to the riddle.\n"
    "Try again!\'\n";

  const char* CORRECT_RIDDLE = 
    "\'Don't you find that odd\?\' the bird inquires,\n"
    "then it flies over and taps on the door with its beak.\n"
    "\'Lualy\' a voice says from inside.\n";

  // Maze 9 Empty State

  void bird_handler(GameState* state, ParsedCommand cmd);

  const StateCommand maze_9_empty_commands[] = {
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_MAZE_6, NULL },
    { VERB_GO, TARGET_WEST, riddle_solved, STATE_MAZE_8, maze_8_handler },
    { VERB_USE, TARGET_BIRD, always_allowed, STATE_MAZE_9_NO_BIRD, bird_handler }
  };
  const uint8_t maze_9_empty_commands_count = sizeof(maze_9_empty_commands) / sizeof(maze_9_empty_commands[0]);

  const char* MAZE_9_EMPTY_DESCRIPTION = 
    "Sprout looks around the solarium, taking in the healthy plants.\n"
    "Whomever is behind the door to the WEST, they can't be that bad.\n"
    "Still, he finds himself a little nervous to go on.\n"
    "He looks back to the SOUTH where he came in.\n"
    "\'Well\?\' the bird asks, \'Are you coming\?\'\n";

  const char* COLLECT_BIRD = 
    "Sprout approaches the door, but then pauses and lifts one branch.\n"
    "The bird hops down and rests on the proffered limb.\n"
    "\'This is comfy,\' the bird says. \'You're alright, kid.\'\n";

  // Maze 9 No Bird State

  const StateCommand maze_9_no_bird_commands[] = {
    { VERB_GO, TARGET_SOUTH, always_allowed, STATE_MAZE_6, NULL },
    { VERB_GO, TARGET_WEST, riddle_solved, STATE_MAZE_8, maze_8_handler }
  };
  const uint8_t maze_9_no_bird_commands_count = sizeof(maze_9_no_bird_commands) / sizeof(maze_9_no_bird_commands[0]);

  const char* MAZE_9_NO_BIRD_DESCRIPTION = 
    "Sprout stands in front of a door going WEST.\n"
    "The solarium, with a passage going SOUTH, is behind him.\n"
    "He takes a deep breath through his leaf.\n" 
    "He is ready to meet the owner of this home.\n";
  
  // End Leave State

  const StateCommand end_leave_commands[] = {
    { VERB_GO, TARGET_EAST, always_allowed, STATE_MAZE_8, maze_8_handler }
  };
  const uint8_t end_leave_commands_count = sizeof(end_leave_commands) / sizeof(end_leave_commands[0]);

  const char* END_LEAVE_DESCRIPTION = 
    "The cyclops seems sad when Sprout leaves, but does not stop him.\n"
    "Sprout makes his way out of the mountain home.\n"
    "He is able to find what he needs to fix his ship,\n"
    "and the next day he continues on his journey.\n"
    "As he sails away, he sees the cyclops up on the cliff.\n"
    "She waves farewell. He waves back.\n"
    "He'll always wonder what the woman was saying.\n";

  // End Friends Solve State

  const StateCommand end_friends_solve_commands[] = {
    { VERB_GO, TARGET_EAST, always_allowed, STATE_MAZE_8, maze_8_handler }
  };
  const uint8_t end_friends_solve_commands_count = sizeof(end_friends_solve_commands) / sizeof(end_friends_solve_commands[0]);

  const char* END_FRIENDS_SOLVE_DESCRIPTION = 
    "Sprout and the cyclops enjoy a fine meal.\n"
    "Afterwards, she gives him a pot in the solarium to rest.\n"
    "Over the next few days Sprout learns more of her language.\n"
    "After a while Sprout starts to feel restless.\n"
    "\'Aohur fvb mvy fvby ovzwpahspaf,\' he says,\n"
    "\'iba pa pz aptl mvy tl av slhcl.\'\n"
    "The cyclops nods, and gets him a gift - a sail woven on her loom!\n"
    "When Sprout sails away, he sees his new friend up on the cliff.\n"
    "He waves goodbye, hoping that some day he'll visit again.\n"
    "He will have many new stories to share!\n";

  // End Friends Bird State

  const StateCommand end_friends_bird_commands[] = {
    { VERB_GO, TARGET_EAST, always_allowed, STATE_MAZE_8, maze_8_handler }
  };
  const uint8_t end_friends_bird_commands_count = sizeof(end_friends_bird_commands) / sizeof(end_friends_bird_commands[0]);

  const char* END_FRIENDS_BIRD_DESCRIPTION = 
    "Sprout, the cyclops, and the bird enjoy a meal.\n"
    "The cyclops eats stew, the bird pecks at seeds,\n"
    "and Sprout lounges in a pot of moist earth.\n"
    "They enjoy many days of interesting conversation.\n"
    "The bird even helps Sprout make friends with the sheep.\n"
    "It speaks their language too, it seems.\n"
    "After some time, Sprout must set out again.\n"
    "He leaves with gifts of good dirt, clean water, and lots of wool.\n"
    "He has made many friends here. It makes him happy.\n";

  // The state table
  // Order MUST match the StateId enum.
   const State STATES[STATE_COUNT] = {
    { ROOM_BEACH,       BEACH_DESCRIPTION,              beach_commands,               beach_commands_count},
    { ROOM_FOREST,      FOREST_DESCRIPTION,             forest_commands,              forest_commands_count },
    { ROOM_CLIFF,       CLIFF_DESCRIPTION,              cliff_commands,               cliff_commands_count },
    { ROOM_CLIFF,       CLIFF_EMPTY_DESCRIPTION,        cliff_empty_commands,         cliff_empty_commands_count },
    { ROOM_CAVE,        CAVE_DESCRIPTION,               cave_commands,                cave_commands_count },
    { ROOM_CAVE,        CAVE_EMPTY_DESCRIPTION,         cave_empty_commands,          cave_empty_commands_count },
    { ROOM_MAZE_1,      MAZE_1_DESCRIPTION,             maze_1_commands,              maze_1_commands_count },
    { ROOM_MAZE_2,      MAZE_2_DESCRIPTION,             maze_2_commands,              maze_2_commands_count },
    { ROOM_MAZE_3,      MAZE_3_DESCRIPTION,             maze_3_commands,              maze_3_commands_count },
    { ROOM_MAZE_4,      MAZE_4_DESCRIPTION,             maze_4_commands,              maze_4_commands_count },
    { ROOM_MAZE_4,      MAZE_4_EMPTY_DESCRIPTION,       maze_4_empty_commands,        maze_4_empty_commands_count },
    { ROOM_MAZE_5,      MAZE_5_DESCRIPTION,             maze_5_commands,              maze_5_commands_count },
    { ROOM_MAZE_5,      MAZE_5_EMPTY_DESCRIPTION,       maze_5_empty_commands,        maze_5_empty_commands_count },
    { ROOM_MAZE_6,      MAZE_6_DESCRIPTION,             maze_6_commands,              maze_6_commands_count },
    { ROOM_MAZE_7,      MAZE_7_DESCRIPTION,             maze_7_commands,              maze_7_commands_count },
    { ROOM_MAZE_8,      MAZE_8_DESCRIPTION,             maze_8_commands,              maze_8_commands_count },
    { ROOM_MAZE_8,      MAZE_8_BIRD_DESCRIPTION,        maze_8_bird_commands,         maze_8_bird_commands_count },
    { ROOM_MAZE_9,      MAZE_9_DESCRIPTION,             maze_9_commands,              maze_9_commands_count },
    { ROOM_MAZE_9,      MAZE_9_EMPTY_DESCRIPTION,       maze_9_empty_commands,        maze_9_empty_commands_count },
    { ROOM_MAZE_9,      MAZE_9_NO_BIRD_DESCRIPTION,     maze_9_no_bird_commands,      maze_9_no_bird_commands_count },
    { ROOM_MAZE_8,      END_LEAVE_DESCRIPTION,          end_leave_commands,           end_leave_commands_count },
    { ROOM_MAZE_8,      END_FRIENDS_SOLVE_DESCRIPTION,  end_friends_solve_commands,   end_friends_solve_commands_count },
    { ROOM_MAZE_8,      END_FRIENDS_BIRD_DESCRIPTION,   end_friends_bird_commands,    end_friends_bird_commands_count },
  };

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

  // Parses an IPAddress from a dotted-string JSON field, e.g. "10.0.0.101".
  bool copyJsonIPField(JsonVariantConst obj, const char* key, IPAddress &dest, bool required) {
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

  // Parses the MAC address from a JSON array of 6 integers, e.g. [0xDE, 0xAD, ...].
  bool copyJsonMacField(JsonVariantConst obj, const char* key, byte* dest) {
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
  bool sproutButton1IsNew() { return registration.topicsBool[idxButton1].update_flag; }
  bool sproutButton2IsNew() { return registration.topicsBool[idxButton2].update_flag; }
  bool sproutInputNum1IsNew() { return registration.topicsFloat[idxInputNum1].update_flag; }
  bool sproutInputNum2IsNew() { return registration.topicsFloat[idxInputNum2].update_flag; }
  bool sproutInputStrIsNew() { return registration.topicsStr[idxInputStr].update_flag; }
  void sproutButton1Ack() { registration.topicsBool[idxButton1].update_flag = false; }
  void sproutButton2Ack() { registration.topicsBool[idxButton2].update_flag = false; }
  void sproutInputNum1Ack() { registration.topicsFloat[idxInputNum1].update_flag = false; }
  void sproutInputNum2Ack() { registration.topicsFloat[idxInputNum2].update_flag = false; }
  void sproutInputStrAck() { registration.topicsStr[idxInputStr].update_flag = false; }


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
    run_game_state_machine(&game_state);
  }

  // ============================================================
  // USER FUNCTIONS
  // ============================================================

  // -- State Machine -------------------------------------------

  // State description printer

  void print_current_description(GameState* state) {
    sproutSetOutputStr(get_state(state->current_state)->description);
  }

  // Condition helpers

  bool always_allowed(GameState* state) {
    return true;
  }

  bool crook_not_yet_taken(GameState* state) {
    return !(state->flags & FLAG_HAS_CROOK);
  }

  bool crook_taken(GameState* state) {
    return (state->flags & FLAG_HAS_CROOK);
  }

  bool door_open(GameState* state) {
    return (state->flags & FLAG_DOOR_OPENED);
  }

  bool panel_not_solved(GameState* state) {
    return !(state->flags & FLAG_PANEL_SOLVED);
  }

  bool panel_solved(GameState* state) {
    return (state->flags & FLAG_PANEL_SOLVED);
  }

  bool sheep_not_defeated(GameState* state) {
    return !(state->flags & FLAG_SHEEP_DEFEATED);
  }

  bool sheep_defeated(GameState* state) {
    return (state->flags & FLAG_SHEEP_DEFEATED);
  }

  bool riddle_not_solved(GameState* state) {
    return !(state->flags & FLAG_RIDDLE_SOLVED);
  }

  bool riddle_solved(GameState* state) {
    return (state->flags & FLAG_RIDDLE_SOLVED);
  }

  bool cipher_not_solved(GameState* state) {
    return !(state->flags & FLAG_CIPHER_SOLVED);
  }

  bool cipher_solved(GameState* state) {
    return (state->flags & FLAG_CIPHER_SOLVED);
  }

  bool sheep_awaiting_defense(GameState* s) { 
    return s->combat_phase == COMBAT_AWAITING_DEFENSE; 
  }

  bool sheep_awaiting_attack(GameState* s)  { 
    return s->combat_phase == COMBAT_AWAITING_ATTACK; 
  }
  
  // Dispatcher

  void handle_command(GameState* state, ParsedCommand cmd) {
    if (cmd.too_many) {
      sproutSetOutputStr(MSG_TOO_MANY);
      return;
    }
    
    if (cmd.verb == VERB_NONE) {
      sproutSetOutputStr(MSG_UNRECOGNIZED);
      return;
    }

    if (cmd.verb == VERB_LOOK) {
      print_current_description(state);
      return;
    }

    if (cmd.verb == VERB_HELP) {
      sproutSetOutputStr(MSG_HELP);
      return;
    }

    const State* current = get_state(state->current_state);
    const StateCommand* match = find_matching_command(current, cmd.verb, cmd.target);

    if (match == NULL) {
      sproutSetOutputStr(MSG_NOT_HERE);
      return;
    }

    if (!match->condition(state)) {
      sproutSetOutputStr(MSG_MISSING_AUX);
      return;
    }

    if (match->handler != NULL) {
      // Handler owns the transition and the output text entirely.
      match->handler(state, cmd);
    } else {
      // Simple transition -- move, then auto-print the new description.
      state->current_state = match->next_state;
      print_current_description(state);
    }
  }

  // State data extractors

  const State* get_state(StateId id) {
    return &STATES[id];
  }

  const StateCommand* find_matching_command(const State* state, Verb verb, Target target) {
    for (uint8_t i = 0; i < state->command_count; i++) {
      if (state->commands[i].verb == verb && state->commands[i].target == target) {
        return &state->commands[i];
      }
    }
    return NULL;
  }

  // Entry point for userLoop
  // Call this once per loop pass to process any new user commands.

  void run_game_state_machine(GameState* state) {
    if (awaiting_ack) {
      handle_acknowledgement_wait(state);
      return;
    }
    
    if (!sproutInputStrIsNew()) {
      return;
    }

    ParsedCommand cmd = parse_input(sproutInputStr());
    sproutInputStrAck();

    handle_command(state, cmd);
  }

  // -- Command Parser ------------------------------------------

  // Tokenize - splits inputs into MAX_TOKENS lowercase words and returns number of tokens found
  int tokenize(const char* input, char tokens[MAX_TOKENS][MAX_TOKEN_LEN]) {
    char buffer[MAX_INPUT_LEN];
    strncpy(buffer, input, MAX_INPUT_LEN - 1);
    buffer[MAX_INPUT_LEN - 1] = '\0';

    // lowercase in place to remove case sensitivity
    for (int i = 0; buffer[i]; i++) {
      buffer[i] = tolower(buffer[i]);
    }

    int count = 0;
    char* token = strtok(buffer, " ");
    while (token != NULL && count < MAX_TOKENS) {
      strncpy(tokens[count], token, MAX_TOKEN_LEN - 1);
      tokens[count][MAX_TOKEN_LEN - 1] = '\0';
      count ++;
      token = strtok(NULL, " ");
    }
    return count;
  }

  // Lookups - check vocabulary tables to see if inputs match any known commands
  Verb lookup_verb(const char* word) {
    for (int i = 0; i < VERB_TABLE_SIZE; i++) {
      if (strcmp(word, VERB_TABLE[i].word) == 0) {
        return VERB_TABLE[i].verb;
      }
    }
    return VERB_NONE;
  }

  Target lookup_target(const char* word) {
    for (int i = 0; i < TARGET_TABLE_SIZE; i++) {
      if (strcmp(word, TARGET_TABLE[i].word) == 0) {
        return TARGET_TABLE[i].target;
      }
    }
    return TARGET_NONE;
  }

  Phrase lookup_phrase(const char* word) {
    for (int i = 0; i < PHRASE_TABLE_SIZE; i++) {
      if (strcmp(word, PHRASE_TABLE[i].word) == 0) {
        return PHRASE_TABLE[i].phrase;
      }
    }
    return PHRASE_NONE;
  }

  // Parse - passes over the tokens, classifies them into {verb, target, phrase} components.

  ParsedCommand parse_input(const char* input) {
    ParsedCommand cmd = { VERB_NONE, TARGET_NONE, PHRASE_NONE, false };

    char tokens[MAX_TOKENS][MAX_TOKEN_LEN];
    int token_count = tokenize(input, tokens);

    for (int i = 0; i < token_count; i++) {
      Verb v = lookup_verb(tokens[i]);
      if (v != VERB_NONE) {
        if (cmd.verb != VERB_NONE) {
            cmd.too_many = true; // a second verb showed up
        }
        cmd.verb = v;
        continue;
      }

      Target t = lookup_target(tokens[i]);
      if (t != TARGET_NONE) {
        if (cmd.target != TARGET_NONE) {
          cmd.too_many = true; // a second target showed up
        }
        cmd.target = t;
        continue;
      }

      Phrase p = lookup_phrase(tokens[i]);
      if (p != PHRASE_NONE) {
        if (cmd.phrase != TARGET_NONE) {
          cmd.too_many = true; // a second target showed up
        }
        cmd.phrase = p;
        continue;
      }
    }

    return cmd;
  }

  // -- Wait-for-Acknowledgement --------------------------------

  void prompt_for_ack(const char* message, AckCallback continuation) {
    char buffer[160];
    snprintf(buffer, sizeof(buffer), "%s Press A to continue.", message);
    sproutSetOutputStr(buffer);

    // Clear stale Button1 press
    sproutButton1Ack();

    awaiting_ack        = true;
    pending_ack_action  = continuation;
  }

  // Called every pass while awaiting_ack is true instead of normal command dispatch
  void handle_acknowledgement_wait(GameState* state) {
    if (sproutButton1IsNew()) {
      sproutButton1Ack();
      AckCallback continuation = pending_ack_action;
      awaiting_ack        = false;
      pending_ack_action  = NULL;
      if (continuation != NULL) {
        continuation(state);
      } 
      return;
    }

    // Ignore other inputs until the player acknowledges the pause
    if (sproutButton2IsNew())   { sproutButton2Ack();   sproutSetOutputStr(MSG_WAITING_FOR_ACK); return; }
    if (sproutInputNum1IsNew()) { sproutInputNum1Ack(); sproutSetOutputStr(MSG_WAITING_FOR_ACK); return; }
    if (sproutInputNum2IsNew()) { sproutInputNum2Ack(); sproutSetOutputStr(MSG_WAITING_FOR_ACK); return; }
    if (sproutInputStrIsNew())  { sproutInputStrAck();  sproutSetOutputStr(MSG_WAITING_FOR_ACK); return; }

  }

  // -- Combat Function -----------------------------------------

  // Bit check helpers

  // Random number generator to select the sheep's attacks and defenses
  uint8_t pick_pattern_index() {
    return random(0, 3);
  }

  // Defense check: the player must submit the bitwise negation of the 
  // shown defense pattern. Returns true on a correct dodge.
  bool check_defense(uint8_t player_input, uint8_t defense_pattern) {
    return player_input == (uint8_t)(~defense_pattern);
  }

  // Attack check: player picks a direction 0-7. A hit lands if that bit
  // position is UNSET (0) in the attack pattern -- i.e., an open lane.
  bool check_attack(uint8_t direction, uint8_t attack_pattern) {
    return ((attack_pattern >> direction) & 0x01) ==0;
  }

  // Combat Acknowledgement continuation

  void sheep_knockback_continue(GameState* state) {
    state->combat_phase = COMBAT_INACTIVE;
    state->current_state = STATE_MAZE_4_EMPTY;
    print_current_description(state);
  }

  void sheep_miss_continue(GameState* state) {
    state->combat_phase = COMBAT_AWAITING_DEFENSE;
    uint8_t idx = pick_pattern_index();
    state->sheep_defense_pattern = SHEEP_DEFENSE_PATTERNS[idx];
    sproutSetOutputStr(SHEEP_DEFENSE_FLAVOR[idx]);
  }

  void sheep_victory_continue(GameState* state) {
    state->combat_phase = COMBAT_INACTIVE;
    state->current_state = STATE_MAZE_5_EMPTY;
    print_current_description(state);
  }

  // Sheep combat initialization
  void start_sheep_combat (GameState* state) {
    state->combat_phase = COMBAT_AWAITING_DEFENSE;
    uint8_t idx = pick_pattern_index();
    state->sheep_defense_pattern = SHEEP_DEFENSE_PATTERNS[idx];

    sproutSetOutputStr(SHEEP_DEFENSE_FLAVOR[idx]);
  }

  // DODGE command handler
  void cmd_sheep_dodge(GameState* state, ParsedCommand cmd) {
    uint8_t input = (uint8_t)sproutInputNum1();
    sproutInputNum1Ack();

    if (check_defense(input, state->sheep_defense_pattern)) {
      // Successful dodge - move to the attack phase
      state->combat_phase = COMBAT_AWAITING_ATTACK;
      uint8_t idx = pick_pattern_index();
      state->sheep_attack_pattern = SHEEP_ATTACK_PATTERNS[idx];
      sproutSetOutputStr(SHEEP_ATTACK_FLAVOR[idx]);
    } else {
      // Failed dodge -- knockback to Maze Room 4, sheep resets
      prompt_for_ack(
        "Sprout barely dodges the sheep's teeth and\n"
        "flees back to the safety of the entryway.\n",
        sheep_knockback_continue
      );
    }
  }

  // SLASH command handler
  void cmd_sheep_slash(GameState* state, ParsedCommand cmd) {
    uint8_t direction = (uint8_t)sproutInputNum1();
    sproutInputNum1Ack();

    if (direction > 7) {
      sproutSetOutputStr("That's not a direction you can strike.");
      return; // Stays in COMBAT_AWAITING_ATTACK - doesn't cost the player a turn
    }

    if (check_attack(direction, state->sheep_attack_pattern)) {
      // Hit - sheep defeated
      state->flags |= FLAG_SHEEP_DEFEATED;
      prompt_for_ack(SHEEP_HIT_TEXT, sheep_victory_continue);
    } else {
      // Miss - the sheep attacks again, back to a fresh defense pattern
      prompt_for_ack(SHEEP_MISS_TEXT, sheep_miss_continue);
    }
  }

  // -- Game States and Descriptions ----------------------------

  // Beach State - No handler functions defined for this state

  // Forest State - No handler functions defined for this state

  // Cliff State
  
  void cliff_handler(GameState* state, ParsedCommand cmd) {
    if (state->flags & FLAG_HAS_CROOK) {
      state->current_state = STATE_CLIFF_EMPTY;
    } else {
      state->current_state = STATE_CLIFF;
    }
    print_current_description(state);
  }

  // crook handler and continuations
  
  void crook_collected_continue(GameState* state) {
    state->flags |= FLAG_HAS_CROOK;
    state->current_state = STATE_CLIFF_EMPTY;
    print_current_description(state);
  }

  void crook_handler(GameState* state, ParsedCommand cmd) {
    prompt_for_ack(
      CROOK_COLLECTION_DESCRIPTION,
      crook_collected_continue);
  }

  // Cliff Empty State - No handler functions defined for this state

  // Cave State

  void cave_handler(GameState* state, ParsedCommand cmd) {
    if (state->flags & FLAG_DOOR_OPENED) {
      state->current_state = STATE_CAVE_EMPTY;
    } else {
      state->current_state = STATE_CAVE;
    }
    print_current_description(state);
  }

  // lever handler and continuations
  
  void lever_solved_continue(GameState* state) {
    state->flags |= FLAG_DOOR_OPENED;
    state->current_state = STATE_CAVE_EMPTY;
    print_current_description(state);
  }

  void lever_handler(GameState* state, ParsedCommand cmd) {
    prompt_for_ack(
      DOOR_OPENING_DESCRIPTION,
      lever_solved_continue);
  }

  // Cave Empty State - the player has opened the door in the cave - No handler functions defined for this state

  // Maze 1 State - No handler functions defined for this state

  // Maze 2 State - No handler functions defined for this state
  
  // Maze 3 State - No handler functions defined for this state
  
  // Maze 4 State

  void maze_4_handler(GameState* state, ParsedCommand cmd) {
    if (state->flags & FLAG_PANEL_SOLVED) {
      state->current_state = STATE_MAZE_4_EMPTY;
    } else {
      state->current_state = STATE_MAZE_4;
    }
    print_current_description(state);
  }

  // panel handler and continuations
  
  void panel_solved_continue(GameState* state) {
    state->flags |= FLAG_PANEL_SOLVED;
    state->current_state = STATE_MAZE_4_EMPTY;
    print_current_description(state);
  }

  void panel_not_solved_continue(GameState* state) {
    state->current_state = STATE_MAZE_4;
    print_current_description(state);
  }

  void panel_handler(GameState* state, ParsedCommand cmd) {
    float n1 = sproutInputNum1();
    float n2 = sproutInputNum2();
    sproutInputNum1Ack();
    sproutInputNum2Ack();
    bool correct = (n1 == 1 && n2 == 3) || (n1 == 3 && n2 == 1);
    
    if (correct) {
      prompt_for_ack(
        PANEL_SOLVED_DESCRIPTION,
        panel_solved_continue);
    } else {
      prompt_for_ack(
        PANEL_WRONG_DESCRIPTION,
        panel_not_solved_continue);
    }
  }

  // Maze 4 Empty State - No handler functions defined for this state

  // Maze 5 State

  void maze_5_handler(GameState* state, ParsedCommand cmd) {
    if (state->flags & FLAG_SHEEP_DEFEATED) {
      state->current_state = STATE_MAZE_5_EMPTY;
      print_current_description(state);
    } else {
      state->current_state = STATE_MAZE_5;
      start_sheep_combat(state);
    }
  }

  // Maze 5 Empty State - No handler functions defined for this state

  // Maze 6 State - No handler functions defined for this state

  // Maze 7 State - No handler functions defined for this state

  // Maze 8 State

  void maze_8_handler(GameState* state, ParsedCommand cmd) {
    if (state->flags & FLAG_HAS_BIRD) {
      state->current_state = STATE_MAZE_8_BIRD;
    } else {
      state->current_state = STATE_MAZE_8;
    }
    print_current_description(state);
  }

  // cipher continuation and handler functions

  void cipher_solved_continue(GameState* state) {
    state->current_state = STATE_END_FRIENDS_SOLVE;
    print_current_description(state);
  }

  void cipher_not_solved_continue(GameState* state) {
    state->current_state = STATE_MAZE_8;
    print_current_description(state);
  }

  void cipher_handler(GameState* state, ParsedCommand cmd) {
    if (cmd.phrase == PHRASE_MYPLUK) {
      prompt_for_ack(
        CORRECT_CIPHER,
        cipher_solved_continue);
    } else {
      prompt_for_ack(
        INCORRECT_CIPHER,
        cipher_not_solved_continue);
    }
  }

  // Maze 8 Bird State

  // translation handler and continuations
  
  void translation_solved_continue(GameState* state) {
    state->current_state = STATE_END_FRIENDS_BIRD;
    print_current_description(state);
  }

  void translation_not_solved_continue(GameState* state) {
    state->current_state = STATE_MAZE_8_BIRD;
    print_current_description(state);
  }

  void translation_handler(GameState* state, ParsedCommand cmd) {
    if (cmd.phrase == PHRASE_FRIEND) {
      prompt_for_ack(
        CORRECT_TRANSLATION,
        translation_solved_continue);
    } else {
      prompt_for_ack(
        INCORRECT_TRANSLATION,
        translation_not_solved_continue);
    }
  }

  // Maze 9 State

  void maze_9_handler(GameState* state, ParsedCommand cmd) {
    if (state->flags & FLAG_RIDDLE_SOLVED) {
      if (state->flags & FLAG_HAS_BIRD) {
        state->current_state = STATE_MAZE_9_NO_BIRD;
      } else {
        state->current_state = STATE_MAZE_9_EMPTY;
      }      
    } else {
      state->current_state = STATE_MAZE_9;
    }
    print_current_description(state);
  }

  // riddle handler and continuations
  
  void riddle_solved_continue(GameState* state) {
    state->current_state = STATE_MAZE_9_EMPTY;
    print_current_description(state);
  }

  void riddle_not_solved_continue(GameState* state) {
    state->current_state = STATE_MAZE_9;
    print_current_description(state);
  }

  void riddle_handler(GameState* state, ParsedCommand cmd) {
    if (cmd.phrase == PHRASE_BLUE) {
      prompt_for_ack(
        CORRECT_RIDDLE,
        riddle_solved_continue);
    } else {
      prompt_for_ack(
        INCORRECT_RIDDLE,
        riddle_not_solved_continue);
    }
  }

  // Maze 9 Empty State

  // bird handler and continuations
  
  void bird_collected_continue(GameState* state) {
    state->flags |= FLAG_HAS_BIRD;
    state->current_state = STATE_MAZE_9_NO_BIRD;
    print_current_description(state);
  }

  void bird_handler(GameState* state, ParsedCommand cmd) {
    prompt_for_ack(
      COLLECT_BIRD,
      bird_collected_continue);
  }

  // Maze 9 No Bird State - No handler functions defined for this state
  
  // End Leave State - No handler functions defined for this state

  // End Friends Solve State - No handler functions defined for this state

  // End Friends Bird State - No handler functions defined for this state