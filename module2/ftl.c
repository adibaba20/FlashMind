#include <stdio.h>
#include <string.h>
#include "ftl.h"

/* ---------------------------------------------------------------------
 * ftl_init
 * ---------------------------------------------------------------------
 * Sets every page to FREE, every mapping entry to "unmapped", and resets
 * all counters. This is the state of a brand new, blank SSD.
 * ------------------------------------------------------------------- */
void ftl_init(ftl_t *ftl) {
    memset(ftl, 0, sizeof(ftl_t));

    for (int b = 0; b < NUM_BLOCKS; b++) {
        ftl->blocks[b].erase_count  = 0;
        ftl->blocks[b].free_pages   = PAGES_PER_BLOCK;
        ftl->blocks[b].valid_pages  = 0;
        ftl->blocks[b].invalid_pages = 0;
        for (int p = 0; p < PAGES_PER_BLOCK; p++) {
            ftl->blocks[b].pages[p].state = PAGE_FREE;
            ftl->blocks[b].pages[p].lba   = -1;
        }
    }

    for (int i = 0; i < TOTAL_PAGES; i++) {
        ftl->mapping_table[i] = INVALID_PPA;
    }

    ftl->next_write_ppa = 0;
}

/* ---------------------------------------------------------------------
 * find_next_free_ppa (internal helper) — MODULE 2 VERSION
 * ---------------------------------------------------------------------
 * Module 1's version scanned physical pages in raw order — "dumb" on
 * purpose, and it eventually got the SSD permanently stuck once full.
 *
 * Module 2 replaces the block choice with wl_select_target_block(),
 * which picks the LEAST-WORN block that still has a free page (dynamic
 * wear leveling). Within that block we still just take the first free
 * page — page choice within a block doesn't matter for wear, only which
 * BLOCK gets erased more often matters, since erase_count lives at the
 * block level, not the page level.
 * ------------------------------------------------------------------- */
static int find_next_free_ppa(ftl_t *ftl) {
    int block_id = wl_select_target_block(ftl, -1);
    if (block_id == -1) {
        return -1; /* no block anywhere has a free page */
    }
    for (int p = 0; p < PAGES_PER_BLOCK; p++) {
        if (ftl->blocks[block_id].pages[p].state == PAGE_FREE) {
            return block_id * PAGES_PER_BLOCK + p;
        }
    }
    return -1; /* shouldn't happen if free_pages accounting is correct */
}

/* ---------------------------------------------------------------------
 * ftl_write
 * ---------------------------------------------------------------------
 * The core "write" flow every real SSD follows:
 *   1. Find a FREE physical page somewhere (NAND can't overwrite in place)
 *   2. Write the new data there, mark it VALID
 *   3. If this LBA already pointed somewhere, mark the OLD page INVALID
 *      (its data is now stale garbage — it stays that way until the
 *      whole block gets erased later)
 *   4. Update the mapping table so future reads of this LBA find the
 *      NEW physical page
 * ------------------------------------------------------------------- */
int ftl_write(ftl_t *ftl, int lba, const char *data) {
    if (lba < 0 || lba >= TOTAL_PAGES) {
        fprintf(stderr, "[FTL] write error: LBA %d out of range\n", lba);
        return -1;
    }

    /* MODULE 2: proactively reclaim space BEFORE we're forced to, same
     * as real SSD controllers do — GC runs in the background well before
     * the drive is truly 100% full. */
    gc_run_if_needed(ftl);

    int new_ppa = find_next_free_ppa(ftl);
    if (new_ppa == -1) {
        fprintf(stderr, "[FTL] write error: SSD full even after GC — "
                        "every page holds live data, nothing left to reclaim\n");
        return -1;
    }

    int nb = ppa_to_block(new_ppa);
    int np = ppa_to_page(new_ppa);

    /* Step 2: write data into the new page */
    strncpy(ftl->blocks[nb].pages[np].data, data, PAGE_DATA_SIZE - 1);
    ftl->blocks[nb].pages[np].data[PAGE_DATA_SIZE - 1] = '\0';
    ftl->blocks[nb].pages[np].state = PAGE_VALID;
    ftl->blocks[nb].pages[np].lba   = lba;
    ftl->blocks[nb].valid_pages++;
    ftl->blocks[nb].free_pages--;

    /* Step 3: invalidate the old physical page for this LBA, if any existed */
    int old_ppa = ftl->mapping_table[lba];
    if (old_ppa != INVALID_PPA) {
        int ob = ppa_to_block(old_ppa);
        int op = ppa_to_page(old_ppa);
        ftl->blocks[ob].pages[op].state = PAGE_INVALID;
        ftl->blocks[ob].valid_pages--;
        ftl->blocks[ob].invalid_pages++;
    }

    /* Step 4: update mapping table (this IS the "translation" in FTL) */
    ftl->mapping_table[lba] = new_ppa;

    ftl->next_write_ppa = (new_ppa + 1) % TOTAL_PAGES;
    ftl->total_writes++;
    ftl->total_physical_page_writes++;  /* this host write cost exactly 1 physical write */

    return 0;
}

/* ---------------------------------------------------------------------
 * ftl_read
 * ---------------------------------------------------------------------
 * Pure lookup: LBA -> mapping table -> PPA -> physical page data.
 * This is the "fast path" every SSD is built to make efficient.
 * ------------------------------------------------------------------- */
int ftl_read(ftl_t *ftl, int lba, char *out_data) {
    if (lba < 0 || lba >= TOTAL_PAGES) {
        fprintf(stderr, "[FTL] read error: LBA %d out of range\n", lba);
        return -1;
    }

    int ppa = ftl->mapping_table[lba];
    if (ppa == INVALID_PPA) {
        fprintf(stderr, "[FTL] read error: LBA %d was never written\n", lba);
        return -1;
    }

    int b = ppa_to_block(ppa);
    int p = ppa_to_page(ppa);
    strncpy(out_data, ftl->blocks[b].pages[p].data, PAGE_DATA_SIZE);

    ftl->total_reads++;
    return 0;
}

/* ---------------------------------------------------------------------
 * ftl_erase_block
 * ---------------------------------------------------------------------
 * The ONLY way invalid pages ever become reusable: erase wipes the whole
 * block back to FREE and bumps erase_count (this is one "P/E cycle" for
 * every page in the block — the thing that eventually wears NAND out).
 *
 * NOTE: this is a low-level operation. In Module 1 you'll call it
 * manually. In Module 2, garbage collection will decide WHEN and WHICH
 * block to erase automatically — and it must first copy out any VALID
 * pages before erasing, or that data would be lost. This function does
 * NOT do that copy-out step; it's intentionally "raw" so the danger of
 * erasing a block with live data in it is obvious.
 * ------------------------------------------------------------------- */
int ftl_erase_block(ftl_t *ftl, int block_id) {
    if (block_id < 0 || block_id >= NUM_BLOCKS) {
        fprintf(stderr, "[FTL] erase error: block %d out of range\n", block_id);
        return -1;
    }

    block_t *blk = &ftl->blocks[block_id];

    if (blk->valid_pages > 0) {
        fprintf(stderr, "[FTL] warning: erasing block %d while it still has "
                        "%d VALID page(s) — that data will be LOST. "
                        "(Module 2's garbage collector will prevent this.)\n",
                        block_id, blk->valid_pages);
        /* We still allow it here on purpose, so you can see the danger.
         * We must also clear the mapping_table entries pointing here, or
         * ftl_read would return stale success on now-erased data. */
        for (int p = 0; p < PAGES_PER_BLOCK; p++) {
            if (blk->pages[p].state == PAGE_VALID) {
                ftl->mapping_table[blk->pages[p].lba] = INVALID_PPA;
            }
        }
    }

    for (int p = 0; p < PAGES_PER_BLOCK; p++) {
        blk->pages[p].state = PAGE_FREE;
        blk->pages[p].lba   = -1;
        memset(blk->pages[p].data, 0, PAGE_DATA_SIZE);
    }

    blk->free_pages    = PAGES_PER_BLOCK;
    blk->valid_pages    = 0;
    blk->invalid_pages  = 0;
    blk->erase_count++;

    ftl->total_erases++;
    return 0;
}

/* ---------------------------------------------------------------------
 * Debug / visualization helpers
 * ------------------------------------------------------------------- */
static char state_char(page_state_t s) {
    switch (s) {
        case PAGE_FREE:    return '.';
        case PAGE_VALID:   return 'V';
        case PAGE_INVALID: return 'X';
    }
    return '?';
}

void ftl_print_state(const ftl_t *ftl) {
    printf("\n=== SSD physical state (%d blocks x %d pages) ===\n",
           NUM_BLOCKS, PAGES_PER_BLOCK);
    printf("Legend: . = FREE   V = VALID   X = INVALID\n\n");

    for (int b = 0; b < NUM_BLOCKS; b++) {
        printf("Block %d [erase_count=%u]: ", b, ftl->blocks[b].erase_count);
        for (int p = 0; p < PAGES_PER_BLOCK; p++) {
            printf("%c ", state_char(ftl->blocks[b].pages[p].state));
        }
        printf(" (free=%d valid=%d invalid=%d)\n",
               ftl->blocks[b].free_pages,
               ftl->blocks[b].valid_pages,
               ftl->blocks[b].invalid_pages);
    }

    printf("\nTotals: writes=%u reads=%u erases=%u\n",
           ftl->total_writes, ftl->total_reads, ftl->total_erases);
}

void ftl_print_wear_histogram(const ftl_t *ftl) {
    printf("\n=== Erase count per block (wear leveling check) ===\n");
    uint32_t min_e = ftl->blocks[0].erase_count, max_e = ftl->blocks[0].erase_count;
    for (int b = 0; b < NUM_BLOCKS; b++) {
        uint32_t e = ftl->blocks[b].erase_count;
        if (e < min_e) min_e = e;
        if (e > max_e) max_e = e;
        printf("Block %d: erase_count=%2u  ", b, e);
        for (uint32_t i = 0; i < e; i++) printf("#");
        printf("\n");
    }
    printf("min=%u  max=%u  spread=%u  ", min_e, max_e, max_e - min_e);
    printf("%s\n", (max_e - min_e <= 1) ? "(well balanced)" : "(uneven — investigate)");
    printf("Write amplification: %.2fx  (physical writes / host writes = %u / %u)\n",
           ftl_write_amplification(ftl), ftl->total_physical_page_writes, ftl->total_writes);
    printf("GC cycles run: %u\n", ftl->total_gc_cycles);
}

void ftl_print_mapping_table(const ftl_t *ftl) {
    printf("\n=== Mapping table (LBA -> PPA [block,page]) ===\n");
    for (int lba = 0; lba < TOTAL_PAGES; lba++) {
        int ppa = ftl->mapping_table[lba];
        if (ppa == INVALID_PPA) {
            printf("LBA %2d -> (unmapped)\n", lba);
        } else {
            printf("LBA %2d -> PPA %2d [block %d, page %d]\n",
                   lba, ppa, ppa_to_block(ppa), ppa_to_page(ppa));
        }
    }
}