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
//  3. Solar Core (10) + Lunar Blade (10) together fill all 20 slots; if
//     both artifacts are present, picking up another weapon evicts one of
//     them to LT storage. Their resource-table locks remain owned by the
//     player so other players cannot acquire them while stored.
// ─────────────────────────────────────────────────────────────────────────────

// Compacts the primary inventory by shifting all placed weapons to the left,
// eliminating fragmentation gaps without changing LT storage.
inline void compact_inventory(Inventory& inv) {
    struct Run { WeaponID wid; int sz; };
    Run runs[INVENTORY_SLOTS];
    int nruns = 0;
    bool seen[WPN_COUNT] = {};
    int i = 0;
    while (i < INVENTORY_SLOTS) {
        if (inv.slots[i] == WPN_NONE) { ++i; continue; }
        WeaponID w = (WeaponID)inv.slots[i];
        int sz = WEAPON_TABLE[w].slot_size;
        if (!seen[(int)w]) {
            seen[(int)w] = true;
            runs[nruns++] = {w, sz};
        }
        i += sz;
    }
    // Wipe and re-place tightly from slot 0
    for (int s = 0; s < INVENTORY_SLOTS; ++s) inv.slots[s] = WPN_NONE;
    int pos = 0;
    for (int r = 0; r < nruns; ++r) {
        for (int s = 0; s < runs[r].sz && pos < INVENTORY_SLOTS; ++s)
            inv.slots[pos++] = (int)runs[r].wid;
    }
}

// Attempt to add weapon to inventory. Returns true on success.
inline bool allocator_add(Inventory& inv, WeaponID id) {
    int size = WEAPON_TABLE[id].slot_size;

    // 1. Try direct fit
    int start = inv.find_contiguous(size);
    if (start >= 0) {
        inv.place(start, id, size);
        return true;
    }

    // 1b. Compact first — close existing gaps before evicting anything.
    // This eliminates fragmentation from prior swap-outs so we evict the
    // minimum number of weapons needed, and the result has no internal gaps.
    compact_inventory(inv);
    start = inv.find_contiguous(size);
    if (start >= 0) {
        inv.place(start, id, size);
        return true;
    }

    // 2. Need to evict minimally. Strategy: prefer evicting the smallest
    //    NON-ARTIFACT weapon whose removal creates a gap large enough.
    //    If no non-artifact eviction is sufficient, allow artifact eviction
    //    as a last resort (artifacts move to LT but their resource-table
    //    locks remain owned by the original player so other players cannot
    //    use them while stored).
    while (true) {
        // Collect current weapon runs using authoritative slot_size
        struct Run { int pos; WeaponID wid; int sz; };
        Run runs[INVENTORY_SLOTS];
        int nruns = 0;
        {
            int i = 0;
            while (i < INVENTORY_SLOTS) {
                if (inv.slots[i] == WPN_NONE) { ++i; continue; }
                WeaponID w = (WeaponID)inv.slots[i];
                // Use the authoritative slot_size, not a counted run.
                // This prevents orphaned slots from mis-sized runs.
                int sz = WEAPON_TABLE[w].slot_size;
                runs[nruns++] = {i, w, sz};
                i += sz; // jump by true size, not counted size
            }
        }

        // Pick best run to evict. Two-tier preference:
        //   Tier 1: non-artifact whose removal creates a sufficient gap.
        //   Tier 2: any weapon (including artifacts) whose removal creates
        //   a sufficient gap.
        // Within a tier, tie-break by smallest weapon size, then largest gap.
        // If no eviction creates a sufficient gap, fall back to whichever
        // creates the largest gap (preferring non-artifacts).
        auto score_eviction = [&](int r, int& out_gap) {
            int gap = 0, max_gap = 0;
            for (int i = 0; i < INVENTORY_SLOTS; ++i) {
                bool free_cell = (inv.slots[i] == WPN_NONE) ||
                    (i >= runs[r].pos && i < runs[r].pos + runs[r].sz);
                if (free_cell) { ++gap; if (gap > max_gap) max_gap = gap; }
                else gap = 0;
            }
            out_gap = max_gap;
        };

        int best_run = -1;
        int best_score = -1;
        int best_sz = 9999;

        // Tier 1 — non-artifacts that fit
        for (int r = 0; r < nruns; ++r) {
            if (WEAPON_TABLE[runs[r].wid].is_artifact) continue;
            int max_gap; score_eviction(r, max_gap);
            if (max_gap >= size) {
                if (best_run < 0 || runs[r].sz < best_sz ||
                    (runs[r].sz == best_sz && max_gap > best_score)) {
                    best_sz = runs[r].sz;
                    best_score = max_gap;
                    best_run = r;
                }
            }
        }

        // Tier 2 — artifacts that fit (only if no non-artifact worked)
        if (best_run < 0) {
            for (int r = 0; r < nruns; ++r) {
                if (!WEAPON_TABLE[runs[r].wid].is_artifact) continue;
                int max_gap; score_eviction(r, max_gap);
                if (max_gap >= size) {
                    if (best_run < 0 || runs[r].sz < best_sz ||
                        (runs[r].sz == best_sz && max_gap > best_score)) {
                        best_sz = runs[r].sz;
                        best_score = max_gap;
                        best_run = r;
                    }
                }
            }
        }

        // Fallback: no single eviction is enough, pick one that
        // creates the largest gap (still preferring non-artifacts).
        if (best_run < 0) {
            int best_gap = 0;
            for (int pass = 0; pass < 2 && best_run < 0; ++pass) {
                bool want_artifacts = (pass == 1);
                for (int r = 0; r < nruns; ++r) {
                    if (WEAPON_TABLE[runs[r].wid].is_artifact != want_artifacts) continue;
                    int max_gap; score_eviction(r, max_gap);
                    if (max_gap > best_gap) { best_gap = max_gap; best_run = r; }
                }
            }
        }

        if (best_run < 0) break; // no evictable weapons remain

        // Evict the chosen weapon to LT storage
        if (inv.lt_count < MAX_LT_STORAGE)
            inv.lt_storage[inv.lt_count++] = (int)runs[best_run].wid;
        inv.remove(runs[best_run].wid);
        fprintf(stderr, "[Allocator] Evicted '%s' to LT storage.\n",
            WEAPON_TABLE[runs[best_run].wid].name);

        // Compact after eviction to close gaps, then retry placement
        compact_inventory(inv);
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

    // Treat swap-in as a transaction: if primary allocation fails after
    // evictions, restore the original inventory and LT storage.
    Inventory backup = inv;

    // Remove from LT storage
    for (int i = lt_idx; i < inv.lt_count - 1; ++i)
        inv.lt_storage[i] = inv.lt_storage[i + 1];
    --inv.lt_count;

    // Add to primary (same eviction rules apply)
    if (allocator_add(inv, id)) return true;

    inv = backup;
    return false;
}
