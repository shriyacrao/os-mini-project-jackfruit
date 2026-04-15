#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 0xCE

/* Register a container PID with soft and hard memory limits (in bytes) */
struct container_reg {
    pid_t  pid;
    unsigned long soft_limit;  /* bytes - warn when exceeded */
    unsigned long hard_limit;  /* bytes - kill when exceeded  */
    char   name[64];           /* human-readable container name */
};

/* Unregister a container by PID */
struct container_unreg {
    pid_t pid;
};

/* Query current RSS for a registered PID (filled by kernel) */
struct container_query {
    pid_t  pid;
    unsigned long rss_bytes;
    int    soft_exceeded;
    int    hard_exceeded;
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_reg)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct container_unreg)
#define MONITOR_QUERY      _IOWR(MONITOR_MAGIC, 3, struct container_query)

#endif /* MONITOR_IOCTL_H */
