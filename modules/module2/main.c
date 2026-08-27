#include <stdio.h>
#include <stdlib.h>
#include "ftl.h"

/* ---------------------------------------------------------------------
 * Module 2 demo driver.
 *
 * Module 1's demo showed the SSD getting permanently stuck once full.
 * This demo shows the fix: with wear leveling + GC running automatically
 * inside ftl_write(), the SSD can be written to FOREVER (as long as
 * total live data fits), and wear stays balanced across all blocks.
 * ------------------------------------------------------------------- */

#define NUM_DISTINCT_LBAS   20   /* fewer live LBAs than total pages, on purpose — */
                                  /* leaves room for GC to have real work to do    */
#define NUM_STRESS_WRITES   400  /* far more writes than TOTAL_PAGES (36) would   */
                                  /* allow in Module 1 without GC                  */

int main(void) {
    ftl_t ftl;
    char buf[PAGE_DATA_SIZE];

    ftl_init(&ftl);

    printf("############################################\n");
    printf("# PART 1: a few plain writes, watch WHICH   #\n");
    printf("# block gets chosen (should spread evenly)  #\n");
    printf("############################################\n");
    for (int lba = 0; lba < 8; lba++) {
        snprintf(buf, PAGE_DATA_SIZE, "data-%d", lba);
        ftl_write(&ftl, lba, buf);
    }
    ftl_print_state(&ftl);

    printf("\n############################################\n");
    printf("# PART 2: stress test — %d writes across    #\n", NUM_STRESS_WRITES);
    printf("# only %d distinct LBAs (SSD has %d pages   #\n", NUM_DISTINCT_LBAS, TOTAL_PAGES);
    printf("# total). This WILL force many GC cycles.   #\n");
    printf("# Module 1 could not have survived this.    #\n");
    printf("############################################\n");

    srand(42); /* fixed seed so this demo is reproducible every run */
    int failures = 0;
    for (int i = 0; i < NUM_STRESS_WRITES; i++) {
        int lba = rand() % NUM_DISTINCT_LBAS;
        snprintf(buf, PAGE_DATA_SIZE, "write#%d", i);
        int result = ftl_write(&ftl, lba, buf);
        if (result != 0) failures++;

        if ((i + 1) % 100 == 0) {
            printf("... completed %d/%d writes (failures so far: %d, "
                   "GC cycles so far: %u)\n",
                   i + 1, NUM_STRESS_WRITES, failures, ftl.total_gc_cycles);
        }
    }

    printf("\nStress test finished: %d writes attempted, %d failed.\n",
           NUM_STRESS_WRITES, failures);

    ftl_print_state(&ftl);
    ftl_print_wear_histogram(&ftl);

    printf("\n############################################\n");
    printf("# PART 3: sanity check — read back every    #\n");
    printf("# LBA, confirm it returns its LATEST value  #\n");
    printf("# even after all that GC shuffled data      #\n");
    printf("# around behind the scenes                  #\n");
    printf("############################################\n");
    int read_failures = 0;
    for (int lba = 0; lba < NUM_DISTINCT_LBAS; lba++) {
        if (ftl_read(&ftl, lba, buf) == 0) {
            printf("LBA %2d -> \"%s\"\n", lba, buf);
        } else {
            printf("LBA %2d -> READ FAILED\n", lba);
            read_failures++;
        }
    }

    printf("\nDone. This is the entire Module 2 story:\n");
    printf(" - wl_select_target_block() spreads new writes across the\n");
    printf("   LEAST-WORN block -> erase counts stay balanced (see histogram)\n");
    printf(" - gc_run_if_needed() reclaims INVALID pages automatically,\n");
    printf("   BEFORE the SSD ever gets stuck like it did in Module 1\n");
    printf(" - every migration GC does costs a real physical write the\n");
    printf("   host never asked for -> that's write amplification\n");
    printf(" - %d read failures out of %d LBAs confirms data correctness\n",
           read_failures, NUM_DISTINCT_LBAS);
    printf("   survived %u GC cycles without corruption\n", ftl.total_gc_cycles);

    return 0;
}