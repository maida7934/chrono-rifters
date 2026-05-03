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
 *     (OS handles SIGSTOP — no code needed; threads resume on SIGCONT)
 *   - Handles SIGUSR1 for per-entity stun  (Section 5)
 *   - Does NOT modify global game state directly
 *   - Supports weapon drops when enemies die (chance-based, Section 6)
 *
 * AI strategy:
 *   - 80% chance: Strike the player with lowest HP
 *   - 20% chance: Skip
 *   - Think delay 200–500ms to simulate non-trivial AI
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
static SharedState*          g_state       = nullptr;
static volatile sig_atomic_t g_running     = 1;
static pthread_t             g_threads[MAX_ENEMIES];
static int                   g_num_threads = 0;

// Per-NPC thread argument
struct NpcArg {
    int          enemy_slot;   // index into entities[] = MAX_PLAYERS + enemy_idx
    SharedState* state;
};

// ─────────────────────────────────────────────
//  SIGUSR1 handler — stun interrupt
//  The flag is set in SHM by the Arbiter.
//  This just interrupts any blocking sleep/wait.
// ─────────────────────────────────────────────
static void handle_stun(int) { /* interrupt; loop re-checks stun flag */ }

// ─────────────────────────────────────────────
//  SIGTERM handler — graceful shutdown
//  (sent by Arbiter on game over)
// ─────────────────────────────────────────────
static void handle_sigterm(int) {
    g_running = 0;
    if (g_state) {
        pthread_cond_broadcast(&g_state->turn_cond);
    }
}

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
//  30% drop chance.  Sets weapon_drop_pending in SHM
//  so HIP can prompt the active player for pickup.
// ─────────────────────────────────────────────
static void maybe_drop_weapon(SharedState* s, int enemy_slot) {
    // Find if NPC held a weapon
    pthread_mutex_lock(&s->global_mutex);
    bool held_weapon = false;
    for (int i = 0; i < INVENTORY_SLOTS; ++i) {
        if (s->entities[enemy_slot].inventory.slots[i] != WPN_NONE) {
            held_weapon = true;
            break;
        }
    }
    pthread_mutex_unlock(&s->global_mutex);

    if (held_weapon) return; // Spec: "If an NPC holds a weapon at the time of its death, the weapon is not dropped."

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
    snprintf(msg, LOG_LEN, "[%s] dropped %s!",
             s->entities[enemy_slot].name, WEAPON_TABLE[dropped].name);
    s->log.push(msg);

    // Find the first alive player to offer the weapon to
    pthread_mutex_lock(&s->global_mutex);

    int offer_to = -1;
    // Prefer the currently active player if they're a player
    if (s->active_entity >= 0 && s->active_entity < s->num_players &&
        s->entities[s->active_entity].alive) {
        offer_to = s->active_entity;
    } else {
        // Otherwise find first alive player
        for (int i = 0; i < s->num_players; ++i) {
            if (s->entities[i].alive) { offer_to = i; break; }
        }
    }

    if (offer_to >= 0) {
        // Set the weapon drop notification for HIP to pick up
        s->weapon_drop_pending = true;
        s->weapon_drop_id      = dropped;
        s->weapon_drop_for     = offer_to;
        snprintf(msg, LOG_LEN, "[World] %s available for [%s] — pick up on next turn.",
                 WEAPON_TABLE[dropped].name, s->entities[offer_to].name);
        s->log.push(msg);
    } else {
        // No players alive — weapon is lost
        snprintf(msg, LOG_LEN, "[World] %s lost — no player to receive it.",
                 WEAPON_TABLE[dropped].name);
        s->log.push(msg);
    }

    pthread_mutex_unlock(&s->global_mutex);
}

// ─────────────────────────────────────────────
//  Eclipse Relic spawn (Section 7)
//  Spawned with 40% chance after 3rd enemy kill.
//  Logs the event so HIP/TUI can display it.
// ─────────────────────────────────────────────
static void maybe_spawn_eclipse(SharedState* s) {
    if (s->total_enemies_killed < 3)  return;

    pthread_mutex_lock(&s->resource_table.table_mutex);
    if (s->eclipse_relic_spawned) {
        pthread_mutex_unlock(&s->resource_table.table_mutex);
        return;
    }
    if ((rand() % 100) >= 40) {
        pthread_mutex_unlock(&s->resource_table.table_mutex);
        return;
    }

    s->eclipse_relic_spawned = true;
    s->resource_table.entries[2].exists  = true;
    s->resource_table.entries[2].held_by = -1;
    s->resource_table.entries[2].locked  = false;
    pthread_mutex_unlock(&s->resource_table.table_mutex);

    s->log.push("[World] *** Eclipse Relic has appeared in the arena! ***");

    // Also notify via weapon drop mechanism so HIP prompts the player
    pthread_mutex_lock(&s->global_mutex);
    for (int i = 0; i < s->num_players; ++i) {
        if (s->entities[i].alive) {
            s->weapon_drop_pending = true;
            s->weapon_drop_id      = WPN_ECLIPSE_RELIC;
            s->weapon_drop_for     = i;
            break;
        }
    }
    pthread_mutex_unlock(&s->global_mutex);
}

// ─────────────────────────────────────────────
//  NPC Thread
//
//  Each NPC enemy has its own dedicated thread.
//  Only the thread matching active_entity may
//  submit an action; all others block on condvar.
// ─────────────────────────────────────────────
static void* npc_thread(void* arg_ptr) {
    NpcArg* arg      = (NpcArg*)arg_ptr;
    int     slot     = arg->enemy_slot;   // index in entities[]
    SharedState* s   = arg->state;

    // Install SIGUSR1 for stun on this thread
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun;
    sa.sa_flags   = 0;  // no SA_RESTART — let signals interrupt blocking calls
    sigaction(SIGUSR1, &sa, nullptr);

    while (g_running) {
        // ── Wait until it is THIS NPC's turn ─────
        pthread_mutex_lock(&s->global_mutex);

        // Exit conditions
        while (s->active_entity != slot) {
            if (!g_running ||
                s->phase == PHASE_QUIT ||
                s->phase == PHASE_WIN  ||
                s->phase == PHASE_LOSE ||
                !s->entities[slot].alive) {
                pthread_mutex_unlock(&s->global_mutex);
                return nullptr;
            }
            pthread_cond_wait(&s->turn_cond, &s->global_mutex);
        }

        // Re-check alive after waking (could have been killed)
        if (!s->entities[slot].alive) {
            pthread_mutex_unlock(&s->global_mutex);
            return nullptr;
        }

        // ── Stun-wait block (Phase 5, item 7) ────
        // If stunned, wait via condvar until Arbiter clears the flag.
        while (s->entities[slot].stunned) {
            pthread_cond_wait(&s->turn_cond, &s->global_mutex);
            if (!g_running) {
                pthread_mutex_unlock(&s->global_mutex);
                return nullptr;
            }
        }
        if (s->active_entity != slot || !s->entities[slot].alive) {
            pthread_mutex_unlock(&s->global_mutex);
            goto next_turn;
        }
        pthread_mutex_unlock(&s->global_mutex);

        // ── Decide action (AI) ───────────────────
        {
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
                }
            } else {
                req.action = ACT_SKIP;
            }

            // Small "think time" to simulate non-trivial AI (200–500ms)
            usleep(200000 + rand() % 300000);

            // ── Submit action to Arbiter ──────────
            pthread_mutex_lock(&s->global_mutex);
            // Final check before submit
            if (!g_running || s->active_entity != slot) {
                pthread_mutex_unlock(&s->global_mutex);
                continue;
            }
            s->npc_action       = req;
            s->npc_action.ready = true;
            pthread_cond_broadcast(&s->turn_cond);
            pthread_mutex_unlock(&s->global_mutex);

            // ── Check Eclipse Relic spawn ─────────
            maybe_spawn_eclipse(s);
        }

next_turn:
        ;  // continue outer loop
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
    while (g_running) {
        usleep(200000);   // check every 200ms
        if (!g_state) continue;

        pthread_mutex_lock(&g_state->global_mutex);
        GamePhase ph = g_state->phase;
        int ne       = g_state->num_enemies;
        pthread_mutex_unlock(&g_state->global_mutex);

        if (ph != PHASE_RUNNING && ph != PHASE_ULTIMATE_PAUSE) break;

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

    // Attach to shared memory (created by Arbiter)
    g_state = shm_attach();
    if (!g_state) {
        fprintf(stderr, "[ASP] Failed to attach shared memory!\n");
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

    int ne = g_state->num_enemies;
    g_num_threads = ne;

    fprintf(stderr, "[ASP] Starting with %d NPC thread(s).\n", ne);

    // One thread per NPC
    NpcArg args[MAX_ENEMIES];
    for (int i = 0; i < ne; ++i) {
        args[i].enemy_slot = MAX_PLAYERS + i;
        args[i].state      = g_state;
        tracked_dead[i]    = false;
        pthread_create(&g_threads[i], nullptr, npc_thread, &args[i]);
    }

    // Death-watcher thread (lifecycle management, Section 2)
    pthread_t t_watcher;
    pthread_create(&t_watcher, nullptr, death_watcher, nullptr);

    // Wait for all NPC threads
    for (int i = 0; i < ne; ++i)
        pthread_join(g_threads[i], nullptr);

    // Signal death watcher to stop and wait
    g_running = 0;
    pthread_join(t_watcher, nullptr);

    fprintf(stderr, "[ASP] Shutting down.\n");
    shm_detach(g_state);
    return 0;
}