#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include "order_comm.h"

extern int add_order(struct order *o);

static const char *menu[] = {
    "Salmon Sushi", "Tuna Sushi", "Veggie Roll", "Cucumber Sushi"
};
#define NMENU ARRAY_SIZE(menu)

static struct task_struct *sender_thread;

static int order_generator(void *data)
{
    int order_id = 1;

    while (!kthread_should_stop()) {
        struct order *o = kmalloc(sizeof(*o), GFP_KERNEL);
        if (!o)
            break;

        o->order_id = order_id++;
        o->menu_idx = (o->order_id - 1) % NMENU;

        /* 如果隊列滿了就等待並重試 */
        while (add_order(o) == -ENOSPC) {
            pr_warn("order_send: queue full, order %d waiting…\n", o->order_id);
            ssleep(1);
            if (kthread_should_stop())
                return 0;
        }

        pr_info("order_send: new order %d [%s]\n",
                o->order_id, menu[o->menu_idx]);

        /* 每秒新增一筆 */
        ssleep(3);
    }
    return 0;
}

static int __init order_send_init(void)
{
    sender_thread = kthread_run(order_generator, NULL, "order_sender");
    pr_info("order_send loaded\n");
    return 0;
}

static void __exit order_send_exit(void)
{
    kthread_stop(sender_thread);
    pr_info("order_send unloaded\n");
}

module_init(order_send_init);
module_exit(order_send_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group7");
MODULE_DESCRIPTION("Order Send Module with retry on full queue");

