/*
 * io_pulse.c — I/O-bound workload for scheduling experiments
 *
 * Repeatedly writes and reads a temporary file to simulate I/O pressure.
 *
 * Usage: ./io_pulse [seconds=30]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define BUF_SZ (64 * 1024)  /* 64 KB chunks */

int main(int argc, char *argv[]) {
    int secs = (argc > 1) ? atoi(argv[1]) : 30;
    printf("io_pulse: running for %d seconds (pid=%d)\n", secs, (int)getpid());

    char *buf = malloc(BUF_SZ);
    memset(buf, 0xAB, BUF_SZ);

    time_t start = time(NULL);
    unsigned long long ops = 0;
    time_t last_report = start;

    while (1) {
        int fd = open("/tmp/io_pulse_tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) { perror("open"); break; }
        for (int i = 0; i < 16; i++) write(fd, buf, BUF_SZ);  /* 1 MB */
        fsync(fd);
        close(fd);

        fd = open("/tmp/io_pulse_tmp", O_RDONLY);
        if (fd >= 0) {
            while (read(fd, buf, BUF_SZ) > 0) {}
            close(fd);
        }
        ops++;

        time_t now = time(NULL);
        if (now >= start + secs) break;
        if (now > last_report) {
            printf("io_pulse: %ld s elapsed, %llu ops\n",
                   (long)(now - start), ops);
            fflush(stdout);
            last_report = now;
        }
    }

    unlink("/tmp/io_pulse_tmp");
    free(buf);
    printf("io_pulse: done, %llu total ops\n", ops);
    return 0;
}
