/*
 * memory_hog.c — Memory-consuming workload for kernel monitor testing
 *
 * Gradually allocates memory in MB-sized chunks, touching each page
 * so the allocation shows up in RSS.
 *
 * Usage: ./memory_hog [total_mb=128] [step_mb=8] [sleep_ms=500]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    long total_mb = (argc > 1) ? atol(argv[1]) : 128;
    long step_mb  = (argc > 2) ? atol(argv[2]) : 8;
    long sleep_ms = (argc > 3) ? atol(argv[3]) : 500;

    printf("memory_hog: will allocate up to %ld MB in %ld MB steps (pid=%d)\n",
           total_mb, step_mb, (int)getpid());
    fflush(stdout);

    long allocated_mb = 0;
    while (allocated_mb < total_mb) {
        size_t chunk = (size_t)step_mb * 1024 * 1024;
        char *p = malloc(chunk);
        if (!p) {
            printf("memory_hog: malloc failed at %ld MB\n", allocated_mb);
            break;
        }
        /* Touch every page so it counts as RSS */
        memset(p, 0x55, chunk);
        allocated_mb += step_mb;

        printf("memory_hog: allocated %ld MB so far\n", allocated_mb);
        fflush(stdout);

        usleep((useconds_t)(sleep_ms * 1000));
    }

    printf("memory_hog: holding %ld MB — sleeping 60s\n", allocated_mb);
    fflush(stdout);
    sleep(60);
    return 0;
}
