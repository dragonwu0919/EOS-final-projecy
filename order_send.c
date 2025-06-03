// order_send.c - 改為 kernel module 套餐發送模式

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/random.h>
#include "order_comm.h"

extern int add_order(struct order *o);

static const char *mains[] = { "Cucuber roll", "Sashimi", "Fish roll" };
static const char *sides[] = { "Cucumber Salad", "Tomato Salad", "Tomato Cucumber Salad" };
static const char *drinks[] = { "greentea", "blacktea", "oolongtea" };

#define NM (sizeof(mains) / sizeof(mains[0]))
#define NS (sizeof(sides) / sizeof(sides[0]))
#define ND (sizeof(drinks) / sizeof(drinks[0]))

static struct task_struct *sender_thread;

static int order_generator(void *data)
{
    int order_id = 1;

    while (!kthread_should_stop()) {
        const char *set[3];
        set[0] = mains[prandom_u32_max(NM)];
        set[1] = sides[prandom_u32_max(NS)];
        set[2] = drinks[prandom_u32_max(ND)];

        for (int i = 0; i < 3; i++) {
            struct order *o = kmalloc(sizeof(*o), GFP_KERNEL);
            if (!o) break;

            o->order_id = order_id;
            // menu_idx 將由接收端解析菜名對應 index
            strncpy(o->name, set[i], sizeof(o->name));

            while (add_order(o) == -ENOSPC) {
                pr_warn("order_send: queue full, order %d waiting…\n", o->order_id);
                ssleep(1);
                if (kthread_should_stop()) return 0;
            }

            pr_info("order_send: order %d item [%s]\n", o->order_id, o->name);
            ssleep(1);
        }

        pr_info("📦 Meal Set %d: %s + %s + %s\n", order_id, set[0], set[1], set[2]);
        order_id++;
        ssleep(3);
    }
    return 0;
}

static int __init order_send_init(void)
{
    sender_thread = kthread_run(order_generator, NULL, "order_sender");
    pr_info("order_send kernel module loaded\n");
    return 0;
}

static void __exit order_send_exit(void)
{
    kthread_stop(sender_thread);
    pr_info("order_send kernel module unloaded\n");
}

module_init(order_send_init);
module_exit(order_send_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group7");
MODULE_DESCRIPTION("Kernel module sending meal sets as orders");
