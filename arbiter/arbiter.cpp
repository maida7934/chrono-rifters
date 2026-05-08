/*
 * arbiter.cpp  —  Game Arbiter Process
 *
 * Rules enforced per project spec:
 *  - Player HP = RollNo + rand(100..1000)
 *  - Enemy  HP = (RollNo % 100) + rand(50..200)
 *  - Player Damage = (RollNo % 10) + 10
 *  - Enemy  Damage = ((RollNo/10) % 10) + 10
 *  - Player Speed  = 100 / num_players
 *  - Enemy  Speed  = rand(10..30)
 *  - Player MaxStamina = 100,  Enemy MaxStamina = 150
 *  - Win = 10 kills, Lose = all players dead, Quit = SIGTERM from HIP
 *  - Weapon drop on enemy death (30% chance), NPC weapons NOT dropped
 *  - Inventory 20 slots linear array with contiguous allocation
 *  - Swap-in costs a full turn; weapon unusable same turn
 *  - Ultimate: need Solar Core + Lunar Blade in primary inventory
 *  - Ultimate: SIGSTOP ASP for 10s, SIGALRM + SIGCONT resumption
 *  - Deadlock monitor: DFS cycle detection, force-release victim
 *  - Stun: 3s via SIGUSR1, skip turn if stamina was full
 *  - NPC turn timeout: 3s -> SKIP
 *  - Eclipse Relic spawned dynamically after 3 kills (40% chance)
 *  - TUI: diamond/rhombus battlefield, rich colour, rendering thread
 */

#include "../shared/game_state.h"
#include "../shared/shm_utils.h"
#include "../shared/allocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <ncurses.h>
#include <algorithm>
#include <climits>
#include <functional>

// ─────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────
static SharedState* g_state  = nullptr;
static pid_t        g_hip_pid = -1;
static pid_t        g_asp_pid = -1;
static volatile bool g_ultimate_running = false;
static volatile sig_atomic_t g_sigterm_received = 0;
static volatile sig_atomic_t g_sigalrm_received = 0;
static volatile sig_atomic_t g_sigchld_received  = 0;

// Battlefield logical bounds
constexpr int BF_W = 80;
constexpr int BF_H = 26;

// ─────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────
static void handle_sigterm(int) { g_sigterm_received = 1; }
static void handle_sigalrm(int) { g_sigalrm_received = 1; }
static void handle_sigchld(int) { g_sigchld_received = 1; }

static void process_pending_signals() {
    if (!g_state) return;
    if (g_sigterm_received) {
        g_sigterm_received = 0;
        g_state->phase = PHASE_QUIT;
        g_state->log.push("[ARBITER] SIGTERM -> shutting down.");
        pthread_cond_broadcast(&g_state->turn_cond);
    }
    if (g_sigalrm_received) {
        g_sigalrm_received = 0;
        if (g_ultimate_running) {
            g_ultimate_running = false;
            if (g_asp_pid > 0) kill(g_asp_pid, SIGCONT);
            g_state->log.push("[Arbiter] Ultimate window expired. ASP resumed.");
            g_state->phase           = PHASE_RUNNING;
            g_state->ultimate_active = false;
            pthread_cond_broadcast(&g_state->turn_cond);
        }
    }
    if (g_sigchld_received) {
        g_sigchld_received = 0;
        int status; pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            char msg[LOG_LEN];
            snprintf(msg, LOG_LEN, "[Arbiter] Child PID %d exited.", (int)pid);
            g_state->log.push(msg);
            if (pid == g_hip_pid) {
                for (int i = 0; i < g_state->num_players; ++i)
                    g_state->entities[i].alive = false;
                g_hip_pid = -1;
                g_state->log.push("[Arbiter] HIP died — all players marked dead.");
            }
            if (pid == g_asp_pid) {
                for (int i = 0; i < g_state->num_enemies; ++i)
                    g_state->entities[MAX_PLAYERS + i].alive = false;
                g_state->npc_action.ready = false;
                g_asp_pid = -1;
                g_state->log.push("[Arbiter] ASP died — all enemies marked dead.");
            }
            pthread_cond_broadcast(&g_state->turn_cond);
        }
    }
}

// ─────────────────────────────────────────────
//  Wall-clock helper (used by hit-flash FX and wave spawn timing)
// ─────────────────────────────────────────────
static inline double wallclock_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ─────────────────────────────────────────────
//  Wave spawning — keeps the field populated until the player reaches
//  WIN_KILL_COUNT total kills.  Holds the global mutex on entry.
// ─────────────────────────────────────────────
static void place_enemy_at_slot(int i) {
    Entity& e = g_state->entities[MAX_PLAYERS + i];
    int row    = i / 3;
    int col    = i % 3;
    int ex = (BF_W * (col + 1)) / 4 + ((row & 1) ? 4 : 0) + 6;
    int ey = (row == 0) ? 4 : (row == 1) ? (BF_H / 2) : (BF_H - 8);
    e.x = std::max(2, std::min(BF_W - 2, ex));
    e.y = std::max(2, std::min(BF_H - 1, ey));
}

static int  g_wave_index = 0;
static void maybe_spawn_wave() {
    constexpr int TARGET_ALIVE = 3;
    int killed    = g_state->total_enemies_killed;
    int remaining = std::max(0, WIN_KILL_COUNT - killed);
    if (remaining <= 0) return;

    int alive = 0;
    for (int i = 0; i < g_state->num_enemies; ++i)
        if (g_state->entities[MAX_PLAYERS + i].alive) ++alive;

    int want = std::min(TARGET_ALIVE, remaining);
    if (alive >= want) return;

    char msg[LOG_LEN];
    snprintf(msg, LOG_LEN,
             "*** WAVE %d INCOMING — %d killed / %d to go ***",
             ++g_wave_index, killed, WIN_KILL_COUNT);
    g_state->log.push(msg);

    int roll_no = (rand() % 90) + 10;

    // First, revive dead slots in-place
    for (int i = 0; i < g_state->num_enemies && alive < want; ++i) {
        Entity& e = g_state->entities[MAX_PLAYERS + i];
        if (e.alive) continue;
        e.init_enemy(i, roll_no);
        place_enemy_at_slot(i);
        ++alive;
    }

    // If we still need more, extend num_enemies up to MAX_ENEMIES
    while (g_state->num_enemies < MAX_ENEMIES && alive < want) {
        int i = g_state->num_enemies;
        Entity& e = g_state->entities[MAX_PLAYERS + i];
        e.init_enemy(i, roll_no);
        place_enemy_at_slot(i);
        ++g_state->num_enemies;
        ++alive;
    }
}

// ─────────────────────────────────────────────
//  Win / Lose check
// ─────────────────────────────────────────────
static bool check_game_over() {
    bool any_alive = false;
    for (int i = 0; i < g_state->num_players; ++i)
        if (g_state->entities[i].alive) { any_alive = true; break; }
    if (!any_alive) { g_state->phase = PHASE_LOSE; return true; }
    if (g_state->total_enemies_killed >= WIN_KILL_COUNT) { g_state->phase = PHASE_WIN; return true; }
    return false;
}

// ─────────────────────────────────────────────
//  Scheduler
// ─────────────────────────────────────────────
static int scheduler_next() {
    int np = g_state->num_players, ne = g_state->num_enemies;
    int indices[MAX_ENTITIES]; int count = 0;
    for (int i = 0; i < np; ++i) indices[count++] = i;
    for (int i = 0; i < ne; ++i) indices[count++] = MAX_PLAYERS + i;

    float min_dt = 1e9f; bool any = false;
    for (int k = 0; k < count; ++k) {
        Entity& e = g_state->entities[indices[k]];
        if (!e.alive || e.stunned) continue;
        any = true;
        float dt = (e.max_stamina - e.stamina) / e.speed;
        if (dt < min_dt) min_dt = dt;
    }
    if (!any) return -1;

    for (int k = 0; k < count; ++k) {
        Entity& e = g_state->entities[indices[k]];
        if (!e.alive) continue;
        if (!e.stunned) {
            e.stamina += e.speed * min_dt;
            if (e.stamina > e.max_stamina) e.stamina = e.max_stamina;
        }
    }
    g_state->virtual_time += min_dt;

    for (int k = 0; k < count; ++k) {
        Entity& e = g_state->entities[indices[k]];
        if (e.alive && !e.stunned && e.stamina >= e.max_stamina) {
            e.swapped_weapon_unavailable = WPN_NONE;
            return indices[k];
        }
    }
    return -1;
}

// ─────────────────────────────────────────────
//  Artifact helpers
// ─────────────────────────────────────────────
static bool artifact_acquire(int entity_id, WeaponID wpn) {
    ResourceTable& rt  = g_state->resource_table;
    WaitForGraph&  wfg = g_state->wait_graph;
    int aidx = rt.find(wpn);
    if (aidx < 0) return false;
    pthread_mutex_lock(&rt.table_mutex);
    ArtifactEntry& ae = rt.entries[aidx];
    if (!ae.exists) { pthread_mutex_unlock(&rt.table_mutex); return false; }
    if (ae.held_by < 0) {
        ae.held_by = entity_id; ae.locked = true;
        wfg.holding[entity_id][aidx] = 1;
        wfg.waiting_for[entity_id] = -1;
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[%s] acquired %s",
                 g_state->entities[entity_id].name, WEAPON_TABLE[wpn].name);
        g_state->log.push(msg);
        pthread_mutex_unlock(&rt.table_mutex);
        return true;
    } else {
        wfg.waiting_for[entity_id] = aidx;
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[%s] waiting for %s (held by [%s])",
                 g_state->entities[entity_id].name, WEAPON_TABLE[wpn].name,
                 g_state->entities[ae.held_by].name);
        g_state->log.push(msg);
        pthread_mutex_unlock(&rt.table_mutex);
        return false;
    }
}

static void artifact_release(int entity_id, WeaponID wpn) {
    ResourceTable& rt  = g_state->resource_table;
    WaitForGraph&  wfg = g_state->wait_graph;
    int aidx = rt.find(wpn);
    if (aidx < 0) return;
    pthread_mutex_lock(&rt.table_mutex);
    ArtifactEntry& ae = rt.entries[aidx];
    if (ae.held_by == entity_id) {
        ae.held_by = -1; ae.locked = false;
        wfg.holding[entity_id][aidx] = 0;
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[%s] released %s",
                 g_state->entities[entity_id].name, WEAPON_TABLE[wpn].name);
        g_state->log.push(msg);
    }
    pthread_mutex_unlock(&rt.table_mutex);
}

static void artifact_release_all(int entity_id) {
    ResourceTable& rt  = g_state->resource_table;
    WaitForGraph&  wfg = g_state->wait_graph;
    pthread_mutex_lock(&rt.table_mutex);
    for (int a = 0; a < NUM_ARTIFACTS; ++a) {
        if (rt.entries[a].held_by == entity_id) {
            rt.entries[a].held_by = -1; rt.entries[a].locked = false;
        }
        wfg.holding[entity_id][a] = 0;
    }
    wfg.waiting_for[entity_id] = -1;
    pthread_mutex_unlock(&rt.table_mutex);
}

// ─────────────────────────────────────────────
//  Stun delivery
// ─────────────────────────────────────────────
static void deliver_stun(int target_id) {
    Entity& t = g_state->entities[target_id];
    t.stunned = true;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    t.stun_end_time = ts.tv_sec + (ts.tv_nsec / 1e9) + STUN_DURATION;
    if (t.stamina >= t.max_stamina) t.skip_turn_from_stun = true;
    char msg[LOG_LEN];
    snprintf(msg, LOG_LEN, "[%s] STUNNED for %.0fs!", t.name, STUN_DURATION);
    g_state->log.push(msg);
    pid_t owner = (t.type == ENT_PLAYER) ? g_state->hip_pid : g_state->asp_pid;
    if (owner > 0) kill(owner, SIGUSR1);
}

static void resolve_weapon_drop_fallback_now() {
    if (!g_state->weapon_drop_pending) return;
    WeaponID dropped = g_state->weapon_drop_id;
    if (dropped == WPN_NONE) {
        g_state->weapon_drop_pending = false;
        g_state->weapon_drop_for = -1;
        return;
    }

    // Spec §6: enemy is GUARANTEED to pick it up.
    // Iterate alive enemies sequentially; give to the first with space.
    bool picked = false;
    for (int i = 0; i < g_state->num_enemies; ++i) {
        Entity& e = g_state->entities[MAX_PLAYERS + i];
        if (!e.alive) continue;
        bool added = allocator_add(e.inventory, dropped);
        if (added) {
            char msg[LOG_LEN];
            snprintf(msg, LOG_LEN, "[%s] picked up dropped %s.",
                     e.name, WEAPON_TABLE[dropped].name);
            g_state->log.push(msg);
            picked = true;
            break;  // only one enemy gets it
        }
    }
    if (!picked) {
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[Drop] %s vanished (no enemy had space).",
                 WEAPON_TABLE[dropped].name);
        g_state->log.push(msg);
    }

    // Important per spec: NPC weapons are NOT dropped when that NPC dies
    g_state->weapon_drop_pending = false;
    g_state->weapon_drop_id = WPN_NONE;
    g_state->weapon_drop_for = -1;
    g_state->weapon_drop_turns_left = 0;
}

// ─────────────────────────────────────────────
//  Apply action
// ─────────────────────────────────────────────
static void apply_action(ActionRequest& req) {
    Entity& actor = g_state->entities[req.entity_id];
    char msg[LOG_LEN];

    // Spec §6: if a weapon drop is pending for this player and they chose
    // anything other than ACT_PICKUP, check the grace period.
    if (g_state->weapon_drop_pending &&
        g_state->weapon_drop_for == req.entity_id &&
        req.action != ACT_PICKUP) {
        if (g_state->weapon_drop_turns_left <= 0) {
            // Grace period expired — enemy guaranteed pickup
            snprintf(msg, LOG_LEN, "[%s] ignored %s -> enemy picks it up.",
                     actor.name, WEAPON_TABLE[g_state->weapon_drop_id].name);
            g_state->log.push(msg);
            resolve_weapon_drop_fallback_now();
        } else {
            // Still within grace period — decrement and let player act
            --g_state->weapon_drop_turns_left;
        }
    }

    switch (req.action) {

    case ACT_MOVE: {
        int nx = std::max(1, std::min(BF_W, actor.x + req.move_dx));
        int ny = std::max(1, std::min(BF_H, actor.y + req.move_dy));
        // Block movement onto another living entity.
        bool blocked = false;
        int n_total = g_state->num_players + g_state->num_enemies;
        for (int j = 0; j < n_total; ++j) {
            int idx = (j < g_state->num_players)
                    ? j
                    : (MAX_PLAYERS + j - g_state->num_players);
            if (idx == req.entity_id) continue;
            Entity& o = g_state->entities[idx];
            if (!o.alive) continue;
            if (o.x == nx && o.y == ny) { blocked = true; break; }
        }
        if (!blocked) { actor.x = nx; actor.y = ny; }
        snprintf(msg, LOG_LEN, "[%s] MOVE -> (%d,%d)%s",
                 actor.name, actor.x, actor.y, blocked ? " (blocked)" : "");
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_STRIKE: {
        if (req.target_id < 0) break;
        Entity& tgt = g_state->entities[req.target_id];
        if (!tgt.alive) break;
        tgt.hp = std::max(0, tgt.hp - actor.damage);
        tgt.last_hit_time = wallclock_sec();
        snprintf(msg, LOG_LEN, "[%s] STRIKE -> [%s] -%d HP (%d/%d)",
                 actor.name, tgt.name, actor.damage, tgt.hp, tgt.max_hp);
        g_state->log.push(msg);

        // Knockback: push the target one cell away from the attacker.
        // Movement is free in this game; this just affects positioning
        // for AoE targeting.
        if (tgt.alive) {
            int kx = (tgt.x > actor.x) ? 1 : (tgt.x < actor.x ? -1 : 0);
            int ky = (tgt.y > actor.y) ? 1 : (tgt.y < actor.y ? -1 : 0);
            if (kx == 0 && ky == 0) kx = 1;     // bump to the right if stacked
            int nx = tgt.x + kx;
            int ny = tgt.y + ky;
            // Keep knockback away from top edge and avoid stacking at borders.
            if (nx < 1 || nx > BF_W || ny < 2 || ny > BF_H) {
                snprintf(msg, LOG_LEN, "  >> [%s] resisted knockback at edge",
                         tgt.name);
            } else {
                tgt.x = nx;
                tgt.y = ny;
                snprintf(msg, LOG_LEN, "  >> [%s] knocked to (%d,%d)",
                         tgt.name, tgt.x, tgt.y);
            }
            g_state->log.push(msg);
        }

        if (tgt.alive && actor.damage >= 15 && (rand() % 100) < 20)
            deliver_stun(req.target_id);
        if (tgt.hp == 0) {
            tgt.alive = false;
            snprintf(msg, LOG_LEN, "[%s] DEFEATED!", tgt.name);
            g_state->log.push(msg);
            if (tgt.type == ENT_ENEMY) {
                ++g_state->total_enemies_killed;
                artifact_release_all(req.target_id);

                // ── Weapon drop trigger (spec §6: 30% chance) ──
                if (actor.type == ENT_PLAYER &&
                    !g_state->weapon_drop_pending) {
                    if ((rand() % 100) < 30) {
                        WeaponID drop_pool[] = {
                            WPN_IRON_HALBERD, WPN_VENOM_DAGGER, WPN_THUNDERSTAFF,
                            WPN_OBSIDIAN_AXE, WPN_FROSTBOW,     WPN_SPLINTER_STICK
                        };
                        WeaponID dropped = drop_pool[rand() % 6];
                        g_state->weapon_drop_pending    = true;
                        g_state->weapon_drop_id         = dropped;
                        g_state->weapon_drop_for        = req.entity_id;
                        g_state->weapon_drop_turns_left = 1; // 1-turn grace
                        snprintf(msg, LOG_LEN,
                            "[DROP] %s dropped \xe2\x80\x94 [%s] press P to pick up!",
                            WEAPON_TABLE[dropped].name, actor.name);
                        g_state->log.push(msg);
                    }
                }

                if (g_state->total_enemies_killed >= WIN_KILL_COUNT)
                    { g_state->phase = PHASE_WIN; actor.stamina = 0; return; }
            }
        }
        actor.stamina = 0;
        break;
    }

    case ACT_EXHAUST: {
        if (req.target_id < 0) break;
        Entity& tgt = g_state->entities[req.target_id];
        if (!tgt.alive) break;
        tgt.stamina = std::max(0.0f, tgt.stamina - actor.damage);
        snprintf(msg, LOG_LEN, "[%s] EXHAUST -> [%s] stamina -%d",
                 actor.name, tgt.name, actor.damage);
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_USE_WEAPON: {
        if (req.target_id < 0 || req.weapon == WPN_NONE) break;
        if (req.weapon == actor.swapped_weapon_unavailable) {
            snprintf(msg, LOG_LEN, "[%s] %s still readying from swap!",
                     actor.name, WEAPON_TABLE[req.weapon].name);
            g_state->log.push(msg); actor.stamina = 0; break;
        }
        if (!actor.inventory.has(req.weapon)) {
            snprintf(msg, LOG_LEN, "[%s] doesn't have %s!",
                     actor.name, WEAPON_TABLE[req.weapon].name);
            g_state->log.push(msg); actor.stamina = 0; break;
        }
        if (WEAPON_TABLE[req.weapon].is_artifact) {
            if (!artifact_acquire(req.entity_id, req.weapon)) {
                snprintf(msg, LOG_LEN, "[%s] can't acquire %s — blocked!",
                         actor.name, WEAPON_TABLE[req.weapon].name);
                g_state->log.push(msg); actor.stamina = 0; break;
            }
        }
        Entity& tgt = g_state->entities[req.target_id];
        if (!tgt.alive) {
            if (WEAPON_TABLE[req.weapon].is_artifact)
                artifact_release(req.entity_id, req.weapon);
            actor.stamina = 0; break;
        }
        int dmg = WEAPON_TABLE[req.weapon].damage;
        tgt.hp = std::max(0, tgt.hp - dmg);
        tgt.last_hit_time = wallclock_sec();
        snprintf(msg, LOG_LEN, "[%s] USE %s -> [%s] -%d HP (%d/%d)",
                 actor.name, WEAPON_TABLE[req.weapon].name, tgt.name, dmg,
                 tgt.hp, tgt.max_hp);
        g_state->log.push(msg);
        if (tgt.alive && dmg >= 45 && (rand() % 100) < 30)
            deliver_stun(req.target_id);
        if (tgt.hp == 0) {
            tgt.alive = false;
            snprintf(msg, LOG_LEN, "[%s] DEFEATED!", tgt.name);
            g_state->log.push(msg);
            if (tgt.type == ENT_ENEMY) {
                ++g_state->total_enemies_killed;
                artifact_release_all(req.target_id);

                // ── Weapon drop trigger (spec §6) ──
                if (actor.type == ENT_PLAYER &&
                    !g_state->weapon_drop_pending) {
                    if ((rand() % 100) < 30) {
                        WeaponID drop_pool[] = {
                            WPN_IRON_HALBERD, WPN_VENOM_DAGGER, WPN_THUNDERSTAFF,
                            WPN_OBSIDIAN_AXE, WPN_FROSTBOW,     WPN_SPLINTER_STICK
                        };
                        WeaponID dropped = drop_pool[rand() % 6];
                        g_state->weapon_drop_pending    = true;
                        g_state->weapon_drop_id         = dropped;
                        g_state->weapon_drop_for        = req.entity_id;
                        g_state->weapon_drop_turns_left = 0;
                        snprintf(msg, LOG_LEN,
                            "[DROP] %s dropped \xe2\x80\x94 [%s] press P to pick up!",
                            WEAPON_TABLE[dropped].name, actor.name);
                        g_state->log.push(msg);
                    }
                }

                if (g_state->total_enemies_killed >= WIN_KILL_COUNT) {
                    g_state->phase = PHASE_WIN;
                    if (WEAPON_TABLE[req.weapon].is_artifact)
                        artifact_release(req.entity_id, req.weapon);
                    actor.stamina = 0; return;
                }
            }
        }
        if (WEAPON_TABLE[req.weapon].is_artifact)
            artifact_release(req.entity_id, req.weapon);
        actor.stamina = 0;
        break;
    }

    case ACT_SWAP_IN: {
        if (req.weapon == WPN_NONE) break;
        bool in_lt = false;
        for (int i = 0; i < actor.inventory.lt_count; ++i)
            if (actor.inventory.lt_storage[i] == (int)req.weapon) { in_lt = true; break; }
        if (!in_lt) {
            snprintf(msg, LOG_LEN, "[%s] %s not in LT storage!",
                     actor.name, WEAPON_TABLE[req.weapon].name);
            g_state->log.push(msg); actor.stamina = 0; break;
        }
        // Track swap-OUT count: allocator_swap_in pulls 1 from LT, then
        // calls allocator_add which may evict other weapons back into LT.
        int lt_before = actor.inventory.lt_count;
        bool ok = allocator_swap_in(actor.inventory, req.weapon);
        int lt_after  = actor.inventory.lt_count;
        // After: lt_after = (lt_before - 1) + evicted
        int evicted = lt_after - (lt_before - 1);
        if (evicted < 0) evicted = 0;
        if (ok) {
            actor.swapped_weapon_unavailable = req.weapon;
            snprintf(msg, LOG_LEN,
                     "[%s] SWAP-IN %s (+%d to LT) — locked this turn",
                     actor.name, WEAPON_TABLE[req.weapon].name, evicted);
        } else {
            snprintf(msg, LOG_LEN, "[%s] SWAP-IN %s FAILED",
                     actor.name, WEAPON_TABLE[req.weapon].name);
        }
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_HEAL: {
        int restored = std::max(1, (int)(actor.max_hp * 0.10f));
        actor.hp = std::min(actor.max_hp, actor.hp + restored);
        snprintf(msg, LOG_LEN, "[%s] HEAL +%d HP (%d/%d)",
                 actor.name, restored, actor.hp, actor.max_hp);
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_SKIP: {
        actor.stamina = actor.max_stamina * 0.50f;
        snprintf(msg, LOG_LEN, "[%s] SKIP (stamina -> 50%%)", actor.name);
        g_state->log.push(msg);
        break;
    }

    case ACT_ULTIMATE: {
        if (!actor.inventory.has(WPN_SOLAR_CORE) || !actor.inventory.has(WPN_LUNAR_BLADE)) {
            snprintf(msg, LOG_LEN, "[%s] ULTIMATE FAILED: need Solar Core + Lunar Blade",
                     actor.name);
            g_state->log.push(msg); actor.stamina = 0; break;
        }
        bool got_sc = artifact_acquire(req.entity_id, WPN_SOLAR_CORE);
        bool got_lb = artifact_acquire(req.entity_id, WPN_LUNAR_BLADE);
        if (!got_sc || !got_lb) {
            if (got_sc) artifact_release(req.entity_id, WPN_SOLAR_CORE);
            if (got_lb) artifact_release(req.entity_id, WPN_LUNAR_BLADE);
            snprintf(msg, LOG_LEN, "[%s] ULTIMATE BLOCKED: artifact contention", actor.name);
            g_state->log.push(msg); actor.stamina = 0; break;
        }
        snprintf(msg, LOG_LEN, "[%s] *** ULTIMATE — CHRONO BURST! ***", actor.name);
        g_state->log.push(msg);
        for (int i = 0; i < g_state->num_enemies; ++i) {
            Entity& e = g_state->entities[MAX_PLAYERS + i];
            if (!e.alive) continue;
            int aoe = e.max_hp / 2;
            e.hp = std::max(0, e.hp - aoe);
            snprintf(msg, LOG_LEN, "  * [%s] takes %d CHRONO dmg!", e.name, aoe);
            g_state->log.push(msg);
            if (e.hp == 0) {
                e.alive = false;
                ++g_state->total_enemies_killed;
                snprintf(msg, LOG_LEN, "  * [%s] VAPORIZED!", e.name);
                g_state->log.push(msg);
                artifact_release_all(MAX_PLAYERS + i);
                if (g_state->total_enemies_killed >= WIN_KILL_COUNT) {
                    g_state->phase = PHASE_WIN;
                    artifact_release(req.entity_id, WPN_SOLAR_CORE);
                    artifact_release(req.entity_id, WPN_LUNAR_BLADE);
                    actor.stamina = 0; return;
                }
            }
        }
        if (g_asp_pid > 0) kill(g_asp_pid, SIGSTOP);
        g_state->phase = PHASE_ULTIMATE_PAUSE;
        g_state->ultimate_active = true;
        g_ultimate_running = true;
        alarm((unsigned int)ULTIMATE_PAUSE);
        artifact_release(req.entity_id, WPN_SOLAR_CORE);
        artifact_release(req.entity_id, WPN_LUNAR_BLADE);
        actor.stamina = 0;
        break;
    }

    case ACT_PICKUP: {
        if (req.weapon == WPN_NONE) break;
        WeaponID wpn = req.weapon;
        if (WEAPON_TABLE[wpn].is_artifact) {
            if (!artifact_acquire(req.entity_id, wpn)) {
                // Clear drop state even on artifact contention failure
                g_state->weapon_drop_pending = false;
                g_state->weapon_drop_id      = WPN_NONE;
                g_state->weapon_drop_for     = -1;
                actor.stamina = 0; break;
            }
            bool added = allocator_add(actor.inventory, wpn);
            if (!added) {
                artifact_release(req.entity_id, wpn);
                snprintf(msg, LOG_LEN, "[%s] PICKUP %s FAILED: no space!",
                         actor.name, WEAPON_TABLE[wpn].name);
            } else {
                snprintf(msg, LOG_LEN, "[%s] picked up %s!",
                         actor.name, WEAPON_TABLE[wpn].name);
            }
        } else {
            bool added = allocator_add(actor.inventory, wpn);
            if (added) {
                snprintf(msg, LOG_LEN, "[%s] picked up %s!",
                         actor.name, WEAPON_TABLE[wpn].name);
            } else {
                snprintf(msg, LOG_LEN, "[%s] no space for %s \xe2\x80\x94 enemy gets it.",
                         actor.name, WEAPON_TABLE[wpn].name);
                g_state->log.push(msg);
                resolve_weapon_drop_fallback_now();
                actor.stamina = 0;
                break;
            }
        }
        g_state->log.push(msg);
        // Clear the drop offer now that it is resolved
        g_state->weapon_drop_pending  = false;
        g_state->weapon_drop_id       = WPN_NONE;
        g_state->weapon_drop_for      = -1;
        actor.stamina = 0;
        break;
    }

    case ACT_AOE: {
        // Area-of-effect attack: hits all alive enemies (or players, for NPC actor)
        // within Manhattan distance AOE_RANGE from the actor.
        constexpr int AOE_RANGE = 20;
        int dmg = std::max(1, actor.damage / 2);   // half damage to each
        int hits = 0;
        if (actor.type == ENT_PLAYER) {
            for (int i = 0; i < g_state->num_enemies; ++i) {
                Entity& tgt = g_state->entities[MAX_PLAYERS + i];
                if (!tgt.alive) continue;
                int d = std::abs(tgt.x - actor.x) + std::abs(tgt.y - actor.y);
                if (d > AOE_RANGE) continue;
                tgt.hp = std::max(0, tgt.hp - dmg);
                tgt.last_hit_time = wallclock_sec();
                ++hits;
                snprintf(msg, LOG_LEN, "  AoE -> [%s] -%d HP (%d/%d)",
                         tgt.name, dmg, tgt.hp, tgt.max_hp);
                g_state->log.push(msg);
                if (tgt.hp == 0) {
                    tgt.alive = false;
                    snprintf(msg, LOG_LEN, "[%s] VAPORIZED by AoE!", tgt.name);
                    g_state->log.push(msg);
                    ++g_state->total_enemies_killed;
                    artifact_release_all(MAX_PLAYERS + i);
                    if (g_state->total_enemies_killed >= WIN_KILL_COUNT) {
                        g_state->phase = PHASE_WIN;
                        actor.stamina = 0;
                        return;
                    }
                }
            }
        }
        snprintf(msg, LOG_LEN, "[%s] AOE BLAST (r=%d) — %d hit%s",
                 actor.name, AOE_RANGE, hits, hits == 1 ? "" : "s");
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_QUIT:
        g_state->phase = PHASE_QUIT;
        break;

    default: break;
    }
}

// ─────────────────────────────────────────────
//  Deadlock Monitor Thread
// ─────────────────────────────────────────────
static void* deadlock_monitor(void*) {
    while (true) {
        usleep(1000000);
        if (!g_state || g_state->phase != PHASE_RUNNING) continue;

        pthread_mutex_lock(&g_state->resource_table.table_mutex);
        WaitForGraph&  wfg = g_state->wait_graph;
        ResourceTable& rt  = g_state->resource_table;

        int np = g_state->num_players, ne = g_state->num_enemies;
        int indices[MAX_ENTITIES]; int count = 0;
        for (int i = 0; i < np; ++i) indices[count++] = i;
        for (int i = 0; i < ne; ++i) indices[count++] = MAX_PLAYERS + i;

        bool visited[MAX_ENTITIES] = {};
        bool in_stack[MAX_ENTITIES] = {};
        int  victim = -1;

        std::function<bool(int)> dfs = [&](int u) -> bool {
            visited[u] = in_stack[u] = true;
            int w = wfg.waiting_for[u];
            if (w >= 0 && w < NUM_ARTIFACTS) {
                int holder = rt.entries[w].held_by;
                if (holder >= 0 && g_state->entities[holder].alive) {
                    if (!visited[holder]) {
                        if (dfs(holder)) { victim = u; return true; }
                    } else if (in_stack[holder]) {
                        victim = u; return true;
                    }
                }
            }
            in_stack[u] = false;
            return false;
        };

        for (int k = 0; k < count && victim < 0; ++k) {
            int idx = indices[k];
            if (!visited[idx] && g_state->entities[idx].alive) dfs(idx);
        }

        if (victim >= 0) {
            for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                if (rt.entries[a].held_by == victim) {
                    rt.entries[a].held_by = -1; rt.entries[a].locked = false;
                    wfg.holding[victim][a] = 0;
                    char msg[LOG_LEN];
                    snprintf(msg, LOG_LEN, "[Arbiter] DEADLOCK -> forced [%s] release %s",
                             g_state->entities[victim].name,
                             WEAPON_TABLE[rt.entries[a].id].name);
                    g_state->log.push(msg);
                }
            }
            wfg.waiting_for[victim] = -1;
        }
        pthread_mutex_unlock(&g_state->resource_table.table_mutex);
    }
    return nullptr;
}

// ─────────────────────────────────────────────
//  Stun tick thread
// ─────────────────────────────────────────────
static void* stun_tick(void*) {
    while (true) {
        usleep(10000);
        if (!g_state) continue;
        pthread_mutex_lock(&g_state->global_mutex);
        process_pending_signals();
        if (g_state->phase != PHASE_RUNNING &&
            g_state->phase != PHASE_ULTIMATE_PAUSE) {
            pthread_mutex_unlock(&g_state->global_mutex); continue;
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        double now = ts.tv_sec + ts.tv_nsec / 1e9;
        int np = g_state->num_players, ne = g_state->num_enemies;
        for (int i = 0; i < np + ne; ++i) {
            int idx = (i < np) ? i : MAX_PLAYERS + (i - np);
            Entity& e = g_state->entities[idx];
            if (e.stunned && now >= e.stun_end_time) {
                e.stunned = false;
                char msg[LOG_LEN];
                if (e.skip_turn_from_stun) {
                    e.skip_turn_from_stun = false;
                    e.stamina = e.max_stamina * 0.50f;
                    snprintf(msg, LOG_LEN, "[%s] recovered from stun (turn skipped).", e.name);
                } else {
                    snprintf(msg, LOG_LEN, "[%s] recovered from stun.", e.name);
                }
                g_state->log.push(msg);
                pthread_cond_broadcast(&g_state->turn_cond);
            }
        }
        pthread_mutex_unlock(&g_state->global_mutex);
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
//  Rendering Thread
// ═══════════════════════════════════════════════════════════════════════

#define CP_TITLE    1
#define CP_PLAYER   2
#define CP_ENEMY    3
#define CP_ACTIVE   4
#define CP_HP_FULL  5
#define CP_HP_LOW   6
#define CP_SP_BAR   7
#define CP_STUN     8
#define CP_LOG      9
#define CP_LOG_HIT 10
#define CP_GOLD    11
#define CP_BORDER  12
#define CP_DEAD    13
#define CP_ARTIFACT 14
#define CP_WIN     15
#define CP_LOSE    16
#define CP_ULT     17
#define CP_BG_A    18
#define CP_BG_B    19
#define CP_ENEMY2  20
#define CP_ENEMY3  21
#define CP_HEADER  22
#define CP_SPEED   23
#define CP_SELECT  24   // selected-target highlight (distinct from enemy colours)
#define CP_HIT     25   // damage-flash highlight (distinct from enemy red)
#define CP_STAR    26   // twinkling sky stars

static void draw_bar_w(int y, int x, float val, float maxv, int len,
                        int full_pair, int empty_pair) {
    int filled = (maxv > 0) ? (int)(val / maxv * len) : 0;
    filled = std::max(0, std::min(len, filled));
    attron(COLOR_PAIR(full_pair) | A_BOLD);
    for (int i = 0; i < filled; ++i) mvaddch(y, x + i, ACS_BLOCK);
    attroff(COLOR_PAIR(full_pair) | A_BOLD);
    attron(COLOR_PAIR(empty_pair));
    for (int i = filled; i < len; ++i) mvaddch(y, x + i, ACS_CKBOARD);
    attroff(COLOR_PAIR(empty_pair));
}

static const char* ENEMY_GLYPHS = "MOGSDCZTO";
static int enemy_color(int /*i*/, bool stunned) {
    // All enemies render in the same colour so the player can tell
    // selected / hit / normal apart from the enemy hue at a glance.
    if (stunned) return CP_STUN;
    return CP_ENEMY;
}

// ── Snapshot struct so render holds the lock for as short as possible ──
struct RenderSnapshot {
    int np, ne, killed, active;
    float vtime;
    bool ult, eclipse;
    double npc_turn_deadline_sec;
    GamePhase phase;
    Entity entities[MAX_ENTITIES];
    WeaponID artifact_id[NUM_ARTIFACTS];
    bool artifact_exists[NUM_ARTIFACTS];
    int  artifact_held_by[NUM_ARTIFACTS];
    char log_lines[LOG_LINES][LOG_LEN];
    int  log_head;
    // Weapon drop state (rendered from snapshot to avoid extra locking)
    bool     drop_pending;
    WeaponID drop_id;
    int      drop_for;
};

static void take_snapshot(RenderSnapshot& snap) {
    // Called under global_mutex
    snap.np      = g_state->num_players;
    snap.ne      = g_state->num_enemies;
    snap.killed  = g_state->total_enemies_killed;
    snap.active  = g_state->active_entity;
    snap.vtime   = g_state->virtual_time;
    snap.ult     = g_state->ultimate_active;
    snap.eclipse = g_state->eclipse_relic_spawned;
    snap.npc_turn_deadline_sec = g_state->npc_turn_deadline_sec;
    snap.phase   = g_state->phase;
    snap.drop_pending = g_state->weapon_drop_pending;
    snap.drop_id      = g_state->weapon_drop_id;
    snap.drop_for     = g_state->weapon_drop_for;
    memcpy(snap.entities,  g_state->entities,  sizeof(snap.entities));
    pthread_mutex_lock(&g_state->resource_table.table_mutex);
    for (int a = 0; a < NUM_ARTIFACTS; ++a) {
        snap.artifact_id[a] = g_state->resource_table.entries[a].id;
        snap.artifact_exists[a] = g_state->resource_table.entries[a].exists;
        snap.artifact_held_by[a] = g_state->resource_table.entries[a].held_by;
    }
    pthread_mutex_unlock(&g_state->resource_table.table_mutex);
    memcpy(snap.log_lines, g_state->log.lines, sizeof(snap.log_lines));
    snap.log_head = g_state->log.head;
}

static void dump_action_log_to_file(const RenderSnapshot& snap) {
    bool log_full = snap.log_lines[snap.log_head][0] != '\0';
    int log_count = log_full ? LOG_LINES : snap.log_head;
    if (log_count <= 0) return;

    int oldest = log_full ? snap.log_head : 0;
    FILE* fp = fopen("action_log_dump.txt", "w");
    if (!fp) return;

    time_t now = time(nullptr);
    fprintf(fp, "CHRONO RIFT ACTION LOG DUMP\n");
    fprintf(fp, "Generated: %s", ctime(&now));
    fprintf(fp, "Entries: %d\n", log_count);
    fprintf(fp, "----------------------------------------\n");
    for (int i = 0; i < log_count; ++i) {
        int idx = (oldest + i) % LOG_LINES;
        if (!snap.log_lines[idx][0]) continue;
        fprintf(fp, "%s\n", snap.log_lines[idx]);
    }
    fclose(fp);
}

static void* render_thread(void*) {
    // ncurses was already initialised (and endwin'd) by lobby_screen.
    // Re-initialise cleanly for the game screen.
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();

    // All colour pairs
    init_pair(CP_TITLE,    COLOR_MAGENTA, -1);
    init_pair(CP_PLAYER,   COLOR_GREEN,   -1);
    init_pair(CP_ENEMY,    COLOR_RED,     -1);
    init_pair(CP_ACTIVE,   COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_HP_FULL,  COLOR_GREEN,   -1);
    init_pair(CP_HP_LOW,   COLOR_RED,     -1);
    init_pair(CP_SP_BAR,   COLOR_CYAN,    -1);
    init_pair(CP_STUN,     COLOR_YELLOW,  -1);
    init_pair(CP_LOG,      COLOR_WHITE,   -1);
    init_pair(CP_LOG_HIT,  COLOR_RED,     -1);
    init_pair(CP_GOLD,     COLOR_YELLOW,  -1);
    init_pair(CP_BORDER,   COLOR_CYAN,    -1);
    init_pair(CP_DEAD,     COLOR_RED,     -1);
    init_pair(CP_ARTIFACT, COLOR_MAGENTA, -1);
    init_pair(CP_WIN,      COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_LOSE,     COLOR_RED,     COLOR_BLACK);
    init_pair(CP_ULT,      COLOR_MAGENTA, COLOR_BLACK);
    init_pair(CP_BG_A,     COLOR_GREEN,   -1);
    init_pair(CP_BG_B,     COLOR_CYAN,    -1);
    init_pair(CP_ENEMY2,   COLOR_YELLOW,  -1);
    init_pair(CP_ENEMY3,   COLOR_CYAN,    -1);
    init_pair(CP_HEADER,   COLOR_BLUE,    -1);
    init_pair(CP_SPEED,    COLOR_WHITE,   -1);
    // Selection highlight: bright magenta on dark — clearly different from
    // CP_ENEMY (red), CP_ENEMY2 (yellow) and CP_ENEMY3 (cyan).
    init_pair(CP_SELECT,   COLOR_MAGENTA, -1);
    // Hit-flash: white text on red background — pops visually and is not
    // mistakable for the red foreground used by CP_ENEMY.
    init_pair(CP_HIT,      COLOR_WHITE,   COLOR_RED);
    // Twinkling stars
    init_pair(CP_STAR,     COLOR_WHITE,   -1);

    // Non-blocking input with 80ms timeout so getch never stalls
    nodelay(stdscr, FALSE);
    timeout(80);

    RenderSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    // Persistent UI state: which enemy index (0..ne-1) is the current target.
    int sel_target_idx = 0;
    // Activity-log scroll offset (0 = follow newest line).
    int log_scroll_offset = 0;
    // Number of currently valid log lines in the ring buffer.
    int log_available_lines = 0;
    bool export_log_requested = false;
    // Wall-clock animation tick (independent of game virtual_time so the
    // sky keeps shimmering even while no actions occur).
    int anim_tick = 0;
    constexpr int LOG_H = 9;
    // Last valid player index for inventory display — persists between turns
    // so the inventory panel never flickers/disappears when active_entity == -1.
    int display_active = 0;

    while (true) {
        ++anim_tick;
        if (!g_state) { napms(50); continue; }

        // ── Input (no lock held) ─────────────────────────────────────
        int ch = getch();   // returns ERR after 80ms if no key

        if (ch != ERR) {
            // Log-only navigation keys (independent of game flow).
            int max_scroll = std::max(0, log_available_lines - (LOG_H - 1));
            // Support multiple key bindings because some terminals do not
            // deliver PgUp/PgDn reliably through ncurses.
            if (ch == KEY_PPAGE || ch == KEY_SR || ch == '[' || ch == ',' || ch == 'z' || ch == 'Z') {
                log_scroll_offset = std::min(max_scroll, log_scroll_offset + 3);
            } else if (ch == KEY_NPAGE || ch == KEY_SF || ch == ']' || ch == '.' || ch == 'x' || ch == 'X') {
                log_scroll_offset = std::max(0, log_scroll_offset - 3);
            } else if (ch == KEY_HOME) {
                log_scroll_offset = 0;
            } else if (ch == 'c' || ch == 'C') {
                export_log_requested = true;
            }

            pthread_mutex_lock(&g_state->global_mutex);
            int active = g_state->active_entity;
            int np     = g_state->num_players;
            GamePhase ph = g_state->phase;

            if (ph == PHASE_RUNNING && active >= 0 && active < np) {
                // Only queue if no action is already pending
                if (!g_state->player_actions[active].ready) {
                    ActionRequest req;
                    
                    memset(&req, 0, sizeof(req));
                    req.entity_id = active;
                    req.target_id = -1;
                    req.weapon    = WPN_NONE;
                    req.action    = ACT_NONE;   
                    int dx = 0, dy = 0;
                    bool got = false;

                    // Helper: pick currently-selected alive enemy entity_id.
                    auto current_target = [&]() -> int {
                        int ne_loc = g_state->num_enemies;
                        if (ne_loc <= 0) return -1;
                        for (int tries = 0; tries < ne_loc; ++tries) {
                            int idx = ((sel_target_idx + tries) % ne_loc + ne_loc) % ne_loc;
                            if (g_state->entities[MAX_PLAYERS + idx].alive) {
                                sel_target_idx = idx;
                                return MAX_PLAYERS + idx;
                            }
                        }
                        return -1;
                    };
                    // Helper: get nth-distinct weapon in inventory (1-indexed).
                    auto nth_weapon = [&](int n) -> WeaponID {
                        Entity& me = g_state->entities[active];
                        int seen[WPN_COUNT]; for (int k=0;k<WPN_COUNT;++k) seen[k]=0;
                        int wcount = 0;
                        for (int s = 0; s < INVENTORY_SLOTS; ++s) {
                            int w = me.inventory.slots[s];
                            if (w == WPN_NONE || seen[w]) continue;
                            seen[w] = 1; ++wcount;
                            if (wcount == n) return (WeaponID)w;
                        }
                        return WPN_NONE;
                    };

                    // Convert non-arrow input to lower-case where it makes sense.
                    int kc = ch;
                    if (kc >= 'A' && kc <= 'Z') kc = kc - 'A' + 'a';

                    // Left/Right arrows cycle target (intuitive for the player).
                    auto cycle_target_dir = [&](int step) {
                        int ne_loc = g_state->num_enemies;
                        if (ne_loc <= 0) return;
                        for (int tries = 1; tries <= ne_loc; ++tries) {
                            int idx = ((sel_target_idx + step * tries) % ne_loc + ne_loc) % ne_loc;
                            if (g_state->entities[MAX_PLAYERS + idx].alive) {
                                sel_target_idx = idx; break;
                            }
                        }
                    };

                    switch (kc) {
                    // ── Target selection via arrows ──
                    case KEY_LEFT:  cycle_target_dir(-1); break;
                    case KEY_RIGHT: cycle_target_dir( 1); break;
                    // ── Free movement (does NOT consume a turn) ──
                    // We mutate the active player's position immediately
                    // and do NOT submit an ActionRequest. Movement only
                    // matters for AoE-attack positioning.
                    case 'w': case KEY_UP:    dy = -1; break;
                    case 's': case KEY_DOWN:  dy =  1; break;
                    case 'a':                 dx = -1; break;
                    case 'd':                 dx =  1; break;
                    case 'h': req.action = ACT_HEAL;     got = true; break;
                    case ' ':
                    case 'k': req.action = ACT_SKIP;     got = true; break;
                    case 'q': req.action = ACT_QUIT;     got = true; break;
                    case 'u': req.action = ACT_ULTIMATE; got = true; break;
                    case 't': {
                        // Cycle through enemies (no action submitted)
                        int ne_loc = g_state->num_enemies;
                        if (ne_loc > 0) {
                            for (int tries = 1; tries <= ne_loc; ++tries) {
                                int idx = (sel_target_idx + tries) % ne_loc;
                                if (g_state->entities[MAX_PLAYERS + idx].alive) {
                                    sel_target_idx = idx; break;
                                }
                            }
                        }
                        break;
                    }
                    case '1': {
                        int t = current_target();
                        if (t >= 0) {
                            req.action    = ACT_STRIKE;
                            req.target_id = t;
                            got = true;
                        }
                        break;
                    }
                    case '2': {
                        int t = current_target();
                        if (t >= 0) {
                            req.action    = ACT_EXHAUST;
                            req.target_id = t;
                            got = true;
                        }
                        break;
                    }
                    case '3': {
                        // AoE blast — hits all enemies within range, no target needed
                        req.action = ACT_AOE;
                        got = true;
                        break;
                    }
                    case '4': case '5': case '6':
                    case '7': case '8': case '9': {
                        int n = kc - '4' + 1;       // 4->1, 5->2, ... 9->6
                        WeaponID w = nth_weapon(n);
                        int t = current_target();
                        if (w != WPN_NONE && t >= 0) {
                            req.action    = ACT_USE_WEAPON;
                            req.weapon    = w;
                            req.target_id = t;
                            got = true;
                        } else if (w == WPN_NONE) {
                            // Log feedback so the player knows why nothing happened
                            char lmsg[LOG_LEN];
                            snprintf(lmsg, LOG_LEN,
                                "[%s] No weapon in slot %d",
                                g_state->entities[active].name, kc - '0');
                            g_state->log.push(lmsg);
                        }
                        break;
                    }
                    case 'v': {
                        // Swap-in: pull first weapon from LT storage.
                        Entity& me = g_state->entities[active];
                        if (me.inventory.lt_count > 0) {
                            req.action = ACT_SWAP_IN;
                            req.weapon = (WeaponID)me.inventory.lt_storage[0];
                            got = true;
                        }
                        break;
                    }
                    case 'p': {
                        if (g_state->weapon_drop_pending &&
                            g_state->weapon_drop_for == active) {
                            req.action = ACT_PICKUP;
                            req.weapon = g_state->weapon_drop_id;
                            // DO NOT clear weapon_drop_pending here —
                            // apply_action will clear it after pickup resolves.
                            got = true;
                        }
                        break;
                    }
                    default: break;
                    }

                    // Free movement: apply immediately without ending turn.
                    // Reject the step if it would land on another live entity.
                    if (dx != 0 || dy != 0) {
                        Entity& me = g_state->entities[active];
                        int nx = std::max(1, std::min(BF_W, me.x + dx));
                        int ny = std::max(1, std::min(BF_H, me.y + dy));

                        bool blocked = false;
                        // Allow same coords as me (no-op clamp at edges).
                        if (nx != me.x || ny != me.y) {
                            int total_e = g_state->num_players
                                        + g_state->num_enemies;
                            for (int j = 0; j < total_e; ++j) {
                                int idx = (j < g_state->num_players)
                                        ? j
                                        : (MAX_PLAYERS + j - g_state->num_players);
                                if (idx == active) continue;
                                Entity& o = g_state->entities[idx];
                                if (!o.alive) continue;
                                if (o.x == nx && o.y == ny) { blocked = true; break; }
                            }
                        }
                        if (!blocked) {
                            me.x = nx;
                            me.y = ny;
                        }
                    }

                    if (got && req.action != ACT_NONE) {
                        g_state->player_actions[active] = req;
                        g_state->player_actions[active].ready = true;
                        pthread_cond_broadcast(&g_state->turn_cond);
                    }
                }
            }
            pthread_mutex_unlock(&g_state->global_mutex);
        }

        // ── Snapshot (very short lock) ───────────────────────────────
        pthread_mutex_lock(&g_state->global_mutex);
        take_snapshot(snap);
        pthread_mutex_unlock(&g_state->global_mutex);

        // Derive ring-buffer occupancy from snapshot.
        bool log_full = snap.log_lines[snap.log_head][0] != '\0';
        log_available_lines = log_full ? LOG_LINES : snap.log_head;
        int max_scroll = std::max(0, log_available_lines - (LOG_H - 1));
        if (log_scroll_offset > max_scroll) log_scroll_offset = max_scroll;

        if (export_log_requested) {
            dump_action_log_to_file(snap);
            export_log_requested = false;
        }

        // ── Render from snapshot (no lock held) ─────────────────────
        int rows, cols;
        getmaxyx(stdscr, rows, cols);

        int np     = snap.np;
        int ne     = snap.ne;
        int killed = snap.killed;
        bool ult   = snap.ult;
        int active = snap.active;
        float vtime = snap.vtime;
        GamePhase phase = snap.phase;

        // Update display_active only when a valid player is active;
        // keeps inventory panel visible between turns (active == -1).
        if (active >= 0 && active < np)
            display_active = active;

        constexpr int LEFT_W  = 24;
        constexpr int RIGHT_W = 24;

        erase();   // erase() is less flickery than clear()

        // Title row
        attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
        const char* title = "C H R O N O   R I F T";
        mvprintw(0, (cols - (int)strlen(title) - 6) / 2, "<<< %s >>>", title);
        attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

        attron(COLOR_PAIR(CP_GOLD) | A_BOLD);
        mvprintw(0, cols - 26, "Kills:%d/%d  t=%.1f", killed, WIN_KILL_COUNT, vtime);
        attroff(COLOR_PAIR(CP_GOLD) | A_BOLD);

        if (active >= MAX_PLAYERS && active < MAX_PLAYERS + ne && phase == PHASE_RUNNING) {
            double now_ui = wallclock_sec();
            double rem = snap.npc_turn_deadline_sec - now_ui;
            if (rem < 0.0) rem = 0.0;
            // NPC name on row 1, bar on row 2 — clamped to battlefield area
            int bar_x = LEFT_W + 2;
            int bar_end = cols - RIGHT_W - 2;
            int bar_w = std::max(10, bar_end - bar_x);
            if (bar_w > 0) {
                attron(COLOR_PAIR(CP_ENEMY) | A_BOLD | A_BLINK);
                mvprintw(1, bar_x, ">> [%s] thinking...          ", snap.entities[active].name);
                attroff(COLOR_PAIR(CP_ENEMY) | A_BOLD | A_BLINK);
                float ratio = (float)(rem / NPC_TURN_TIMEOUT);
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                int filled = (int)(ratio * bar_w);
                attron(COLOR_PAIR(CP_STUN) | A_BOLD);
                for (int i = 0; i < filled && bar_x + i < bar_end; ++i)
                    mvaddch(2, bar_x + i, ACS_BLOCK);
                attroff(COLOR_PAIR(CP_STUN) | A_BOLD);
                attron(COLOR_PAIR(CP_BORDER));
                for (int i = filled; i < bar_w && bar_x + i < bar_end; ++i)
                    mvaddch(2, bar_x + i, ACS_CKBOARD);
                attroff(COLOR_PAIR(CP_BORDER));
                attron(COLOR_PAIR(CP_GOLD));
                mvprintw(2, bar_x, "NPC timeout: %.1fs ", rem);
                attroff(COLOR_PAIR(CP_GOLD));
            }
        }

        if (ult) {
            attron(COLOR_PAIR(CP_ULT) | A_BOLD | A_BLINK);
            mvprintw(0, LEFT_W + 2, "*** ULTIMATE ACTIVE ***");
            attroff(COLOR_PAIR(CP_ULT) | A_BOLD | A_BLINK);
        }
        // Weapon-drop offer indicator — prominent banner on row 1
        if (snap.drop_pending && snap.drop_for == active &&
            active >= 0 && snap.drop_id != WPN_NONE) {
            int banner_x = LEFT_W + 2;
            attron(COLOR_PAIR(CP_GOLD) | A_BOLD | A_BLINK);
            mvprintw(1, banner_x, ">>> DROP: %s (dmg:%d slots:%d) — press P to pick up! <<<",
                     WEAPON_TABLE[snap.drop_id].name,
                     WEAPON_TABLE[snap.drop_id].damage,
                     WEAPON_TABLE[snap.drop_id].slot_size);
            attroff(COLOR_PAIR(CP_GOLD) | A_BOLD | A_BLINK);
        }

        // ─── Open-scene battlefield (no bounding box) ───────────────
        // The arena is drawn as a sky -> mountains -> ground scene. Heroes
        // and enemies stand on a wavy ground line. No hard border.
        int bf_x0 = LEFT_W + 1;
        int bf_x1 = cols - RIGHT_W - 1;
        int bf_y0 = 2;
        int bf_y1 = rows - LOG_H - 2;
        if (bf_x1 <= bf_x0 + 4) bf_x1 = bf_x0 + 4;
        if (bf_y1 <= bf_y0 + 4) bf_y1 = bf_y0 + 4;

        int bf_w_px = bf_x1 - bf_x0;
        int bf_h_px = bf_y1 - bf_y0;
        int horizon_y = bf_y0 + bf_h_px * 55 / 100;   // ground starts here

        // Title plate floating at top of the scene
        {
            const char* dtitle = " ~~~ CHRONO RIFT ARENA ~~~ ";
            int tx = (bf_x0 + bf_x1)/2 - (int)strlen(dtitle)/2;
            attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
            mvprintw(bf_y0, tx, "%s", dtitle);
            attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);
        }

        // ── Sky band: twinkling stars ──
        // Each star has a deterministic phase derived from its (r, c) hash;
        // we cycle a per-frame "tick" so different stars blink at different
        // beats, giving an alive, sparkling night sky.
        // Wall-clock driven (anim_tick is incremented every render frame
        // ≈ 12 fps), so stars shimmer continuously even when the game
        // sits idle waiting for input.
        int blink_tick = anim_tick / 2;
        for (int r = bf_y0 + 1; r < horizon_y - 2; ++r) {
            for (int c = bf_x0; c < bf_x1; ++c) {
                unsigned h = (unsigned)(c * 73856093u) ^ (unsigned)(r * 19349663u);
                int hr = h % 200;
                if (hr >= 8) continue;
                int phase = (h >> 3) & 7;           // 0..7 phase per star
                int beat  = (blink_tick + phase) & 7;
                // Three-frame "shimmer" cycle: dim -> bright -> off -> dim
                char ch_star;
                int  cp_star;
                int  attr = A_BOLD;
                if (beat == 0)        { ch_star = '*'; cp_star = CP_GOLD;  attr |= A_BLINK; }
                else if (beat == 1)   { ch_star = '+'; cp_star = CP_STAR;  }
                else if (beat == 2)   { ch_star = '.'; cp_star = CP_BG_B;  }
                else if (beat == 3)   { ch_star = '\''; cp_star = CP_STAR; }
                else if (beat == 4)   { ch_star = '*'; cp_star = CP_STAR;  }
                else if (beat == 5)   { ch_star = '.'; cp_star = CP_GOLD;  }
                else                  { continue; }   // dark beats — star off
                if (hr < 3) {                          // brightest tier
                    attron(COLOR_PAIR(cp_star) | attr);
                    mvaddch(r, c, ch_star);
                    attroff(COLOR_PAIR(cp_star) | attr);
                } else if (hr < 6) {                   // mid tier (no blink)
                    attron(COLOR_PAIR(cp_star) | A_BOLD);
                    mvaddch(r, c, (beat & 1) ? '.' : '\'');
                    attroff(COLOR_PAIR(cp_star) | A_BOLD);
                } else {                               // dim tier (steady)
                    attron(COLOR_PAIR(CP_BORDER));
                    mvaddch(r, c, '.');
                    attroff(COLOR_PAIR(CP_BORDER));
                }
            }
        }

        // ── Distant mountain silhouettes along the horizon ──
        {
            attron(COLOR_PAIR(CP_BORDER));
            for (int c = bf_x0; c < bf_x1; ++c) {
                // Use a few overlapping sine-like ridges for a hilly horizon.
                double t = (double)(c - bf_x0) / std::max(1, bf_w_px);
                double ridge = 0.55 + 0.30 * std::sin(t * 6.28318 * 1.7)
                                    + 0.18 * std::sin(t * 6.28318 * 4.1 + 1.3);
                int peak_y = horizon_y - 1
                           - (int)((horizon_y - bf_y0 - 2) * (ridge - 0.4));
                if (peak_y < bf_y0 + 1) peak_y = bf_y0 + 1;
                if (peak_y >= horizon_y) continue;
                mvaddch(peak_y, c, ((c & 1) ? '^' : '/'));
                for (int r = peak_y + 1; r < horizon_y; ++r)
                    mvaddch(r, c, ((r ^ c) & 1) ? '.' : ' ');
            }
            attroff(COLOR_PAIR(CP_BORDER));
        }

        // ── Wavy ground line ──
        int ground_row[1024];
        int gw = std::min(1024, bf_w_px + 1);
        for (int i = 0; i < gw; ++i) {
            double t = (double)i / std::max(1, bf_w_px);
            double wave = std::sin(t * 6.28318 * 1.4) * 0.5
                        + std::sin(t * 6.28318 * 3.1 + 0.7) * 0.25;
            int gy = horizon_y + 1 + (int)(wave * 1.5);
            if (gy < bf_y0 + 1) gy = bf_y0 + 1;
            if (gy > bf_y1 - 1) gy = bf_y1 - 1;
            ground_row[i] = gy;
        }
        attron(COLOR_PAIR(CP_PLAYER) | A_BOLD);
        for (int c = bf_x0; c < bf_x1; ++c) {
            int gy = ground_row[c - bf_x0];
            mvaddch(gy, c, ((c & 3) == 0) ? '_' : '~');
        }
        attroff(COLOR_PAIR(CP_PLAYER) | A_BOLD);

        // ── Foreground texture below ground line: dirt & grass tufts ──
        for (int c = bf_x0; c < bf_x1; ++c) {
            int gy = ground_row[c - bf_x0];
            for (int r = gy + 1; r < bf_y1; ++r) {
                unsigned h = (unsigned)(c * 374761393u) ^ (unsigned)(r * 668265263u);
                int hr = h % 100;
                if (hr < 18)      { attron(COLOR_PAIR(CP_PLAYER)); mvaddch(r, c, ','); attroff(COLOR_PAIR(CP_PLAYER)); }
                else if (hr < 35) { attron(COLOR_PAIR(CP_BG_A));   mvaddch(r, c, '.'); attroff(COLOR_PAIR(CP_BG_A)); }
                else if (hr < 42) { attron(COLOR_PAIR(CP_BG_B));   mvaddch(r, c, '\''); attroff(COLOR_PAIR(CP_BG_B)); }
            }
        }

        // ── Occasional foreground tree silhouettes ──
        {
            attron(COLOR_PAIR(CP_BORDER));
            for (int c = bf_x0 + 6; c < bf_x1 - 6; c += 14) {
                int gy = ground_row[c - bf_x0];
                if (gy - 2 > bf_y0 + 1) {
                    mvaddch(gy - 2, c, '^');
                    mvaddch(gy - 1, c, '|');
                }
            }
            attroff(COLOR_PAIR(CP_BORDER));
        }

        // ── Sprite rendering ────────────────────────────────────────
        // bf coords (1..BF_W, 1..BF_H) -> screen cells.  Sprites are snapped
        // to the wavy ground; BF-y adds a small depth/perspective offset
        // (high BF y = front of arena, low BF y = back).
        // Safe vertical band for sprites — keeps them on/around the
        // ground line and never near the very top or bottom of the scene.
        int safe_top    = bf_y0 + 7;             // leaves room for tag + HP bar
        int safe_bottom = bf_y1 - 2;
        auto bf_pos = [&](int bx, int by, int& sc, int& sr) {
            float nx = (float)(bx - 1) / (BF_W - 1);
            sc = bf_x0 + 4 + (int)(nx * (bf_x1 - bf_x0 - 8));
            int gx = sc - bf_x0;
            if (gx < 0) gx = 0;
            if (gx >= gw) gx = gw - 1;
            int gy = ground_row[gx];
            // Depth: bigger by -> closer to camera -> drawn lower (closer to ground).
            // Use a small fraction so vertical motion is gentle (avoids the
            // sprite jumping wildly across the screen on each y change).
            int depth_max = std::max(1, bf_h_px / 8);
            int depth = (BF_H - by) * depth_max / std::max(1, BF_H);
            sr = gy - 1 - depth;
            if (sr < safe_top)    sr = safe_top;
            if (sr > safe_bottom) sr = safe_bottom;
        };

        // Multi-row sprite drawer.  Each line is centred on (sc).
        // Lines may be NULL to skip.
        auto draw_lines = [&](int sc, int top_row, int cp,
                              const char* const* lines, int n) {
            attron(COLOR_PAIR(cp) | A_BOLD);
            for (int k = 0; k < n; ++k) {
                if (!lines[k]) continue;
                int rr2 = top_row + k;
                if (rr2 <= bf_y0 || rr2 >= bf_y1) continue;
                int len = (int)strlen(lines[k]);
                int start = sc - len / 2;
                if (start < bf_x0 + 1) start = bf_x0 + 1;
                // Truncate at right edge instead of skipping the line
                // (prevents half-rendered sprites near edges).
                int draw_len = std::min(len, bf_x1 - 1 - start);
                if (draw_len <= 0) continue;
                mvprintw(rr2, start, "%.*s", draw_len, lines[k]);
            }
            attroff(COLOR_PAIR(cp) | A_BOLD);
        };

        // Hit-flash window — entity damaged within this many WALL-CLOCK
        // seconds glows.  Driven by real time, not game virtual_time, so
        // the flash is always visible regardless of game pacing.
        constexpr double HIT_FLASH_SEC = 0.9;
        double now_w = wallclock_sec();

        int total = np + ne;
        for (int i = 0; i < total; ++i) {
            int idx = (i < np) ? i : MAX_PLAYERS + (i - np);
            Entity& e = snap.entities[idx];
            if (!e.alive) continue;

            int sc, sr;
            bf_pos(e.x, e.y, sc, sr);
            // Clamp the sprite centre to a safe drawable area so the hero
            // never disappears or partially renders when at the edge.
            if (sc < bf_x0 + 5)    sc = bf_x0 + 5;
            if (sc > bf_x1 - 6)    sc = bf_x1 - 6;
            if (sr < bf_y0 + 7)    sr = bf_y0 + 7;
            if (sr > bf_y1 - 2)    sr = bf_y1 - 2;

            bool is_act  = (e.id == active);
            bool flash   = (now_w - e.last_hit_time) < HIT_FLASH_SEC
                           && e.last_hit_time > 0;

            if (e.type == ENT_PLAYER) {
                int cp = flash ? CP_HIT
                              : (is_act ? CP_ACTIVE : CP_PLAYER);

                // 4-row hero sprite, two stances:
                //   active   — arms wide, weapon-up
                //   inactive — relaxed
                const char* lines_active[4] = {
                    " O ",
                    ">>O<<",
                    "/|\\",
                    "/ \\",
                };
                const char* lines_idle[4] = {
                    "\\o/",
                    " | ",
                    "/|\\",
                    "/ \\",
                };
                const char* const* L = is_act ? lines_active : lines_idle;
                draw_lines(sc, sr - 2, cp, L, 4);

                // Player tag above sprite
                attron(COLOR_PAIR(CP_PLAYER) | (is_act ? A_BOLD : 0));
                char tag[8];
                snprintf(tag, sizeof(tag), is_act ? "[P%d]" : "(P%d)", e.id + 1);
                int tlen = (int)strlen(tag);
                if (sr - 4 > bf_y0 && sc - tlen / 2 > bf_x0)
                    mvprintw(sr - 4, sc - tlen / 2, "%s", tag);
                attroff(COLOR_PAIR(CP_PLAYER) | (is_act ? A_BOLD : 0));

                // Tiny HP/STM bars below tag, above sprite
                if (sr - 3 > bf_y0) {
                    int hp_pair = (e.hp > e.max_hp / 2) ? CP_HP_FULL : CP_HP_LOW;
                    int filled = (e.max_hp > 0) ? (int)((float)e.hp / e.max_hp * 5) : 0;
                    attron(COLOR_PAIR(hp_pair));
                    for (int k = 0; k < 5; ++k) {
                        char ch3 = (k < filled) ? '#' : '.';
                        if (sc - 2 + k > bf_x0 && sc - 2 + k < bf_x1)
                            mvaddch(sr - 3, sc - 2 + k, ch3);
                    }
                    attroff(COLOR_PAIR(hp_pair));
                }
            } else {
                int ei = i - np;
                char glyph = ENEMY_GLYPHS[ei % (int)(sizeof(ENEMY_GLYPHS)-1)];
                int cpair  = enemy_color(ei, e.stunned);
                bool sel   = (ei == sel_target_idx);
                int cp = flash ? CP_HIT
                              : (is_act ? CP_ACTIVE : (sel ? CP_SELECT : cpair));

                // Build the 5-line cat-style enemy.  Three face variants
                // cycle to give visual variety.
                char l2[12];
                switch (ei % 3) {
                case 0: snprintf(l2, sizeof(l2), "( >%c.%c< )", glyph, glyph); break;
                case 1: snprintf(l2, sizeof(l2), "( >%c-%c< )", glyph, glyph); break;
                default:snprintf(l2, sizeof(l2), "( =%c~%c= )", glyph, glyph); break;
                }
                const char* lines[5] = {
                    " /\\___/\\",
                    l2,
                    " \\  W  /",
                    "  |   |",
                    " /|   |\\",
                };
                draw_lines(sc, sr - 4, cp, lines, 5);

                // [ENEMY n] tag above sprite
                char tag[16];
                snprintf(tag, sizeof(tag),
                         (sel || is_act) ? "[ENEMY %d]" : "(ENEMY %d)", ei + 1);
                int tlen = (int)strlen(tag);
                int tag_cp = sel ? CP_SELECT : cpair;
                attron(COLOR_PAIR(tag_cp) | A_BOLD | (sel ? A_REVERSE : 0));
                if (sr - 6 > bf_y0)
                    mvprintw(sr - 6, sc - tlen / 2, "%s", tag);
                attroff(COLOR_PAIR(tag_cp) | A_BOLD | (sel ? A_REVERSE : 0));

                // 5-cell HP bar just below the tag.
                if (sr - 5 > bf_y0) {
                    int hp_pair = (e.hp > e.max_hp / 2) ? CP_HP_FULL : CP_HP_LOW;
                    int filled = (e.max_hp > 0) ? (int)((float)e.hp / e.max_hp * 5) : 0;
                    attron(COLOR_PAIR(hp_pair));
                    for (int k = 0; k < 5; ++k) {
                        char ch3 = (k < filled) ? '#' : '.';
                        if (sc - 2 + k > bf_x0 && sc - 2 + k < bf_x1)
                            mvaddch(sr - 5, sc - 2 + k, ch3);
                    }
                    attroff(COLOR_PAIR(hp_pair));
                }

                // Stun indicator
                if (e.stunned && sr - 7 > bf_y0) {
                    attron(COLOR_PAIR(CP_STUN) | A_BOLD | A_BLINK);
                    mvprintw(sr - 7, sc - 2, "*STUN*");
                    attroff(COLOR_PAIR(CP_STUN) | A_BOLD | A_BLINK);
                }
            }

            // Big visible flash overlay when struck this frame
            if (flash) {
                attron(COLOR_PAIR(CP_HIT) | A_BOLD | A_BLINK);
                if (sr - 5 > bf_y0 && sc - 4 > bf_x0)
                    mvprintw(sr - 5, sc - 4, " *!HIT!* ");
                attroff(COLOR_PAIR(CP_HIT) | A_BOLD | A_BLINK);
            }
        }

        // Left Panel: Players
        int lr = 1;
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);
        mvprintw(lr++, 1, " PLAYERS ");
        attroff(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);

        for (int i = 0; i < np && lr < rows - LOG_H - 1; ++i) {
            Entity& e = snap.entities[i];
            bool act = (e.id == active);

            if (!e.alive) {
                attron(COLOR_PAIR(CP_DEAD));
                mvprintw(lr++, 1, " [FALLEN] %-8s", e.name);
                attroff(COLOR_PAIR(CP_DEAD));
                continue;
            }
            attron(COLOR_PAIR(act ? CP_ACTIVE : CP_PLAYER) | A_BOLD);
            mvprintw(lr++, 1, " ^  %-8s%s", e.name, act ? " <" : "  ");
            attroff(COLOR_PAIR(act ? CP_ACTIVE : CP_PLAYER) | A_BOLD);

            if (lr < rows - LOG_H - 1) {
                int hp_pair = (e.hp > e.max_hp / 2) ? CP_HP_FULL : CP_HP_LOW;
                mvprintw(lr, 1, " HP ");
                draw_bar_w(lr, 4, (float)e.hp, (float)e.max_hp, 14, hp_pair, CP_DEAD);
                attron(COLOR_PAIR(hp_pair));
                mvprintw(lr, 19, "%4d", e.hp);
                attroff(COLOR_PAIR(hp_pair));
                ++lr;
            }
            if (lr < rows - LOG_H - 1) {
                mvprintw(lr, 1, " SP ");
                int sp_pair = e.stunned ? CP_STUN : CP_SP_BAR;
                draw_bar_w(lr, 4, e.stamina, e.max_stamina, 14, sp_pair, CP_BORDER);
                if (e.stunned) {
                    attron(COLOR_PAIR(CP_STUN) | A_BLINK | A_BOLD);
                    mvprintw(lr, 19, "S%2.0f", e.stamina);
                    attroff(COLOR_PAIR(CP_STUN) | A_BLINK | A_BOLD);
                } else {
                    attron(COLOR_PAIR(CP_SP_BAR));
                    mvprintw(lr, 19, "%3.0f/%-3.0f", e.stamina, e.max_stamina);
                    attroff(COLOR_PAIR(CP_SP_BAR));
                }
                ++lr;
            }
            if (lr < rows - LOG_H - 1) {
                attron(COLOR_PAIR(CP_SPEED));
                mvprintw(lr++, 1, " Spd:%.0f Dmg:%d", e.speed, e.damage);
                attroff(COLOR_PAIR(CP_SPEED));
            }
            if (lr < rows - LOG_H - 1) {
                attron(COLOR_PAIR(CP_ARTIFACT));
                bool has_sc = e.inventory.has(WPN_SOLAR_CORE);
                bool has_lb = e.inventory.has(WPN_LUNAR_BLADE);
                mvprintw(lr++, 1, " %s%s",
                         has_sc ? "[SC]" : "    ",
                         has_lb ? "[LB]" : "    ");
                attroff(COLOR_PAIR(CP_ARTIFACT));
            }
            ++lr;
        }

        // Active-player inventory panel (under players list)
        if (display_active >= 0 && display_active < np) {
            Entity& me = snap.entities[display_active];
            int iy = lr;
            if (iy < rows - LOG_H - 8) {
                attron(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);
                mvprintw(iy++, 1, " INVENTORY ");
                attroff(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);

                // Visual 20-slot bar (2 rows of 10)
                attron(COLOR_PAIR(CP_BORDER));
                mvprintw(iy,   1, "Slots:");
                attroff(COLOR_PAIR(CP_BORDER));
                for (int s = 0; s < INVENTORY_SLOTS; ++s) {
                    int row = iy + (s / 10);
                    int col = 8 + (s % 10);
                    int w = me.inventory.slots[s];
                    if (w == WPN_NONE) {
                        attron(COLOR_PAIR(CP_BORDER));
                        mvaddch(row, col, '.');
                        attroff(COLOR_PAIR(CP_BORDER));
                    } else {
                        bool art = WEAPON_TABLE[w].is_artifact;
                        int cp = art ? CP_ARTIFACT : CP_GOLD;
                        attron(COLOR_PAIR(cp) | A_BOLD);
                        // Use first letter of weapon name as the glyph
                        char ch2 = WEAPON_TABLE[w].name[0];
                        mvaddch(row, col, ch2);
                        attroff(COLOR_PAIR(cp) | A_BOLD);
                    }
                }
                iy += 2;

                // Distinct weapons list with hotkeys 1..9
                attron(COLOR_PAIR(CP_LOG));
                int wcount = 0;
                int seen[WPN_COUNT]; for (int k=0;k<WPN_COUNT;++k) seen[k]=0;
                for (int s = 0; s < INVENTORY_SLOTS && iy < rows - LOG_H - 5 && wcount < 6; ++s) {
                    int w = me.inventory.slots[s];
                    if (w == WPN_NONE || seen[w]) continue;
                    seen[w] = 1; ++wcount;
                    int cp = WEAPON_TABLE[w].is_artifact ? CP_ARTIFACT : CP_GOLD;
                    attron(COLOR_PAIR(cp));
                    mvprintw(iy++, 1, " %d:%-12s d%d",
                             3 + wcount, WEAPON_TABLE[w].name, WEAPON_TABLE[w].damage);
                    attroff(COLOR_PAIR(cp));
                }
                if (me.inventory.lt_count > 0 && iy < rows - LOG_H - 5) {
                    attron(COLOR_PAIR(CP_BORDER));
                    mvprintw(iy++, 1, " LT:%d stored (V=swap-in)", me.inventory.lt_count);
                    attroff(COLOR_PAIR(CP_BORDER));
                }
                attroff(COLOR_PAIR(CP_LOG));
                lr = iy + 1;
            }
        }

        // Controls help — always visible at fixed position above the log panel.
        // Placed at a guaranteed position so it never gets pushed off-screen
        // by large player counts.
        {
            int ctrl_y = rows - LOG_H - 8;
            if (ctrl_y < lr) ctrl_y = lr;  // don't overlap with panels above
            if (ctrl_y + 7 < rows - LOG_H) {
                attron(COLOR_PAIR(CP_LOG));
                mvprintw(ctrl_y,   1, "WASD: free move       ");
                mvprintw(ctrl_y+1, 1, "<-/->: pick target    ");
                mvprintw(ctrl_y+2, 1, "1:Strike 2:Exhaust    ");
                mvprintw(ctrl_y+3, 1, "3:AoE  4-9:Use Weapon ");
                mvprintw(ctrl_y+4, 1, "V:swap-in  H:heal     ");
                mvprintw(ctrl_y+5, 1, "K/Spc:skip U:ultimate ");
                mvprintw(ctrl_y+6, 1, "P:pick up  Q:quit     ");
                attroff(COLOR_PAIR(CP_LOG));
            }
        }

        // Right Panel: Enemies
        int rpx = cols - RIGHT_W + 1;
        int rr  = 1;
        attron(COLOR_PAIR(CP_ENEMY) | A_BOLD | A_UNDERLINE);
        mvprintw(rr++, rpx, " ENEMIES %d/%d ", killed, WIN_KILL_COUNT);
        attroff(COLOR_PAIR(CP_ENEMY) | A_BOLD | A_UNDERLINE);

        for (int i = 0; i < ne && rr < rows - LOG_H - 1; ++i) {
            Entity& e = snap.entities[MAX_PLAYERS + i];
            char glyph = ENEMY_GLYPHS[i % (int)(sizeof(ENEMY_GLYPHS)-1)];
            bool act   = (e.id == active);
            int cpair  = enemy_color(i, e.stunned);

            if (!e.alive) {
                attron(COLOR_PAIR(CP_DEAD));
                mvprintw(rr++, rpx, " [%c][DEAD]%-6s", glyph, e.name);
                attroff(COLOR_PAIR(CP_DEAD));
                continue;
            }
            bool sel = (i == sel_target_idx);
            int rcp = act ? CP_ACTIVE : (sel ? CP_GOLD : cpair);
            attron(COLOR_PAIR(rcp) | A_BOLD);
            mvprintw(rr++, rpx, "%s[%c]%-9s%s",
                     sel ? ">" : " ", glyph, e.name, act ? "<" : " ");
            attroff(COLOR_PAIR(rcp) | A_BOLD);

            if (rr < rows - LOG_H - 1) {
                int hp_pair = (e.hp > e.max_hp / 2) ? CP_HP_FULL : CP_HP_LOW;
                mvprintw(rr, rpx, " HP");
                draw_bar_w(rr, rpx + 3, (float)e.hp, (float)e.max_hp, 14, hp_pair, CP_DEAD);
                attron(COLOR_PAIR(hp_pair));
                mvprintw(rr, rpx + 18, "%3d", e.hp);
                attroff(COLOR_PAIR(hp_pair));
                ++rr;
            }
            if (rr < rows - LOG_H - 1) {
                mvprintw(rr, rpx, " SP");
                draw_bar_w(rr, rpx + 3, e.stamina, e.max_stamina, 14, CP_SP_BAR, CP_BORDER);
                if (e.stunned) {
                    attron(COLOR_PAIR(CP_STUN) | A_BOLD | A_BLINK);
                    mvprintw(rr, rpx + 15, "S%2.0f/%-2.0f", e.stamina, e.max_stamina);
                    attroff(COLOR_PAIR(CP_STUN) | A_BOLD | A_BLINK);
                } else {
                    attron(COLOR_PAIR(CP_SP_BAR));
                    mvprintw(rr, rpx + 15, "%3.0f/%-3.0f", e.stamina, e.max_stamina);
                    attroff(COLOR_PAIR(CP_SP_BAR));
                }
                ++rr;
            }
            if (rr < rows - LOG_H - 1) {
                attron(COLOR_PAIR(CP_SPEED));
                mvprintw(rr, rpx, " Spd:%2.0f Dmg:%2d", e.speed, e.damage);
                attroff(COLOR_PAIR(CP_SPEED));
                ++rr;
            }
            ++rr;
        }

        // Population / status corner
        int alive_p = 0, alive_e = 0;
        for (int i = 0; i < np; ++i)
            if (snap.entities[i].alive) ++alive_p;
        for (int i = 0; i < ne; ++i)
            if (snap.entities[MAX_PLAYERS+i].alive) ++alive_e;

        int pop_y = rows - LOG_H - 10;
        if (pop_y > rr) {
            attron(COLOR_PAIR(CP_BORDER) | A_BOLD);
            mvprintw(pop_y, rpx, "Population:");
            attroff(COLOR_PAIR(CP_BORDER) | A_BOLD);
            attron(COLOR_PAIR(CP_PLAYER) | A_BOLD);
            mvprintw(pop_y + 1, rpx, " P:%d ", alive_p);
            attroff(COLOR_PAIR(CP_PLAYER) | A_BOLD);
            attron(COLOR_PAIR(CP_ENEMY) | A_BOLD);
            mvprintw(pop_y + 1, rpx + 5, " E:%d ", alive_e);
            attroff(COLOR_PAIR(CP_ENEMY) | A_BOLD);
            attron(COLOR_PAIR(CP_SPEED));
            mvprintw(pop_y + 2, rpx, "Speed: %s",
                     phase == PHASE_ULTIMATE_PAUSE ? "Pause" :
                     phase == PHASE_RUNNING        ? "Run"   : "---");
            attroff(COLOR_PAIR(CP_SPEED));
            if (snap.eclipse) {
                attron(COLOR_PAIR(CP_ARTIFACT) | A_BOLD | A_BLINK);
                mvprintw(pop_y + 3, rpx, "[ECLIPSE RELIC LIVE]");
                attroff(COLOR_PAIR(CP_ARTIFACT) | A_BOLD | A_BLINK);
            }

            int art_y = pop_y + 5;
            if (art_y + 3 < rows - LOG_H) {
                attron(COLOR_PAIR(CP_ARTIFACT) | A_BOLD | A_UNDERLINE);
                mvprintw(art_y, rpx, "ARTIFACTS");
                attroff(COLOR_PAIR(CP_ARTIFACT) | A_BOLD | A_UNDERLINE);
                for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                    int holder = snap.artifact_held_by[a];
                    const char* holder_name = "FREE";
                    if (holder >= 0 && holder < MAX_ENTITIES) {
                        holder_name = snap.entities[holder].name;
                    }
                    mvprintw(art_y + 1 + a, rpx, "%-7s %c %-8.8s",
                             WEAPON_TABLE[snap.artifact_id[a]].name,
                             snap.artifact_exists[a] ? 'Y' : 'N',
                             holder_name);
                }
            }
        }

        // Log panel
        int log_start = rows - LOG_H;
        attron(COLOR_PAIR(CP_BORDER) | A_BOLD);
        for (int c = 0; c < cols; ++c) mvaddch(log_start, c, ACS_HLINE);
        attroff(COLOR_PAIR(CP_BORDER) | A_BOLD);

        attron(COLOR_PAIR(CP_GOLD) | A_BOLD | A_UNDERLINE);
        mvprintw(log_start, 1, " ACTION LOG ");
        attroff(COLOR_PAIR(CP_GOLD) | A_BOLD | A_UNDERLINE);
        attron(COLOR_PAIR(CP_BORDER));
        int max_scroll_now = std::max(0, log_available_lines - (LOG_H - 1));
        mvprintw(log_start, 15, "[PgUp/PgDn [ ] , . Z/X Home C=export] lines:%d off:%d/%d",
                 log_available_lines, log_scroll_offset, max_scroll_now);
        attroff(COLOR_PAIR(CP_BORDER));

        int lines_show = LOG_H - 1;
        int head = snap.log_head;
        bool log_ring_full = snap.log_lines[head][0] != '\0';
        int log_count = log_ring_full ? LOG_LINES : head;
        int newest = (head - 1 + LOG_LINES) % LOG_LINES;
        int first_idx = (newest - log_scroll_offset - (lines_show - 1) + LOG_LINES) % LOG_LINES;
        if (log_count <= 0) first_idx = head;
        for (int i = 0; i < lines_show; ++i) {
            int idx = (first_idx + i) % LOG_LINES;
            if (!snap.log_lines[idx][0]) continue;
            char trunc[120];
            snprintf(trunc, sizeof(trunc), "%.116s", snap.log_lines[idx]);
            int lrow = log_start + 1 + i;
            if (lrow >= rows) break;
            const char* line = snap.log_lines[idx];
            int lcp = CP_LOG;
            if (strstr(line,"DEFEATED") || strstr(line,"VAPORIZED") || strstr(line,"STRIKE"))
                lcp = CP_LOG_HIT;
            else if (strstr(line,"HEAL") || strstr(line,"recovered"))
                lcp = CP_HP_FULL;
            else if (strstr(line,"ULTIMATE") || strstr(line,"CHRONO"))
                lcp = CP_ARTIFACT;
            else if (strstr(line,"STUN") || strstr(line,"DEADLOCK"))
                lcp = CP_STUN;
            attron(COLOR_PAIR(lcp));
            mvprintw(lrow, 1, " %s", trunc);
            attroff(COLOR_PAIR(lcp));
        }

        // Visual scrollbar on the right edge of the log panel.
        if (cols > 2 && lines_show > 0 && log_count > lines_show) {
            int track_x = cols - 1;
            int track_top = log_start + 1;
            int track_h = std::min(lines_show, rows - track_top);
            if (track_h > 0) {
                int max_scroll = std::max(1, log_count - lines_show);
                int thumb_h = std::max(1, (lines_show * track_h) / std::max(log_count, 1));
                if (thumb_h > track_h) thumb_h = track_h;
                int thumb_y = track_top + ((track_h - thumb_h) * log_scroll_offset) / max_scroll;

                attron(COLOR_PAIR(CP_BORDER));
                for (int r = 0; r < track_h; ++r) mvaddch(track_top + r, track_x, ACS_CKBOARD);
                attroff(COLOR_PAIR(CP_BORDER));

                attron(COLOR_PAIR(CP_GOLD) | A_BOLD);
                for (int r = 0; r < thumb_h; ++r) mvaddch(thumb_y + r, track_x, ACS_BLOCK);
                attroff(COLOR_PAIR(CP_GOLD) | A_BOLD);
            }
        }

        // Mid-game overlays
        if (phase == PHASE_ULTIMATE_PAUSE) {
            attron(COLOR_PAIR(CP_ULT) | A_BOLD | A_BLINK);
            mvprintw(rows/2, cols/2 - 16, "*** CHRONO BURST — 10s WINDOW ***");
            attroff(COLOR_PAIR(CP_ULT) | A_BOLD | A_BLINK);
        }

        // ── Full-screen GAME-OVER screens ──
        if (phase == PHASE_WIN || phase == PHASE_LOSE || phase == PHASE_QUIT) {
            erase();

            // Tally per-player stats from the snapshot.
            int alive_p = 0, dead_p = 0;
            int alive_e = 0;
            for (int i = 0; i < np; ++i)
                (snap.entities[i].alive ? alive_p : dead_p)++;
            for (int i = 0; i < ne; ++i)
                if (snap.entities[MAX_PLAYERS + i].alive) ++alive_e;

            int main_cp = (phase == PHASE_WIN) ? CP_WIN
                        : (phase == PHASE_LOSE) ? CP_LOSE : CP_TITLE;

            // Big ASCII banner
            const char* banner_win[7] = {
                " __     __  ___   ____  _____  ___    ____   __   __ ",
                " \\ \\   / / |_ _| / ___||_   _|/ _ \\  |  _ \\  \\ \\ / / ",
                "  \\ \\ / /   | | | |      | | | | | | | |_) |  \\ V /  ",
                "   \\ V /    | | | |___   | | | |_| | |  _ <    | |   ",
                "    \\_/    |___| \\____|  |_|  \\___/  |_| \\_\\   |_|   ",
                "                                                      ",
                "          The Chrono Rift has been mended.           ",
            };
            const char* banner_lose[7] = {
                "  ____   _____  _____  _____    _    _____             ",
                " |  _ \\ | ____|| ____|| ____|  / \\  |_   _|            ",
                " | | | ||  _|  |  _|  |  _|   / _ \\   | |              ",
                " | |_| || |___ | |    | |___ / ___ \\  | |              ",
                " |____/ |_____||_|    |_____/_/   \\_\\ |_|              ",
                "                                                       ",
                "        All heroes have fallen to the rift...         ",
            };
            const char* banner_quit[7] = {
                "   ___   _   _ ___  _____ _____ ___ _   _  ____  ",
                "  / _ \\ | | | |_ _||_   _|_   _|_ _| \\ | |/ ___| ",
                " | | | || | | || |   | |   | |  | ||  \\| | |  _  ",
                " | |_| || |_| || |   | |   | |  | || |\\  | |_| | ",
                "  \\__\\_\\ \\___/|___|  |_|   |_| |___|_| \\_|\\____| ",
                "                                                  ",
                "          You closed the rift early.             ",
            };
            const char* const* banner = (phase == PHASE_WIN) ? banner_win
                                      : (phase == PHASE_LOSE) ? banner_lose
                                      : banner_quit;
            int blines = 7;
            int b_top = std::max(2, rows/2 - 10);
            attron(COLOR_PAIR(main_cp) | A_BOLD);
            for (int k = 0; k < blines; ++k) {
                int blen = (int)strlen(banner[k]);
                int bx   = std::max(0, (cols - blen) / 2);
                if (b_top + k < rows - 1)
                    mvprintw(b_top + k, bx, "%s", banner[k]);
            }
            attroff(COLOR_PAIR(main_cp) | A_BOLD);

            // Stats summary box
            int sy = b_top + blines + 2;
            int sx = std::max(2, cols/2 - 28);
            attron(COLOR_PAIR(CP_BORDER) | A_BOLD);
            mvprintw(sy,   sx, "╔══════════════════════════════════════════════════╗");
            mvprintw(sy+1, sx, "║                  BATTLE  RESULTS                 ║");
            mvprintw(sy+2, sx, "╠══════════════════════════════════════════════════╣");
            mvprintw(sy+6, sx, "╚══════════════════════════════════════════════════╝");
            for (int dy = 3; dy <= 5; ++dy) mvprintw(sy+dy, sx, "║");
            for (int dy = 3; dy <= 5; ++dy) mvprintw(sy+dy, sx + 51, "║");
            attroff(COLOR_PAIR(CP_BORDER) | A_BOLD);

            attron(COLOR_PAIR(CP_GOLD) | A_BOLD);
            mvprintw(sy+3, sx + 4, "Enemies defeated   :  %d / %d",
                     killed, WIN_KILL_COUNT);
            mvprintw(sy+4, sx + 4, "Heroes alive       :  %d / %d   (fallen: %d)",
                     alive_p, np, dead_p);
            mvprintw(sy+5, sx + 4, "Enemies remaining  :  %d / %d   t = %.1fs",
                     alive_e, ne, vtime);
            attroff(COLOR_PAIR(CP_GOLD) | A_BOLD);

            // Per-player line
            int py = sy + 8;
            attron(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);
            if (py < rows - 2)
                mvprintw(py, std::max(2, cols/2 - 18), " HEROES OF THIS RUN ");
            attroff(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);
            for (int i = 0; i < np && py + 1 + i < rows - 2; ++i) {
                Entity& e = snap.entities[i];
                int cp_p = e.alive ? CP_HP_FULL : CP_DEAD;
                attron(COLOR_PAIR(cp_p) | (e.alive ? A_BOLD : 0));
                mvprintw(py + 1 + i, std::max(2, cols/2 - 22),
                         "  %-10s  HP %4d/%-4d   %s",
                         e.name, e.hp, e.max_hp,
                         e.alive ? "STANDING" : "FALLEN");
                attroff(COLOR_PAIR(cp_p) | (e.alive ? A_BOLD : 0));
            }

            // Footer
            int fy = std::min(rows - 2, py + 2 + np + 1);
            attron(COLOR_PAIR(CP_TITLE) | A_BOLD | A_BLINK);
            const char* footer = "─── press any key to exit ───";
            mvprintw(fy, std::max(0, (cols - (int)strlen(footer)) / 2),
                     "%s", footer);
            attroff(COLOR_PAIR(CP_TITLE) | A_BOLD | A_BLINK);

            refresh();

            // Block until a key is pressed (or ~10 s timeout) so the
            // user actually sees the screen.
            nodelay(stdscr, FALSE);
            timeout(-1);
            // But cap waiting so the process doesn't hang forever:
            for (int waited = 0; waited < 100; ++waited) {
                timeout(100);
                int kk = getch();
                if (kk != ERR) break;
            }
            break;
        }

        // Single refresh per frame — eliminates flicker
        refresh();
    }


    endwin();
    return nullptr;
}

// ─────────────────────────────────────────────
//  Lobby screen
//  NOTE: We do NOT call endwin() here anymore.
//  Instead we call endwin() once at the end of
//  lobby_screen so that the render thread can
//  call initscr() on a clean slate.
// ─────────────────────────────────────────────
static void lobby_screen(int &out_players, int &out_roll, int &out_level) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();

    init_pair(CP_TITLE,   COLOR_MAGENTA, -1);
    init_pair(CP_PLAYER,  COLOR_GREEN,   -1);
    init_pair(CP_GOLD,    COLOR_YELLOW,  -1);
    init_pair(CP_ACTIVE,  COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_BORDER,  COLOR_CYAN,    -1);
    init_pair(CP_LOG,     COLOR_WHITE,   -1);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int w = 80, h = 28;
    int sy = (rows - h) / 2, sx = (cols - w) / 2;
    if (sy < 0) sy = 0;
    if (sx < 0) sx = 0;

    WINDOW* win = newwin(h, w, sy, sx);
    keypad(win, TRUE);

    wborder(win, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE,
            ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);

    // ASCII-art title block
    static const char* TITLE_ART[7] = {
        "  /$$$$$$  /$$   /$$ /$$$$$$$   /$$$$$$  /$$   /$$  /$$$$$$         ",
        " /$$__  $$| $$  | $$| $$__  $$ /$$__  $$| $$$ | $$ /$$__  $$        ",
        "| $$  \\__/| $$  | $$| $$  \\ $$| $$  \\ $$| $$$$| $$| $$  \\ $$       ",
        "| $$      | $$$$$$$$| $$$$$$$/| $$  | $$| $$ $$ $$| $$  | $$        ",
        "| $$      | $$__  $$| $$__  $$| $$  | $$| $$  $$$$| $$  | $$        ",
        "| $$    $$| $$  | $$| $$  \\ $$| $$  | $$| $$\\  $$$| $$  | $$       ",
        "|  $$$$$$/| $$  | $$| $$  | $$|  $$$$$$/| $$ \\  $$|  $$$$$$/        "
    };
    wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    for (int i = 0; i < 7; ++i)
        mvwprintw(win, 1 + i, std::max(2, (w - (int)strlen(TITLE_ART[0])) / 2),
                  "%s", TITLE_ART[i]);
    wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    wattron(win, COLOR_PAIR(CP_GOLD) | A_BOLD);
    const char* sub = "==  R   I   F   T  ==   Multi-Process Tactical RPG";
    mvwprintw(win, 9, (w - (int)strlen(sub)) / 2, "%s", sub);
    wattroff(win, COLOR_PAIR(CP_GOLD) | A_BOLD);

    wattron(win, COLOR_PAIR(CP_BORDER));
    mvwhline(win, 10, 1, ACS_HLINE, w-2);
    mvwprintw(win, 11, 2, " Navigate: W/S or UP/DN    Adjust: A/D or LT/RT");
    mvwprintw(win, 12, 2, " Confirm: ENTER            Quit: Q / ESC");
    mvwhline(win, 13, 1, ACS_HLINE, w-2);
    wattroff(win, COLOR_PAIR(CP_BORDER));

    int players = 1, roll_no = 1234, level = 1, focus = 0;

    while (true) {
        for (int i = 0; i < 3; ++i) {
            int y = 15 + i * 2;
            char label[56];
            if (i==0)      snprintf(label, sizeof(label), "  Players (1-%d):    [ %d ]", MAX_PLAYERS, players);
            else if (i==1) snprintf(label, sizeof(label), "  Roll Number:       [ %d ]", roll_no);
            else           snprintf(label, sizeof(label), "  Difficulty (1-5):  [ %d ]", level);

            if (focus == i) {
                wattron(win, COLOR_PAIR(CP_ACTIVE) | A_BOLD);
                mvwprintw(win, y, 2, ">> %-60s", label);
                wattroff(win, COLOR_PAIR(CP_ACTIVE) | A_BOLD);
            } else {
                wattron(win, COLOR_PAIR(CP_LOG));
                mvwprintw(win, y, 2, "   %-60s", label);
                wattroff(win, COLOR_PAIR(CP_LOG));
            }
        }
        wattron(win, COLOR_PAIR(CP_PLAYER) | A_BOLD);
        mvwprintw(win, h-3, (w-32)/2, "[ Press ENTER to BEGIN THE RIFT ]");
        wattroff(win, COLOR_PAIR(CP_PLAYER) | A_BOLD);
        wattron(win, COLOR_PAIR(CP_GOLD));
        mvwprintw(win, h-5, 2, "Player HP preview: ~%d  Dmg: %d  Spd: %.0f",
                  roll_no + 550, (roll_no%10)+10, 100.0f/players);
        wattroff(win, COLOR_PAIR(CP_GOLD));
        wrefresh(win);

        int c = wgetch(win);
        if (c == 'q' || c == 'Q' || c == 27) { players=1; roll_no=0; level=1; break; }
        else if (c == KEY_UP   || c == 'w' || c == 'W') focus = (focus-1+3)%3;
        else if (c == KEY_DOWN || c == 's' || c == 'S') focus = (focus+1)%3;
        else if (c == KEY_LEFT || c == 'a' || c == 'A') {
            if (focus==0 && players>1) --players;
            else if (focus==1 && roll_no>0) --roll_no;
            else if (focus==2 && level>1) --level;
        }
        else if (c == KEY_RIGHT || c == 'd' || c == 'D') {
            if (focus==0 && players<MAX_PLAYERS) ++players;
            else if (focus==1) ++roll_no;
            else if (focus==2 && level<5) ++level;
        }
        else if (c == '\n' || c == KEY_ENTER) break;
    }

    delwin(win);
    // End lobby ncurses session cleanly so render_thread can re-init
    endwin();

    out_players = players;
    out_roll    = roll_no;
    out_level   = level;
}

// ─────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    srand((unsigned)time(nullptr));

    signal(SIGTERM, handle_sigterm);
    signal(SIGALRM, handle_sigalrm);
    signal(SIGCHLD, handle_sigchld);

    g_state = shm_create();
    g_state->arbiter_pid = getpid();

    int num_players = 1, roll_no = 0, level = 1;
    lobby_screen(num_players, roll_no, level);

    if (num_players < 1) num_players = 1;
    if (num_players > MAX_PLAYERS) num_players = MAX_PLAYERS;
    g_state->num_players = num_players;
    g_state->game_level  = level;

    // First wave: just a small group; the wave system replenishes the
    // field after each action until WIN_KILL_COUNT enemies are dead.
    int num_enemies = std::min(3, MAX_ENEMIES);
    g_state->num_enemies = num_enemies;
    g_wave_index = 1;

    // Init players
    for (int i = 0; i < num_players; ++i) {
        g_state->entities[i].init_player(i, roll_no, num_players);
        g_state->entities[i].max_hp   = roll_no + 100 + rand() % 901;
        g_state->entities[i].hp       = g_state->entities[i].max_hp;
        g_state->entities[i].damage   = (roll_no % 10) + 10;
        g_state->entities[i].speed    = 100.0f / num_players;
        g_state->entities[i].max_stamina = 100.0f;
        g_state->entities[i].stamina  = (g_state->entities[i].max_stamina * i) / std::max(1, num_players);

        g_state->player_actions[i].ready = false;
        // Heroes line up across the foreground (high BF y = closer to camera).
        int x_step = std::max(8, (BF_W / 3) / std::max(1, num_players));
        int x0 = 8;
        g_state->entities[i].x = std::max(2, std::min(BF_W - 2,
                                          x0 + i * x_step));
        g_state->entities[i].y = BF_H - 3;     // foreground row
    }

    // Init enemies
    for (int i = 0; i < num_enemies; ++i) {
        g_state->entities[MAX_PLAYERS + i].init_enemy(i, roll_no);
        int last2 = roll_no % 100;
        g_state->entities[MAX_PLAYERS + i].max_hp  = last2 + 50 + rand() % 151;
        g_state->entities[MAX_PLAYERS + i].hp      = g_state->entities[MAX_PLAYERS + i].max_hp;
        g_state->entities[MAX_PLAYERS + i].damage  = ((roll_no / 10) % 10) + 10;
        g_state->entities[MAX_PLAYERS + i].speed   = (float)(10 + rand() % 21);
        g_state->entities[MAX_PLAYERS + i].max_stamina = 150.0f;
        g_state->entities[MAX_PLAYERS + i].stamina = 0.0f;

        // Spread enemies across a 3-row × wide layout so the big sprites
        // never overlap. Rows further "back" (low BF y) appear higher
        // on screen due to the perspective offset in bf_pos.
        int row    = i / 3;                    // 0..2 (back / mid / front)
        int col    = i % 3;                    // 0..2 across the field
        // Columns staggered horizontally across most of the BF width.
        int ex = (BF_W * (col + 1)) / 4 + ((row & 1) ? 4 : 0) + 6;
        // Three depth bands: back, mid, front-but-still-behind-heroes.
        int ey = (row == 0) ? 4
               : (row == 1) ? (BF_H / 2)
                            : (BF_H - 8);
        g_state->entities[MAX_PLAYERS + i].x = std::max(2, std::min(BF_W - 2, ex));
        g_state->entities[MAX_PLAYERS + i].y = std::max(2, std::min(BF_H - 1, ey));
    }

    g_state->phase = PHASE_RUNNING;
    g_state->use_ncurses_ui = true;

    printf("[Arbiter] %d players, %d enemies. Level %d. Roll %d\n",
           num_players, num_enemies, level, roll_no);

    g_hip_pid = fork();
    if (g_hip_pid == 0) { execl("./hip_bin", "hip", nullptr); perror("execl hip"); exit(1); }
    g_state->hip_pid = g_hip_pid;

    g_asp_pid = fork();
    if (g_asp_pid == 0) { execl("./asp_bin", "asp", nullptr); perror("execl asp"); exit(1); }
    g_state->asp_pid = g_asp_pid;

    // Small delay to let children attach to SHM before scheduling begins
    usleep(50000);

    pthread_t t_render, t_deadlock, t_stun;
    pthread_create(&t_render,   nullptr, render_thread,    nullptr);
    pthread_create(&t_deadlock, nullptr, deadlock_monitor, nullptr);
    pthread_create(&t_stun,     nullptr, stun_tick,        nullptr);
    pthread_detach(t_deadlock);
    pthread_detach(t_stun);

    // Main scheduling loop
    char msg[LOG_LEN];
    while (true) {
        pthread_mutex_lock(&g_state->global_mutex);

        if (g_state->phase == PHASE_QUIT ||
            g_state->phase == PHASE_WIN  ||
            g_state->phase == PHASE_LOSE) {
            pthread_mutex_unlock(&g_state->global_mutex);
            break;
        }
        while (g_state->phase == PHASE_ULTIMATE_PAUSE)
            pthread_cond_wait(&g_state->turn_cond, &g_state->global_mutex);

        if (g_state->phase == PHASE_QUIT ||
            g_state->phase == PHASE_WIN  ||
            g_state->phase == PHASE_LOSE) {
            pthread_mutex_unlock(&g_state->global_mutex);
            break;
        }

        int next = scheduler_next();
        if (next < 0) {
            pthread_mutex_unlock(&g_state->global_mutex);
            usleep(1000);
            continue;
        }

        g_state->active_entity = next;
        g_state->npc_turn_deadline_sec = 0.0;
        pthread_cond_broadcast(&g_state->turn_cond);

        Entity& actor = g_state->entities[next];
        snprintf(msg, LOG_LEN, ">> [%s]'s turn (t=%.2f)", actor.name, g_state->virtual_time);
        g_state->log.push(msg);
        pthread_mutex_unlock(&g_state->global_mutex);

        // Wait for action
        ActionRequest req; req.ready = false;

        if (actor.type == ENT_PLAYER) {
            int pid_idx = actor.id;
            while (true) {
                pthread_mutex_lock(&g_state->global_mutex);
                if (g_state->player_actions[pid_idx].ready) {
                    req = g_state->player_actions[pid_idx];
                    g_state->player_actions[pid_idx].ready = false;
                    pthread_mutex_unlock(&g_state->global_mutex);
                    break;
                }
                if (g_state->phase != PHASE_RUNNING) {
                    pthread_mutex_unlock(&g_state->global_mutex);
                    goto done;
                }
                pthread_mutex_unlock(&g_state->global_mutex);
                usleep(5000);   // poll at 200 Hz — responsive without busy-spinning
            }
        } else {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += NPC_TURN_TIMEOUT;

            pthread_mutex_lock(&g_state->global_mutex);
            g_state->npc_turn_deadline_sec = deadline.tv_sec + deadline.tv_nsec / 1e9;
            pthread_cond_broadcast(&g_state->turn_cond);

            bool timed_out = false;
            while (!g_state->npc_action.ready || g_state->npc_action.entity_id != next) {
                if (g_state->phase != PHASE_RUNNING) {
                    pthread_mutex_unlock(&g_state->global_mutex); goto done;
                }
                int rc = pthread_cond_timedwait(&g_state->turn_cond,
                                                &g_state->global_mutex, &deadline);
                if (rc == ETIMEDOUT) { timed_out = true; break; }
            }
            if (timed_out) {
                req.entity_id = next; req.action = ACT_SKIP;
                snprintf(msg, LOG_LEN, "[Arbiter] NPC [%s] timeout -> SKIP",
                         g_state->entities[next].name);
                g_state->log.push(msg);
            } else {
                req = g_state->npc_action;
                g_state->npc_action.ready = false;
            }
            g_state->npc_turn_deadline_sec = 0.0;
            pthread_mutex_unlock(&g_state->global_mutex);
        }

        pthread_mutex_lock(&g_state->global_mutex);
        apply_action(req);
        // After each action, replenish enemy waves if room/budget remains.
        maybe_spawn_wave();
        check_game_over();
        g_state->active_entity = -1;
        g_state->npc_turn_deadline_sec = 0.0;
        pthread_cond_broadcast(&g_state->turn_cond);
        pthread_mutex_unlock(&g_state->global_mutex);
    }

done:
    printf("[Arbiter] Game over — phase=%d\n", (int)g_state->phase);
    if (g_state->phase == PHASE_WIN)
        printf("VICTORY! %d enemies defeated.\n", g_state->total_enemies_killed);
    if (g_state->phase == PHASE_LOSE)
        printf("DEFEAT. All heroes fell.\n");

    if (g_hip_pid > 0) kill(g_hip_pid, SIGTERM);
    if (g_asp_pid > 0) { kill(g_asp_pid, SIGCONT); kill(g_asp_pid, SIGTERM); }

    // Wait for render thread to finish (it will endwin before returning)
    pthread_join(t_render, nullptr);

    waitpid(g_hip_pid, nullptr, 0);
    waitpid(g_asp_pid, nullptr, 0);

    shm_detach(g_state);
    shm_destroy();
    return 0;
}