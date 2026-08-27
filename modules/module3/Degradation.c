#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ftl.h"

/* ---------------------------------------------------------------------
 * deg_compute_rber
 * ---------------------------------------------------------------------
 * Combines three multiplicative factors into one Raw Bit Error Rate:
 *
 *   wear_factor:      grows with erase_count. Each P/E cycle stresses
 *                      the oxide layer inside a flash cell a little
 *                      more permanently — this is cumulative damage,
 *                      not something that heals, so it only ever climbs.
 *
 *   retention_factor:  grows with how long it's been since the block was
 *                      last erased/rewritten. Charge stored in a flash
 *                      cell slowly leaks over time ("charge leakage") —
 *                      the longer data sits untouched, the more likely a
 *                      stored bit has drifted enough to misread.
 *
 *   temp_factor:       grows with temperature. Higher heat accelerates
 *                      charge leakage (basic semiconductor physics) —
 *                      modeled here as a simple exponential bump per °C
 *                      above baseline, in the spirit of (not a literal
 *                      copy of) real Arrhenius-style acceleration used
 *                      in reliability engineering.
 *
 * All three get multiplied against BASE_RBER (the error rate of a
 * perfectly fresh, just-erased, room-temperature block). This is a toy
 * formula tuned for THIS simulator's small scale — not a citation of
 * any specific vendor's real numbers — but the SHAPE of the curve
 * (roughly polynomial/exponential growth with wear and time) reflects
 * well-known, widely published trends in flash reliability research.
 * ------------------------------------------------------------------- */
double deg_compute_rber(uint32_t erase_count, uint64_t retention_ticks, int temp_c) {
    double wear_factor      = pow(1.0 + (double)erase_count / 40.0, 2.4);
    double retention_factor = pow(1.0 + (double)retention_ticks / 300.0, 1.2);
    double temp_factor      = pow(1.10, (double)(temp_c - 40));  /* +10% per °C above 40 */

    return BASE_RBER * wear_factor * retention_factor * temp_factor;
}

/* ---------------------------------------------------------------------
 * deg_simulate_bit_errors
 * ---------------------------------------------------------------------
 * A crude Monte Carlo bit-flip simulation: for every bit in a page,
 * roll a random number and compare it against rber. This is literally
 * a Binomial(BITS_PER_PAGE, rber) sample, done the slow-but-obvious way
 * (a loop) since BITS_PER_PAGE is tiny (256) in this toy simulator.
 *
 * Why simulate instead of just returning rber * BITS_PER_PAGE directly?
 * Because real error counts are NOISY — two pages with identical RBER
 * won't always have identical error counts. That noise is exactly what
 * makes this a realistic (if small) dataset for a model to learn from,
 * rather than a perfectly deterministic lookup table.
 * ------------------------------------------------------------------- */
int deg_simulate_bit_errors(double rber) {
    int errors = 0;
    for (int i = 0; i < BITS_PER_PAGE; i++) {
        double r = (double)rand() / (double)RAND_MAX;
        if (r < rber) errors++;
    }
    return errors;
}

/* ---------------------------------------------------------------------
 * CSV telemetry logging
 * ---------------------------------------------------------------------
 * Columns, in order:
 *   tick             - simulated time of this snapshot
 *   block_id         - which physical block this row describes
 *   erase_count      - P/E cycles so far (the main wear signal)
 *   valid_pages      - how much live data the block currently holds
 *   invalid_pages    - how much reclaimable garbage it currently holds
 *   retention_ticks  - time since this block was last erased
 *   temp_c           - simulated temperature at snapshot time
 *   rber             - computed raw bit error rate (the "ground truth" rate)
 *   error_bits       - simulated actual bit errors observed this snapshot
 *   uncorrectable    - 1 if error_bits exceeded what ECC could fix, else 0
 *
 * "uncorrectable" is your FAILURE LABEL for Module 4/5 — it's what a
 * classifier will eventually try to predict in advance.
 * ------------------------------------------------------------------- */
void deg_log_header(FILE *f) {
    fprintf(f, "tick,block_id,erase_count,valid_pages,invalid_pages,"
               "retention_ticks,temp_c,rber,error_bits,uncorrectable\n");
}

void deg_log_snapshot(FILE *f, ftl_t *ftl, int block_id) {
    block_t *blk = &ftl->blocks[block_id];

    uint64_t retention_ticks = ftl->sim_tick - blk->last_erase_tick;
    double   rber            = deg_compute_rber(blk->erase_count, retention_ticks, blk->sim_temp_c);
    int      error_bits      = deg_simulate_bit_errors(rber);
    int      uncorrectable   = (error_bits > ECC_CORRECTABLE_BITS) ? 1 : 0;

    fprintf(f, "%llu,%d,%u,%d,%d,%llu,%d,%.6f,%d,%d\n",
            (unsigned long long)ftl->sim_tick,
            block_id,
            blk->erase_count,
            blk->valid_pages,
            blk->invalid_pages,
            (unsigned long long)retention_ticks,
            blk->sim_temp_c,
            rber,
            error_bits,
            uncorrectable);
}