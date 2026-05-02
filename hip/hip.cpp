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
 */

#include "../shared/game_state.h"
#include "../shared/shm_utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <ncurses.h>

// ─────────────────────────────────────────────
//  Per-thread argument
// ─────────────────────────────────────────────
struct ThreadArg {
    int           player_idx;   // index into entities[] (0..num_players-1)
    SharedState*  state;
};

static SharedState* g_state = nullptr;

// ─────────────────────────────────────────────
//  SIGUSR1 handler — stun this thread's player
//  (delivered to the specific player thread via
//   pthread_kill from the ASP/Arbiter)
// ─────────────────────────────────────────────
static void handle_stun(int) {
    // The stun flag is already set in shared memory by the Arbiter.
    // This handler just interrupts any blocking call in the thread.
    // Actual 3-second pause is enforced by the thread loop checking the flag.
}

// ─────────────────────────────────────────────
//  Print the menu for a player's turn
// ─────────────────────────────────────────────
static void print_menu(SharedState* s, int pidx) {
    Entity& me = s->entities[pidx];
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("[%s] HP: %d/%d | Stamina: %.1f/%.0f\n",
           me.name, me.hp, me.max_hp, me.stamina, me.max_stamina);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    // Show alive enemies
    printf("ENEMIES:\n");
    int ne = s->num_enemies;
    for (int i = 0; i < ne; ++i) {
        Entity& e = s->entities[MAX_PLAYERS + i];
        if (e.alive)
            printf("  [%d] %s  HP:%d/%d\n",
                   MAX_PLAYERS + i, e.name, e.hp, e.max_hp);
    }

    // Show inventory (condensed — unique weapons)
    printf("INVENTORY: ");
    bool any = false;
    for (int i = 0; i < INVENTORY_SLOTS; ) {
        if (me.inventory.slots[i] == WPN_NONE) { ++i; continue; }
        WeaponID w = (WeaponID)me.inventory.slots[i];
        printf("%s ", WEAPON_TABLE[w].name);
        any = true;
        // Skip rest of this weapon's slots
        while (i < INVENTORY_SLOTS && me.inventory.slots[i] == (int)w) ++i;
    }
    if (!any) printf("(empty)");
    printf("\n");

    // Long-term storage
    if (me.inventory.lt_count > 0) {
        printf("LT STORAGE: ");
        for (int i = 0; i < me.inventory.lt_count; ++i)
            printf("%s ", WEAPON_TABLE[me.inventory.lt_storage[i]].name);
        printf("\n");
    }

    bool can_ultimate = me.inventory.has(WPN_SOLAR_CORE) &&
                        me.inventory.has(WPN_LUNAR_BLADE);

    printf("\nActions:\n");
    printf("  1) Strike        (attack enemy)\n");
    printf("  2) Exhaust       (drain enemy stamina)\n");
    printf("  3) Use Weapon    (attack with inventory weapon)\n");
    printf("  4) Swap In       (bring weapon from LT storage)\n");
    printf("  5) Heal          (restore 10%% HP)\n");
    printf("  6) Skip          (stamina → 50%%)\n");
    if (can_ultimate)
        printf("  7) ULTIMATE      (requires Solar Core + Lunar Blade)\n");
    printf("  q) Quit game\n");
    printf("Choice: ");
    fflush(stdout);
}

// ─────────────────────────────────────────────
//  Prompt the user to pick a live enemy.
//  Returns entity index or -1.
// ─────────────────────────────────────────────
static int pick_enemy(SharedState* s) {
    printf("Target enemy index: ");
    fflush(stdout);
    int t = -1;
    if (scanf("%d", &t) != 1) return -1;
    // Validate
    int base = MAX_PLAYERS;
    int ne   = s->num_enemies;
    if (t < base || t >= base + ne) { printf("Invalid target.\n"); return -1; }
    if (!s->entities[t].alive)      { printf("Enemy is dead.\n");  return -1; }
    return t;
}

// ─────────────────────────────────────────────
//  Prompt for a weapon in the player's inventory
// ─────────────────────────────────────────────
static WeaponID pick_inventory_weapon(SharedState* s, int pidx) {
    Entity& me = s->entities[pidx];
    printf("Weapon (name or index 0-%d):\n", WPN_COUNT - 1);
    for (int w = 0; w < WPN_COUNT; ++w) {
        if (me.inventory.has((WeaponID)w))
            printf("  %d) %s (dmg %d)\n", w,
                   WEAPON_TABLE[w].name, WEAPON_TABLE[w].damage);
    }
    int choice = -1;
    scanf("%d", &choice);
    if (choice < 0 || choice >= WPN_COUNT) return WPN_NONE;
    if (!me.inventory.has((WeaponID)choice)) {
        printf("Not in inventory.\n"); return WPN_NONE;
    }
    return (WeaponID)choice;
}

// ─────────────────────────────────────────────
//  Prompt for a weapon in LT storage
// ─────────────────────────────────────────────
static WeaponID pick_lt_weapon(SharedState* s, int pidx) {
    Entity& me = s->entities[pidx];
    if (me.inventory.lt_count == 0) {
        printf("Long-term storage is empty.\n");
        return WPN_NONE;
    }
    printf("LT Storage weapons:\n");
    for (int i = 0; i < me.inventory.lt_count; ++i)
        printf("  %d) %s\n", me.inventory.lt_storage[i],
               WEAPON_TABLE[me.inventory.lt_storage[i]].name);
    printf("Choice (weapon index): ");
    fflush(stdout);
    int choice = -1;
    scanf("%d", &choice);
    if (choice < 0 || choice >= WPN_COUNT) return WPN_NONE;
    // Check it's actually in LT
    for (int i = 0; i < me.inventory.lt_count; ++i)
        if (me.inventory.lt_storage[i] == choice) return (WeaponID)choice;
    printf("Not in LT storage.\n");
    return WPN_NONE;
}

// ─────────────────────────────────────────────
//  Player Thread
// ─────────────────────────────────────────────
static void* player_thread(void* arg_ptr) {
    ThreadArg* arg   = (ThreadArg*)arg_ptr;
    int        pidx  = arg->player_idx;
    SharedState* s   = arg->state;

    // Install SIGUSR1 handler for stun
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun;
    sigaction(SIGUSR1, &sa, nullptr);

    while (true) {
        // ── Wait until it is THIS player's turn ──
        pthread_mutex_lock(&s->global_mutex);
        while (s->active_entity != pidx) {
            // Exit conditions
            if (s->phase == PHASE_QUIT ||
                s->phase == PHASE_WIN  ||
                s->phase == PHASE_LOSE) {
                pthread_mutex_unlock(&s->global_mutex);
                return nullptr;
            }
            pthread_cond_wait(&s->turn_cond, &s->global_mutex);
        }
        pthread_mutex_unlock(&s->global_mutex);

        // ── Check if this player is stunned ──
        {
            pthread_mutex_lock(&s->global_mutex);
            bool stunned = s->entities[pidx].stunned;
            pthread_mutex_unlock(&s->global_mutex);

            if (stunned) {
                // Halted for 3 seconds — the stun_tick thread in Arbiter
                // will clear the flag.  We just sleep and re-check.
                // The Arbiter will not advance this player's turn while stunned.
                sleep(1);
                continue;
            }
        }

        // ── Print menu and get input ──────────
        pthread_mutex_lock(&s->global_mutex);
        // Take a read-only snapshot for display
        pthread_mutex_unlock(&s->global_mutex);

        print_menu(s, pidx);

        char choice[8] = {};
        if (scanf("%7s", choice) != 1) continue;

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
                printf("You need Solar Core + Lunar Blade!\n");
                valid = false;
            } else {
                req.action = ACT_ULTIMATE;
            }
            break;
        }

        default:
            printf("Unknown action.\n");
            valid = false;
            break;
        }

        if (!valid) continue;

        // ── Submit action to Arbiter via SHM ──
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
    g_state = shm_attach();

    int np = g_state->num_players;

    // Spawn one thread per player
    pthread_t threads[MAX_PLAYERS];
    ThreadArg args[MAX_PLAYERS];

    for (int i = 0; i < np; ++i) {
        args[i].player_idx = i;
        args[i].state      = g_state;
        pthread_create(&threads[i], nullptr, player_thread, &args[i]);
    }

    // Wait for all player threads to finish
    for (int i = 0; i < np; ++i)
        pthread_join(threads[i], nullptr);

    shm_detach(g_state);
    return 0;
}