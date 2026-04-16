#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/device.h>

#define DEVICE_NAME "container_monitor"
#define CLASS_NAME "container"

MODULE_LICENSE("GPL");

struct container {
    char id[32];
    pid_t pid;
    long soft;
    long hard;
    struct container *next;
};

static struct container *head = NULL;
static int major;
static struct class *cls;
static struct device *dev;
static struct timer_list monitor_timer;

/* ================= GET RSS ================= */
static long get_rss(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss = 0;

    rcu_read_lock();

    task = pid_task(find_vpid(pid), PIDTYPE_PID);   // ✅ FIX
    if (!task) {
        rcu_read_unlock();
        return -1;
    }

    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return -1;

    rss = get_mm_rss(mm) << PAGE_SHIFT;
    mmput(mm);

    return rss;
}

/* ================= REGISTER ================= */
static void add_container(const char *id, pid_t pid, long soft, long hard)
{
    struct container *c = kmalloc(sizeof(*c), GFP_KERNEL);

    strcpy(c->id, id);
    c->pid = pid;
    c->soft = soft;
    c->hard = hard;

    c->next = head;
    head = c;

    printk(KERN_INFO "[container_monitor] Registering container=%s pid=%d soft=%ld hard=%ld\n",
           id, pid, soft, hard);
}

/* ================= REMOVE ================= */
static void remove_container(pid_t pid)
{
    struct container **c = &head;

    while (*c) {
        if ((*c)->pid == pid) {
            struct container *tmp = *c;
            *c = (*c)->next;

            printk(KERN_INFO "[container_monitor] Unregister container=%s pid=%d\n",
                   tmp->id, tmp->pid);

            kfree(tmp);
            return;
        }
        c = &(*c)->next;
    }
}

/* ================= MONITOR ================= */
static void monitor_fn(struct timer_list *t)
{
    struct container *c = head;

    while (c) {
        long rss = get_rss(c->pid);

        if (rss < 0) {
            printk(KERN_INFO "[container_monitor] Process gone container=%s pid=%d\n",
                   c->id, c->pid);
            remove_container(c->pid);
            break;
        }

        if (rss > c->hard) {
            printk(KERN_INFO "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%ld\n",
                   c->id, c->pid, rss, c->hard);

            kill_pid(find_vpid(c->pid), SIGKILL, 1);
        }
        else if (rss > c->soft) {
            printk(KERN_INFO "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%ld\n",
                   c->id, c->pid, rss, c->soft);
        }

        c = c->next;
    }

    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(2000));
}

/* ================= WRITE ================= */
static ssize_t dev_write(struct file *file,
                         const char __user *buf,
                         size_t len, loff_t *off)
{
    char kbuf[256];
    char id[32];
    int pid;
    long soft, hard;

    if (len > 255)
        len = 255;

    if (copy_from_user(kbuf, buf, len))
        return -EFAULT;

    kbuf[len] = '\0';

    /* EXPECT: register:id:pid:soft:hard */
    if (sscanf(kbuf, "register:%31[^:]:%d:%ld:%ld",
               id, &pid, &soft, &hard) == 4)
    {
        add_container(id, pid, soft, hard);
    }

    return len;
}

/* ================= FILE OPS ================= */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .write = dev_write,
};

/* ================= INIT ================= */
static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);

    cls = class_create(CLASS_NAME);   // ✅ FIX for kernel 6.x
    dev = device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

    timer_setup(&monitor_timer, monitor_fn, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(2000));

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);

    return 0;
}

/* ================= EXIT ================= */
static void __exit monitor_exit(void)
{
    del_timer(&monitor_timer);

    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);
    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "[container_monitor] Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
