#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/poll.h> 
#include "order_comm.h"

#define MAX_PLATES 3
static int plate_count = MAX_PLATES;
static DEFINE_MUTEX(plate_mutex);

extern struct order *recv_order(void);

/* 各工作區的信號量 */
static struct semaphore sem_ingr;    // 食材區 (1)
static struct semaphore sem_cut;     // 砧板區 (2)
static struct semaphore sem_stove;   // 瓦斯爐 (1)
static struct semaphore sem_plate;   // 擺盤區 (3)
static struct semaphore sem_return;  // 回收區 (1)
static struct semaphore sem_sink;    // 流理臺 (1)

typedef struct {
    const char     *name;      /* 步驟名稱 */
    int              duration;  /* 持續秒數 */
    struct semaphore *sem;      /* 對應的 semaphore */
} step_t;

/* 每道菜的配方：recipes[menu_idx][0..recipe_len[menu_idx]-1] */
static step_t recipes[][9] = {
    /* dish_id=1 (menu_idx=0) 小黃瓜手捲 */
    {
      { "Cucumber",    1, &sem_ingr  },
      { "Chop",        2, &sem_cut   },
      { "Plate Dish",  1, &sem_plate },
      { "Rice",        1, &sem_ingr  },
      { "Cook Rice",   5, &sem_stove },
      { "Plate Dish",  1, &sem_plate },
      { "Seaweed",     1, &sem_ingr  },
      { "Plate Dish",  1, &sem_plate },
      { NULL,          0, NULL       },
    },
    /* dish_id=2 (menu_idx=1) 生魚片*/
    {
      { "Fish",        1, &sem_ingr  },
      { "Chop",        2, &sem_cut   },
      { "Plate Dish",  1, &sem_plate },
      { NULL,          0, NULL       }, 
    },
    /* dish_id=3 (menu_idx=2) 魚肉手捲 */
    {
      { "Fish",        1, &sem_ingr  },
      { "Chop",        2, &sem_cut   },
      { "Plate Dish",  1, &sem_plate },
      { "Rice",        1, &sem_ingr  },
      { "Cook Rice",   5, &sem_stove },
      { "Plate Dish",  1, &sem_plate },
      { "Seaweed",     1, &sem_ingr  },
      { "Plate Dish",  1, &sem_plate },
      { NULL,          0, NULL       },
    },
    /* dish_id=4 (menu_idx=3) 黃瓜沙拉 */
    {
      { "Cucumber",    1, &sem_ingr  },
      { "Chop",        2, &sem_cut   },
      { "Plate Dish",  1, &sem_plate },
      { "Cabbage",     1, &sem_ingr  },
      { "Chop",        2, &sem_cut   },
      { "Plate Dish",  1, &sem_plate },
      { NULL,          0, NULL       },
    },
    /* dish_id=5 (menu_idx=4) 蕃茄沙拉 */
     {
      { "Tomato",       1, &sem_ingr },
      { "Chop",         2, &sem_cut },
      { "Plate Dish 1", 1, &sem_plate },
      { "Cabbage",      1, &sem_ingr },
      { "Chop",         2, &sem_cut },
      { "Plate Dish 1", 1, &sem_plate},
      { NULL,           0, NULL       },
    },
    /* dish_id=6 (menu_idx=5) 蕃茄黃瓜沙拉 */
    {
        { "Tomato",       1, &sem_ingr },
        { "Chop",         2, &sem_cut },
        { "Plate Dish 1", 1, &sem_plate },
        { "Cucumber",     1, &sem_ingr },
        { "Chop",         2, &sem_cut },
        { "Plate Dish 1", 1, &sem_plate},
        { "Cabbage",      1, &sem_ingr },
        { "Chop",         2, &sem_cut },
        { "Plate Dish 1", 1, &sem_plate },
     }, 
};
static const int recipe_len[] = { 8, 3, 8, 6, 6, 9};

static const char *menu[] = {
    "Cucuber roll", "Sashimi", "Fish roll", "Cucumber Salad", "Tomato Salad", "Tomato Cucumber Salad"
};

struct step_location {
    const char* name;
    int x;
    int y;
};

static struct step_location step_positions[] = {
    { "Cucumber",      28, 26 },
    { "Tomato",        28, 26 },
    { "Cabbage",       28, 26 },
    { "Fish",          28, 26 },
    { "Rice",          28, 26 },
    { "Seaweed",       28, 26 },
    { "Chop 1",        15, 15 },
    { "Chop 2",        15, 20 },
    { "Cook Rice",     60, 28 },
    { "Plate Dish 1",  91, 12 },
    { "Plate Dish 2",  91, 23 },
    { "Plate Dish 3",  91, 34 },
    { "WASH_DISHES",   28,  9 },
    { "Return_DISHES", 60,  7 },
    { NULL,            0,  0 }  // sentinel
};


#define NMENU ARRAY_SIZE(menu)

#define EVENT_LOG_FIFO_SIZE 4096
static DECLARE_KFIFO(event_log_fifo, char, EVENT_LOG_FIFO_SIZE);
static DECLARE_WAIT_QUEUE_HEAD(kitchen_wq);

/* 結構化事件 FIFO，將 order_event 傳給 user space GUI */
#define KITCHEN_FIFO_SIZE 64
static DECLARE_KFIFO(kitchen_fifo, struct order_event, KITCHEN_FIFO_SIZE);

/* GUI 回傳的 ACK FIFO */
#define ACK_FIFO_SIZE 64
static DECLARE_KFIFO(ack_fifo, struct order_ack, ACK_FIFO_SIZE);
static DECLARE_WAIT_QUEUE_HEAD(ack_wq);

/* 字符設備 misc */
static struct miscdevice kitchen_dev;

/* 寫入事件到 FIFO，並喚醒讀者 */
static void notify_event(const char *fmt, ...)
{
    char buf[128];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vscnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* 輸入 FIFO */
    kfifo_in(&event_log_fifo, buf, len);
    wake_up_interruptible(&kitchen_wq);
}

/* 用戶空間 read() */
static ssize_t kitchen_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    unsigned int copied;
    int ret;

    if ((file->f_flags & O_NONBLOCK) && kfifo_is_empty(&event_log_fifo))
        return -EAGAIN;

    wait_event_interruptible(kitchen_wq, !kfifo_is_empty(&event_log_fifo));
    ret = kfifo_to_user(&event_log_fifo, buf, count, &copied);
    if (ret)
        return ret;
    else
        return copied;

    // 加保險
    return 0;
}

/* 用戶空間 write()，接收 ACK 字串 */
static ssize_t kitchen_write(struct file *file,
                             const char __user *buf,
                             size_t count, loff_t *ppos)
{
    unsigned int copied;
    int ret;
    /* 最多讀一行 ACK */
    if (count > ACK_FIFO_SIZE) count = ACK_FIFO_SIZE;
    ret = kfifo_from_user(&ack_fifo, buf, count, &copied);
    if (!ret)
        wake_up_interruptible(&ack_wq);
    return ret ? ret : copied;
}

static unsigned int kitchen_poll(struct file *file, poll_table *wait)
{
    unsigned int mask = 0;
    poll_wait(file, &kitchen_wq, wait);
    if (!kfifo_is_empty(&kitchen_fifo))
        mask |= POLLIN | POLLRDNORM;
    return mask;
}

static const struct file_operations kitchen_fops = {
    .owner   = THIS_MODULE,
    .read    = kitchen_read,
    .poll    = kitchen_poll,
    .write   = kitchen_write,
};

/* 三位廚師的執行緒 */
static struct task_struct *chef_thread[3];

struct chef_task_status {
    int order_id;
    int dish_id;
    int step;
    int position_x;
    int position_y;
    int completed; // GUI 回報: 0 未完成, 1 已完成
};

#define TASK_BUFSIZE 128

static DECLARE_WAIT_QUEUE_HEAD(task_wq);
static int task_ready = 0;

static struct chef_task_status current_task;

static ssize_t task_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[TASK_BUFSIZE];
    int len;

    if (!task_ready)
        return 0; // 無任務

    len = snprintf(kbuf, TASK_BUFSIZE, "%d %d %d %d %d\n",
                   current_task.order_id,
                   current_task.dish_id,
                   current_task.step,
                   current_task.position_x,
                   current_task.position_y);

    if (*ppos > 0 || count < len)
        return 0;

    if (copy_to_user(buf, kbuf, len))
        return -EFAULT;

    task_ready = 0; // 任務已讀取
    *ppos = len;
    return len;
}

static ssize_t task_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[TASK_BUFSIZE];
    int completed;

    if (count >= TASK_BUFSIZE)
        return -EINVAL;

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';

    if (sscanf(kbuf, "%d", &completed) != 1)
        return -EINVAL;

    current_task.completed = completed;

    wake_up(&task_wq); // 通知kernel端已完成

    return count;
}

static unsigned int task_dev_poll(struct file *file, poll_table *wait)
{
    poll_wait(file, &task_wq, wait);
    if (task_ready)
        return POLLIN | POLLRDNORM;
    return 0;
}

static const struct file_operations task_fops = {
    .owner = THIS_MODULE,
    .read = task_dev_read,
    .write = task_dev_write,
    .poll = task_dev_poll,
};

static struct miscdevice task_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "chef_tasks",
    .fops = &task_fops,
};

// 發送任務給 GUI
void write_task_to_chef_device(int order_id, int dish_id, int step, int x, int y)
{
    current_task.order_id = order_id;
    current_task.dish_id = dish_id;
    current_task.step = step;
    current_task.position_x = x;
    current_task.position_y = y;
    current_task.completed = 0;

    task_ready = 1;
    wake_up(&task_wq);
}

// 等待 GUI 完成回報
void wait_for_completion_from_gui(void)
{
    wait_event(task_wq, current_task.completed == 1);
}

static int __init kitchen_task_init(void)
{
    misc_register(&task_dev);
    printk("Kitchen task device registered.\n");
    return 0;
}

static void __exit kitchen_task_exit(void)
{
    misc_deregister(&task_dev);
    printk("Kitchen task device unregistered.\n");
}

void get_step_position(const char* step_name, int *x, int *y) {
    for (int i = 0; step_positions[i].name != NULL; i++) {
        if (strcmp(step_positions[i].name, step_name) == 0) {
            *x = step_positions[i].x;
            *y = step_positions[i].y;
            return;
        }
    }
    *x = 50;  // fallback 預設座標
    *y = 12;
}

static int chef_fn(void *data)
{
    int id = (int)(long)data;
    struct order *o;

    while (!kthread_should_stop()) {
        o = recv_order();  /* 阻塞直到有訂單 */
        if (!o)
            continue;

        /* ORDER_START 依舊直接發給 GUI */
        notify_event("%d:ORDER_START:%d:%s\n",
                     id,
                     o->order_id,
                     (o->menu_idx < NMENU ? menu[o->menu_idx] : "Unknown"));
        pr_info("Chef[%d] start order %d [%s]\n",
                id,
                o->order_id,
                (o->menu_idx < NMENU ? menu[o->menu_idx] : "Unknown"));

        
        int idx   = o->menu_idx;
        int steps = recipe_len[idx];
        int s;
        for (s = 0; s < steps; s++) {
            step_t *st = &recipes[idx][s];

            /* —— 新增：用 order_event 下發 STEP_START —— */
            
            int px, py;
            get_step_position(st->name, &px, &py);

            struct order_event ev = {
                .chef_id   = id,
                .step_idx  = s,
                .x         = px,
                .y         = py,
                .pause_sec = st->duration
            };
            
            /* 推到 kitchen_fifo，讓 user-space 讀 */
            kfifo_in(&kitchen_fifo, &ev, 1);
            wake_up_interruptible(&kitchen_wq);

            /* 等 user-space 回 ACK */
            wait_event_interruptible(ack_wq,
                !kfifo_is_empty(&ack_fifo));

            /* 讀出並檢查 ACK */
            struct order_ack ack;
            if (kfifo_out(&ack_fifo, &ack, 1) != 1) {
                pr_warn("Chef[%d] failed to receive ack\n", id);
                continue; // 或 return/error handling
            }
            if (ack.chef_id != id || ack.step_idx != s) {
                pr_warn("Chef[%d] got unexpected ACK %d\n",
                        id, ack.step_idx);
            }
            
            

            /* 真正佔用資源並執行該步驟 */
            down(st->sem);
            ssleep(st->duration);
            up(st->sem);

            /* 如果是擺盤步驟，就扣除一個盤子 */
            if (st->sem == &sem_plate) {
                mutex_lock(&plate_mutex);
                if (plate_count > 0)
                    plate_count--;
                mutex_unlock(&plate_mutex);
            }

            /* STEP_DONE 仍保留原有通知 */
            notify_event("%d:STEP_DONE:%d:%s\n",
                            id, o->order_id, st->name);
        }
        

        /* ORDER_DONE 及洗碗流程不變 */
        notify_event("%d:ORDER_DONE:%d\n", id, o->order_id);
        pr_info("Chef[%d] done  order %d\n", id, o->order_id);

        notify_event("%d:STEP_START:%d:WASH_DISHES\n",
                     id, o->order_id);
        down(&sem_sink);
        ssleep(3);
        up(&sem_sink);
        notify_event("%d:STEP_DONE:%d:WASH_DISHES\n",
                     id, o->order_id);

        mutex_lock(&plate_mutex);
        if (plate_count < MAX_PLATES)
            plate_count++;
        mutex_unlock(&plate_mutex);

        kfree(o);
    }

    return 0;
}

static int __init kitchen_init(void)
{
    int i, ret;

    /* 初始化工作區信號量 */
    sema_init(&sem_ingr,   1);
    sema_init(&sem_cut,    2);
    sema_init(&sem_stove,  1);
    sema_init(&sem_plate,  3);
    sema_init(&sem_return, 1);
    sema_init(&sem_sink,   1);

    /* 初始化 FIFO */
    INIT_KFIFO(kitchen_fifo);

    /* 註冊 misc device /dev/kitchen */
    kitchen_dev.minor = MISC_DYNAMIC_MINOR;
    kitchen_dev.name  = "kitchen";
    kitchen_dev.fops  = &kitchen_fops;

    kitchen_task_init();

    ret = misc_register(&kitchen_dev);
    if (ret) {
        pr_err("kitchen: misc_register failed (%d)\n", ret);
        return ret;
    }

    /* 啟動 3 個廚師執行緒 */
    for (i = 0; i < 3; i++) {
        chef_thread[i] = kthread_run(chef_fn,
                                     (void *)(long)i,
                                     "chef_%d", i);
        if (IS_ERR(chef_thread[i])) {
            pr_err("kitchen: failed to start chef_%d\n", i);
            chef_thread[i] = NULL;
        }
    }

    pr_info("kitchen loaded with 3 chef threads and /dev/kitchen\n");
    return 0;
}

static void __exit kitchen_exit(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (chef_thread[i])
            kthread_stop(chef_thread[i]);
    }
    misc_deregister(&kitchen_dev);
    kitchen_task_exit();
    pr_info("kitchen unloaded\n");
}

module_init(kitchen_init);
module_exit(kitchen_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group7");
MODULE_DESCRIPTION("Kitchen module with misc device /dev/kitchen for events");