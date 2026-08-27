#include <stdio.h>
#include <string.h>
#include "ftl.h"

/* ---------------------------------------------------------------------
 * gc_select_victim_block
 * ---------------------------------------------------------------------
 * GC's job is to turn INVALID pages back into FREE pages. The only way
 * to do that is erase a whole block — so we want to pick the block
 * where erasing gives us the MOST free space back for the LEAST work.
 *
 * Strategy: pick the block with the most INVALID pages. That block has
 * the fewest VALID pages to migrate (least work) and frees up the most
 * space (most reward) — this is the standard "greedy" GC policy used
 * as a baseline in real FTL research.
 * ------------------------------------------------------------------- */
int gc_select_victim_block(const ftl_t *ftl) {
    int best_block = -1;
    int best_invalid_count = 0;

    for (int b = 0; b < NUM_BLOCKS; b++) {
        if (ftl->blocks[b].invalid_pages > best_invalid_count) {
            best_invalid_count = ftl->blocks[b].invalid_pages;
            best_block = b;
        }
    }

    return best_block; /* -1 if no block has any invalid pages to reclaim */
}

/* ---------------------------------------------------------------------
 * gc_run
 * ---------------------------------------------------------------------
 * Reclaims victim_block in 3 steps:
 *   1. For every VALID page in the victim, copy its data to a free page
 *      in a DIFFERENT block (chosen by the same wear-leveling logic
 *      normal writes use) and update the mapping table to point there.
 *   2. INVALID pages in the victim need no action — they're about to be
 *      erased anyway.
 *   3. Erase the victim block. Since step 1 already moved every VALID
 *      page out, ftl_erase_block() will find zero valid pages left and
 *      erase safely (no warning, no data loss).
 *
 * This is exactly what "garbage collection" means in flash storage:
 * copy the live ("valid") data out of the way, THEN reclaim the space.
 * ------------------------------------------------------------------- */
int gc_run(ftl_t *ftl, int victim_block) {
    block_t *victim = &ftl->blocks[victim_block];

    for (int p = 0; p < PAGES_PER_BLOCK; p++) {
        if (victim->pages[p].state != PAGE_VALID) continue;

        int lba = victim->pages[p].lba;

        int dest_block = wl_select_target_block(ftl, victim_block);
        if (dest_block == -1) {
            fprintf(stderr, "[GC] failed: no free page anywhere to migrate "
                            "LBA %d out of victim block %d — SSD is genuinely full "
                            "of live data\n", lba, victim_block);
            return -1;
        }

        int dest_page = -1;
        for (int dp = 0; dp < PAGES_PER_BLOCK; dp++) {
            if (ftl->blocks[dest_block].pages[dp].state == PAGE_FREE) {
                dest_page = dp;
                break;
            }
        }
        /* dest_page should always be found since wl_select_target_block
         * only returns blocks that have free_pages > 0 */

        /* Step 1: copy the data */
        memcpy(ftl->blocks[dest_block].pages[dest_page].data,
               victim->pages[p].data, PAGE_DATA_SIZE);
        ftl->blocks[dest_block].pages[dest_page].state = PAGE_VALID;
        ftl->blocks[dest_block].pages[dest_page].lba   = lba;
        ftl->blocks[dest_block].valid_pages++;
        ftl->blocks[dest_block].free_pages--;

        /* This copy is a PHYSICAL write the host never asked for directly
         * — this is exactly what write amplification measures. */
        ftl->total_physical_page_writes++;

        /* Update mapping table: this LBA now lives at the new location */
        ftl->mapping_table[lba] = dest_block * PAGES_PER_BLOCK + dest_page;

        /* Old page in the victim no longer matters — mark it invalid so
         * the block-level counters stay consistent right up until erase */
        victim->pages[p].state = PAGE_INVALID;
        victim->valid_pages--;
        victim->invalid_pages++;
    }

    /* Step 3: victim now has zero VALID pages -> safe, clean erase */
    ftl->total_gc_cycles++;
    return ftl_erase_block(ftl, victim_block);
}

/* ---------------------------------------------------------------------
 * gc_run_if_needed
 * ---------------------------------------------------------------------
 * Called at the top of every ftl_write(). Keeps reclaiming blocks (worst
 * victim first) until either:
 *   (a) free pages across the whole SSD are back above the threshold, or
 *   (b) there's no victim left to reclaim (nothing is INVALID anymore)
 * (b) is the honest "the SSD is truly full of live data" case — no
 * amount of GC can help that; the next write will simply fail, and in
 * a real system this is when the drive reports "disk full" to the OS.
 * ------------------------------------------------------------------- */
void gc_run_if_needed(ftl_t *ftl) {
    int total_free = 0;
    for (int b = 0; b < NUM_BLOCKS; b++) {
        total_free += ftl->blocks[b].free_pages;
    }

    while (total_free < GC_FREE_PAGE_THRESHOLD) {
        int victim = gc_select_victim_block(ftl);
        if (victim == -1) {
            /* nothing left to reclaim — genuinely full of live data */
            return;
        }

        if (gc_run(ftl, victim) != 0) {
            return; /* GC itself failed (no destination space) — give up */
        }

        total_free = 0;
        for (int b = 0; b < NUM_BLOCKS; b++) {
            total_free += ftl->blocks[b].free_pages;
        }
    }
}