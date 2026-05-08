#pragma once
#include "game_state.h"
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
//  Space Allocator  (Section 6)
//
//  Rules:
//  1. Find contiguous free space in primary inventory.
//  2. If none, evict the MINIMUM number of weapons to LT storage until
//     enough contiguous space is freed (first-fit eviction order).
//  3. Hard constraint: Solar Core + Lunar Blade together fill all 20 slots;
//     allocator must enforce this.
// ─────────────────────────────────────────────────────────────────────────────

// Attempt to add weapon to inventory. Returns true on success.
inline bool allocator_add(Inventory& inv, WeaponID id) {
    int size = WEAPON_TABLE[id].slot_size;

    // Hard constraint: Solar Core (10) + Lunar Blade (10) = all 20 slots.
    // If both are already in primary inventory, refuse any further addition.
    if (inv.has(WPN_SOLAR_CORE) && inv.has(WPN_LUNAR_BLADE) && id != WPN_SOLAR_CORE && id != WPN_LUNAR_BLADE) {
        fprintf(stderr, "[Allocator] Inventory full (20/20 with both artifacts). Cannot add '%s'.\n",
               WEAPON_TABLE[id].name);
        return false;
    }

    // 1. Try direct fit
    int start = inv.find_contiguous(size);
    if (start >= 0) {
        inv.place(start, id, size);
        return true;
    }

    // 2. Need to evict minimally. Strategy: repeatedly pick the weapon
    //    whose removal would create the largest contiguous free region
    //    (adjacency-aware), evict it to LT storage, and retry.
    //    This naturally favours weapons adjacent to existing gaps —
    //    e.g. evicting a 3-slot weapon at index 13 when slots 14-19
    //    are free is better than evicting a 6-slot weapon at index 0.
    //    Never evict artifacts.
    while (true) {
        // Collect current weapon runs
        struct Run { int pos; WeaponID wid; int sz; };
        Run runs[INVENTORY_SLOTS];
        int nruns = 0;
        {
            int i = 0;
            while (i < INVENTORY_SLOTS) {
                if (inv.slots[i] == WPN_NONE) { ++i; continue; }
                WeaponID w = (WeaponID)inv.slots[i];
                int j = i;
                while (j < INVENTORY_SLOTS && inv.slots[j] == (int)w) ++j;
                runs[nruns++] = {i, w, j - i};
                i = j;
            }
        }

        // For each evictable weapon, simulate removal and measure the
        // resulting largest contiguous free region.
        int best_run = -1;
        int best_gap = 0;
        for (int r = 0; r < nruns; ++r) {
            if (WEAPON_TABLE[runs[r].wid].is_artifact) continue;
            int gap = 0, max_gap = 0;
            for (int i = 0; i < INVENTORY_SLOTS; ++i) {
                bool free_cell = (inv.slots[i] == WPN_NONE) ||
                                 (i >= runs[r].pos && i < runs[r].pos + runs[r].sz);
                if (free_cell) { ++gap; if (gap > max_gap) max_gap = gap; }
                else gap = 0;
            }
            if (max_gap > best_gap) {
                best_gap = max_gap;
                best_run = r;
            }
        }

        if (best_run < 0) break;  // no evictable weapons remain

        // Evict the chosen weapon to LT storage
        if (inv.lt_count < MAX_LT_STORAGE)
            inv.lt_storage[inv.lt_count++] = (int)runs[best_run].wid;
        inv.remove(runs[best_run].wid);
        fprintf(stderr, "[Allocator] Evicted '%s' to LT storage.\n",
               WEAPON_TABLE[runs[best_run].wid].name);

        // Retry placement
        start = inv.find_contiguous(size);
        if (start >= 0) {
            inv.place(start, id, size);
            return true;
        }
        // Still not enough — loop and evict another
    }

    // Cannot fit even after eviction
    fprintf(stderr, "[Allocator] Cannot fit '%s' (needs %d slots).\n",
           WEAPON_TABLE[id].name, size);
    return false;
}

// Swap a weapon in from LT storage (Section 6 — costs a full turn)
// Returns true if the weapon was found in LT storage and loaded.
inline bool allocator_swap_in(Inventory& inv, WeaponID id) {
    // Find in LT storage
    int lt_idx = -1;
    for (int i = 0; i < inv.lt_count; ++i)
        if (inv.lt_storage[i] == (int)id) { lt_idx = i; break; }
    if (lt_idx < 0) {
        fprintf(stderr, "[Allocator] Weapon '%s' not in LT storage.\n",
               WEAPON_TABLE[id].name);
        return false;
    }
    // Remove from LT storage
    for (int i = lt_idx; i < inv.lt_count - 1; ++i)
        inv.lt_storage[i] = inv.lt_storage[i + 1];
    --inv.lt_count;

    // Add to primary (same eviction rules apply)
    return allocator_add(inv, id);
}