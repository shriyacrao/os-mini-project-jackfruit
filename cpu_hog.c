/*
 * cpu_hog.c — CPU-bound workload for scheduling experiments
 *
 * Runs a tight arithmetic loop for a specified number of seconds,
 * printing throughput every second.
 *
 * Usage: ./cpu_hog [seconds=30]
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[]) {
    int secs = (argc > 1) ? atoi(argv[1]) : 30;
    printf("cpu_hog: running for %d seconds (pid=%d)\n", secs, (int)getpid());

    time_t start = time(NULL);
    unsigned long long counter = 0;
    time_t last_report = start;

    while (1) {
        /* Tight arithmetic loop — CPU-bound */
        for (volatile unsigned long i = 0; i < 10000000UL; i++);
        counter++;

        time_t now = time(NULL);
        if (now >= start + secs) break;
        if (now > last_report) {
            printf("cpu_hog: %ld s elapsed, %llu iterations\n",
                   (long)(now - start), counter);
            fflush(stdout);
            last_report = now;
        }
    }
    printf("cpu_hog: done, %llu total iterations\n", counter);
    return 0;
}
