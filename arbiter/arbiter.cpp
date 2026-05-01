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
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <ncurses.h>
#include <algorithm>
#include <climits>

// ─────────────────────────────────────────────
//  Globals (arbiter-local)
// ─────────────────────────────────────────────
static SharedState* g_state  = nullptr;
static pid_t        g_hip_pid = -1;
static pid_t        g_asp_pid = -1;
static volatile bool g_ultimate_running = false;

// ─────────────────────────────────────────────
//  SIGTERM handler  (player quit)
// ─────────────────────────────────────────────
static void handle_sigterm(int) {
    if (g_state) {
        pthread_mutex_lock(&g_state->global_mutex);
        g_state->phase = PHASE_QUIT;
        pthread_cond_broadcast(&g_state->turn_cond);
        pthread_mutex_unlock(&g_state->global_mutex);
    }
}

// ─────────────────────────────────────────────
//  SIGALRM handler  (Ultimate 10-second window)
// ─────────────────────────────────────────────
static void handle_sigalrm(int) {
    if (!g_state || !g_ultimate_running) return;
    g_ultimate_running = false;

    // Resume ASP
    if (g_asp_pid > 0) kill(g_asp_pid, SIGCONT);

    // Log
    char msg[LOG_LEN];
    snprintf(msg, LOG_LEN, "[Arbiter] Ultimate window expired. ASP resumed.");
    g_state->log.push(msg);

    pthread_mutex_lock(&g_state->global_mutex);
    g_state->phase          = PHASE_RUNNING;
    g_state->ultimate_active = false;
    pthread_cond_broadcast(&g_state->turn_cond);
    pthread_mutex_unlock(&g_state->global_mutex);
}

// ─────────────────────────────────────────────
//  SIGCHLD handler  (process death)
// ─────────────────────────────────────────────
static void handle_sigchld(int) {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid > 0 && g_state) {
        char msg[LOG_LEN];
        snprintf(msg, LOG_LEN, "[Arbiter] Child PID %d exited.", (int)pid);
        g_state->log.push(msg);
    }
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
    float min_dt = 1e9f;
    int n = g_state->num_players + g_state->num_enemies;
    for (int i = 0; i < n; ++i) {
        Entity& e = g_state->entities[i];
        if (!e.alive) continue;
        if (e.stunned)  continue;  // stunned entities skip scheduling
        float dt = (e.max_stamina - e.stamina) / e.speed;
        if (dt < min_dt) min_dt = dt;
    }
    // Advance stamina for all entities
    for (int i = 0; i < n; ++i) {
        Entity& e = g_state->entities[i];
        if (!e.alive) continue;
        if (!e.stunned)
            e.stamina += e.speed * min_dt;
        // Clamp
        if (e.stamina > e.max_stamina) e.stamina = e.max_stamina;
    }
    g_state->virtual_time += min_dt;

    // Find entity with full stamina (ties broken by index)
    for (int i = 0; i < n; ++i) {
        Entity& e = g_state->entities[i];
        if (e.alive && !e.stunned &&
            e.stamina >= e.max_stamina) return i;
    }
    return -1;
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
        if (target.hp == 0) {
            target.alive = false;
            snprintf(msg, LOG_LEN, "[%s] is DEFEATED!", target.name);
            g_state->log.push(msg);
            if (target.type == ENT_ENEMY)
                ++g_state->total_enemies_killed;
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
        if (!actor.inventory.has(req.weapon)) {
            snprintf(msg, LOG_LEN, "[%s] doesn't have %s!",
                     actor.name, WEAPON_TABLE[req.weapon].name);
            g_state->log.push(msg);
            break;
        }
        Entity& target = g_state->entities[req.target_id];
        int dmg = WEAPON_TABLE[req.weapon].damage;
        target.hp -= dmg;
        if (target.hp < 0) target.hp = 0;
        snprintf(msg, LOG_LEN, "[%s] USE %s → [%s] for %d dmg",
                 actor.name, WEAPON_TABLE[req.weapon].name,
                 target.name, dmg);
        g_state->log.push(msg);
        if (target.hp == 0) {
            target.alive = false;
            if (target.type == ENT_ENEMY)
                ++g_state->total_enemies_killed;
        }
        actor.stamina = 0;
        break;
    }

    case ACT_SWAP_IN: {
        if (req.weapon == WPN_NONE) break;
        bool ok = allocator_swap_in(actor.inventory, req.weapon);
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
        snprintf(msg, LOG_LEN, "[%s] ★ ULTIMATE ABILITY!", actor.name);
        g_state->log.push(msg);

        // Suspend ASP with SIGSTOP
        if (g_asp_pid > 0) kill(g_asp_pid, SIGSTOP);
        g_state->phase          = PHASE_ULTIMATE_PAUSE;
        g_state->ultimate_active = true;
        g_ultimate_running       = true;

        // Set SIGALRM for 10 seconds
        alarm((unsigned int)ULTIMATE_PAUSE);
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

        // Build wait-for graph from resource table + wait_graph
        // Detect cycle: entity A waits for artifact X held by entity B,
        // entity B waits for artifact Y held by entity A (or longer cycle).
        WaitForGraph& wfg = g_state->wait_graph;
        ResourceTable& rt = g_state->resource_table;
        int n = g_state->num_players + g_state->num_enemies;

        // Simple cycle detection via DFS
        // Build adjacency: edge (i → j) if entity i waits for something held by j
        bool visited[MAX_ENTITIES] = {};
        bool in_stack[MAX_ENTITIES] = {};
        int  victim = -1;

        std::function<bool(int)> dfs = [&](int u) -> bool {
            visited[u] = in_stack[u] = true;
            int w = wfg.waiting_for[u];
            if (w >= 0) {
                // Find who holds artifact w
                int holder = rt.entries[w].held_by;
                if (holder >= 0 && holder < n) {
                    if (!visited[holder]) {
                        if (dfs(holder)) { victim = u; return true; }
                    } else if (in_stack[holder]) {
                        victim = u;  // break cycle here
                        return true;
                    }
                }
            }
            in_stack[u] = false;
            return false;
        };

        for (int i = 0; i < n && victim < 0; ++i)
            if (!visited[i] && g_state->entities[i].alive)
                dfs(i);

        if (victim >= 0) {
            // Force victim to release its held artifact
            for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                if (rt.entries[a].held_by == victim) {
                    rt.entries[a].held_by = -1;
                    rt.entries[a].locked  = false;
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
        // ── snapshot ──
        int np  = g_state->num_players;
        int ne  = g_state->num_enemies;
        GamePhase phase = g_state->phase;
        int killed      = g_state->total_enemies_killed;
        bool ult        = g_state->ultimate_active;

        // Copy entity data
        Entity ents[MAX_ENTITIES];
        memcpy(ents, g_state->entities, sizeof(ents));

        // Copy log
        char loglines[LOG_LINES][LOG_LEN];
        memcpy(loglines, g_state->log.lines, sizeof(loglines));
        int log_head = g_state->log.head;

        pthread_mutex_unlock(&g_state->global_mutex);

        clear();

        // Title
        attron(COLOR_PAIR(5) | A_BOLD);
        mvprintw(0, 2, "╔══════════════════════════════════════╗");
        mvprintw(1, 2, "║        C H R O N O   R I F T         ║");
        mvprintw(2, 2, "╚══════════════════════════════════════╝");
        attroff(COLOR_PAIR(5) | A_BOLD);

        if (ult) {
            attron(COLOR_PAIR(3) | A_BLINK | A_BOLD);
            mvprintw(1, 45, "★ ULTIMATE ABILITY ACTIVE ★");
            attroff(COLOR_PAIR(3) | A_BLINK | A_BOLD);
        }

        int row = 4;

        // ── Players ──
        attron(COLOR_PAIR(4) | A_UNDERLINE);
        mvprintw(row++, 2, "PLAYER PARTY");
        attroff(COLOR_PAIR(4) | A_UNDERLINE);

        for (int i = 0; i < np; ++i) {
            Entity& e = ents[i];
            if (!e.alive) {
                attron(COLOR_PAIR(2));
                mvprintw(row++, 2, "%-12s  [DEAD]", e.name);
                attroff(COLOR_PAIR(2));
                continue;
            }
            // HP bar (20 chars)
            int hp_fill = (int)(20.0f * e.hp / e.max_hp);
            char hp_bar[24] = {};
            for (int j = 0; j < 20; ++j)
                hp_bar[j] = (j < hp_fill) ? '#' : '.';

            // Stamina bar (15 chars)
            int st_fill = (int)(15.0f * e.stamina / e.max_stamina);
            char st_bar[18] = {};
            for (int j = 0; j < 15; ++j)
                st_bar[j] = (j < st_fill) ? '=' : ' ';

            attron(e.stunned ? COLOR_PAIR(3) : COLOR_PAIR(1));
            mvprintw(row++, 2,
                     "%-12s HP[%s]%4d/%-4d  SP[%s]%5.1f/%-5.0f%s",
                     e.name, hp_bar, e.hp, e.max_hp,
                     st_bar, e.stamina, e.max_stamina,
                     e.stunned ? "  STUNNED" : "");
            attroff(e.stunned ? COLOR_PAIR(3) : COLOR_PAIR(1));
        }

        row++;

        // ── Enemies ──
        attron(COLOR_PAIR(2) | A_UNDERLINE);
        mvprintw(row++, 2, "ENEMIES  (Kills: %d/%d)", killed, WIN_KILL_COUNT);
        attroff(COLOR_PAIR(2) | A_UNDERLINE);

        for (int i = 0; i < ne; ++i) {
            Entity& e = ents[MAX_PLAYERS + i];
            if (!e.alive) {
                mvprintw(row++, 2, "%-12s  [DEAD]", e.name);
                continue;
            }
            int hp_fill = (int)(20.0f * e.hp / e.max_hp);
            char hp_bar[24] = {};
            for (int j = 0; j < 20; ++j)
                hp_bar[j] = (j < hp_fill) ? '#' : '.';

            int st_fill = (int)(15.0f * e.stamina / e.max_stamina);
            char st_bar[18] = {};
            for (int j = 0; j < 15; ++j)
                st_bar[j] = (j < st_fill) ? '=' : ' ';

            attron(COLOR_PAIR(2));
            mvprintw(row++, 2,
                     "%-12s HP[%s]%4d/%-4d  SP[%s]%5.1f/%-5.0f%s",
                     e.name, hp_bar, e.hp, e.max_hp,
                     st_bar, e.stamina, e.max_stamina,
                     e.stunned ? " STUNNED" : "");
            attroff(COLOR_PAIR(2));
        }

        row += 1;

        // ── Action Log ──
        attron(A_UNDERLINE);
        mvprintw(row++, 2, "ACTION LOG");
        attroff(A_UNDERLINE);
        for (int i = 0; i < LOG_LINES; ++i) {
            int idx = (log_head + i) % LOG_LINES;
            if (loglines[idx][0])
                mvprintw(row++, 2, "%s", loglines[idx]);
        }

        // ── Phase banner ──
        if (phase == PHASE_WIN) {
            attron(COLOR_PAIR(1) | A_BOLD);
            mvprintw(2, 45, "★ VICTORY! ★");
            attroff(COLOR_PAIR(1) | A_BOLD);
        } else if (phase == PHASE_LOSE) {
            attron(COLOR_PAIR(2) | A_BOLD);
            mvprintw(2, 45, "✗ DEFEAT ✗");
            attroff(COLOR_PAIR(2) | A_BOLD);
        } else if (phase == PHASE_QUIT) {
            mvprintw(2, 45, "Quitting...");
        }

        refresh();
        usleep(100000);  // 10 FPS
    }
    endwin();
    return nullptr;
}

// ─────────────────────────────────────────────
//  Stun tick thread — decrements stun timers
// ─────────────────────────────────────────────
static void* stun_tick(void*) {
    while (true) {
        usleep(100000);  // tick every 100ms
        if (!g_state) continue;
        pthread_mutex_lock(&g_state->global_mutex);
        int n = g_state->num_players + g_state->num_enemies;
        for (int i = 0; i < n; ++i) {
            Entity& e = g_state->entities[i];
            if (e.stunned) {
                e.stun_remaining -= 0.1f;
                if (e.stun_remaining <= 0.0f) {
                    e.stunned        = false;
                    e.stun_remaining = 0.0f;
                    char msg[LOG_LEN];
                    snprintf(msg, LOG_LEN, "[%s] recovered from stun.", e.name);
                    g_state->log.push(msg);
                }
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
    (void)argc; (void)argv;  // suppress unused parameter warnings
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
        execl("./hip", "./hip", nullptr);
        perror("execl hip"); exit(1);
    }
    g_state->hip_pid = g_hip_pid;

    // ── Spawn ASP ────────────────────────────
    g_asp_pid = fork();
    if (g_asp_pid == 0) {
        execl("./asp", "./asp", nullptr);
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
            // 3-second timeout
            g_state->npc_timeout.store(false);
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += NPC_TURN_TIMEOUT;

            // Notify ASP via turn_cond
            pthread_mutex_lock(&g_state->global_mutex);
            pthread_cond_broadcast(&g_state->turn_cond);
            pthread_mutex_unlock(&g_state->global_mutex);

            bool got_action = false;
            while (true) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec > deadline.tv_sec ||
                    (now.tv_sec == deadline.tv_sec &&
                     now.tv_nsec >= deadline.tv_nsec)) {
                    // Timeout → SKIP
                    req.entity_id = next;
                    req.action    = ACT_SKIP;
                    snprintf(msg, LOG_LEN, "[Arbiter] NPC timeout → SKIP");
                    g_state->log.push(msg);
                    got_action = true;
                    break;
                }
                pthread_mutex_lock(&g_state->global_mutex);
                if (g_state->npc_action.ready &&
                    g_state->npc_action.entity_id == next) {
                    req = g_state->npc_action;
                    g_state->npc_action.ready = false;
                    got_action = true;
                    pthread_mutex_unlock(&g_state->global_mutex);
                    break;
                }
                pthread_mutex_unlock(&g_state->global_mutex);
                usleep(10000);
            }
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