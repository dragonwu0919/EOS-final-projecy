// order_receive.c - 等待一整套餐出完才允許下一單進入

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/kthread.h>
#include <linux/string.h>
#include "order_comm.h"

#define MAX_ORDERS    9

/* ----- 菜單對應表 ----- */
static const char *menu[] = {
    "Cucuber roll", "Sashimi", "Fish roll",
    "Cucumber Salad", "Tomato Salad", "Tomato Cucumber Salad",
    "greentea", "blacktea", "oolongtea"
};
#define NMENU ARRAY_SIZE(menu)

static struct order *buf[MAX_ORDERS];
static int head, tail;

static DEFINE_MUTEX(buf_mutex);
static struct semaphore order_sem;
static DECLARE_WAIT_QUEUE_HEAD(order_wq);

// 套餐控制
#define MEAL_SIZE 3
static struct order *meal_buffer[MEAL_SIZE];
static int meal_count = 0;
static int pending_order_id = 0;
static int pending_count = 0;

int lookup_menu_idx(const char *name) {
    for (int i = 0; i < NMENU; i++) {
        if (strcmp(menu[i], name) == 0)
            return i;
    }
    return -1; // unknown
}

int add_order(struct order *o)
{
    int idx = lookup_menu_idx(o->name);
    if (idx < 0) {
        pr_warn("order_receive: unknown item [%s], dropped\n", o->name);
        kfree(o);
        return -EINVAL;
    }
    o->menu_idx = idx;
    meal_buffer[meal_count++] = o;

    if (meal_count < MEAL_SIZE) return 0;

    pr_info("🍱 order_receive: meal ready, dispatching 3 items\n");

    for (int i = 0; i < MEAL_SIZE; i++) {
        mutex_lock(&buf_mutex);
        int next = (tail + 1) % MAX_ORDERS;
        if (next == head) {
            mutex_unlock(&buf_mutex);
            pr_warn("order_receive: internal buffer full!\n");
            // rollback buffer
            for (int j = 0; j <= i; j++) {
                kfree(meal_buffer[j]);
            }
            meal_count = 0;
            return -ENOSPC;
        }
        buf[tail] = meal_buffer[i];
        tail = next;
        up(&order_sem);
        wake_up_interruptible(&order_wq);
        mutex_unlock(&buf_mutex);
    }
    meal_count = 0;
    return 0;
}
EXPORT_SYMBOL(add_order);

struct order *recv_order(void)
{
    struct order *o;

    if (down_interruptible(&order_sem)) {
        if (kthread_should_stop())
            return NULL;
        return NULL;
    }

    mutex_lock(&buf_mutex);
    o = buf[head];
    head = (head + 1) % MAX_ORDERS;
    mutex_unlock(&buf_mutex);
    return o;
}
EXPORT_SYMBOL(recv_order);

static ssize_t order_dev_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    return -ENOSYS;
}

static unsigned int order_dev_poll(struct file *file, poll_table *wait)
{
    unsigned int mask = 0;
    poll_wait(file, &order_wq, wait);
    if (down_trylock(&order_sem) == 0) {
        up(&order_sem);
        mask |= POLLIN | POLLRDNORM;
    }
    return mask;
}

static const struct file_operations order_fops = {
    .owner = THIS_MODULE,
    .read  = order_dev_read,
    .poll  = order_dev_poll,
};

static struct miscdevice order_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "order",
    .fops  = &order_fops,
};

static int __init order_receive_init(void)
{
    head = tail = 0;
    sema_init(&order_sem, 0);

    if (misc_register(&order_dev)) {
        pr_err("order_receive: device registration failed\n");
        return -1;
    }

    pr_info("order_receive loaded; /dev/order ready\n");
    return 0;
}

static void __exit order_receive_exit(void)
{
    misc_deregister(&order_dev);
    pr_info("order_receive unloaded\n");
}

module_init(order_receive_init);
module_exit(order_receive_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group7");
MODULE_DESCRIPTION("Order Receive Module with meal set handling and safe circular queue");
