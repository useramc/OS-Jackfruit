/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 *   - device registration and teardown
 *   - timer setup
 *   - RSS helper
 *   - soft-limit and hard-limit event helpers
 *   - ioctl dispatch shell
 *
 * YOUR WORK: Fill in all sections marked // TODO.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>



#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/sched/mm.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1
#define MONITOR_CONTAINER_ID_LEN 32

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 *
 * Requirements:
 *   - track PID, container ID, soft limit, and hard limit
 *   - remember whether the soft-limit warning was already emitted
 *   - include `struct list_head` linkage
 * ============================================================== */
struct monitor_node
{
    pid_t pid;
    char container_id[MONITOR_CONTAINER_ID_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_warned;
    struct list_head list;
};

/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 *
 * Requirements:
 *   - shared across ioctl and timer code paths
 *   - protect insert, remove, and iteration safely
 *
 * You may choose either a mutex or a spinlock, but your README must
 * justify the choice in terms of the code paths you implemented.
 * ============================================================== */
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 *
 * Log a warning when a process exceeds the soft limit.
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 *
 * Kill a process when it exceeds the hard limit.
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Implement periodic monitoring.
     *
     * Requirements:
     *   - iterate through tracked entries safely
     *   - remove entries for exited processes
     *   - emit soft-limit warning once per entry
     *   - enforce hard limit and then remove the entry
     *   - avoid use-after-free while deleting during iteration
     * ============================================================== */
    struct monitor_node *node,*temp;
    LIST_HEAD(to_cleanup);

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(node,temp,&monitor_list,list)
    {
        long rss=get_rss_bytes(node->pid);

        /* process exited */
        if(rss<0)
        {
            list_del(&node->list);
            list_add(&node->list,&to_cleanup);
            continue;
        }

        /* hard limit */
        if((unsigned long)rss>=node->hard_limit_bytes)
        {
            kill_process(
                node->container_id,
                node->pid,
                node->hard_limit_bytes,
                rss
            );

            list_del(&node->list);
            list_add(&node->list,&to_cleanup);
            continue;
        }

        /* soft limit */
        if((unsigned long)rss>=node->soft_limit_bytes &&
        !node->soft_warned)
        {
            log_soft_limit_event(
                node->container_id,
                node->pid,
                node->soft_limit_bytes,
                rss
            );

            node->soft_warned=1;
        }
    }

    mutex_unlock(&monitor_lock);

    /* free removed entries */
    list_for_each_entry_safe(node,temp,&to_cleanup,list)
    {
        list_del(&node->list);
        kfree(node);
    }
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 *
 * Supported operations:
 *   - register a PID with soft + hard limits
 *   - unregister a PID when the runtime no longer needs tracking
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a monitored entry.
         *
         * Requirements:
         *   - allocate and initialize one node from req
         *   - validate allocation and limits
         *   - insert into the shared list under the chosen lock
         * ============================================================== */
        
        struct monitor_node *node;
        
        if(req.soft_limit_bytes>req.hard_limit_bytes)
        {
            printk(KERN_ERR
                "[container_monitor] invalid limits soft=%lu hard=%lu\n",
                req.soft_limit_bytes,
                req.hard_limit_bytes
            );
            return -EINVAL;
        }

        node=kzalloc(sizeof(*node),GFP_KERNEL);

        if(!node)
        return -ENOMEM;

        node->pid=req.pid;
        node->soft_limit_bytes=req.soft_limit_bytes;
        node->hard_limit_bytes=req.hard_limit_bytes;
        node->soft_warned=0;

        strncpy(
            node->container_id,
            req.container_id,
            MONITOR_CONTAINER_ID_LEN-1
        );

        node->container_id[MONITOR_CONTAINER_ID_LEN-1]='\0';    

        INIT_LIST_HEAD(&node->list);

        mutex_lock(&monitor_lock);
        list_add(&node->list,&monitor_list);
        mutex_unlock(&monitor_lock);
        
        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a monitored entry on explicit unregister.
     *
     * Requirements:
     *   - search by PID, container ID, or both
     *   - remove the matching entry safely if found
     *   - return status indicating whether a matching entry was removed
     * ============================================================== */
    struct monitor_node *node,*temp;
    struct monitor_node *found=NULL;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(node,temp,&monitor_list,list)
    {
        if(node->pid==req.pid)
        {
            list_del(&node->list);
            found=node;
            break;
        }
    }

    mutex_unlock(&monitor_lock);

    if(!found)
        return -ENOENT;

    kfree(found);
    return 0;    
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    del_timer_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries.
     *
     * Requirements:
     *   - remove and free every list node safely
     *   - leave no leaked state on module unload
     * ============================================================== */
    struct monitor_node *node,*temp;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(node,temp,&monitor_list,list)
    {
        list_del(&node->list);
        kfree(node);
    }

    mutex_unlock(&monitor_lock);
    
    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor"); 


/*

//og
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
*/
