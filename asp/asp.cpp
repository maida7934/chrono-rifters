/*
 * asp.cpp  —  Automated Strategic Process
 */
#include "shared/game_state.h"
#include "shared/shm_utils.h"
#include "shared/allocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <climits>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

static SharedState*          g_state       = nullptr;
static volatile sig_atomic_t g_running     = 1;
static pthread_t             g_threads[MAX_ENEMIES];
static int                   g_num_threads = 0;
static bool                  tracked_dead[MAX_ENEMIES] = {};

struct NpcArg { int enemy_slot; SharedState* state; };

static void handle_stun(int)   {}
static void handle_sigterm(int) {
    g_running = 0;
    if (g_state) pthread_cond_broadcast(&g_state->turn_cond);
}

static int ai_pick_target(SharedState* s) {
    int best = -1, best_hp = INT_MAX;
    for (int i = 0; i < s->num_players; ++i) {
        if (s->entities[i].alive && s->entities[i].hp < best_hp) {
            best_hp = s->entities[i].hp; best = i;
        }
    }
    return best;
}

static void ai_move_toward(SharedState* s, int slot, int target,
                           int& dx, int& dy) {
    dx = dy = 0;
    if (target < 0) return;
    Entity& e = s->entities[slot];
    Entity& t = s->entities[target];
    int ddx = t.x - e.x, ddy = t.y - e.y;
    // Step on the dominant axis; small lateral drift on the other.
    if (std::abs(ddx) >= std::abs(ddy))
        dx = (ddx > 0) ? 1 : (ddx < 0 ? -1 : 0);
    else
        dy = (ddy > 0) ? 1 : (ddy < 0 ? -1 : 0);
}

static void maybe_drop_weapon(SharedState* s, int slot) {
    // Spec: NPC weapons are NOT dropped on death
    pthread_mutex_lock(&s->global_mutex);
    bool held = false;
    for (int i = 0; i < INVENTORY_SLOTS; ++i)
        if (s->entities[slot].inventory.slots[i] != WPN_NONE) { held = true; break; }
    pthread_mutex_unlock(&s->global_mutex);
    if (held) return;

    if ((rand() % 100) >= 30) return;

    WeaponID drops[] = { WPN_IRON_HALBERD, WPN_VENOM_DAGGER,
                         WPN_THUNDERSTAFF, WPN_OBSIDIAN_AXE,
                         WPN_FROSTBOW,     WPN_SPLINTER_STICK };
    WeaponID dropped = drops[rand() % 6];

    char msg[LOG_LEN];
    snprintf(msg, LOG_LEN, "[%s] dropped %s!", s->entities[slot].name, WEAPON_TABLE[dropped].name);
    s->log.push(msg);

    pthread_mutex_lock(&s->global_mutex);
    int offer = -1;
    for (int i = 0; i < s->num_players; ++i)
        if (s->entities[i].alive) { offer = i; break; }
    if (offer >= 0) {
        s->weapon_drop_pending = true;
        s->weapon_drop_id      = dropped;
        s->weapon_drop_for     = offer;
    }
    pthread_mutex_unlock(&s->global_mutex);
}

static void maybe_spawn_eclipse(SharedState* s) {
    if (s->total_enemies_killed < 3) return;
    pthread_mutex_lock(&s->resource_table.table_mutex);
    if (s->eclipse_relic_spawned || (rand()%100) >= 40) {
        pthread_mutex_unlock(&s->resource_table.table_mutex); return;
    }
    s->eclipse_relic_spawned = true;
    s->resource_table.entries[2].exists  = true;
    s->resource_table.entries[2].held_by = -1;
    s->resource_table.entries[2].locked  = false;
    pthread_mutex_unlock(&s->resource_table.table_mutex);
    s->log.push("[World] *** Eclipse Relic appeared! ***");
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

static void* npc_thread(void* arg_ptr) {
    NpcArg* arg    = (NpcArg*)arg_ptr;
    int     slot   = arg->enemy_slot;
    SharedState* s = arg->state;

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun; sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, nullptr);

    while (g_running) {
        pthread_mutex_lock(&s->global_mutex);
        while (s->active_entity != slot) {
            if (!g_running || s->phase == PHASE_QUIT ||
                s->phase == PHASE_WIN  || s->phase == PHASE_LOSE ||
                !s->entities[slot].alive) {
                pthread_mutex_unlock(&s->global_mutex);
                return nullptr;
            }
            pthread_cond_wait(&s->turn_cond, &s->global_mutex);
        }
        if (!s->entities[slot].alive) { pthread_mutex_unlock(&s->global_mutex); return nullptr; }

        // Stun wait
        while (s->entities[slot].stunned) {
            pthread_cond_wait(&s->turn_cond, &s->global_mutex);
            if (!g_running) { pthread_mutex_unlock(&s->global_mutex); return nullptr; }
        }
        if (s->active_entity != slot || !s->entities[slot].alive) {
            pthread_mutex_unlock(&s->global_mutex); continue;
        }
        pthread_mutex_unlock(&s->global_mutex);

        ActionRequest req; memset(&req, 0, sizeof(req));
        req.entity_id = slot; req.weapon = WPN_NONE; req.target_id = -1;

        pthread_mutex_lock(&s->global_mutex);
        int target = ai_pick_target(s);
        int dist = INT_MAX;
        if (target >= 0) {
            Entity& me = s->entities[slot];
            Entity& tp = s->entities[target];
            dist = std::abs(tp.x - me.x) + std::abs(tp.y - me.y);
        }
        pthread_mutex_unlock(&s->global_mutex);

        // Mixed AI:
        //   - if no target alive: SKIP
        //   - if very close: STRIKE most of the time
        //   - if far away:    MOVE toward target most of the time
        //   - small chance to SKIP regardless (per spec enemies have STRIKE/SKIP;
        //     MOVE is added as a quality-of-life enhancement so combat feels alive)
        int roll = rand() % 100;
        if (target < 0) {
            req.action = ACT_SKIP;
        } else if (dist <= 4) {
            // Right next to a hero — strike almost always.
            if (roll < 92) {
                req.action    = ACT_STRIKE;
                req.target_id = target;
            } else {
                req.action = ACT_SKIP;
            }
        } else {
            // Far away — still strikes more often than not (Strike has no
            // range cost in this game), occasionally moves to flavour combat.
            if (roll < 75) {
                req.action    = ACT_STRIKE;
                req.target_id = target;
            } else if (roll < 92) {
                req.action = ACT_MOVE;
                pthread_mutex_lock(&s->global_mutex);
                ai_move_toward(s, slot, target, req.move_dx, req.move_dy);
                pthread_mutex_unlock(&s->global_mutex);
            } else {
                req.action = ACT_SKIP;
            }
        }

        usleep(200000 + rand() % 300000);

        pthread_mutex_lock(&s->global_mutex);
        if (!g_running || s->active_entity != slot) {
            pthread_mutex_unlock(&s->global_mutex); continue;
        }
        s->npc_action = req; s->npc_action.ready = true;
        pthread_cond_broadcast(&s->turn_cond);
        pthread_mutex_unlock(&s->global_mutex);

        maybe_spawn_eclipse(s);
    }
    return nullptr;
}

static void* death_watcher(void*) {
    while (g_running) {
        usleep(200000);
        if (!g_state) continue;
        pthread_mutex_lock(&g_state->global_mutex);
        GamePhase ph = g_state->phase;
        int ne = g_state->num_enemies;
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

int main() {
    srand((unsigned)time(nullptr) ^ (unsigned)getpid());
    g_state = shm_attach();
    if (!g_state) { fprintf(stderr, "[ASP] SHM attach failed\n"); return 1; }

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigterm; sigaction(SIGTERM, &sa, nullptr);
    sa.sa_handler = handle_stun;    sigaction(SIGUSR1, &sa, nullptr);

    int ne = g_state->num_enemies; g_num_threads = ne;
    fprintf(stderr, "[ASP] %d NPC thread(s)\n", ne);

    NpcArg args[MAX_ENEMIES];
    for (int i = 0; i < ne; ++i) {
        args[i].enemy_slot = MAX_PLAYERS + i; args[i].state = g_state;
        tracked_dead[i] = false;
        pthread_create(&g_threads[i], nullptr, npc_thread, &args[i]);
    }
    pthread_t t_watcher;
    pthread_create(&t_watcher, nullptr, death_watcher, nullptr);
    for (int i = 0; i < ne; ++i) pthread_join(g_threads[i], nullptr);
    g_running = 0;
    pthread_join(t_watcher, nullptr);
    fprintf(stderr, "[ASP] done\n");
    shm_detach(g_state);
    return 0;
}