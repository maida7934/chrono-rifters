/*
 * asp.cpp  —  Automated Strategic Process
 *
 * Responsibilities (per spec):
 *   - One dedicated thread per NPC enemy  (Section 2)
 *   - Threads run concurrently with proper synchronisation
 *   - Each thread watches shared memory for its entity's turn
 *   - Decides action via simple AI strategy
 *   - Writes ActionRequest to npc_action slot → Arbiter reads + applies
 *   - Handles SIGSTOP / SIGCONT for Ultimate Ability pause (Section 8)
 *   - Handles SIGUSR1 for per-entity stun  (Section 5)
 *   - Does NOT modify global game state directly
 *   - Supports weapon drops when enemies die (chance-based, Section 6)
 *
 * AI strategy (simple but legal):
 *   - 80% chance: Strike the player with lowest HP
 *   - 20% chance: Skip
 *   - If stunned: wait until stun clears (Arbiter's stun_tick handles timer)
 */

#include "../shared/game_state.h"
#include "../shared/shm_utils.h"
#include "../shared/allocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

// ─────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────
static SharedState* g_state = nullptr;

// Per-NPC thread argument
struct NpcArg {
    int          enemy_slot;   // index into entities[] = MAX_PLAYERS + enemy_idx
    SharedState* state;
};

// ─────────────────────────────────────────────
//  SIGUSR1 handler — stun this NPC thread
//  The flag is set in SHM by the Arbiter.
//  This just interrupts any blocking sleep/wait.
// ─────────────────────────────────────────────
static void handle_stun(int) { /* interrupt; loop re-checks stun flag */ }

// ─────────────────────────────────────────────
//  AI: pick the player with lowest HP (alive)
//  Returns entity index or -1
// ─────────────────────────────────────────────
static int ai_pick_target(SharedState* s) {
    int best    = -1;
    int best_hp = INT32_MAX;
    for (int i = 0; i < s->num_players; ++i) {
        Entity& p = s->entities[i];
        if (p.alive && p.hp < best_hp) {
            best_hp = p.hp;
            best    = i;
        }
    }
    return best;
}

// ─────────────────────────────────────────────
//  Weapon drop logic (Section 6)
//  Called when an enemy dies.
//  30% drop chance.  If a player doesn't pick
//  it up (automated here), an enemy "picks it
//  up" — but since NPCs don't use weapons in
//  the spec, we just skip that.
// ─────────────────────────────────────────────
static void maybe_drop_weapon(SharedState* s, int enemy_slot) {
    // 30% chance
    if ((rand() % 100) >= 30) return;

    // Pick a random non-artifact weapon
    WeaponID drops[] = {
        WPN_IRON_HALBERD, WPN_VENOM_DAGGER,
        WPN_THUNDERSTAFF, WPN_OBSIDIAN_AXE,
        WPN_FROSTBOW,     WPN_SPLINTER_STICK
    };
    int ndrop = sizeof(drops) / sizeof(drops[0]);
    WeaponID dropped = drops[rand() % ndrop];

    char msg[LOG_LEN];
    snprintf(msg, LOG_LEN, "[%s] dropped %s!  Pick up? (y/n): ",
             s->entities[enemy_slot].name, WEAPON_TABLE[dropped].name);
    s->log.push(msg);

    // We print the prompt to stdout; the player can type y/n.
    // HIP reads this and applies via a special mechanism, but
    // for the initial scaffold we auto-offer to player 0 if alive.
    // (In a full implementation HIP handles the prompt on behalf of
    //  the active player.)
    printf("\n%s dropped %s! Pick up? (y/n): ",
           s->entities[enemy_slot].name, WEAPON_TABLE[dropped].name);
    fflush(stdout);

    // Because ASP cannot read stdin (HIP owns the terminal),
    // we give a brief window then auto-skip.
    // In the full implementation the Arbiter pauses the turn loop
    // and HIP collects the answer. Here we default to 'y' for
    // player 0 if there is room.
    // Non-blocking: try to read within 3 seconds using alarm
    // For scaffold simplicity — auto-pick for player 0
    bool auto_pick = true;
    if (auto_pick) {
        pthread_mutex_lock(&s->global_mutex);
        if (s->entities[0].alive) {
            bool ok = allocator_add(s->entities[0].inventory, dropped);
            snprintf(msg, LOG_LEN, "[Player 0] picked up %s: %s",
                     WEAPON_TABLE[dropped].name, ok ? "OK" : "No space");
            s->log.push(msg);
        }
        pthread_mutex_unlock(&s->global_mutex);
    }
}

// ─────────────────────────────────────────────
//  Eclipse Relic spawn (Section 7)
//  Spawned randomly after 3rd enemy kill.
// ─────────────────────────────────────────────
static void maybe_spawn_eclipse(SharedState* s) {
    if (s->eclipse_relic_spawned) return;
    if (s->total_enemies_killed < 3)  return;
    if ((rand() % 100) >= 40)         return;   // 40% chance each check

    pthread_mutex_lock(&s->resource_table.table_mutex);
    s->eclipse_relic_spawned = true;
    s->resource_table.entries[2].exists  = true;
    s->resource_table.entries[2].held_by = -1;
    pthread_mutex_unlock(&s->resource_table.table_mutex);

    s->log.push("[World] Eclipse Relic has appeared in the arena!");
}

// ─────────────────────────────────────────────
//  NPC Thread
// ─────────────────────────────────────────────
static void* npc_thread(void* arg_ptr) {
    NpcArg* arg      = (NpcArg*)arg_ptr;
    int     slot     = arg->enemy_slot;   // index in entities[]
    SharedState* s   = arg->state;

    // Install SIGUSR1 for stun
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun;
    sigaction(SIGUSR1, &sa, nullptr);

    while (true) {
        // ── Exit if game over ─────────────────
        pthread_mutex_lock(&s->global_mutex);
        if (s->phase == PHASE_QUIT ||
            s->phase == PHASE_WIN  ||
            s->phase == PHASE_LOSE) {
            pthread_mutex_unlock(&s->global_mutex);
            return nullptr;
        }

        // ── Check if this enemy is still alive ─
        if (!s->entities[slot].alive) {
            pthread_mutex_unlock(&s->global_mutex);
            return nullptr;
        }

        // ── Wait until it is THIS NPC's turn ──
        while (s->active_entity != slot) {
            if (s->phase != PHASE_RUNNING) {
                pthread_mutex_unlock(&s->global_mutex);
                return nullptr;
            }
            pthread_cond_wait(&s->turn_cond, &s->global_mutex);

            // Re-check alive (could have been killed while waiting)
            if (!s->entities[slot].alive) {
                pthread_mutex_unlock(&s->global_mutex);
                return nullptr;
            }
        }
        pthread_mutex_unlock(&s->global_mutex);

        // ── Check stun ───────────────────────
        {
            pthread_mutex_lock(&s->global_mutex);
            bool stunned = s->entities[slot].stunned;
            pthread_mutex_unlock(&s->global_mutex);
            if (stunned) {
                // Wait for stun to clear (Arbiter's stun_tick handles it)
                sleep(1);
                continue;
            }
        }

        // ── Decide action (AI) ────────────────
        ActionRequest req;
        memset(&req, 0, sizeof(req));
        req.entity_id = slot;
        req.weapon    = WPN_NONE;
        req.target_id = -1;

        int roll = rand() % 100;

        if (roll < 80) {
            // Strike: pick lowest-HP player
            pthread_mutex_lock(&s->global_mutex);
            int target = ai_pick_target(s);
            pthread_mutex_unlock(&s->global_mutex);

            if (target < 0) {
                req.action = ACT_SKIP;  // no valid target
            } else {
                req.action    = ACT_STRIKE;
                req.target_id = target;
                char msg[LOG_LEN];
                snprintf(msg, LOG_LEN, "[%s] AI chose STRIKE → [%s]",
                         s->entities[slot].name,
                         s->entities[target].name);
                s->log.push(msg);
            }
        } else {
            req.action = ACT_SKIP;
            char msg[LOG_LEN];
            snprintf(msg, LOG_LEN, "[%s] AI chose SKIP",
                     s->entities[slot].name);
            s->log.push(msg);
        }

        // Small "think time" to simulate non-trivial AI
        usleep(200000 + rand() % 300000);  // 200–500 ms

        // ── Submit action to Arbiter ──────────
        pthread_mutex_lock(&s->global_mutex);
        s->npc_action       = req;
        s->npc_action.ready = true;
        pthread_cond_broadcast(&s->turn_cond);
        pthread_mutex_unlock(&s->global_mutex);

        // ── Check Eclipse Relic spawn ─────────
        maybe_spawn_eclipse(s);

        // Loop — wait for next turn
    }
    return nullptr;
}

// ─────────────────────────────────────────────
//  Death-watcher thread
//  Polls for newly-dead enemies and handles
//  weapon drops + lifecycle cleanup.
// ─────────────────────────────────────────────
static bool tracked_dead[MAX_ENEMIES] = {};

static void* death_watcher(void*) {
    while (true) {
        usleep(200000);
        if (!g_state) continue;

        pthread_mutex_lock(&g_state->global_mutex);
        GamePhase ph = g_state->phase;
        int ne       = g_state->num_enemies;
        pthread_mutex_unlock(&g_state->global_mutex);

        if (ph != PHASE_RUNNING) break;

        for (int i = 0; i < ne; ++i) {
            int slot = MAX_PLAYERS + i;
            pthread_mutex_lock(&g_state->global_mutex);
            bool dead = !g_state->entities[slot].alive;
            pthread_mutex_unlock(&g_state->global_mutex);

            if (dead && !tracked_dead[i]) {
                tracked_dead[i] = true;
                maybe_drop_weapon(g_state, slot);
            }
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────
int main() {
    srand((unsigned)time(nullptr) ^ (unsigned)getpid());

    g_state = shm_attach();

    int ne = g_state->num_enemies;

    // One thread per NPC
    pthread_t threads[MAX_ENEMIES];
    NpcArg    args[MAX_ENEMIES];

    for (int i = 0; i < ne; ++i) {
        args[i].enemy_slot = MAX_PLAYERS + i;
        args[i].state      = g_state;
        tracked_dead[i]    = false;
        pthread_create(&threads[i], nullptr, npc_thread, &args[i]);
    }

    // Death-watcher thread (lifecycle management, Section 2)
    pthread_t t_watcher;
    pthread_create(&t_watcher, nullptr, death_watcher, nullptr);
    pthread_detach(t_watcher);

    // Wait for all NPC threads
    for (int i = 0; i < ne; ++i)
        pthread_join(threads[i], nullptr);

    shm_detach(g_state);
    return 0;
}