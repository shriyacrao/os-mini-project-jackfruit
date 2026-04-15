// monitor.c — Kernel-space memory monitor (LKM)
// Tracks container processes, enforces soft/hard RSS limits via ioctl.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/pid.h>
#include <linux/jiffies.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Container Memory Monitor LKM");
MODULE_VERSION("1.0");

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_MS 2000   /* Poll every 2 seconds */

/* Per-container tracking entry */
struct container_entry {
    struct list_head list;
    pid_t            pid;
    unsigned long    soft_limit;
    unsigned long    hard_limit;
    char             name[64];
    int              soft_warned;   /* have we already warned for soft limit? */
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_mutex);
static struct timer_list check_timer;

/* ---- RSS helper ---------------------------------------------------------- */

static unsigned long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    unsigned long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm)
        rss = get_mm_rss(task->mm) << PAGE_SHIFT;
    rcu_read_unlock();
    return rss;
}

/* ---- Periodic timer callback --------------------------------------------- */

static void check_containers(struct timer_list *t)
{
    struct container_entry *entry, *tmp;

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        struct task_struct *task;
        unsigned long rss;

        /* Remove stale entries for dead processes */
        rcu_read_lock();
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
        rcu_read_unlock();

        if (!task) {
            pr_info("container_monitor: [%s pid=%d] exited, removing\n",
                    entry->name, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        rss = get_rss_bytes(entry->pid);

        /* Hard limit: kill */
        if (entry->hard_limit && rss > entry->hard_limit) {
            pr_warn("container_monitor: [%s pid=%d] HARD LIMIT exceeded "
                    "(%lu > %lu bytes) — sending SIGKILL\n",
                    entry->name, entry->pid, rss, entry->hard_limit);
            rcu_read_lock();
            task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
            if (task)
                send_sig(SIGKILL, task, 0);
            rcu_read_unlock();
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit: warn once */
        if (entry->soft_limit && rss > entry->soft_limit && !entry->soft_warned) {
            pr_warn("container_monitor: [%s pid=%d] SOFT LIMIT exceeded "
                    "(%lu > %lu bytes) — warning\n",
                    entry->name, entry->pid, rss, entry->soft_limit);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&list_mutex);

    /* Reschedule */
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ---- ioctl handler -------------------------------------------------------- */

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case MONITOR_REGISTER: {
        struct container_reg reg;
        struct container_entry *entry;

        if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
            return -EFAULT;

        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid        = reg.pid;
        entry->soft_limit = reg.soft_limit;
        entry->hard_limit = reg.hard_limit;
        entry->soft_warned = 0;
        strncpy(entry->name, reg.name, sizeof(entry->name) - 1);

        mutex_lock(&list_mutex);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&list_mutex);

        pr_info("container_monitor: registered [%s pid=%d] soft=%lu hard=%lu\n",
                entry->name, entry->pid, entry->soft_limit, entry->hard_limit);
        return 0;
    }

    case MONITOR_UNREGISTER: {
        struct container_unreg unreg;
        struct container_entry *entry, *tmp;

        if (copy_from_user(&unreg, (void __user *)arg, sizeof(unreg)))
            return -EFAULT;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == unreg.pid) {
                pr_info("container_monitor: unregistered [%s pid=%d]\n",
                        entry->name, entry->pid);
                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }
        mutex_unlock(&list_mutex);
        return 0;
    }

    case MONITOR_QUERY: {
        struct container_query q;
        struct container_entry *entry;
        int found = 0;

        if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
            return -EFAULT;

        mutex_lock(&list_mutex);
        list_for_each_entry(entry, &container_list, list) {
            if (entry->pid == q.pid) {
                q.rss_bytes     = get_rss_bytes(q.pid);
                q.soft_exceeded = entry->soft_limit && q.rss_bytes > entry->soft_limit;
                q.hard_exceeded = entry->hard_limit && q.rss_bytes > entry->hard_limit;
                found = 1;
                break;
            }
        }
        mutex_unlock(&list_mutex);

        if (!found)
            return -ESRCH;

        if (copy_to_user((void __user *)arg, &q, sizeof(q)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

/* ---- file operations ------------------------------------------------------ */

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static struct miscdevice monitor_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &monitor_fops,
    .mode  = 0666,
};

/* ---- Module init / exit --------------------------------------------------- */

static int __init monitor_init(void)
{
    int ret = misc_register(&monitor_dev);
    if (ret) {
        pr_err("container_monitor: failed to register device (%d)\n", ret);
        return ret;
    }

    timer_setup(&check_timer, check_containers, 0);
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    pr_info("container_monitor: loaded — /dev/%s ready\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_entry *entry, *tmp;

    del_timer_sync(&check_timer);

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    misc_deregister(&monitor_dev);
    pr_info("container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
