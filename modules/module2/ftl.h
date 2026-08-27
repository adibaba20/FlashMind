#ifndef FTL_H
#define FTL_H

#include <stdint.h>

/* ---------------------------------------------------------------------
 * MODULE 1: FTL CORE CONFIGURATION
 * ---------------------------------------------------------------------
 * Small numbers on purpose so you can print the ENTIRE SSD state to the
 * screen and trace every write by hand. Change these later once you're
 * comfortable with the flow.
 * ------------------------------------------------------------------- */
#define NUM_BLOCKS        6     /* bumped from 4 -> 6 for Module 2 so wear  */
#define PAGES_PER_BLOCK    6     /* leveling has room to actually show a    */
#define TOTAL_PAGES       (NUM_BLOCKS * PAGES_PER_BLOCK)  /* visible spread across blocks */
#define PAGE_DATA_SIZE     32    /* bytes of "data" a page can hold (toy)    */
#define INVALID_PPA       (-1)   /* sentinel: "no physical page mapped yet"  */

/* ---------------------------------------------------------------------
 * MODULE 2 ADDITIONS: garbage collection trigger threshold.
 * ---------------------------------------------------------------------
 * When total free pages across the WHOLE SSD drops below this number,
 * ftl_write() will run garbage collection BEFORE attempting the write.
 * Real SSDs do the same thing - GC runs proactively, not only when
 * completely full, so there's always some free space in reserve.
 * ------------------------------------------------------------------- */
#define GC_FREE_PAGE_THRESHOLD  PAGES_PER_BLOCK   /* keep at least ~1 block worth free */

/* ---------------------------------------------------------------------
 * Page state — every physical page is always in exactly one of these.
 * This is the core rule of NAND flash:
 *   FREE    -> can be written to directly
 *   VALID   -> holds the current, live data for some logical address
 *   INVALID -> held OLD data that has since been overwritten elsewhere;
 *              cannot be reused until the whole BLOCK is erased
 * ------------------------------------------------------------------- */
typedef enum {
    PAGE_FREE = 0,
    PAGE_VALID,
    PAGE_INVALID
} page_state_t;

/* One physical page */
typedef struct {
    page_state_t state;
    int32_t      lba;                   /* which logical address this page currently holds (-1 if none) */
    char         data[PAGE_DATA_SIZE];  /* toy payload                                                   */
} page_t;

/* One physical block = a fixed number of pages.
 * Blocks are the unit of ERASE. Pages are the unit of READ/WRITE.
 * This mismatch is the single most important fact in flash storage —
 * it's the reason FTL, garbage collection, and wear leveling all exist. */
typedef struct {
    page_t   pages[PAGES_PER_BLOCK];
    uint32_t erase_count;      /* how many P/E cycles this block has endured   */
    int      free_pages;       /* quick count, avoids rescanning every write   */
    int      valid_pages;
    int      invalid_pages;
} block_t;

/* The FTL itself:
 *  - mapping_table:  LBA -> PPA (Logical Page/Block Address -> Physical Page Address)
 *  - blocks:         the physical NAND array
 *  - next_write_ppa: extremely naive "where do I write next" pointer for Module 1
 *                     (Module 2 will replace this with real wear-leveling logic)
 */
typedef struct {
    block_t blocks[NUM_BLOCKS];
    int32_t mapping_table[TOTAL_PAGES];  /* index = LBA, value = PPA (physical page index) */
    int     next_write_ppa;              /* kept for reference; Module 2 no longer relies on this alone */
    uint32_t total_writes;               /* HOST writes: calls to ftl_write() by the "user"              */
    uint32_t total_reads;
    uint32_t total_erases;
    uint32_t total_physical_page_writes; /* HOST writes + GC migration copies -> used for write amplification */
    uint32_t total_gc_cycles;
} ftl_t;

/* Write amplification = total_physical_page_writes / total_writes.
 * 1.0 = perfect (every host write costs exactly one physical write).
 * Higher = GC is copying data around more than the host actually asked
 * for. This is a REAL metric SSD vendors report and optimize for. */
static inline double ftl_write_amplification(const ftl_t *ftl) {
    if (ftl->total_writes == 0) return 0.0;
    return (double)ftl->total_physical_page_writes / (double)ftl->total_writes;
}

/* Convert a physical page index (PPA) into (block_id, page_id_within_block) */
static inline int ppa_to_block(int ppa) { return ppa / PAGES_PER_BLOCK; }
static inline int ppa_to_page(int ppa)  { return ppa % PAGES_PER_BLOCK; }

/* ---------------------------------------------------------------------
 * Public API — Module 1
 * ------------------------------------------------------------------- */
void ftl_init(ftl_t *ftl);

/* Returns 0 on success, -1 if the SSD is completely full (no free page
 * anywhere) — Module 1 does NOT garbage collect yet, so this WILL happen
 * if you write more than TOTAL_PAGES times without erasing. That's
 * intentional: it's what motivates Module 2. */
int  ftl_write(ftl_t *ftl, int lba, const char *data);

/* Returns 0 on success + copies data into out_data, -1 if LBA was never written */
int  ftl_read(ftl_t *ftl, int lba, char *out_data);

/* Erases an entire block: every page -> FREE, erase_count++ */
int  ftl_erase_block(ftl_t *ftl, int block_id);

/* Debug helpers */
void ftl_print_state(const ftl_t *ftl);
void ftl_print_mapping_table(const ftl_t *ftl);
void ftl_print_wear_histogram(const ftl_t *ftl);

/* ---------------------------------------------------------------------
 * MODULE 2 ADDITIONS: wear leveling + garbage collection
 * ------------------------------------------------------------------- */

/* wear_leveling.c
 * Picks WHICH block a new page should be written into. Unlike Module 1's
 * "just take the next free page in order", this scans every block that
 * still has a free page and returns the one with the LOWEST erase_count.
 * exclude_block: pass -1 normally; GC passes the victim block's id so it
 * never tries to migrate data back into the block it's about to erase.
 * Returns block_id, or -1 if no block anywhere has a free page. */
int wl_select_target_block(const ftl_t *ftl, int exclude_block);

/* gc.c
 * Returns the block_id with the most INVALID pages (the best candidate
 * to reclaim), or -1 if no block has any invalid pages to clean up. */
int gc_select_victim_block(const ftl_t *ftl);

/* gc.c
 * Runs one full GC cycle on victim_block: copies every VALID page out to
 * free space elsewhere (via wl_select_target_block), updates the mapping
 * table so those LBAs now point to the new location, then erases the
 * now-empty victim block. Returns 0 on success, -1 if it couldn't find
 * anywhere to migrate valid data to (SSD truly full of live data). */
int gc_run(ftl_t *ftl, int victim_block);

/* gc.c
 * Called automatically at the start of ftl_write(). If free pages across
 * the whole SSD are below GC_FREE_PAGE_THRESHOLD, repeatedly runs GC on
 * the best victim block until either enough space is free again, or no
 * more victims exist (nothing left to reclaim). */
void gc_run_if_needed(ftl_t *ftl);

#endif /* FTL_H */