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
#define NUM_BLOCKS        4     /* total blocks in our tiny simulated SSD   */
#define PAGES_PER_BLOCK    4     /* pages inside each block                 */
#define TOTAL_PAGES       (NUM_BLOCKS * PAGES_PER_BLOCK)
#define PAGE_DATA_SIZE     32    /* bytes of "data" a page can hold (toy)    */
#define INVALID_PPA       (-1)   /* sentinel: "no physical page mapped yet"  */

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
    int     next_write_ppa;
    uint32_t total_writes;
    uint32_t total_reads;
    uint32_t total_erases;
} ftl_t;

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

#endif /* FTL_H */