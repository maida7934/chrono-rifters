/*
 * hip.cpp  —  Human Interfacing Process
 *
 * Responsibilities (per spec):
 *   - Separate process from Arbiter
 *   - One dedicated thread per player character
 *   - Only the thread for the CURRENTLY ACTIVE player processes input
 *   - All other player threads remain idle (wait on cond)
 *   - Does NOT modify global game state directly
 *   - Writes ActionRequest into shared memory → Arbiter reads + applies
 *   - Sends SIGTERM to Arbiter when player chooses Quit
 *   - Handles SIGUSR1 on a player thread = Stun (halts that thread 3s)
 *
 * I/O Strategy:
 *   HIP writes to stderr to avoid conflict with Arbiter's ncurses TUI.
 *   For clean interaction, run HIP in a separate Docker exec session:
 *     docker exec -it <container> ./hip_bin
 */

#include "../shared/game_state.h"
#include "../shared/shm_utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

// ─────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────
static SharedState*          g_state      = nullptr;
static volatile sig_atomic_t g_running    = 1;
static pthread_t             g_threads[MAX_PLAYERS];
static int                   g_num_threads = 0;

// ─────────────────────────────────────────────
//  I/O helpers — write to stderr to avoid
//  ncurses conflict on stdout
// ─────────────────────────────────────────────
static void hip_print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static int hip_read_int(int fallback) {
    char buf[32] = {};
    if (fgets(buf, sizeof(buf), stdin) == nullptr) return fallback;
    return atoi(buf);
}

static void hip_read_str(char* out, int max) {
    if (fgets(out, max, stdin) == nullptr) { out[0] = '\0'; return; }
    int len = (int)strlen(out);
    if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';
}

// ─────────────────────────────────────────────
//  SIGUSR1 handler — stun interrupt
//  (just interrupts blocking calls; flag is in SHM)
// ─────────────────────────────────────────────
static void handle_stun(int) {
    // No-op body. The signal interrupts sleep/cond_wait.
    // The player_thread loop checks the stunned flag.
}

// ─────────────────────────────────────────────
//  SIGTERM handler — graceful shutdown
//  (sent by Arbiter on game over)
// ─────────────────────────────────────────────
static void handle_sigterm(int) {
    g_running = 0;
    if (g_state) {
        // Wake all threads so they can check g_running and exit
        pthread_cond_broadcast(&g_state->turn_cond);
    }
}

// ─────────────────────────────────────────────
//  Per-thread argument
// ─────────────────────────────────────────────
struct ThreadArg {
    int           player_idx;
    SharedState*  state;
};

// ─────────────────────────────────────────────
//  Print the menu for a player's turn
// ─────────────────────────────────────────────
static void print_menu(SharedState* s, int pidx) {
    Entity& me = s->entities[pidx];
    hip_print("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    hip_print("[%s] HP: %d/%d | Stamina: %.1f/%.0f\n",
           me.name, me.hp, me.max_hp, me.stamina, me.max_stamina);
    hip_print("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    // Show alive enemies
    hip_print("ENEMIES:\n");
    int ne = s->num_enemies;
    for (int i = 0; i < ne; ++i) {
        Entity& e = s->entities[MAX_PLAYERS + i];
        if (e.alive)
            hip_print("  [%d] %s  HP:%d/%d\n",
                   MAX_PLAYERS + i, e.name, e.hp, e.max_hp);
    }

    // Show inventory
    hip_print("INVENTORY: ");
    bool any = false;
    for (int i = 0; i < INVENTORY_SLOTS; ) {
        if (me.inventory.slots[i] == WPN_NONE) { ++i; continue; }
        WeaponID w = (WeaponID)me.inventory.slots[i];
        hip_print("%s ", WEAPON_TABLE[w].name);
        any = true;
        while (i < INVENTORY_SLOTS && me.inventory.slots[i] == (int)w) ++i;
    }
    if (!any) hip_print("(empty)");
    hip_print("\n");

    // Long-term storage
    if (me.inventory.lt_count > 0) {
        hip_print("LT STORAGE: ");
        for (int i = 0; i < me.inventory.lt_count; ++i)
            hip_print("%s ", WEAPON_TABLE[me.inventory.lt_storage[i]].name);
        hip_print("\n");
    }

    bool can_ultimate = me.inventory.has(WPN_SOLAR_CORE) &&
                        me.inventory.has(WPN_LUNAR_BLADE);

    hip_print("\nActions:\n");
    hip_print("  1) Strike        (attack enemy)\n");
    hip_print("  2) Exhaust       (drain enemy stamina)\n");
    hip_print("  3) Use Weapon    (attack with inventory weapon)\n");
    hip_print("  4) Swap In       (bring weapon from LT storage)\n");
    hip_print("  5) Heal          (restore 10%% HP)\n");
    hip_print("  6) Skip          (stamina -> 50%%)\n");
    if (can_ultimate)
        hip_print("  7) ULTIMATE      (requires Solar Core + Lunar Blade)\n");

    // Show available artifacts for pickup
    bool any_artifact = false;
    for (int a = 0; a < NUM_ARTIFACTS; ++a) {
        pthread_mutex_lock(&s->resource_table.table_mutex);
        bool avail = s->resource_table.entries[a].exists &&
                     s->resource_table.entries[a].held_by < 0;
        WeaponID aid = s->resource_table.entries[a].id;
        pthread_mutex_unlock(&s->resource_table.table_mutex);
        if (avail) {
            if (!any_artifact) {
                hip_print("  8) Pickup artifact:\n");
                any_artifact = true;
            }
            hip_print("       %d = %s (slots %d)\n",
                   (int)aid, WEAPON_TABLE[aid].name, WEAPON_TABLE[aid].slot_size);
        }
    }

    hip_print("  q) Quit game\n");
    hip_print("Choice: ");
}

// ─────────────────────────────────────────────
//  Prompt the user to pick a live enemy.
// ─────────────────────────────────────────────
static int pick_enemy(SharedState* s) {
    hip_print("Target enemy index: ");
    int t = hip_read_int(-1);
    int base = MAX_PLAYERS;
    int ne   = s->num_enemies;
    if (t < base || t >= base + ne) { hip_print("Invalid target.\n"); return -1; }
    if (!s->entities[t].alive)      { hip_print("Enemy is dead.\n");  return -1; }
    return t;
}

// ─────────────────────────────────────────────
//  Prompt for a weapon in the player's inventory
// ─────────────────────────────────────────────
static WeaponID pick_inventory_weapon(SharedState* s, int pidx) {
    Entity& me = s->entities[pidx];
    hip_print("Weapon (index 0-%d):\n", WPN_COUNT - 1);
    for (int w = 0; w < WPN_COUNT; ++w) {
        if (me.inventory.has((WeaponID)w))
            hip_print("  %d) %s (dmg %d)\n", w,
                   WEAPON_TABLE[w].name, WEAPON_TABLE[w].damage);
    }
    int choice = hip_read_int(-1);
    if (choice < 0 || choice >= WPN_COUNT) return WPN_NONE;
    if (!me.inventory.has((WeaponID)choice)) {
        hip_print("Not in inventory.\n"); return WPN_NONE;
    }
    return (WeaponID)choice;
}

// ─────────────────────────────────────────────
//  Prompt for a weapon in LT storage
// ─────────────────────────────────────────────
static WeaponID pick_lt_weapon(SharedState* s, int pidx) {
    Entity& me = s->entities[pidx];
    if (me.inventory.lt_count == 0) {
        hip_print("Long-term storage is empty.\n");
        return WPN_NONE;
    }
    hip_print("LT Storage weapons:\n");
    for (int i = 0; i < me.inventory.lt_count; ++i)
        hip_print("  %d) %s\n", me.inventory.lt_storage[i],
               WEAPON_TABLE[me.inventory.lt_storage[i]].name);
    hip_print("Choice (weapon index): ");
    int choice = hip_read_int(-1);
    if (choice < 0 || choice >= WPN_COUNT) return WPN_NONE;
    for (int i = 0; i < me.inventory.lt_count; ++i)
        if (me.inventory.lt_storage[i] == choice) return (WeaponID)choice;
    hip_print("Not in LT storage.\n");
    return WPN_NONE;
}

// ─────────────────────────────────────────────
//  Weapon pickup prompt (Phase 4, item 4)
//  Called when a weapon drop is pending for this player.
//  Returns true if player picks it up.
// ─────────────────────────────────────────────
static bool weapon_pickup_prompt(SharedState* s, int pidx, WeaponID wpn) {
    hip_print("\n*** WEAPON DROP! ***\n");
    hip_print("A [%s] (dmg %d, slots %d) is available!\n",
           WEAPON_TABLE[wpn].name, WEAPON_TABLE[wpn].damage,
           WEAPON_TABLE[wpn].slot_size);
    hip_print("Pick up? (1=yes, 0=no): ");
    int choice = hip_read_int(0);
    return (choice == 1);
}

// ─────────────────────────────────────────────
//  Player Thread
//
//  Each player character has its own thread.
//  Only the thread matching active_entity may
//  process input; all others block on condvar.
// ─────────────────────────────────────────────
static void* player_thread(void* arg_ptr) {
    ThreadArg* arg   = (ThreadArg*)arg_ptr;
    int        pidx  = arg->player_idx;
    SharedState* s   = arg->state;

    // Install SIGUSR1 handler for stun on this thread
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun;
    sa.sa_flags   = 0;  // no SA_RESTART — let signals interrupt blocking calls
    sigaction(SIGUSR1, &sa, nullptr);

    while (g_running) {
        // ── Wait until it is THIS player's turn ──────
        pthread_mutex_lock(&s->global_mutex);
        while (s->active_entity != pidx) {
            // Exit conditions
            if (!g_running ||
                s->phase == PHASE_QUIT ||
                s->phase == PHASE_WIN  ||
                s->phase == PHASE_LOSE) {
                pthread_mutex_unlock(&s->global_mutex);
                return nullptr;
            }
            pthread_cond_wait(&s->turn_cond, &s->global_mutex);
        }
        pthread_mutex_unlock(&s->global_mutex);

        // ── Stun-wait block (Phase 4, item 5) ────────
        // If stunned, wait via condvar until Arbiter clears the flag.
        {
            pthread_mutex_lock(&s->global_mutex);
            while (s->entities[pidx].stunned) {
                hip_print("[%s] You are STUNNED! Waiting...\n", s->entities[pidx].name);
                pthread_cond_wait(&s->turn_cond, &s->global_mutex);
                if (!g_running) {
                    pthread_mutex_unlock(&s->global_mutex);
                    return nullptr;
                }
                if (s->active_entity != pidx) {
                    break;
                }
            }
            if (s->active_entity != pidx) {
                pthread_mutex_unlock(&s->global_mutex);
                continue;
            }
            pthread_mutex_unlock(&s->global_mutex);
        }

        // ── Weapon pickup check (Phase 4, item 4) ───
        {
            pthread_mutex_lock(&s->global_mutex);
            if (s->weapon_drop_pending && s->weapon_drop_for == pidx) {
                WeaponID drop = s->weapon_drop_id;
                s->weapon_drop_pending = false;  // consume the notification
                pthread_mutex_unlock(&s->global_mutex);

                if (weapon_pickup_prompt(s, pidx, drop)) {
                    // Player accepted — submit a PICKUP action
                    ActionRequest req;
                    memset(&req, 0, sizeof(req));
                    req.entity_id = pidx;
                    req.action    = ACT_PICKUP;
                    req.weapon    = drop;
                    req.target_id = -1;
                    req.ready     = true;

                    pthread_mutex_lock(&s->global_mutex);
                    s->player_actions[pidx] = req;
                    pthread_cond_broadcast(&s->turn_cond);
                    pthread_mutex_unlock(&s->global_mutex);
                    continue;  // turn consumed by pickup
                }
                // Player declined — weapon stays available for enemies
                // (ASP can pick it up on its next cycle)
            } else {
                pthread_mutex_unlock(&s->global_mutex);
            }
        }

        // ── Re-verify active + alive before showing menu ──
        {
            pthread_mutex_lock(&s->global_mutex);
            if (s->active_entity != pidx || !s->entities[pidx].alive ||
                !g_running) {
                pthread_mutex_unlock(&s->global_mutex);
                continue;
            }
            pthread_mutex_unlock(&s->global_mutex);
        }

        // ── Print menu and get input ─────────────────
        print_menu(s, pidx);

        char choice[8] = {};
        hip_read_str(choice, sizeof(choice));

        if (choice[0] == '\0') continue;

        ActionRequest req;
        memset(&req, 0, sizeof(req));
        req.entity_id = pidx;
        req.weapon    = WPN_NONE;
        req.target_id = -1;
        req.ready     = false;

        if (choice[0] == 'q' || choice[0] == 'Q') {
            req.action = ACT_QUIT;
            pthread_mutex_lock(&s->global_mutex);
            s->player_actions[pidx] = req;
            s->player_actions[pidx].ready = true;
            pthread_cond_broadcast(&s->turn_cond);
            pthread_mutex_unlock(&s->global_mutex);
            // Send SIGTERM to Arbiter (Quit Condition, Section 10)
            kill(s->arbiter_pid, SIGTERM);
            return nullptr;
        }

        int c = atoi(choice);
        bool valid = true;

        switch (c) {
        case 1:  // Strike
            req.action    = ACT_STRIKE;
            req.target_id = pick_enemy(s);
            if (req.target_id < 0) valid = false;
            break;

        case 2:  // Exhaust
            req.action    = ACT_EXHAUST;
            req.target_id = pick_enemy(s);
            if (req.target_id < 0) valid = false;
            break;

        case 3:  // Use Weapon
            req.action    = ACT_USE_WEAPON;
            req.weapon    = pick_inventory_weapon(s, pidx);
            if (req.weapon == WPN_NONE) { valid = false; break; }
            req.target_id = pick_enemy(s);
            if (req.target_id < 0) valid = false;
            break;

        case 4:  // Swap In
            req.action = ACT_SWAP_IN;
            req.weapon = pick_lt_weapon(s, pidx);
            if (req.weapon == WPN_NONE) valid = false;
            break;

        case 5:  // Heal
            req.action = ACT_HEAL;
            break;

        case 6:  // Skip
            req.action = ACT_SKIP;
            break;

        case 7: {  // Ultimate
            pthread_mutex_lock(&s->global_mutex);
            bool can = s->entities[pidx].inventory.has(WPN_SOLAR_CORE) &&
                       s->entities[pidx].inventory.has(WPN_LUNAR_BLADE);
            pthread_mutex_unlock(&s->global_mutex);
            if (!can) {
                hip_print("You need Solar Core + Lunar Blade!\n");
                valid = false;
            } else {
                req.action = ACT_ULTIMATE;
            }
            break;
        }

        case 8: {  // Pickup artifact
            hip_print("Enter artifact weapon index to pick up: ");
            int wpn_choice = hip_read_int(-1);
            if (wpn_choice < 0 || wpn_choice >= WPN_COUNT) {
                hip_print("Invalid weapon index.\n");
                valid = false;
            } else if (!WEAPON_TABLE[wpn_choice].is_artifact) {
                hip_print("That is not an artifact.\n");
                valid = false;
            } else {
                req.action = ACT_PICKUP;
                req.weapon = (WeaponID)wpn_choice;
            }
            break;
        }

        default:
            hip_print("Unknown action.\n");
            valid = false;
            break;
        }

        if (!valid) continue;

        // ── Submit action to Arbiter via SHM ─────────
        pthread_mutex_lock(&s->global_mutex);
        s->player_actions[pidx] = req;
        s->player_actions[pidx].ready = true;
        pthread_cond_broadcast(&s->turn_cond);
        pthread_mutex_unlock(&s->global_mutex);

        // Loop back — wait for our next turn
    }
    return nullptr;
}

// ─────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────
int main() {
    // Attach to shared memory (created by Arbiter)
    g_state = shm_attach();
    if (!g_state) {
        fprintf(stderr, "[HIP] Failed to attach shared memory!\n");
        return 1;
    }

    // Install signal handlers
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_sigterm;
    sa_term.sa_flags   = 0;
    sigaction(SIGTERM, &sa_term, nullptr);

    struct sigaction sa_stun;
    memset(&sa_stun, 0, sizeof(sa_stun));
    sa_stun.sa_handler = handle_stun;
    sa_stun.sa_flags   = 0;
    sigaction(SIGUSR1, &sa_stun, nullptr);

    int np = g_state->num_players;
    g_num_threads = np;

    fprintf(stderr, "[HIP] Starting with %d player thread(s).\n", np);

    // Spawn one thread per player
    ThreadArg args[MAX_PLAYERS];
    for (int i = 0; i < np; ++i) {
        args[i].player_idx = i;
        args[i].state      = g_state;
        pthread_create(&g_threads[i], nullptr, player_thread, &args[i]);
    }

    // If we're shutting down (SIGTERM received),
    // cancel any remaining threads before joining
    if (!g_running) {
        for (int i = 0; i < np; ++i)
            pthread_cancel(g_threads[i]);
    }

    // Wait for all player threads to finish
    for (int i = 0; i < np; ++i)
        pthread_join(g_threads[i], nullptr);

    fprintf(stderr, "[HIP] Shutting down.\n");
    shm_detach(g_state);
    return 0;
}