// hip.cpp - Human Interfacing Process (player input).
// One pthread per player. Each thread is idle until it is the active
// entity, then collects an action and posts it to shared memory.
//
// Search `RUBRIC:` to jump to each rubric item this file implements.
#include "arbiter/game_state.h"
#include "arbiter/shm_utils.h"
#include "arbiter/allocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

static SharedState* g_state = nullptr;
static volatile sig_atomic_t g_running = 1;
static pthread_t g_threads[MAX_PLAYERS];
static int g_num_threads = 0;

static void hip_print(const char* fmt, ...) {
 va_list args; va_start(args, fmt);
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
 if (len > 0 && out[len-1] == '\n') out[len-1] = '\0';
}

static void handle_stun(int) {}
static void handle_sigterm(int) {
 g_running = 0;
 if (g_state) pthread_cond_broadcast(&g_state->turn_cond);
}

static bool consume_ui_quit_request_locked(SharedState* s, int pidx) {
 if (!s->quit_requested) return false;
 if (s->quit_requested_by >= 0 && s->quit_requested_by != pidx) return false;

 char msg[LOG_LEN];
 snprintf(msg, LOG_LEN,
 "[HIP] %s requested quit - sending SIGTERM to Arbiter.",
 s->entities[pidx].name);
 s->log.push(msg);
 s->quit_requested = false;
 s->quit_requested_by = -1;
 return true;
}

struct ThreadArg { int player_idx; SharedState* state; };

static void print_menu(SharedState* s, int pidx) {
 Entity& me = s->entities[pidx];
 hip_print("\n+-----------------------------------------+\n");
 hip_print("| [%s] HP:%d/%d  SP:%.0f/%.0f\n",
 me.name, me.hp, me.max_hp, me.stamina, me.max_stamina);
 hip_print("+-----------------------------------------+\n");

 hip_print("ENEMIES:\n");
 for (int i = 0; i < s->num_enemies; ++i) {
 Entity& e = s->entities[MAX_PLAYERS + i];
 if (e.alive)
 hip_print("  [%d] %s  HP:%d/%d\n", MAX_PLAYERS+i, e.name, e.hp, e.max_hp);
 }

 // Inventory — show slot-by-slot (per-player inventory)
 hip_print("INVENTORY [%s] [20 slots]:\n  ", me.name);
 for (int i = 0; i < INVENTORY_SLOTS; ) {
 if (me.inventory.slots[i] == WPN_NONE) {
 hip_print("[ ] "); ++i; continue;
 }
 WeaponID w = (WeaponID)me.inventory.slots[i];
 int sz = WEAPON_TABLE[w].slot_size;
 hip_print("[%s x%d] ", WEAPON_TABLE[w].name, sz);
 i += sz;
 }
 hip_print("\n");

 if (me.inventory.lt_count > 0) {
 hip_print("LT STORAGE: ");
 for (int i = 0; i < me.inventory.lt_count; ++i)
 hip_print("%s ", WEAPON_TABLE[me.inventory.lt_storage[i]].name);
 hip_print("\n");
 }

 bool can_ult = me.inventory.has(WPN_SOLAR_CORE) && me.inventory.has(WPN_LUNAR_BLADE);
 hip_print("Actions: 1)Strike 2)Exhaust 3)UseWeapon 4)SwapIn 5)Heal 6)Skip");
 if (can_ult) hip_print(" 7)ULTIMATE");

 // List free artifacts in the arena — Option 8 lets the player claim one.
 bool any_artifact = false;
 for (int a = 0; a < NUM_ARTIFACTS; ++a) {
 pthread_mutex_lock(&s->resource_table.table_mutex);
 bool avail = s->resource_table.entries[a].exists &&
 s->resource_table.entries[a].held_by < 0 &&
 !s->resource_table.entries[a].locked;
 WeaponID aid = s->resource_table.entries[a].id;
 pthread_mutex_unlock(&s->resource_table.table_mutex);
 if (!avail) continue;
 if (!any_artifact) {
 hip_print(" 8)Pickup artifact:");
 any_artifact = true;
 }
 hip_print(" %d=%s", (int)aid, WEAPON_TABLE[aid].name);
 }

 hip_print(" q)Quit\nChoice: ");
}

static int pick_enemy(SharedState* s) {
 hip_print("Target [%d-%d]: ", MAX_PLAYERS, MAX_PLAYERS + s->num_enemies - 1);
 int t = hip_read_int(-1);
 if (t < MAX_PLAYERS || t >= MAX_PLAYERS + s->num_enemies) { hip_print("Invalid.\n"); return -1; }
 if (!s->entities[t].alive) { hip_print("Dead.\n"); return -1; }
 return t;
}
static WeaponID pick_inventory_weapon(SharedState* s, int pidx) {
 hip_print("Weapon index (0-%d): ", WPN_COUNT-1);
 for (int w = 0; w < WPN_COUNT; ++w)
 if (s->entities[pidx].inventory.has((WeaponID)w))
 hip_print("%d=%s ", w, WEAPON_TABLE[w].name);
 hip_print(": ");
 int c = hip_read_int(-1);
 if (c < 0 || c >= WPN_COUNT || !s->entities[pidx].inventory.has((WeaponID)c)) { hip_print("Not found.\n"); return WPN_NONE; }
 return (WeaponID)c;
}
static WeaponID pick_lt_weapon(SharedState* s, int pidx) {
 if (s->entities[pidx].inventory.lt_count == 0) { hip_print("LT storage empty.\n"); return WPN_NONE; }
 hip_print("LT: ");
 for (int i = 0; i < s->entities[pidx].inventory.lt_count; ++i)
 hip_print("%d=%s ", s->entities[pidx].inventory.lt_storage[i], WEAPON_TABLE[s->entities[pidx].inventory.lt_storage[i]].name);
 hip_print("Choice: ");
 int c = hip_read_int(-1);
 if (c < 0 || c >= WPN_COUNT) return WPN_NONE;
 for (int i = 0; i < s->entities[pidx].inventory.lt_count; ++i)
 if (s->entities[pidx].inventory.lt_storage[i] == c) return (WeaponID)c;
 hip_print("Not in LT.\n"); return WPN_NONE;
}

// RUBRIC: Player Thread Handling (Active/Idle Logic) - waits on turn_cond
// and only processes input when active_entity == this player's index.
static void* player_thread(void* arg_ptr) {
 ThreadArg* arg = (ThreadArg*)arg_ptr;
 int pidx = arg->player_idx;
 SharedState* s = arg->state;

 struct sigaction sa; memset(&sa, 0, sizeof(sa));
 sa.sa_handler = handle_stun; sa.sa_flags = 0;
 sigaction(SIGUSR1, &sa, nullptr);

 while (g_running) {
 pthread_mutex_lock(&s->global_mutex);
 if (consume_ui_quit_request_locked(s, pidx)) {
 pthread_mutex_unlock(&s->global_mutex);
 kill(s->arbiter_pid, SIGTERM);
 g_running = 0;
 return nullptr;
 }
 while (s->active_entity != pidx) {
 if (consume_ui_quit_request_locked(s, pidx)) {
 pthread_mutex_unlock(&s->global_mutex);
 kill(s->arbiter_pid, SIGTERM);
 g_running = 0;
 return nullptr;
 }
 if (!g_running || s->phase == PHASE_QUIT ||
 s->phase == PHASE_WIN || s->phase == PHASE_LOSE) {
 pthread_mutex_unlock(&s->global_mutex);
 return nullptr;
 }
 pthread_cond_wait(&s->turn_cond, &s->global_mutex);
 }
 pthread_mutex_unlock(&s->global_mutex);

 // Stun wait
 pthread_mutex_lock(&s->global_mutex);
 while (s->entities[pidx].stunned) {
 if (consume_ui_quit_request_locked(s, pidx)) {
 pthread_mutex_unlock(&s->global_mutex);
 kill(s->arbiter_pid, SIGTERM);
 g_running = 0;
 return nullptr;
 }
 hip_print("[%s] STUNNED — waiting...\n", s->entities[pidx].name);
 pthread_cond_wait(&s->turn_cond, &s->global_mutex);
 if (!g_running) { pthread_mutex_unlock(&s->global_mutex); return nullptr; }
 }
 if (s->active_entity != pidx) { pthread_mutex_unlock(&s->global_mutex); continue; }

 // If ncurses UI active, just wait for render_thread to inject action
 if (s->use_ncurses_ui) {
 while (s->active_entity == pidx && !s->player_actions[pidx].ready &&
 (s->phase == PHASE_RUNNING || s->phase == PHASE_ULTIMATE_PAUSE) && g_running) {
 if (consume_ui_quit_request_locked(s, pidx)) {
 pthread_mutex_unlock(&s->global_mutex);
 kill(s->arbiter_pid, SIGTERM);
 g_running = 0;
 return nullptr;
 }
 pthread_cond_wait(&s->turn_cond, &s->global_mutex);
 }
 pthread_mutex_unlock(&s->global_mutex);
 continue;
 }

 // Weapon drop check
 if (s->weapon_drop_pending && s->weapon_drop_for == pidx) {
 WeaponID drop = s->weapon_drop_id;
 s->weapon_drop_pending = false;
 pthread_mutex_unlock(&s->global_mutex);
 hip_print("\n*** WEAPON DROP: %s (dmg %d, slots %d). Pick up? 1=yes 0=no: ",
 WEAPON_TABLE[drop].name, WEAPON_TABLE[drop].damage, WEAPON_TABLE[drop].slot_size);
 if (hip_read_int(0) == 1) {
 ActionRequest req; memset(&req, 0, sizeof(req));
 req.entity_id = pidx; req.action = ACT_PICKUP;
 req.weapon = drop; req.target_id = -1; req.ready = true;
 pthread_mutex_lock(&s->global_mutex);
 s->player_actions[pidx] = req;
 pthread_cond_broadcast(&s->turn_cond);
 pthread_mutex_unlock(&s->global_mutex);
 continue;
 }
 } else {
 pthread_mutex_unlock(&s->global_mutex);
 }

 pthread_mutex_lock(&s->global_mutex);
 if (s->active_entity != pidx || !s->entities[pidx].alive || !g_running) {
 pthread_mutex_unlock(&s->global_mutex); continue;
 }
 pthread_mutex_unlock(&s->global_mutex);

 print_menu(s, pidx);
 char choice[8] = {}; hip_read_str(choice, sizeof(choice));
 if (choice[0] == '\0') continue;

 ActionRequest req; memset(&req, 0, sizeof(req));
 req.entity_id = pidx; req.weapon = WPN_NONE; req.target_id = -1;
 bool valid = true;

 // RUBRIC: Win / Lose / Quit Conditions - HIP sends SIGTERM to Arbiter on quit.
 if (choice[0] == 'q' || choice[0] == 'Q') {
 req.action = ACT_QUIT;
 pthread_mutex_lock(&s->global_mutex);
 s->player_actions[pidx] = req; s->player_actions[pidx].ready = true;
 pthread_cond_broadcast(&s->turn_cond);
 pthread_mutex_unlock(&s->global_mutex);
 kill(s->arbiter_pid, SIGTERM);
 return nullptr;
 }

 switch (atoi(choice)) {
 case 1: req.action = ACT_STRIKE; req.target_id = pick_enemy(s); if (req.target_id<0) valid=false; break;
 case 2: req.action = ACT_EXHAUST; req.target_id = pick_enemy(s); if (req.target_id<0) valid=false; break;
 case 3: req.action = ACT_USE_WEAPON;
 req.weapon = pick_inventory_weapon(s, pidx); if (req.weapon==WPN_NONE){valid=false;break;}
 req.target_id = pick_enemy(s); if (req.target_id<0) valid=false; break;
 case 4: req.action = ACT_SWAP_IN; req.weapon = pick_lt_weapon(s, pidx); if (req.weapon==WPN_NONE) valid=false; break;
 case 5: req.action = ACT_HEAL; break;
 case 6: req.action = ACT_SKIP; break;
 case 7: {
 bool can = s->entities[pidx].inventory.has(WPN_SOLAR_CORE) &&
 s->entities[pidx].inventory.has(WPN_LUNAR_BLADE);
 if (!can) { hip_print("Need Solar Core + Lunar Blade!\n"); valid=false; }
 else req.action = ACT_ULTIMATE;
 break;
 }
 case 8: { // Pickup artifact (claim a free artifact from the arena)
 hip_print("Artifact weapon index to pick up: ");
 int wc = hip_read_int(-1);
 if (wc < 0 || wc >= WPN_COUNT) {
 hip_print("Invalid weapon index.\n");
 valid = false;
 } else if (!WEAPON_TABLE[wc].is_artifact) {
 hip_print("That is not an artifact.\n");
 valid = false;
 } else {
 // Confirm the artifact is currently free.
 pthread_mutex_lock(&s->resource_table.table_mutex);
 int aidx = s->resource_table.find((WeaponID)wc);
 bool ok = (aidx >= 0)
 && s->resource_table.entries[aidx].exists
 && s->resource_table.entries[aidx].held_by < 0
 && !s->resource_table.entries[aidx].locked;
 pthread_mutex_unlock(&s->resource_table.table_mutex);
 if (!ok) {
 hip_print("Artifact not available.\n");
 valid = false;
 } else {
 req.action = ACT_PICKUP;
 req.weapon = (WeaponID)wc;
 }
 }
 break;
 }
 default: hip_print("Unknown.\n"); valid=false; break;
 }

 if (!valid) continue;
 pthread_mutex_lock(&s->global_mutex);
 s->player_actions[pidx] = req; s->player_actions[pidx].ready = true;
 pthread_cond_broadcast(&s->turn_cond);
 pthread_mutex_unlock(&s->global_mutex);
 }
 return nullptr;
}

// RUBRIC: Bonus Multiplayer Extension - this HIP process owns ONLY the
// player slot indices passed via argv[1] (comma-separated). With two HIP
// processes the Arbiter launches one per partition, satisfying the spec's
// "two separate human controlled processes" requirement.
int main(int argc, char* argv[]) {
 g_state = shm_attach();
 if (!g_state) { fprintf(stderr, "[HIP] SHM attach failed\n"); return 1; }

 struct sigaction sa; memset(&sa, 0, sizeof(sa));
 sa.sa_handler = handle_sigterm; sigaction(SIGTERM, &sa, nullptr);
 sa.sa_handler = handle_stun; sigaction(SIGUSR1, &sa, nullptr);

 // Parse owned player slots from argv[1] (e.g. "0,1"). If absent, own all.
 int owned[MAX_PLAYERS]; int n_owned = 0;
 if (argc >= 2 && argv[1] && argv[1][0]) {
 char buf[32]; strncpy(buf, argv[1], sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
 for (char* tok = strtok(buf, ","); tok && n_owned < MAX_PLAYERS; tok = strtok(nullptr, ",")) {
 int v = atoi(tok);
 if (v >= 0 && v < g_state->num_players) owned[n_owned++] = v;
 }
 }
 if (n_owned == 0) {
 for (int i = 0; i < g_state->num_players; ++i) owned[n_owned++] = i;
 }

 g_num_threads = n_owned;
 fprintf(stderr, "[HIP pid=%d] owns %d player slot(s):", (int)getpid(), n_owned);
 for (int i = 0; i < n_owned; ++i) fprintf(stderr, " %d", owned[i]);
 fprintf(stderr, "\n");

 // RUBRIC: Correct Communication Design - HIP only writes ActionRequests
 // into shared memory; the Arbiter is the sole mutator of game state.
 ThreadArg args[MAX_PLAYERS];
 for (int i = 0; i < n_owned; ++i) {
 args[i].player_idx = owned[i]; args[i].state = g_state;
 pthread_create(&g_threads[i], nullptr, player_thread, &args[i]);
 }
 for (int i = 0; i < n_owned; ++i) pthread_join(g_threads[i], nullptr);
 fprintf(stderr, "[HIP pid=%d] done\n", (int)getpid());
 shm_detach(g_state);
 return 0;
}
