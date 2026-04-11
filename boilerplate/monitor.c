// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "jackfruit: " fmt

#include <linux/string.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME      "container_monitor"
#define CHECK_INTERVAL   (HZ)

/* ─── container structure ─── */
struct tracked_container {
    struct list_head list;
    pid_t pid;
    unsigned long soft_limit_kb;
    unsigned long hard_limit_kb;
    char name[64];
    bool soft_warned;
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_lock);

static dev_t devno;
static struct cdev mon_cdev;
static struct class *mon_class;
static struct delayed_work check_work;

/* ─── helpers ─── */
static unsigned long get_rss_kb(pid_t pid)
{
    struct task_struct *task;
    unsigned long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm)
        rss = (unsigned long)get_mm_rss(task->mm) << (PAGE_SHIFT - 10);
    rcu_read_unlock();

    return rss;
}

static bool process_exists(pid_t pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    rcu_read_unlock();

    return task != NULL;
}

static void kill_container(pid_t pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 0);
    rcu_read_unlock();
}

/* ─── periodic check ─── */
static void do_check(struct work_struct *work)
{
    struct tracked_container *entry, *tmp;

    mutex_lock(&list_lock);

    list_for_each_entry_safe(entry, tmp, &container_list, list) {

        if (!process_exists(entry->pid)) {
            pr_info("container '%s' (pid %d) exited\n",
                    entry->name, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        unsigned long rss = get_rss_kb(entry->pid);

        if (rss > entry->hard_limit_kb) {
            pr_warn("HARD LIMIT: '%s' pid=%d rss=%luKB limit=%luKB\n",
                    entry->name, entry->pid, rss, entry->hard_limit_kb);

            kill_container(entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss > entry->soft_limit_kb && !entry->soft_warned) {
            pr_warn("SOFT LIMIT: '%s' pid=%d rss=%luKB limit=%luKB\n",
                    entry->name, entry->pid, rss, entry->soft_limit_kb);
            entry->soft_warned = true;
        }
    }

    mutex_unlock(&list_lock);

    schedule_delayed_work(&check_work, CHECK_INTERVAL);
}

/* ─── ioctl ─── */
static long monitor_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {

    case MONITOR_REGISTER: {
        struct monitor_request req;
        struct tracked_container *entry;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        mutex_lock(&list_lock);
        list_for_each_entry(entry, &container_list, list) {
            if (entry->pid == req.pid) {
                mutex_unlock(&list_lock);
                return -EEXIST;
            }
        }
        mutex_unlock(&list_lock);

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        INIT_LIST_HEAD(&entry->list);
        entry->pid = req.pid;
        entry->soft_limit_kb = req.soft_limit_bytes >> 10;
        entry->hard_limit_kb = req.hard_limit_bytes >> 10;
        entry->soft_warned = false;
        strncpy(entry->name, req.container_id, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';

        mutex_lock(&list_lock);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&list_lock);

        pr_info("registered '%s' pid=%d\n", entry->name, entry->pid);
        return 0;
    }

    case MONITOR_UNREGISTER: {
        struct monitor_request req;
        struct tracked_container *entry, *tmp;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        mutex_lock(&list_lock);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                mutex_unlock(&list_lock);
                return 0;
            }
        }
        mutex_unlock(&list_lock);

        return -ESRCH;
    }

    default:
        return -ENOTTY;
    }
}

/* ─── file ops ─── */
static int monitor_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int monitor_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner = THIS_MODULE,
    .open = monitor_open,
    .release = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

/* ─── init ─── */
static int __init monitor_init(void)
{
    int ret;
    struct device *dev;

    ret = alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    if (ret < 0)
        return ret;

    cdev_init(&mon_cdev, &monitor_fops);
    ret = cdev_add(&mon_cdev, devno, 1);
    if (ret < 0)
        goto fail_cdev;

    mon_class = class_create(DEVICE_NAME);
    if (IS_ERR(mon_class)) {
        ret = PTR_ERR(mon_class);
        goto fail_class;
    }

    dev = device_create(mon_class, NULL, devno, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        goto fail_device;
    }

    INIT_DELAYED_WORK(&check_work, do_check);
    schedule_delayed_work(&check_work, CHECK_INTERVAL);

    pr_info("loaded\n");
    return 0;

fail_device:
    class_destroy(mon_class);
fail_class:
    cdev_del(&mon_cdev);
fail_cdev:
    unregister_chrdev_region(devno, 1);
    return ret;
}

/* ─── exit ─── */
static void __exit monitor_exit(void)
{
    struct tracked_container *entry, *tmp;

    cancel_delayed_work_sync(&check_work);

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_lock);

    device_destroy(mon_class, devno);
    class_destroy(mon_class);
    cdev_del(&mon_cdev);
    unregister_chrdev_region(devno, 1);

    pr_info("unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
