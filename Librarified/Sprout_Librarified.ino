  #include "sprout_api.h"
  // -- [USER CONFIGURATION] ---------------------
  Sprout_API sprout("Opta");

// -- Thin wrappers so game logic can keep calling these as free functions,
// exactly as it did before the library existed. Each one just forwards
// to the single global `sprout` instance.
bool        sproutButton1()        { return sprout.sproutButton1(); }
bool        sproutButton2()        { return sprout.sproutButton2(); }
float       sproutInputNum1()      { return sprout.sproutInputNum1(); }
float       sproutInputNum2()      { return sprout.sproutInputNum2(); }
const char* sproutInputStr()       { return sprout.sproutInputStr(); }
bool        sproutButton1IsNew()   { return sprout.sproutButton1IsNew(); }
bool        sproutButton2IsNew()   { return sprout.sproutButton2IsNew(); }
bool        sproutInputNum1IsNew() { return sprout.sproutInputNum1IsNew(); }
bool        sproutInputNum2IsNew() { return sprout.sproutInputNum2IsNew(); }
bool        sproutInputStrIsNew()  { return sprout.sproutInputStrIsNew(); }
void        sproutButton1Ack()     { sprout.sproutButton1Ack(); }
void        sproutButton2Ack()     { sprout.sproutButton2Ack(); }
void        sproutInputNum1Ack()   { sprout.sproutInputNum1Ack(); }
void        sproutInputNum2Ack()   { sprout.sproutInputNum2Ack(); }
void        sproutInputStrAck()    { sprout.sproutInputStrAck(); }
void        sproutSetOutputStatus1(bool v)      { sprout.sproutSetOutputStatus1(v); }
void        sproutSetOutputStatus2(bool v)      { sprout.sproutSetOutputStatus2(v); }
void        sproutSetOutputNum1(float v)        { sprout.sproutSetOutputNum1(v); }
void        sproutSetOutputNum2(float v)        { sprout.sproutSetOutputNum2(v); }
bool        sproutSetOutputStr(const char* v)   { return sprout.sproutSetOutputStr(v); }
bool        sproutSetDeviceStatus(const char* v){ return sprout.sproutSetDeviceStatus(v); }

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
  // Game loop entry point prototype
void run_game_state_machine(GameState* state);

// Command parser prototypes
int           tokenize(const char* input, char tokens[MAX_TOKENS][MAX_TOKEN_LEN]);
Verb          lookup_verb(const char* word);
Target        lookup_target(const char* word);
Phrase        lookup_phrase(const char* word);
ParsedCommand parse_input(const char* input);

// Combat continuation prototypes
void sheep_knockback_continue(GameState* state);
void sheep_miss_continue(GameState* state);
void sheep_victory_continue(GameState* state);
void start_sheep_combat(GameState* state);

// Puzzle / story continuation prototypes
void crook_collected_continue(GameState* state);
void lever_solved_continue(GameState* state);
void panel_solved_continue(GameState* state);
void panel_not_solved_continue(GameState* state);
void cipher_solved_continue(GameState* state);
void cipher_not_solved_continue(GameState* state);
void translation_solved_continue(GameState* state);
void translation_not_solved_continue(GameState* state);
void riddle_solved_continue(GameState* state);
void riddle_not_solved_continue(GameState* state);
void bird_collected_continue(GameState* state);
const State* get_state(StateId id);
const StateCommand* find_matching_command(const State* state, Verb verb, Target target);
void handle_command(GameState* state, ParsedCommand cmd);
  // -- Wait-for-Acknowledgement --------------------------------

  typedef void (*AckCallback)(GameState*);
  typedef void (*AckCallback)(GameState*);

// Acknowledgement-wait prototypes (need AckCallback defined above)
void prompt_for_ack(const char* message, AckCallback continuation);
void handle_acknowledgement_wait(GameState* state);


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
  // Print state description prototype

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

  sprout.initSprout();

  if (!sprout.connectWiFi()) {
    Serial.println("WiFi not connected at boot -- will keep retrying from loop().");
  }

  if (sprout.connectMQTT()) {
    sprout.applyTopicSubscriptions();
    sprout.publishOtaCompleteStatus();
    print_current_description(&game_state);
  } else {
    Serial.println("MQTT not connected at boot -- will keep retrying from loop().");
  }
}


  // --Main Program Loop-------------------------
  // Code here runs continuously

  void loop() {

    sprout.poll();
    // USER CODE
    // Write your code in the function call at the bottom of the sketch.
    // Update this call with any return values and inputs that you add.
    userLoop();


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
        if (cmd.phrase != PHRASE_NONE) {
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
    char buffer[600];
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
      state->flags |= FLAG_CIPHER_SOLVED;
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
      state->flags |= FLAG_RIDDLE_SOLVED;
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
