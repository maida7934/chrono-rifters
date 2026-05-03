/*
 * arbiter.cpp  —  Game Arbiter Process
 *
 * Responsibilities (per spec):
 *   - Central authority; owns global state in SHM
 *   - Temporal scheduling (stamina / speed model, Section 3)
 *   - Action execution (validate + apply player/NPC actions)
 *   - Deadlock monitor thread  (Section 7)
 *   - Ultimate Ability pause via SIGALRM  (Section 8)
 *   - NPC turn timeout (3 s, Section 8)
 *   - TUI rendering thread  (Section 9)
 *   - Lifecycle: spawn HIP + ASP, watch for SIGTERM/process death
 *   - Win / Lose / Quit detection
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
//  Globals (arbiter-local)
// ─────────────────────────────────────────────
static SharedState* g_state  = nullptr;
static pid_t        g_hip_pid = -1;
static pid_t        g_asp_pid = -1;
static volatile bool g_ultimate_running = false;
static volatile sig_atomic_t g_sigterm_received = 0;
static volatile sig_atomic_t g_sigalrm_received = 0;
static volatile sig_atomic_t g_sigchld_received = 0;

static void process_pending_signals() {
    if (!g_state) return;
    if (g_sigterm_received) {
        g_sigterm_received = 0;
        g_state->phase = PHASE_QUIT;
        g_state->log.push("[ARBITER] SIGTERM received. Shutting down.");
        pthread_cond_broadcast(&g_state->turn_cond);
    }
    if (g_sigalrm_received) {
        g_sigalrm_received = 0;
        if (g_ultimate_running) {
            g_ultimate_running = false;
            if (g_asp_pid > 0) kill(g_asp_pid, SIGCONT);
            g_state->log.push("[Arbiter] Ultimate window expired. ASP resumed.");
            g_state->phase          = PHASE_RUNNING;
            g_state->ultimate_active = false;
            pthread_cond_broadcast(&g_state->turn_cond);
        }
    }
    if (g_sigchld_received) {
        g_sigchld_received = 0;
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            char msg[LOG_LEN];
            snprintf(msg, LOG_LEN, "[Arbiter] Child PID %d exited.", (int)pid);
            g_state->log.push(msg);
            if (pid == g_hip_pid) {
                for (int i = 0; i < g_state->num_players; ++i) {
                    g_state->entities[i].alive = false;
                    g_state->player_actions[i].ready = false;
                }
                g_hip_pid = -1;
                snprintf(msg, LOG_LEN, "[Arbiter] HIP process died — all players marked dead.");
                g_state->log.push(msg);
            }
            if (pid == g_asp_pid) {
                for (int i = 0; i < g_state->num_enemies; ++i) {
                    g_state->entities[MAX_PLAYERS + i].alive = false;
                }
                g_state->npc_action.ready = false;
                g_asp_pid = -1;
                snprintf(msg, LOG_LEN, "[Arbiter] ASP process died — all enemies marked dead.");
                g_state->log.push(msg);
            }
            pthread_cond_broadcast(&g_state->turn_cond);
        }
    }
}

// ─────────────────────────────────────────────
//  SIGTERM handler  (player quit)
// ─────────────────────────────────────────────
static void handle_sigterm(int) {
    g_sigterm_received = 1;
}

// ─────────────────────────────────────────────
//  SIGALRM handler  (Ultimate 10-second window)
// ─────────────────────────────────────────────
static void handle_sigalrm(int) {
    g_sigalrm_received = 1;
}

// ─────────────────────────────────────────────
//  SIGCHLD handler  (process death)
// ─────────────────────────────────────────────
static void handle_sigchld(int) {
    g_sigchld_received = 1;
}

// ─────────────────────────────────────────────
//  Helper: check win / lose
// ─────────────────────────────────────────────
static bool check_game_over() {
    // Lose: all players dead
    bool any_player_alive = false;
    for (int i = 0; i < g_state->num_players; ++i)
        if (g_state->entities[i].alive) { any_player_alive = true; break; }
    if (!any_player_alive) {
        g_state->phase = PHASE_LOSE;
        return true;
    }
    // Win: 10 enemies killed
    if (g_state->total_enemies_killed >= WIN_KILL_COUNT) {
        g_state->phase = PHASE_WIN;
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────
//  Scheduler: pick next entity whose stamina
//  fills first under the speed model.
//
//  For each alive entity, time_to_full = (max_stamina - stamina) / speed
//  Advance virtual_time by min time_to_full.
// ─────────────────────────────────────────────
static int scheduler_next() {
    // Build list of active entity indices
    // Players are at 0..num_players-1, enemies at MAX_PLAYERS..MAX_PLAYERS+num_enemies-1
    int np = g_state->num_players;
    int ne = g_state->num_enemies;
    int indices[MAX_ENTITIES];
    int count = 0;
    for (int i = 0; i < np; ++i)
        indices[count++] = i;
    for (int i = 0; i < ne; ++i)
        indices[count++] = MAX_PLAYERS + i;

    float min_dt = 1e9f;
    bool any_schedulable = false;
    for (int k = 0; k < count; ++k) {
        Entity& e = g_state->entities[indices[k]];
        if (!e.alive || e.stunned) continue;
        any_schedulable = true;
        float dt = (e.max_stamina - e.stamina) / e.speed;
        if (dt < min_dt) min_dt = dt;
    }
    if (!any_schedulable) return -1;

    // Advance stamina for all alive/unstunned entities
    for (int k = 0; k < count; ++k) {
        Entity& e = g_state->entities[indices[k]];
        if (!e.alive) continue;
        if (!e.stunned)
            e.stamina += e.speed * min_dt;
        // Clamp
        if (e.stamina > e.max_stamina) e.stamina = e.max_stamina;
    }
    g_state->virtual_time += min_dt;

    // ── Tiebreak Logic ────────────────────────
    // If multiple entities reach max_stamina simultaneously, the first one encountered in the indices array 
    // wins the tie. Since we loaded players (0..np-1) first, then enemies (MAX_PLAYERS..), players inherently
    // win ties against enemies, and lower-indexed players win ties against higher-indexed players.
    // ──────────────────────────────────────────
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
//  Artifact helpers — WaitForGraph maintenance (Section 7)
// ─────────────────────────────────────────────

// Try to acquire an artifact for an entity.
// Returns true if acquired, false if blocked (sets wait_graph).
static bool artifact_acquire(int entity_id, WeaponID wpn) {
    ResourceTable& rt = g_state->resource_table;
    WaitForGraph& wfg = g_state->wait_graph;
    int aidx = rt.find(wpn);
    if (aidx < 0) return false;  // not an artifact

    pthread_mutex_lock(&rt.table_mutex);
    ArtifactEntry& ae = rt.entries[aidx];

    if (!ae.exists) {
        pthread_mutex_unlock(&rt.table_mutex);
        return false;
    }

    if (ae.held_by < 0) {
        // Free — acquire it
        ae.held_by = entity_id;
        ae.locked  = true;
        wfg.holding[entity_id][aidx] = 1;
        wfg.waiting_for[entity_id]   = -1;  // not waiting anymore
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[%s] acquired %s",
                 g_state->entities[entity_id].name, WEAPON_TABLE[wpn].name);
        g_state->log.push(msg);
        pthread_mutex_unlock(&rt.table_mutex);
        return true;
    } else {
        // Held by someone else — register wait
        wfg.waiting_for[entity_id] = aidx;
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[%s] waiting for %s (held by [%s])",
                 g_state->entities[entity_id].name,
                 WEAPON_TABLE[wpn].name,
                 g_state->entities[ae.held_by].name);
        g_state->log.push(msg);
        pthread_mutex_unlock(&rt.table_mutex);
        return false;
    }
}

// Release an artifact held by an entity.
static void artifact_release(int entity_id, WeaponID wpn) {
    ResourceTable& rt = g_state->resource_table;
    WaitForGraph& wfg = g_state->wait_graph;
    int aidx = rt.find(wpn);
    if (aidx < 0) return;

    pthread_mutex_lock(&rt.table_mutex);
    ArtifactEntry& ae = rt.entries[aidx];
    if (ae.held_by == entity_id) {
        ae.held_by = -1;
        ae.locked  = false;
        wfg.holding[entity_id][aidx] = 0;
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[%s] released %s",
                 g_state->entities[entity_id].name, WEAPON_TABLE[wpn].name);
        g_state->log.push(msg);
    }
    pthread_mutex_unlock(&rt.table_mutex);
}

// Release ALL artifacts held by an entity (called on death).
static void artifact_release_all(int entity_id) {
    ResourceTable& rt = g_state->resource_table;
    WaitForGraph& wfg = g_state->wait_graph;
    pthread_mutex_lock(&rt.table_mutex);
    for (int a = 0; a < NUM_ARTIFACTS; ++a) {
        if (rt.entries[a].held_by == entity_id) {
            rt.entries[a].held_by = -1;
            rt.entries[a].locked  = false;
        }
        wfg.holding[entity_id][a] = 0;
    }
    wfg.waiting_for[entity_id] = -1;
    pthread_mutex_unlock(&rt.table_mutex);
}

// Deliver stun to an entity: set flags + send SIGUSR1 to owning process
static void deliver_stun(int target_id) {
    Entity& target = g_state->entities[target_id];
    target.stunned = true;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    target.stun_end_time = ts.tv_sec + (ts.tv_nsec / 1e9) + STUN_DURATION;
    if (target.stamina >= target.max_stamina) {
        target.skip_turn_from_stun = true;
    }

    char msg[LOG_LEN];
    snprintf(msg, LOG_LEN, "[%s] is STUNNED for %.0fs!", target.name, STUN_DURATION);
    g_state->log.push(msg);

    // Deliver SIGUSR1 to the owning process to interrupt it asynchronously
    pid_t owner = (target.type == ENT_PLAYER) ? g_state->hip_pid
                                               : g_state->asp_pid;
    if (owner > 0) {
        kill(owner, SIGUSR1);
    }
}

// ─────────────────────────────────────────────
//  Apply action
// ─────────────────────────────────────────────
static void apply_action(ActionRequest& req) {
    Entity& actor  = g_state->entities[req.entity_id];
    char msg[LOG_LEN];

    switch (req.action) {

    case ACT_STRIKE: {
        if (req.target_id < 0) break;
        Entity& target = g_state->entities[req.target_id];
        target.hp -= actor.damage;
        if (target.hp < 0) target.hp = 0;
        snprintf(msg, LOG_LEN, "[%s] STRIKE → [%s] for %d dmg",
                 actor.name, target.name, actor.damage);
        g_state->log.push(msg);

        // Stun chance: 20% on high-damage strikes (damage >= 15)
        if (target.alive && actor.damage >= 15 && (rand() % 100) < 20) {
            deliver_stun(req.target_id);
        }

        if (target.hp == 0) {
            target.alive = false;
            snprintf(msg, LOG_LEN, "[%s] is DEFEATED!", target.name);
            g_state->log.push(msg);
            if (target.type == ENT_ENEMY) {
                ++g_state->total_enemies_killed;
                if (g_state->total_enemies_killed >= WIN_KILL_COUNT) {
                    g_state->phase = PHASE_WIN;
                    actor.stamina = 0;
                    return;
                }
            }
            artifact_release_all(req.target_id);
        }
        actor.stamina = 0;
        break;
    }

    case ACT_EXHAUST: {
        if (req.target_id < 0) break;
        Entity& target = g_state->entities[req.target_id];
        target.stamina -= actor.damage;
        if (target.stamina < 0) target.stamina = 0;
        snprintf(msg, LOG_LEN, "[%s] EXHAUST → [%s] stamina -%d",
                 actor.name, target.name, actor.damage);
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_USE_WEAPON: {
        if (req.target_id < 0 || req.weapon == WPN_NONE) break;
        if (req.weapon == actor.swapped_weapon_unavailable) {
            snprintf(msg, LOG_LEN, "[%s] %s is still readying from swap!", actor.name, WEAPON_TABLE[req.weapon].name);
            g_state->log.push(msg);
            break;
        }
        if (!actor.inventory.has(req.weapon)) {
            snprintf(msg, LOG_LEN, "[%s] doesn't have %s!",
                     actor.name, WEAPON_TABLE[req.weapon].name);
            g_state->log.push(msg);
            break;
        }
        // If it's an artifact weapon, acquire it in the resource table
        if (WEAPON_TABLE[req.weapon].is_artifact) {
            artifact_acquire(req.entity_id, req.weapon);
        }
        Entity& target = g_state->entities[req.target_id];
        int dmg = WEAPON_TABLE[req.weapon].damage;
        target.hp -= dmg;
        if (target.hp < 0) target.hp = 0;
        snprintf(msg, LOG_LEN, "[%s] USE %s → [%s] for %d dmg",
                 actor.name, WEAPON_TABLE[req.weapon].name,
                 target.name, dmg);
        g_state->log.push(msg);
        if (target.alive && dmg >= 45 && (rand() % 100) < 30) {
            deliver_stun(req.target_id);
        }

        if (target.hp == 0) {
            target.alive = false;
            snprintf(msg, LOG_LEN, "[%s] is DEFEATED!", target.name);
            g_state->log.push(msg);
            if (target.type == ENT_ENEMY) {
                ++g_state->total_enemies_killed;
                if (g_state->total_enemies_killed >= WIN_KILL_COUNT) {
                    g_state->phase = PHASE_WIN;
                    if (WEAPON_TABLE[req.weapon].is_artifact) artifact_release(req.entity_id, req.weapon);
                    actor.stamina = 0;
                    return;
                }
            }
            artifact_release_all(req.target_id);
        }
        // Release artifact after use
        if (WEAPON_TABLE[req.weapon].is_artifact) {
            artifact_release(req.entity_id, req.weapon);
        }
        actor.stamina = 0;
        break;
    }

    case ACT_SWAP_IN: {
        if (req.weapon == WPN_NONE) break;
        bool ok = allocator_swap_in(actor.inventory, req.weapon);
        if (ok) actor.swapped_weapon_unavailable = req.weapon;
        snprintf(msg, LOG_LEN, "[%s] SWAP IN %s: %s",
                 actor.name, WEAPON_TABLE[req.weapon].name,
                 ok ? "OK" : "FAIL");
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_HEAL: {
        int restored = (int)(actor.max_hp * 0.10f);
        actor.hp += restored;
        if (actor.hp > actor.max_hp) actor.hp = actor.max_hp;
        snprintf(msg, LOG_LEN, "[%s] HEAL +%d HP", actor.name, restored);
        g_state->log.push(msg);
        actor.stamina = 0;
        break;
    }

    case ACT_SKIP: {
        actor.stamina = actor.max_stamina * 0.50f;
        snprintf(msg, LOG_LEN, "[%s] SKIP (stamina → 50%%)", actor.name);
        g_state->log.push(msg);
        break;
    }

    case ACT_ULTIMATE: {
        // Verify actor has Solar Core + Lunar Blade in primary inventory
        if (!actor.inventory.has(WPN_SOLAR_CORE) ||
            !actor.inventory.has(WPN_LUNAR_BLADE)) {
            snprintf(msg, LOG_LEN,
                     "[%s] ULTIMATE FAILED: missing artifacts", actor.name);
            g_state->log.push(msg);
            actor.stamina = 0;
            break;
        }
        // Acquire both artifacts in the resource table
        bool got_sc = artifact_acquire(req.entity_id, WPN_SOLAR_CORE);
        bool got_lb = artifact_acquire(req.entity_id, WPN_LUNAR_BLADE);
        if (!got_sc || !got_lb) {
            if (got_sc) artifact_release(req.entity_id, WPN_SOLAR_CORE);
            if (got_lb) artifact_release(req.entity_id, WPN_LUNAR_BLADE);
            snprintf(msg, LOG_LEN,
                     "[%s] ULTIMATE BLOCKED: artifact contention", actor.name);
            g_state->log.push(msg);
            actor.stamina = 0;
            break;
        }

        snprintf(msg, LOG_LEN, "[%s] ★ ULTIMATE — CHRONO BURST!", actor.name);
        g_state->log.push(msg);

        // AOE damage: deal 50% of each enemy's max HP to ALL alive enemies
        for (int i = 0; i < g_state->num_enemies; ++i) {
            Entity& e = g_state->entities[MAX_PLAYERS + i];
            if (!e.alive) continue;
            int aoe_dmg = e.max_hp / 2;
            e.hp -= aoe_dmg;
            if (e.hp < 0) e.hp = 0;
            snprintf(msg, LOG_LEN, "  ★ [%s] takes %d CHRONO damage!", e.name, aoe_dmg);
            g_state->log.push(msg);
            if (e.hp == 0) {
                e.alive = false;
                ++g_state->total_enemies_killed;
                snprintf(msg, LOG_LEN, "  ★ [%s] is VAPORIZED!", e.name);
                g_state->log.push(msg);
                artifact_release_all(MAX_PLAYERS + i);
                if (g_state->total_enemies_killed >= WIN_KILL_COUNT) {
                    g_state->phase = PHASE_WIN;
                    artifact_release(req.entity_id, WPN_SOLAR_CORE);
                    artifact_release(req.entity_id, WPN_LUNAR_BLADE);
                    actor.stamina = 0;
                    return;
                }
            }
        }

        // Suspend ASP with SIGSTOP (Section 8 — signal-only enforcement)
        if (g_asp_pid > 0) kill(g_asp_pid, SIGSTOP);
        g_state->phase          = PHASE_ULTIMATE_PAUSE;
        g_state->ultimate_active = true;
        g_ultimate_running       = true;

        // Set SIGALRM for 10 seconds
        alarm((unsigned int)ULTIMATE_PAUSE);

        // Release artifacts after triggering
        artifact_release(req.entity_id, WPN_SOLAR_CORE);
        artifact_release(req.entity_id, WPN_LUNAR_BLADE);
        actor.stamina = 0;
        break;
    }

    case ACT_PICKUP: {
        // Pick up an artifact from the arena (Section 7)
        if (req.weapon == WPN_NONE) break;
        WeaponID wpn = req.weapon;

        if (!WEAPON_TABLE[wpn].is_artifact) {
            snprintf(msg, LOG_LEN, "[%s] %s is not an artifact!",
                     actor.name, WEAPON_TABLE[wpn].name);
            g_state->log.push(msg);
            break;
        }

        // Lock resource table and attempt to acquire
        if (!artifact_acquire(req.entity_id, wpn)) {
            // Blocked — wait registered in WaitForGraph
            actor.stamina = 0;
            break;
        }

        // Add to inventory via allocator (enforces 20-slot hard limit)
        bool added = allocator_add(actor.inventory, wpn);
        if (!added) {
            artifact_release(req.entity_id, wpn);
            snprintf(msg, LOG_LEN, "[%s] PICKUP %s FAILED: inventory full!",
                     actor.name, WEAPON_TABLE[wpn].name);
            g_state->log.push(msg);
        } else {
            snprintf(msg, LOG_LEN, "[%s] picked up %s!",
                     actor.name, WEAPON_TABLE[wpn].name);
            g_state->log.push(msg);
        }
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
//  Deadlock Monitor Thread  (Section 7)
// ─────────────────────────────────────────────
static void* deadlock_monitor(void*) {
    while (true) {
        sleep(1);
        if (!g_state || g_state->phase != PHASE_RUNNING) continue;

        pthread_mutex_lock(&g_state->resource_table.table_mutex);

        WaitForGraph& wfg = g_state->wait_graph;
        ResourceTable& rt = g_state->resource_table;

        // Build list of valid entity indices (same as scheduler)
        int np = g_state->num_players;
        int ne = g_state->num_enemies;
        int indices[MAX_ENTITIES];
        int count = 0;
        for (int i = 0; i < np; ++i)
            indices[count++] = i;
        for (int i = 0; i < ne; ++i)
            indices[count++] = MAX_PLAYERS + i;

        // DFS cycle detection on the wait-for graph
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
                        victim = u;
                        return true;
                    }
                }
            }
            in_stack[u] = false;
            return false;
        };

        for (int k = 0; k < count && victim < 0; ++k) {
            int idx = indices[k];
            if (!visited[idx] && g_state->entities[idx].alive)
                dfs(idx);
        }

        if (victim >= 0) {
            // Force victim to release all held artifacts
            for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                if (rt.entries[a].held_by == victim) {
                    rt.entries[a].held_by = -1;
                    rt.entries[a].locked  = false;
                    wfg.holding[victim][a] = 0;
                    char msg[LOG_LEN];
                    snprintf(msg, LOG_LEN,
                             "[Arbiter] DEADLOCK detected — forced [%s] to release %s",
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
//  TUI Rendering Thread  (Section 9)
// ─────────────────────────────────────────────
static void* render_thread(void*) {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    start_color();
    use_default_colors();

    init_pair(1, COLOR_GREEN,  -1);
    init_pair(2, COLOR_RED,    -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_CYAN,   -1);
    init_pair(5, COLOR_MAGENTA,-1);

    while (true) {
        if (!g_state) { usleep(100000); continue; }

        pthread_mutex_lock(&g_state->global_mutex);

        int np  = g_state->num_players;
        int ne  = g_state->num_enemies;
        GamePhase phase = g_state->phase;
        int killed      = g_state->total_enemies_killed;
        bool ult        = g_state->ultimate_active;
        int active      = g_state->active_entity;
        float vtime     = g_state->virtual_time;
        int log_head    = g_state->log.head;

        clear();
        int maxcol = 78;  // safe for 80-col terminal
        (void)maxcol;

        // ── Title bar (row 0-2) ──
        attron(COLOR_PAIR(5) | A_BOLD);
        mvprintw(0, 1, "=== C H R O N O   R I F T ===");
        attroff(COLOR_PAIR(5) | A_BOLD);

        // Phase / status on row 0 right side
        if (ult) {
            attron(COLOR_PAIR(3) | A_BLINK | A_BOLD);
            mvprintw(0, 42, "* ULTIMATE ACTIVE *");
            attroff(COLOR_PAIR(3) | A_BLINK | A_BOLD);
        } else if (phase == PHASE_WIN) {
            attron(COLOR_PAIR(1) | A_BOLD);
            mvprintw(0, 42, "** VICTORY! **");
            attroff(COLOR_PAIR(1) | A_BOLD);
        } else if (phase == PHASE_LOSE) {
            attron(COLOR_PAIR(2) | A_BOLD);
            mvprintw(0, 42, "** DEFEAT **");
            attroff(COLOR_PAIR(2) | A_BOLD);
        } else if (phase == PHASE_QUIT) {
            mvprintw(0, 42, "Quitting...");
        }

        mvprintw(1, 1, "Kills: %d/%d  Time: %.1f  Active: %s",
                 killed, WIN_KILL_COUNT, vtime,
                 (active >= 0 ? g_state->entities[active].name : "---"));

        int row = 3;

        // ── Players ──
        attron(COLOR_PAIR(4) | A_UNDERLINE);
        mvprintw(row++, 1, "PLAYERS");
        attroff(COLOR_PAIR(4) | A_UNDERLINE);

        for (int i = 0; i < np; ++i) {
            Entity& e = g_state->entities[i];
            if (!e.alive) {
                attron(COLOR_PAIR(2));
                mvprintw(row++, 1, " %-10s [DEAD]", e.name);
                attroff(COLOR_PAIR(2));
                continue;
            }
            // HP bar (15 chars)
            int hp_fill = (int)(15.0f * e.hp / e.max_hp);
            char hp_bar[18] = {};
            for (int j = 0; j < 15; ++j)
                hp_bar[j] = (j < hp_fill) ? '#' : '.';

            // Stamina bar (10 chars)
            int st_fill = (int)(10.0f * e.stamina / e.max_stamina);
            char st_bar[14] = {};
            for (int j = 0; j < 10; ++j)
                st_bar[j] = (j < st_fill) ? '=' : ' ';

            attron(e.stunned ? COLOR_PAIR(3) : COLOR_PAIR(1));
            mvprintw(row++, 1,
                     " %-10s HP[%s]%4d/%-4d SP[%s]%s",
                     e.name, hp_bar, e.hp, e.max_hp,
                     st_bar, e.stunned ? " STUN" : "");
            attroff(e.stunned ? COLOR_PAIR(3) : COLOR_PAIR(1));
        }

        row++;

        // ── Enemies ──
        attron(COLOR_PAIR(2) | A_UNDERLINE);
        mvprintw(row++, 1, "ENEMIES (Kills: %d/%d)", killed, WIN_KILL_COUNT);
        attroff(COLOR_PAIR(2) | A_UNDERLINE);

        for (int i = 0; i < ne; ++i) {
            Entity& e = g_state->entities[MAX_PLAYERS + i];
            if (!e.alive) {
                mvprintw(row++, 1, " %-10s [DEAD]", e.name);
                continue;
            }
            int hp_fill = (int)(15.0f * e.hp / e.max_hp);
            char hp_bar[18] = {};
            for (int j = 0; j < 15; ++j)
                hp_bar[j] = (j < hp_fill) ? '#' : '.';

            int st_fill = (int)(10.0f * e.stamina / e.max_stamina);
            char st_bar[14] = {};
            for (int j = 0; j < 10; ++j)
                st_bar[j] = (j < st_fill) ? '=' : ' ';

            attron(COLOR_PAIR(2));
            mvprintw(row++, 1,
                     " %-10s HP[%s]%4d/%-4d SP[%s]%s",
                     e.name, hp_bar, e.hp, e.max_hp,
                     st_bar, e.stunned ? " STUN" : "");
            attroff(COLOR_PAIR(2));
        }

        row++;

        // ── Action Log (show last 8 lines to fit screen) ──
        attron(A_UNDERLINE);
        mvprintw(row++, 1, "LOG");
        attroff(A_UNDERLINE);
        int log_show = 8;
        int log_start = (log_head - log_show + LOG_LINES) % LOG_LINES;
        for (int i = 0; i < log_show; ++i) {
            int idx = (log_start + i) % LOG_LINES;
            if (g_state->log.lines[idx][0]) {
                // Truncate to 76 chars for 80-col safety
                char trunc[78];
                snprintf(trunc, sizeof(trunc), "%.76s", g_state->log.lines[idx]);
                mvprintw(row++, 1, " %s", trunc);
            }
        }

        refresh();
        pthread_mutex_unlock(&g_state->global_mutex);

        usleep(100000);  // 10 FPS

        // Exit render loop when game ends
        if (phase == PHASE_WIN || phase == PHASE_LOSE || phase == PHASE_QUIT) {
            usleep(2000000);  // hold final frame for 2 seconds
            break;
        }
    }
    endwin();
    return nullptr;
}

// ─────────────────────────────────────────────
//  Stun tick thread — decrements stun timers
// ─────────────────────────────────────────────
static void* stun_tick(void*) {
    while (true) {
        usleep(10000);  // tick every 10ms for precision
        if (!g_state) continue;

        pthread_mutex_lock(&g_state->global_mutex);
        process_pending_signals();

        if (g_state->phase != PHASE_RUNNING &&
            g_state->phase != PHASE_ULTIMATE_PAUSE) {
            pthread_mutex_unlock(&g_state->global_mutex);
            continue;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double now = ts.tv_sec + (ts.tv_nsec / 1e9);

        int np = g_state->num_players;
        int ne = g_state->num_enemies;
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

// ─────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    srand((unsigned)time(nullptr));

    // Install signal handlers
    signal(SIGTERM, handle_sigterm);
    signal(SIGALRM, handle_sigalrm);
    signal(SIGCHLD, handle_sigchld);

    // ── Create + init shared memory ──────────
    g_state = shm_create();
    g_state->arbiter_pid = getpid();

    // ── Prompt for party size ─────────────────
    printf("Welcome to Chrono Rift!\n");
    printf("Select party size (1-4): ");
    int num_players = 1;
    scanf("%d", &num_players);
    if (num_players < 1) num_players = 1;
    if (num_players > MAX_PLAYERS) num_players = MAX_PLAYERS;

    int roll_no = 0;
    printf("Enter your Roll No (for stat seeding): ");
    scanf("%d", &roll_no);

    g_state->num_players = num_players;

    // Random enemy count 2–9
    int num_enemies = 2 + rand() % 8;
    g_state->num_enemies = num_enemies;

    // Initialise player entities
    for (int i = 0; i < num_players; ++i) {
        g_state->entities[i].init_player(i, roll_no, num_players);
        g_state->player_actions[i].ready = false;
    }

    // Initialise enemy entities
    for (int i = 0; i < num_enemies; ++i) {
        g_state->entities[MAX_PLAYERS + i].init_enemy(i, roll_no);
    }

    g_state->phase = PHASE_RUNNING;
    printf("[Arbiter] %d players, %d enemies.\n", num_players, num_enemies);

    // ── Spawn HIP ────────────────────────────
    g_hip_pid = fork();
    if (g_hip_pid == 0) {
        execl("./hip_bin", "hip", nullptr);
        perror("execl hip"); exit(1);
    }
    g_state->hip_pid = g_hip_pid;

    // ── Spawn ASP ────────────────────────────
    g_asp_pid = fork();
    if (g_asp_pid == 0) {
        execl("./asp_bin", "asp", nullptr);
        perror("execl asp"); exit(1);
    }
    g_state->asp_pid = g_asp_pid;

    // ── Launch background threads ─────────────
    pthread_t t_render, t_deadlock, t_stun;
    pthread_create(&t_render,   nullptr, render_thread,    nullptr);
    pthread_create(&t_deadlock, nullptr, deadlock_monitor, nullptr);
    pthread_create(&t_stun,     nullptr, stun_tick,        nullptr);
    pthread_detach(t_render);
    pthread_detach(t_deadlock);
    pthread_detach(t_stun);

    // ── Main scheduling loop ──────────────────
    while (true) {
        pthread_mutex_lock(&g_state->global_mutex);

        if (g_state->phase == PHASE_QUIT ||
            g_state->phase == PHASE_WIN  ||
            g_state->phase == PHASE_LOSE) {
            pthread_mutex_unlock(&g_state->global_mutex);
            break;
        }

        // Wait while ultimate pause is active
        while (g_state->phase == PHASE_ULTIMATE_PAUSE) {
            pthread_cond_wait(&g_state->turn_cond, &g_state->global_mutex);
        }

        // Re-check game-over after ultimate pause exits
        if (g_state->phase == PHASE_QUIT ||
            g_state->phase == PHASE_WIN  ||
            g_state->phase == PHASE_LOSE) {
            pthread_mutex_unlock(&g_state->global_mutex);
            break;
        }

        // Advance scheduler
        int next = scheduler_next();
        if (next < 0) {
            pthread_mutex_unlock(&g_state->global_mutex);
            usleep(1000);
            continue;
        }

        g_state->active_entity = next;
        pthread_cond_broadcast(&g_state->turn_cond);

        Entity& actor = g_state->entities[next];
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, ">> [%s]'s turn (t=%.2f)",
                 actor.name, g_state->virtual_time);
        g_state->log.push(msg);

        pthread_mutex_unlock(&g_state->global_mutex);

        // ── Wait for action ───────────────────
        ActionRequest req;
        req.ready = false;

        if (actor.type == ENT_PLAYER) {
            // HIP will fill player_actions[actor.id].ready
            int pid_idx = actor.id;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 30;  // generous player timeout

            // Spin-wait (could use cond_timedwait with a dedicated cond)
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
                usleep(10000);
            }
        } else {
            // NPC — ASP will fill npc_action.ready
            // 3-second timeout (Section 8)
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += NPC_TURN_TIMEOUT;

            // Use pthread_cond_timedwait for precise timeout under load
            pthread_mutex_lock(&g_state->global_mutex);
            pthread_cond_broadcast(&g_state->turn_cond);  // wake ASP threads

            bool timed_out = false;
            while (!g_state->npc_action.ready ||
                   g_state->npc_action.entity_id != next) {
                // Check for game-over while waiting
                if (g_state->phase != PHASE_RUNNING) {
                    pthread_mutex_unlock(&g_state->global_mutex);
                    goto done;
                }
                int rc = pthread_cond_timedwait(&g_state->turn_cond,
                                                &g_state->global_mutex,
                                                &deadline);
                if (rc == ETIMEDOUT) {
                    timed_out = true;
                    break;
                }
            }

            if (timed_out) {
                // Timeout → SKIP (Section 8)
                req.entity_id = next;
                req.action    = ACT_SKIP;
                snprintf(msg, LOG_LEN, "[Arbiter] NPC [%s] timeout → SKIP",
                         g_state->entities[next].name);
                g_state->log.push(msg);
            } else {
                req = g_state->npc_action;
                g_state->npc_action.ready = false;
            }
            pthread_mutex_unlock(&g_state->global_mutex);
        }

        // Apply action
        pthread_mutex_lock(&g_state->global_mutex);
        apply_action(req);
        check_game_over();
        g_state->active_entity = -1;
        pthread_cond_broadcast(&g_state->turn_cond);
        pthread_mutex_unlock(&g_state->global_mutex);
    }

done:
    // ── Graceful shutdown ─────────────────────
    printf("[Arbiter] Game over — phase=%d\n", (int)g_state->phase);

    if (g_state->phase == PHASE_WIN)
        printf("VICTORY! You defeated %d enemies.\n",
               g_state->total_enemies_killed);
    else if (g_state->phase == PHASE_LOSE)
        printf("DEFEAT. All heroes fell.\n");

    // Signal children
    if (g_hip_pid > 0) kill(g_hip_pid, SIGTERM);
    if (g_asp_pid > 0) {
        kill(g_asp_pid, SIGCONT);  // un-suspend if needed
        kill(g_asp_pid, SIGTERM);
    }
    waitpid(g_hip_pid, nullptr, 0);
    waitpid(g_asp_pid, nullptr, 0);

    sleep(1);  // let TUI show final state
    endwin();
    shm_detach(g_state);
    shm_destroy();
    return 0;
}