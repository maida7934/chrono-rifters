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

    // 2. Need to evict. Collect unique weapons currently in inventory.
    //    Build a list of (start_index, weapon_id, slot_size) for each run.
    struct Run { int start; WeaponID wid; int sz; };
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

    // Try evicting runs one-by-one (fewest first) until we have enough space.
    // Never evict artifacts — they must stay in primary inventory.
    bool evicted[INVENTORY_SLOTS] = {};
    int evict_list[INVENTORY_SLOTS];
    int nevict = 0;

    // Mark all free slots
    for (int i = 0; i < INVENTORY_SLOTS; ++i)
        evicted[i] = (inv.slots[i] == WPN_NONE);

    for (int r = 0; r < nruns; ++r) {
        // Never evict artifacts to LT storage
        if (WEAPON_TABLE[runs[r].wid].is_artifact) continue;

        // Evict this run
        for (int k = runs[r].start; k < runs[r].start + runs[r].sz; ++k)
            evicted[k] = true;
        evict_list[nevict++] = r;

        // Check if contiguous block of 'size' now exists
        for (int i = 0; i <= INVENTORY_SLOTS - size; ++i) {
            bool ok = true;
            for (int j = 0; j < size; ++j)
                if (!evicted[i + j]) { ok = false; break; }
            if (ok) {
                // Actually perform the evictions
                for (int e = 0; e < nevict; ++e) {
                    Run& er = runs[evict_list[e]];
                    if (inv.lt_count < MAX_LT_STORAGE)
                        inv.lt_storage[inv.lt_count++] = (int)er.wid;
                    inv.remove(er.wid);
                    fprintf(stderr, "[Allocator] Evicted '%s' to LT storage.\n",
                           WEAPON_TABLE[er.wid].name);
                }
                // Now place
                start = inv.find_contiguous(size);
                inv.place(start, id, size);
                return true;
            }
        }
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