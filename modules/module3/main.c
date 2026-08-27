#include <stdio.h>
#include <stdlib.h>
#include "ftl.h"

/* ---------------------------------------------------------------------
 * Module 3 demo driver.
 *
 * Goal: generate a realistic-looking telemetry.csv by running the SSD
 * through heavy, sustained write pressure (much more than Module 2's
 * demo) so blocks accumulate real wear, and periodically "sensor scan"
 * every block's estimated health into the CSV log.
 *
 * This CSV is literally your Module 4/5 dataset — everything downstream
 * (feature engineering, RUL regression, failure classification) reads
 * from the file this program produces.
 * ------------------------------------------------------------------- */

#define NUM_DISTINCT_LBAS     15     /* small working set -> concentrated wear on fewer blocks */
#define NUM_STRESS_WRITES   6000     /* enough writes to push erase_count well into the        */
                                      /* hundreds, so RBER growth is actually visible            */
#define SNAPSHOT_INTERVAL     50     /* log a telemetry row for every block every N writes      */

int main(void) {
    ftl_t ftl;
    char buf[PAGE_DATA_SIZE];

    ftl_init(&ftl);
    srand(7); /* fixed seed -> reproducible telemetry.csv every run */

    FILE *csv = fopen("telemetry.csv", "w");
    if (!csv) {
        fprintf(stderr, "could not open telemetry.csv for writing\n");
        return 1;
    }
    deg_log_header(csv);

    printf("############################################\n");
    printf("# Running %d writes across %d distinct LBAs #\n", NUM_STRESS_WRITES, NUM_DISTINCT_LBAS);
    printf("# on a %d-block x %d-page SSD, logging a     #\n", NUM_BLOCKS, PAGES_PER_BLOCK);
    printf("# telemetry snapshot every %d writes.        #\n", SNAPSHOT_INTERVAL);
    printf("############################################\n\n");

    int write_failures = 0;
    long total_uncorrectable_events = 0;
    long total_snapshots = 0;

    for (int i = 0; i < NUM_STRESS_WRITES; i++) {
        int lba = rand() % NUM_DISTINCT_LBAS;
        snprintf(buf, PAGE_DATA_SIZE, "w#%d", i);
        if (ftl_write(&ftl, lba, buf) != 0) write_failures++;

        if ((i + 1) % SNAPSHOT_INTERVAL == 0) {
            for (int b = 0; b < NUM_BLOCKS; b++) {
                deg_log_snapshot(csv, &ftl, b);
                total_snapshots++;
            }
        }

        if ((i + 1) % 1000 == 0) {
            printf("... %d/%d writes done (erase cycles so far: %u, GC cycles: %u)\n",
                   i + 1, NUM_STRESS_WRITES, ftl.total_erases, ftl.total_gc_cycles);
        }
    }
    fclose(csv);

    printf("\nSimulation complete. %d write failures, %u total erases, "
           "%u GC cycles.\n", write_failures, ftl.total_erases, ftl.total_gc_cycles);

    ftl_print_wear_histogram(&ftl);

    /* Re-open the CSV read-only just to report a few summary stats back
     * to the terminal, so you can sanity-check it without leaving the
     * program or opening the file yourself. */
    csv = fopen("telemetry.csv", "r");
    if (csv) {
        char line[256];
        long row_count = 0;
        double max_rber_seen = 0.0;
        int max_rber_block = -1;
        uint32_t max_rber_erase_count = 0;

        fgets(line, sizeof(line), csv); /* skip header */
        while (fgets(line, sizeof(line), csv)) {
            row_count++;
            unsigned long long tick, retention_ticks;
            int block_id, valid_pages, invalid_pages, temp_c, error_bits, uncorrectable;
            unsigned int erase_count;
            double rber;

            int fields = sscanf(line, "%llu,%d,%u,%d,%d,%llu,%d,%lf,%d,%d",
                   &tick, &block_id, &erase_count, &valid_pages, &invalid_pages,
                   &retention_ticks, &temp_c, &rber, &error_bits, &uncorrectable);

            if (fields == 10) {
                if (uncorrectable) total_uncorrectable_events++;
                if (rber > max_rber_seen) {
                    max_rber_seen = rber;
                    max_rber_block = block_id;
                    max_rber_erase_count = erase_count;
                }
            }
        }
        fclose(csv);

        printf("\n=== telemetry.csv summary ===\n");
        printf("Rows written: %ld (%ld snapshot events x %d blocks)\n",
               row_count, total_snapshots / NUM_BLOCKS, NUM_BLOCKS);
        printf("Uncorrectable error events observed: %ld / %ld snapshots (%.2f%%)\n",
               total_uncorrectable_events, row_count,
               row_count > 0 ? 100.0 * total_uncorrectable_events / row_count : 0.0);
        printf("Highest RBER seen: %.6f on block %d at erase_count=%u\n",
               max_rber_seen, max_rber_block, max_rber_erase_count);
    }

    printf("\nDone. This is the entire Module 3 story:\n");
    printf(" - deg_compute_rber() models error rate growing with wear,\n");
    printf("   retention time, and temperature — same shape as real NAND\n");
    printf(" - deg_simulate_bit_errors() turns that rate into a noisy,\n");
    printf("   realistic error COUNT per snapshot, not a flat number\n");
    printf(" - every snapshot is one row in telemetry.csv — this file IS\n");
    printf("   your Module 4/5 machine learning dataset\n");
    printf(" - 'uncorrectable' column is your failure label: Module 5's\n");
    printf("   model will learn to PREDICT this before it happens\n");

    return 0;
}