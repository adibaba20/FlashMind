#include "ftl.h"

/* ---------------------------------------------------------------------
 * wl_select_target_block
 * ---------------------------------------------------------------------
 * THE CORE IDEA OF WEAR LEVELING, in one sentence:
 *   "Every block can only survive a limited number of erases before it
 *    wears out — so don't keep hammering the same block, spread erases
 *    evenly across all of them."
 *
 * This function is the ONLY place that decides which physical block a
 * new write lands in. The strategy: look at every block that still has
 * at least one free page, and pick whichever one has the LOWEST
 * erase_count so far. Over many writes, this naturally keeps every
 * block's wear roughly equal — you'll see this directly in the
 * "erase count per block" histogram Module 2's demo prints.
 *
 * This is called DYNAMIC wear leveling because it only affects data
 * that's actively being written/rewritten. It does nothing for data
 * that's written once and never touched again (e.g. a config file) —
 * that data sits in a low-wear block forever while other blocks wear
 * out around it. Real SSDs also do STATIC wear leveling (periodically
 * relocating even untouched cold data) — noted as a stretch goal below,
 * intentionally left out of Module 2 to keep this module focused.
 *
 * exclude_block: pass -1 for a normal write. Garbage collection passes
 * the victim block's own id here, so migration destinations are never
 * chosen inside the very block that's about to be erased.
 * ------------------------------------------------------------------- */
int wl_select_target_block(const ftl_t *ftl, int exclude_block) {
    int best_block = -1;
    uint32_t best_erase_count = 0xFFFFFFFF; /* start higher than any real count */

    for (int b = 0; b < NUM_BLOCKS; b++) {
        if (b == exclude_block) continue;
        if (ftl->blocks[b].free_pages <= 0) continue; /* no room here */

        if (ftl->blocks[b].erase_count < best_erase_count) {
            best_erase_count = ftl->blocks[b].erase_count;
            best_block = b;
        }
    }

    return best_block; /* -1 if literally nothing has a free page */
}

/* ---------------------------------------------------------------------
 * STRETCH GOAL (not implemented — for when you extend this yourself):
 * static wear leveling. Sketch of the idea:
 *   - Track how long it's been since each block's data last changed.
 *   - Periodically find a block with LOW erase_count AND mostly VALID,
 *     unchanged pages (i.e. "cold" data sitting untouched).
 *   - Force-migrate that cold data out (like GC does), which frees the
 *     block up to be erased and reused for hot, frequently-written data.
 *   - This prevents the scenario where a block holding e.g. a firmware
 *     image sits at erase_count=0 forever while every other block wears
 *     out around it.
 * ------------------------------------------------------------------- */