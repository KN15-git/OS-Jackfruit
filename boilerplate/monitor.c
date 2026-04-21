#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

#define SOFT_LIMIT (40 * 1024 * 1024) // 40 MB
#define HARD_LIMIT (64 * 1024 * 1024) // 64 MB

static int major;
static struct class *monitor_class = NULL;
static struct device *monitor_device = NULL;

struct container_node {
    pid_t pid;
    struct list_head list;
};

static LIST_HEAD(container_list);
static struct timer_list monitor_timer;

// 🔥 GET RSS MEMORY
unsigned long get_rss(struct task_struct *task) {
    struct mm_struct *mm = task->mm;
    if (!mm) return 0;
    return get_mm_rss(mm) << PAGE_SHIFT;
}

// 🔥 TIMER FUNCTION
void monitor_memory(struct timer_list *t) {
    struct container_node *entry, *tmp;

    list_for_each_entry_safe(entry, tmp, &container_list, list) {

        struct task_struct *task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);

        if (!task) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        unsigned long rss = get_rss(task);

        if (rss > SOFT_LIMIT) {
            printk(KERN_INFO "monitor: PID %d exceeded SOFT limit (%lu MB)\n",
                   entry->pid, rss / (1024 * 1024));
        }

        if (rss > HARD_LIMIT) {
            printk(KERN_INFO "monitor: PID %d exceeded HARD limit → killing\n",
                   entry->pid);

            send_sig(SIGKILL, task, 0);

            list_del(&entry->list);
            kfree(entry);
        }
    }

    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

// 🔥 IOCTL
static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int pid;

    switch (cmd) {
        case REGISTER_PID:
            if (copy_from_user(&pid, (int *)arg, sizeof(pid)))
                return -EFAULT;

            struct container_node *node = kmalloc(sizeof(*node), GFP_KERNEL);
            node->pid = pid;
            INIT_LIST_HEAD(&node->list);
            list_add(&node->list, &container_list);

            printk(KERN_INFO "monitor: registered PID %d\n", pid);
            break;

        default:
            return -EINVAL;
    }

    return 0;
}

static int dev_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "monitor: device opened\n");
    return 0;
}

static int dev_release(struct inode *inode, struct file *file) {
    printk(KERN_INFO "monitor: device closed\n");
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .unlocked_ioctl = dev_ioctl,
};

// INIT
static int __init monitor_init(void) {
    printk(KERN_INFO "monitor: initializing\n");

    major = register_chrdev(0, DEVICE_NAME, &fops);

    monitor_class = class_create("monitor_class");
    monitor_device = device_create(monitor_class, NULL,
                                   MKDEV(major, 0), NULL,
                                   DEVICE_NAME);

    // Start timer
    timer_setup(&monitor_timer, monitor_memory, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));

    printk(KERN_INFO "monitor: started memory monitoring\n");

    return 0;
}

// EXIT
static void __exit monitor_exit(void) {
    struct container_node *entry, *tmp;

    del_timer(&monitor_timer);

    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    device_destroy(monitor_class, MKDEV(major, 0));
    class_destroy(monitor_class);
    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "monitor: exiting\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Container Monitor with Memory Limits");
