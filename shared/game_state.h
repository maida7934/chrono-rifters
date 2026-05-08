#pragma once
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <atomic>
#include <cstdint>
#include <csignal>

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────
constexpr int MAX_PLAYERS       = 4;
constexpr int MAX_ENEMIES       = 9;
constexpr int MAX_ENTITIES      = MAX_PLAYERS + MAX_ENEMIES;
constexpr int INVENTORY_SLOTS   = 20;
constexpr int MAX_LT_STORAGE    = 64;   // long-term storage slots
constexpr int MAX_WEAPONS_WORLD = 16;
constexpr int NAME_LEN          = 32;
constexpr int LOG_LEN           = 128;
constexpr int LOG_LINES         = 2000;

constexpr int  WIN_KILL_COUNT   = 10;
constexpr float STUN_DURATION   = 3.0f;   // seconds
constexpr float ULTIMATE_PAUSE  = 10.0f;  // seconds
constexpr int  NPC_TURN_TIMEOUT = 3;      // seconds

// Shared-memory keys / names
constexpr const char* SHM_NAME        = "/chrono_rift_shm";
constexpr const char* SEM_ARBITER     = "/cr_sem_arbiter";

// ─────────────────────────────────────────────
//  Weapon definitions
// ─────────────────────────────────────────────
enum WeaponID : int {
    WPN_NONE = -1,
    WPN_SOLAR_CORE = 0,
    WPN_LUNAR_BLADE,
    WPN_IRON_HALBERD,
    WPN_VENOM_DAGGER,
    WPN_THUNDERSTAFF,
    WPN_OBSIDIAN_AXE,
    WPN_FROSTBOW,
    WPN_SPLINTER_STICK,
    WPN_ECLIPSE_RELIC,   // dynamic artifact
    WPN_COUNT
};

struct WeaponDef {
    const char* name;
    int         slot_size;
    int         damage;
    bool        is_artifact;   // Solar Core, Lunar Blade, Eclipse Relic
};

// Defined in a shared .cpp compiled into each executable
extern const WeaponDef WEAPON_TABLE[WPN_COUNT];

// ─────────────────────────────────────────────
//  Inventory (primary: 20 slots, each cell holds
//  a weapon ID; WPN_NONE = free)
// ─────────────────────────────────────────────
struct Inventory {
    int  slots[INVENTORY_SLOTS];  // WPN_NONE or WeaponID occupying that slot
    // Long-term storage: simple list of weapon IDs
    int  lt_storage[MAX_LT_STORAGE];
    int  lt_count;

    void init() {
        for (int i = 0; i < INVENTORY_SLOTS; ++i) slots[i] = WPN_NONE;
        lt_count = 0;
    }

    // Returns starting slot index or -1 if not found
    int find_contiguous(int size) const {
        for (int i = 0; i <= INVENTORY_SLOTS - size; ++i) {
            bool ok = true;
            for (int j = 0; j < size; ++j)
                if (slots[i + j] != WPN_NONE) { ok = false; break; }
            if (ok) return i;
        }
        return -1;
    }

    // Place weapon starting at slot 'start'
    void place(int start, WeaponID id, int size) {
        for (int i = 0; i < size; ++i) slots[start + i] = (int)id;
    }

    // Remove ALL slots of a weapon; returns how many slots freed
    int remove(WeaponID id) {
        int freed = 0;
        for (int i = 0; i < INVENTORY_SLOTS; ++i)
            if (slots[i] == (int)id) { slots[i] = WPN_NONE; ++freed; }
        return freed;
    }

    bool has(WeaponID id) const {
        for (int i = 0; i < INVENTORY_SLOTS; ++i)
            if (slots[i] == (int)id) return true;
        return false;
    }
};

// ─────────────────────────────────────────────
//  Entity (player or enemy)
// ─────────────────────────────────────────────
enum EntityType { ENT_PLAYER, ENT_ENEMY };

enum ActionType {
    ACT_NONE = 0,
    ACT_MOVE,
    ACT_STRIKE,       // normal attack
    ACT_EXHAUST,      // reduce stamina
    ACT_USE_WEAPON,   // attack with weapon
    ACT_SWAP_IN,      // bring weapon from LT storage
    ACT_HEAL,
    ACT_SKIP,
    ACT_ULTIMATE,     // needs Solar Core + Lunar Blade
    ACT_PICKUP,       // pick up artifact from arena
    ACT_AOE,          // area-of-effect attack (hits enemies within range)
    ACT_QUIT          // player quit signal
};

struct ActionRequest {
    int        entity_id;    // who is acting
    ActionType action;
    int        target_id;    // for attacks
    WeaponID   weapon;       // for USE_WEAPON / SWAP_IN
    int        move_dx;      // for ACT_MOVE
    int        move_dy;
    bool       ready;        // HIP sets true; Arbiter clears after consuming
};

struct Entity {
    char        name[NAME_LEN];
    EntityType  type;
    int         id;          // index in entities[]
    pid_t       process_pid; // PID of owning process (HIP or ASP)
    int         thread_idx;  // thread index within owning process

    // Stats
    int   hp;
    int   max_hp;
    int   damage;
    float speed;
    float stamina;
    float max_stamina;
    int   level;

    bool  alive;
    bool   stunned;              // set by signal, cleared after 3s
    double stun_end_time;        // exact deadline in seconds
    bool   skip_turn_from_stun;  // skip next turn if full stamina when stunned
    WeaponID swapped_weapon_unavailable; // weapon swapped this turn

    // Inventory (only meaningful for players)
    Inventory inventory;

    // Scheduling
    float next_action_time;  // virtual time when stamina will be full
    int   x;                 // screen x position for TUI (cols)
    int   y;                 // screen y position for TUI (rows)
    int   offset_x;          // visual offset from orbital position
    int   offset_y;
    double last_hit_time;    // virtual_time at last damage, for hit-flash FX

    void init_player(int idx, int roll_no, int num_players) {
        type        = ENT_PLAYER;
        id          = idx;
        alive       = true;
        stunned     = false;
        stun_end_time = 0;
        skip_turn_from_stun = false;
        swapped_weapon_unavailable = WPN_NONE;
        max_stamina = 100.0f;
        stamina     = 0.0f;
        speed       = 100.0f / num_players;
        // HP = roll_no + rand(100..1000)
        max_hp = roll_no + 100 + rand() % 901;
        hp     = max_hp;
        // Damage = last digit of roll_no + 10
        damage = (roll_no % 10) + 10;
        inventory.init();
        level = 1;
        x = -1; y = -1;
        offset_x = 0; offset_y = 0;
        last_hit_time = -1e9;
        snprintf(name, NAME_LEN, "Hero-%d", idx + 1);
    }

    void init_enemy(int idx, int roll_no) {
        type        = ENT_ENEMY;
        id          = MAX_PLAYERS + idx;
        alive       = true;
        stunned     = false;
        stun_end_time = 0;
        skip_turn_from_stun = false;
        swapped_weapon_unavailable = WPN_NONE;
        max_stamina = 150.0f;
        stamina     = 0.0f;
        speed       = 10.0f + rand() % 21;  // 10..30
        // HP = last 2 digits of roll_no + rand(50..200)
        max_hp = (roll_no % 100) + 50 + rand() % 151;
        hp     = max_hp;
        // Damage = second last digit + 10
        damage = ((roll_no / 10) % 10) + 10;
        inventory.init();
        level = 1 + (rand() % 3);
        x = -1; y = -1;
        offset_x = 0; offset_y = 0;
        last_hit_time = -1e9;
        snprintf(name, NAME_LEN, "Enemy-%d", idx + 1);
    }
};

// ─────────────────────────────────────────────
//  Artifact / Resource Table  (Section 7)
// ─────────────────────────────────────────────
constexpr int NUM_ARTIFACTS = 3;  // Solar Core, Lunar Blade, Eclipse Relic

struct ArtifactEntry {
    WeaponID  id;
    bool      exists;       // Eclipse Relic may not exist yet
    int       held_by;      // entity id, -1 = free
    bool      locked;       // currently being acquired/released
};

struct ResourceTable {
    ArtifactEntry entries[NUM_ARTIFACTS];
    pthread_mutex_t table_mutex;  // protects entire table

    void init() {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&table_mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        entries[0] = {WPN_SOLAR_CORE,   true, -1, false};
        entries[1] = {WPN_LUNAR_BLADE,  true, -1, false};
        entries[2] = {WPN_ECLIPSE_RELIC,false,-1, false};
    }

    // Returns index or -1
    int find(WeaponID id) const {
        for (int i = 0; i < NUM_ARTIFACTS; ++i)
            if (entries[i].id == id) return i;
        return -1;
    }
};

// ─────────────────────────────────────────────
//  Deadlock detection support
// ─────────────────────────────────────────────
// For each entity: which artifact are they waiting for?
struct WaitForGraph {
    int waiting_for[MAX_ENTITIES];  // artifact index, -1 = not waiting
    int holding[MAX_ENTITIES][NUM_ARTIFACTS]; // 1 if entity holds artifact i

    void init() {
        memset(waiting_for, -1, sizeof(waiting_for));
        memset(holding, 0, sizeof(holding));
    }
};

// ─────────────────────────────────────────────
//  Action Log for TUI
// ─────────────────────────────────────────────
struct ActionLog {
    char lines[LOG_LINES][LOG_LEN];
    int  head;   // ring-buffer index
    pthread_mutex_t log_mutex;

    void init() {
        memset(lines, 0, sizeof(lines));
        head = 0;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&log_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    void push(const char* msg) {
        pthread_mutex_lock(&log_mutex);
        snprintf(lines[head % LOG_LINES], LOG_LEN, "%s", msg);
        head = (head + 1) % LOG_LINES;
        pthread_mutex_unlock(&log_mutex);
    }
};

// ─────────────────────────────────────────────
//  Global Shared State  (lives in SHM)
// ─────────────────────────────────────────────
enum GamePhase {
    PHASE_LOBBY = 0,
    PHASE_RUNNING,
    PHASE_ULTIMATE_PAUSE,  // ASP suspended for 10s
    PHASE_WIN,
    PHASE_LOSE,
    PHASE_QUIT
};

struct SharedState {
    // ── Synchronization ──────────────────────
    pthread_mutex_t global_mutex;  // coarse lock for state reads/writes
    pthread_cond_t  turn_cond;     // broadcast when active_entity changes

    // ── Entities ─────────────────────────────
    Entity entities[MAX_ENTITIES];
    int    num_players;
    int    num_enemies;
    int    total_enemies_killed;

    // ── Shared Player Party Inventory (spec §6: one 20-slot array for all heroes)
    Inventory player_party_inventory;

    // ── Turn Scheduling ───────────────────────
    int    active_entity;   // index into entities[], -1 = calculating
    float  virtual_time;    // current scheduler time

    // ── Action Channels (HIP -> Arbiter) ──────
    ActionRequest player_actions[MAX_PLAYERS];

    // ── Action Channel (ASP -> Arbiter) ───────
    ActionRequest npc_action;   // single slot; ASP fills, Arbiter reads

    // ── Resource / Artifact Table ─────────────
    ResourceTable resource_table;
    WaitForGraph  wait_graph;

    // ── Process PIDs ─────────────────────────
    pid_t arbiter_pid;
    pid_t hip_pid;
    pid_t asp_pid;

    // ── Game State ───────────────────────────
    GamePhase phase;
    bool      eclipse_relic_spawned;
    bool      ultimate_active;
    int       game_level;   // chosen difficulty / level

    // ── NPC timeout flag (Arbiter sets, ASP reads) ──
    std::atomic<bool> npc_timeout;
    double    npc_turn_deadline_sec; // wall-clock deadline for current NPC turn; 0 if inactive

    // ── Weapon drop notification (ASP -> HIP via Arbiter) ──
    bool      weapon_drop_pending;   // true when a weapon is available
    WeaponID  weapon_drop_id;        // which weapon was dropped
    int       weapon_drop_for;       // entity index of player to prompt
    int       weapon_drop_turns_left; // turns remaining before enemy auto-pickup fallback
        // If true, HIP should not prompt on the terminal and Arbiter's
        // ncurses render_thread will capture input instead.
        bool      use_ncurses_ui;

    // ── Action Log ───────────────────────────
    ActionLog log;

    // ── Init ─────────────────────────────────
    void init() {
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&global_mutex, &mattr);
        pthread_mutexattr_destroy(&mattr);

        pthread_condattr_t cattr;
        pthread_condattr_init(&cattr);
        pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
        pthread_cond_init(&turn_cond, &cattr);
        pthread_condattr_destroy(&cattr);

        memset(entities, 0, sizeof(entities));
        num_players  = 0;
        num_enemies  = 0;
        total_enemies_killed = 0;
        active_entity = -1;
        virtual_time  = 0.0f;
        phase         = PHASE_LOBBY;
        eclipse_relic_spawned = false;
        ultimate_active       = false;
        game_level = 1;
        npc_timeout.store(false);
        npc_turn_deadline_sec = 0.0;
        weapon_drop_pending   = false;
        weapon_drop_id        = WPN_NONE;
        weapon_drop_for       = -1;
        weapon_drop_turns_left = 0;
        use_ncurses_ui        = false;

        player_party_inventory.init();

        for (int i = 0; i < MAX_PLAYERS; ++i)
            player_actions[i].ready = false;
        npc_action.ready = false;

        resource_table.init();
        wait_graph.init();
        log.init();
    }
};