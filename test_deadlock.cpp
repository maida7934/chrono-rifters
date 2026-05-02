/*
 * test_deadlock.cpp  —  Deadlock Detection Scenario Test
 *
 * Simulates a circular wait:
 *   - Entity 0 (player) holds Solar Core, waits for Lunar Blade
 *   - Entity 4 (enemy)  holds Lunar Blade, waits for Solar Core
 *
 * The Arbiter's deadlock monitor should detect and resolve this
 * within 1–2 seconds by forcing one entity to release its artifact.
 *
 * Build: g++ -std=c++17 -pthread test_deadlock.cpp shared/weapon_table.cpp -o test_deadlock -lrt
 * Run:   ./test_deadlock
 */

#include "shared/game_state.h"
#include "shared/shm_utils.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

int main() {
    printf("=== Deadlock Detection Test ===\n\n");

    // Create shared memory
    SharedState* s = shm_create();
    s->num_players = 1;
    s->num_enemies = 1;
    s->entities[0].init_player(0, 12345, 1);
    s->entities[MAX_PLAYERS].init_enemy(0, 12345);
    s->phase = PHASE_RUNNING;

    // Setup: Entity 0 holds Solar Core (artifact 0)
    ResourceTable& rt = s->resource_table;
    WaitForGraph& wfg = s->wait_graph;

    pthread_mutex_lock(&rt.table_mutex);

    // Entity 0 holds Solar Core
    rt.entries[0].held_by = 0;
    rt.entries[0].locked  = true;
    wfg.holding[0][0]     = 1;

    // Entity MAX_PLAYERS holds Lunar Blade
    rt.entries[1].held_by = MAX_PLAYERS;
    rt.entries[1].locked  = true;
    wfg.holding[MAX_PLAYERS][1] = 1;

    // Entity 0 wants Lunar Blade (held by entity MAX_PLAYERS)
    wfg.waiting_for[0] = 1;  // artifact index 1 = Lunar Blade

    // Entity MAX_PLAYERS wants Solar Core (held by entity 0)
    wfg.waiting_for[MAX_PLAYERS] = 0;  // artifact index 0 = Solar Core

    pthread_mutex_unlock(&rt.table_mutex);

    printf("Setup complete:\n");
    printf("  Entity 0 (Player) holds Solar Core, waits for Lunar Blade\n");
    printf("  Entity %d (Enemy)  holds Lunar Blade, waits for Solar Core\n", MAX_PLAYERS);
    printf("  -> Circular wait: 0 -> Lunar(held by %d) -> Solar(held by 0)\n\n", MAX_PLAYERS);

    // Now run the deadlock monitor logic inline (same as arbiter's monitor)
    printf("Running deadlock monitor...\n");

    for (int attempt = 0; attempt < 3; ++attempt) {
        sleep(1);

        pthread_mutex_lock(&rt.table_mutex);

        // Build entity indices
        int indices[] = {0, MAX_PLAYERS};
        int count = 2;

        bool visited[MAX_ENTITIES] = {};
        bool in_stack[MAX_ENTITIES] = {};
        int victim = -1;

        // DFS
        for (int k = 0; k < count && victim < 0; ++k) {
            int u = indices[k];
            if (visited[u]) continue;

            // Simple DFS chain
            int path[MAX_ENTITIES];
            int plen = 0;
            int cur = u;
            while (cur >= 0 && !visited[cur]) {
                visited[cur] = in_stack[cur] = true;
                path[plen++] = cur;
                int w = wfg.waiting_for[cur];
                if (w >= 0 && w < NUM_ARTIFACTS) {
                    int holder = rt.entries[w].held_by;
                    if (holder >= 0) {
                        if (in_stack[holder]) {
                            victim = cur;
                            printf("  [Attempt %d] DEADLOCK DETECTED! Cycle involves entity %d\n",
                                   attempt + 1, victim);
                            break;
                        }
                        cur = holder;
                        continue;
                    }
                }
                break;
            }
            for (int i = 0; i < plen; ++i)
                in_stack[path[i]] = false;
        }

        if (victim >= 0) {
            // Force victim to release
            for (int a = 0; a < NUM_ARTIFACTS; ++a) {
                if (rt.entries[a].held_by == victim) {
                    printf("  Forcing entity %d to release %s\n",
                           victim, WEAPON_TABLE[rt.entries[a].id].name);
                    rt.entries[a].held_by = -1;
                    rt.entries[a].locked  = false;
                    wfg.holding[victim][a] = 0;
                }
            }
            wfg.waiting_for[victim] = -1;
            printf("\n=== DEADLOCK RESOLVED in %d second(s) ===\n", attempt + 1);
            pthread_mutex_unlock(&rt.table_mutex);
            break;
        }

        pthread_mutex_unlock(&rt.table_mutex);
    }

    // Verify resolution
    pthread_mutex_lock(&rt.table_mutex);
    printf("\nPost-resolution state:\n");
    for (int a = 0; a < NUM_ARTIFACTS; ++a) {
        printf("  %s: held_by=%d, locked=%s\n",
               WEAPON_TABLE[rt.entries[a].id].name,
               rt.entries[a].held_by,
               rt.entries[a].locked ? "true" : "false");
    }
    printf("  Entity 0 waiting_for=%d\n", wfg.waiting_for[0]);
    printf("  Entity %d waiting_for=%d\n", MAX_PLAYERS, wfg.waiting_for[MAX_PLAYERS]);
    pthread_mutex_unlock(&rt.table_mutex);

    shm_detach(s);
    shm_destroy();
    printf("\n=== Test complete ===\n");
    return 0;
}
