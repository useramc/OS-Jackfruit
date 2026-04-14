#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

// ---------------- NODE ----------------
struct monitor_node {
    char container_id[50];
    pid_t pid;

    unsigned long soft_limit;
    unsigned long hard_limit;

    int soft_triggered;

    struct list_head list;
};

// ---------------- GLOBALS ----------------
static LIST_HEAD(monitor_list);
static DEFINE_SPINLOCK(list_lock);
static struct timer_list monitor_timer;

// ---------------- RSS CALC ----------------
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss = -1;

    rcu_read_lock();

    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }

    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return -1;

    rss = get_mm_rss(mm) * PAGE_SIZE;
    mmput(mm);

    return rss;
}

// ---------------- TIMER ----------------
static void monitor_fn(struct timer_list *t)
{
    struct monitor_node *n, *tmp;

    spin_lock(&list_lock);

    list_for_each_entry_safe(n, tmp, &monitor_list, list) {

        long rss = get_rss_bytes(n->pid);

        // process died → remove
        if (rss < 0) {
            list_del(&n->list);
            kfree(n);
            continue;
        }

        // soft limit (log once)
        if (!n->soft_triggered && rss > n->soft_limit) {
            printk(KERN_WARNING
                "[container_monitor] SOFT LIMIT: %s pid=%d rss=%ld\n",
                n->container_id, n->pid, rss);

            n->soft_triggered = 1;
        }

        // hard limit (kill + remove)
        if (rss > n->hard_limit) {

            struct task_struct *task = pid_task(find_vpid(n->pid), PIDTYPE_PID);
            if (task)
                send_sig(SIGKILL, task, 1);

            printk(KERN_WARNING
                "[container_monitor] HARD LIMIT: %s pid=%d rss=%ld\n",
                n->container_id, n->pid, rss);

            list_del(&n->list);
            kfree(n);
        }
    }

    spin_unlock(&list_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

// ---------------- IOCTL ----------------
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {

        struct monitor_node *n = kmalloc(sizeof(*n), GFP_KERNEL);
        if (!n)
            return -ENOMEM;

        memset(n, 0, sizeof(*n));

        strncpy(n->container_id, req.container_id, sizeof(n->container_id) - 1);
        n->pid = req.pid;
        n->soft_limit = req.soft_limit_bytes;
        n->hard_limit = req.hard_limit_bytes;

        spin_lock(&list_lock);
        list_add(&n->list, &monitor_list);
        spin_unlock(&list_lock);

        printk(KERN_INFO
            "[container_monitor] REGISTERED %s PID=%d\n",
            n->container_id, n->pid);

        return 0;
    }

    return -EINVAL;
}

// ---------------- FILE OPS ----------------
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

// ---------------- DEVICE ----------------
static dev_t dev;
static struct cdev cdev;
static struct class *cls;

// ---------------- INIT ----------------
static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);

    cls = class_create(DEVICE_NAME);
    device_create(cls, NULL, dev, NULL, DEVICE_NAME);

    cdev_init(&cdev, &fops);
    cdev_add(&cdev, dev, 1);

    timer_setup(&monitor_timer, monitor_fn, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] LOADED\n");
    return 0;
}

// ---------------- EXIT ----------------
static void __exit monitor_exit(void)
{
    struct monitor_node *n, *tmp;

    del_timer_sync(&monitor_timer);

    spin_lock(&list_lock);

    list_for_each_entry_safe(n, tmp, &monitor_list, list) {
        list_del(&n->list);
        kfree(n);
    }

    spin_unlock(&list_lock);

    device_destroy(cls, dev);
    class_destroy(cls);
    unregister_chrdev_region(dev, 1);

    printk(KERN_INFO "[container_monitor] EXIT\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container Memory Monitor with Soft & Hard Limits");

