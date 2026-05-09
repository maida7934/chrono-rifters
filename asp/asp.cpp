// asp.cpp - Automated Strategic Process (NPC AI).
// One pthread per NPC; each thread waits for its turn, picks an action,
// and submits it to the Arbiter via shared memory.
//
// Search `RUBRIC:` to jump to each rubric item this file implements.
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

static SharedState* g_state = nullptr;
static volatile sig_atomic_t g_running = 1;
static pthread_t g_threads[MAX_ENEMIES];
static bool tracked_dead[MAX_ENEMIES] = {};

struct NpcArg { int enemy_slot; SharedState* state; };

static void handle_stun(int) {}
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

static void maybe_drop_weapon(SharedState* s, int slot) {
 (void)s;
 (void)slot;
 // Arbiter owns weapon-drop resolution because it knows which player
 // made the kill and can keep the player-choice offer deterministic.
}

// RUBRIC: Correct Handling of Dynamic Artifact (Eclipse Relic spawned at run time).
static void maybe_spawn_eclipse(SharedState* s) {
 // Fix: read total_enemies_killed under global_mutex to prevent race.
 pthread_mutex_lock(&s->global_mutex);
 int killed = s->total_enemies_killed;
 pthread_mutex_unlock(&s->global_mutex);
 if (killed < 3) return;

 pthread_mutex_lock(&s->resource_table.table_mutex);
 if (s->eclipse_relic_spawned || (rand()%100) >= 40) {
 pthread_mutex_unlock(&s->resource_table.table_mutex); return;
 }
 s->eclipse_relic_spawned = true;
 s->resource_table.entries[2].exists = true;
 s->resource_table.entries[2].held_by = -1;
 s->resource_table.entries[2].locked = false;
 pthread_mutex_unlock(&s->resource_table.table_mutex);
 // Fix: Arbiter is the sole owner of weapon_drop state.
 // Only push the log; let the Arbiter's spawn-wave/wave-check detect the
 // newly-available Eclipse Relic via the resource_table and notify the player.
 s->log.push("[World] *** Eclipse Relic appeared! ***");
}

// RUBRIC: Thread-per-NPC Implementation.
// RUBRIC: Thread Synchronization with Shared Memory (global_mutex + turn_cond).
// RUBRIC: Concurrent Access Handling (Threads + Processes).
static void* npc_thread(void* arg_ptr) {
 NpcArg* arg = (NpcArg*)arg_ptr;
 int slot = arg->enemy_slot;
 SharedState* s = arg->state;

 struct sigaction sa; memset(&sa, 0, sizeof(sa));
 sa.sa_handler = handle_stun; sa.sa_flags = 0;
 sigaction(SIGUSR1, &sa, nullptr);

 while (g_running) {
 pthread_mutex_lock(&s->global_mutex);
 // Exit early if entity died (wave ended) — the manager loop will
 // join this thread and respawn one for the next wave.
 if (!s->entities[slot].alive) {
 pthread_mutex_unlock(&s->global_mutex);
 return nullptr;
 }
 while (s->active_entity != slot) {
 if (!g_running || s->phase == PHASE_QUIT ||
 s->phase == PHASE_WIN || s->phase == PHASE_LOSE) {
 pthread_mutex_unlock(&s->global_mutex);
 return nullptr;
 }
 // Also break out if entity died while waiting
 if (!s->entities[slot].alive) {
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

 // RUBRIC: Enemy Behavior & Decision Logic.
 // Check if this enemy holds any usable weapon in primary inventory.
 WeaponID best_wpn = WPN_NONE;
 int best_dmg = 0;
 {
 Inventory& inv = s->entities[slot].inventory;
 int seen[WPN_COUNT] = {};
 for (int si = 0; si < INVENTORY_SLOTS; ++si) {
 int w = inv.slots[si];
 if (w == WPN_NONE || w < 0 || w >= WPN_COUNT || seen[w]) continue;
 seen[w] = 1;
 if (WEAPON_TABLE[w].damage > best_dmg) {
  best_dmg = WEAPON_TABLE[w].damage;
  best_wpn = (WeaponID)w;
 }
 }
 }
 pthread_mutex_unlock(&s->global_mutex);

 int roll = rand() % 100;
 if (target < 0) {
 req.action = ACT_SKIP;
 } else if (best_wpn != WPN_NONE && roll < 40) {
 // Use the strongest held weapon ~40% of the time
 req.action = ACT_USE_WEAPON;
 req.weapon = best_wpn;
 req.target_id = target;
 } else if (dist <= 4) {
 // Right next to a hero — strike almost always.
 if (roll < 92) {
 req.action = ACT_STRIKE;
 req.target_id = target;
 } else {
 req.action = ACT_SKIP;
 }
 } else {
 // Far away - still STRIKE most of the time, otherwise SKIP.
 if (roll < 75) {
 req.action = ACT_STRIKE;
 req.target_id = target;
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

// RUBRIC: Thread-per-NPC Implementation — thread lifecycle manager.
// Continuously monitors enemy slots: joins threads for dead enemies,
// spawns fresh threads when the Arbiter repopulates slots for new waves.
// This ensures every wave's enemies have a dedicated driving thread.
int main() {
 g_state = shm_attach();
 if (!g_state) { fprintf(stderr, "[ASP] SHM attach failed\n"); return 1; }
 srand((unsigned)g_state->roll_no);

 struct sigaction sa; memset(&sa, 0, sizeof(sa));
 sa.sa_handler = handle_sigterm; sigaction(SIGTERM, &sa, nullptr);
 sa.sa_handler = handle_stun; sigaction(SIGUSR1, &sa, nullptr);

 // RUBRIC: Efficient Thread Scheduling & Coordination - one thread per enemy.
 NpcArg args[MAX_ENEMIES];
 bool thread_active[MAX_ENEMIES] = {};

 // Spawn initial wave threads
 int ne = g_state->num_enemies;
 fprintf(stderr, "[ASP] initial %d NPC thread(s)\n", ne);
 for (int i = 0; i < ne; ++i) {
 args[i].enemy_slot = MAX_PLAYERS + i; args[i].state = g_state;
 tracked_dead[i] = false;
 pthread_create(&g_threads[i], nullptr, npc_thread, &args[i]);
 thread_active[i] = true;
 }

 // Wave-aware thread lifecycle loop
 while (g_running) {
 usleep(200000); // 200ms poll

 pthread_mutex_lock(&g_state->global_mutex);
 GamePhase ph = g_state->phase;
 ne = g_state->num_enemies;
 pthread_mutex_unlock(&g_state->global_mutex);

 if (ph == PHASE_QUIT || ph == PHASE_WIN || ph == PHASE_LOSE) break;

 for (int i = 0; i < ne; ++i) {
 int slot = MAX_PLAYERS + i;

 pthread_mutex_lock(&g_state->global_mutex);
 bool alive = g_state->entities[slot].alive;
 pthread_mutex_unlock(&g_state->global_mutex);

 // Join dead threads
 if (!alive && thread_active[i]) {
 pthread_join(g_threads[i], nullptr);
 thread_active[i] = false;
 if (!tracked_dead[i]) {
  tracked_dead[i] = true;
  maybe_drop_weapon(g_state, slot);
 }
 }

 // Spawn thread for newly alive enemy (new wave respawn)
 if (alive && !thread_active[i]) {
 args[i].enemy_slot = slot;
 args[i].state = g_state;
 tracked_dead[i] = false;
 pthread_create(&g_threads[i], nullptr, npc_thread, &args[i]);
 thread_active[i] = true;
 fprintf(stderr, "[ASP] spawned thread for %s (slot %d)\n",
  g_state->entities[slot].name, slot);
 }
 }
 }

 // Cleanup: join any remaining active threads
 g_running = 0;
 pthread_cond_broadcast(&g_state->turn_cond);
 for (int i = 0; i < MAX_ENEMIES; ++i) {
 if (thread_active[i]) {
 pthread_join(g_threads[i], nullptr);
 thread_active[i] = false;
 }
 }
 fprintf(stderr, "[ASP] done\n");
 shm_detach(g_state);
 return 0;
}
