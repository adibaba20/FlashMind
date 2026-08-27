#include <stdio.h>
#include "ftl.h"

/* ---------------------------------------------------------------------
 * Module 1 demo driver.
 *
 * This walks through 5 scenes, each printing the SSD state so you can
 * trace exactly what happened after every operation. Read the printed
 * output slowly — that IS the learning material.
 * ------------------------------------------------------------------- */
int main(void) {
    ftl_t ftl;
    char buf[PAGE_DATA_SIZE];

    ftl_init(&ftl);

    printf("############################################\n");
    printf("# SCENE 1: fresh SSD, write LBA 0..3        #\n");
    printf("############################################\n");
    ftl_write(&ftl, 0, "hello-A");
    ftl_write(&ftl, 1, "hello-B");
    ftl_write(&ftl, 2, "hello-C");
    ftl_write(&ftl, 3, "hello-D");
    ftl_print_state(&ftl);
    ftl_print_mapping_table(&ftl);

    printf("\n############################################\n");
    printf("# SCENE 2: overwrite LBA 0 -> old page      #\n");
    printf("#          becomes INVALID (not reused!)    #\n");
    printf("############################################\n");
    ftl_write(&ftl, 0, "hello-A-v2");
    ftl_print_state(&ftl);
    ftl_print_mapping_table(&ftl);

    printf("\n############################################\n");
    printf("# SCENE 3: read back LBA 0 -> should return #\n");
    printf("#          the NEW value, not the old one   #\n");
    printf("############################################\n");
    if (ftl_read(&ftl, 0, buf) == 0) {
        printf("Read LBA 0 -> \"%s\"\n", buf);
    }

    printf("\n############################################\n");
    printf("# SCENE 4: keep writing until the SSD is    #\n");
    printf("#          completely full (no GC exists    #\n");
    printf("#          yet in Module 1 -> this WILL fail)#\n");
    printf("############################################\n");
    for (int lba = 4; lba < TOTAL_PAGES + 2; lba++) {
        int result = ftl_write(&ftl, lba % TOTAL_PAGES, "filler-data");
        printf("write LBA %d -> %s\n", lba % TOTAL_PAGES,
               result == 0 ? "OK" : "FAILED");
    }
    ftl_print_state(&ftl);

    printf("\n############################################\n");
    printf("# SCENE 5: erase block 0 to reclaim space,  #\n");
    printf("#          then write successfully again    #\n");
    printf("############################################\n");
    ftl_erase_block(&ftl, 0);
    ftl_print_state(&ftl);

    int result = ftl_write(&ftl, 0, "reborn-page");
    printf("\nwrite LBA 0 after erase -> %s\n", result == 0 ? "OK" : "FAILED");
    ftl_print_state(&ftl);
    ftl_print_mapping_table(&ftl);

    printf("\nDone. This is the entire Module 1 story:\n");
    printf(" - NAND can't overwrite in place -> every write finds a FREE page\n");
    printf(" - old data becomes INVALID, not deleted, until block erase\n");
    printf(" - the SSD fills up permanently without erase/GC -> motivates Module 2\n");
    printf(" - erase is the only way to get FREE pages back, and it's coarse\n");
    printf("   (whole block at a time) -> this is why wear leveling matters\n");

    return 0;
}