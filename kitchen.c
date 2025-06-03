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
#include <linux/skbuff.h>
#include <linux/net_namespace.h>
#include <net/sock.h>
#include <linux/socket.h>
#include <linux/netlink.h>
#include <linux/in.h>
#include <linux/inet.h> 
#include "order_comm.h"

#define MAX_PLATES 3
#define MAX_CHEFS 3
#define NETLINK_USER 31
#define NMENU 9
#define MAX_ORDER_ID 128
#define MEAL_SIZE 3

static int plate_count = MAX_PLATES;
static DEFINE_MUTEX(plate_mutex);
static int chef_assigned_plate[MAX_CHEFS];
extern struct order *recv_order(void);
static struct order* chef_queue[3] = {NULL, NULL, NULL};
static DEFINE_MUTEX(dispatch_lock);
static atomic_t order_done_count[MAX_ORDER_ID];
static DECLARE_WAIT_QUEUE_HEAD(order_done_wq);
static DEFINE_MUTEX(order_done_mutex);
static struct task_struct *dispatch_thread;
static int dispatch_thread_fn(void *data);

void get_step_position(const char* step_name, int *x, int *y);

/* 各工作區的信號量 */
static struct semaphore sem_ingr;    // 食材區 (1)
static struct semaphore sem_cut;     // 砧板區 (2)
static struct semaphore sem_stove;   // 瓦斯爐 (1)
static struct semaphore sem_plate;   // 擺盤區 (3)
static struct semaphore sem_return;  // 回收區 (1)
static struct semaphore sem_sink;    // 流理臺 (1)

static struct socket *tcp_sock = NULL;

struct plate {
    int id;
    int in_use;
    int use_count;
    struct semaphore sem;
    struct mutex lock;
};
static struct plate plates[MAX_PLATES];

int connect_to_vm_monitor(const char *ip, int port) {
    struct sockaddr_in addr;
    int ret;

    ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &tcp_sock);
    if (ret < 0) {
        pr_err("❌ socket create failed: %d\n", ret);
        return ret;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = in_aton(ip);  // e.g. "192.168.56.1"

    ret = tcp_sock->ops->connect(tcp_sock, (struct sockaddr *)&addr, sizeof(addr), 0);
    if (ret < 0) {
        pr_err("❌ TCP connect failed: %d\n", ret);
        sock_release(tcp_sock);
        tcp_sock = NULL;
    } else {
        pr_info("✅ Connected to VM %s:%d\n", ip, port);
    }

    return ret;
}

void send_plate_tcp(int plate_id, int count) {
    char msg[64];
    struct msghdr msg_hdr = {
        .msg_flags = MSG_DONTWAIT
    };
    struct kvec iov;

    if (!tcp_sock) return;

    snprintf(msg, sizeof(msg), "PLATE %d USE %d\n", plate_id, count);
    iov.iov_base = msg;
    iov.iov_len = strlen(msg);

    int ret = kernel_sendmsg(tcp_sock, &msg_hdr, &iov, 1, iov.iov_len);
    if (ret < 0)
        pr_warn("send_plate_tcp: blocked or failed, error %d\n", ret);
}

void update_plate_usage(int chef_id, int plate_id) {
    plates[plate_id].use_count++;
    send_plate_tcp(plate_id, plates[plate_id].use_count);
}

static int assign_plate_to_chef(int chef_id) {
    for (int i = 0; i < MAX_PLATES; i++) {
        if (mutex_trylock(&plates[i].lock)) {
            if (!plates[i].in_use) {
                plates[i].in_use = 1;
                chef_assigned_plate[chef_id] = i;
                mutex_unlock(&plates[i].lock);
                return i;
            }
            mutex_unlock(&plates[i].lock);
        }
    }
    return -1;
}

// 【✅函數】釋放廚師所用盤子（最後一次 plate 步驟後）
static void release_chef_plate(int chef_id) {
    int plate_id = chef_assigned_plate[chef_id];
    if (plate_id >= 0 && plate_id < MAX_PLATES) {
        up(&plates[plate_id].sem);
        mutex_lock(&plates[plate_id].lock);
        plates[plate_id].in_use = 0;
        plates[plate_id].use_count = 0;
        mutex_unlock(&plates[plate_id].lock);
        send_plate_tcp(plate_id, 0);
        chef_assigned_plate[chef_id] = -1;
    }
}

static void reset_all_plate_usage(void)
{
    for (int i = 0; i < MAX_PLATES; i++) {
        release_chef_plate(i);
        pr_info("Plate %d usage reset to 0\n", i);
    }
}

void report_order_completion(int order_id) {
    if (order_id < 0 || order_id >= MAX_ORDER_ID) return;

    int done = atomic_inc_return(&order_done_count[order_id]);
    pr_info("Completion: order %d has %d items done\n", order_id, done);

    if (done == MEAL_SIZE) {
        pr_info("🍱 Completion: full meal %d finished!\n", order_id);
        reset_all_plate_usage();
        wake_up(&order_done_wq);  // 通知 dispatcher
    }
}

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
      { "Chop 1",        2, &sem_cut   },
      { "Plate Dish 1",  1, &sem_plate },
      { "Rice",        1, &sem_ingr  },
      { "Cook Rice",   5, &sem_stove },
      { "Plate Dish 1",  1, &sem_plate },
      { "Seaweed",     1, &sem_ingr  },
      { "Plate Dish 1",  1, &sem_plate },
      { NULL,          0, NULL       },
    },
    /* dish_id=2 (menu_idx=1) 生魚片*/
    {
      { "Fish",        1, &sem_ingr  },
      { "Chop 1",        2, &sem_cut   },
      { "Plate Dish 1",  1, &sem_plate },
      { NULL,          0, NULL       }, 
    },
    /* dish_id=3 (menu_idx=2) 魚肉手捲 */
    {
      { "Fish",        1, &sem_ingr  },
      { "Chop 1",        2, &sem_cut   },
      { "Plate Dish 1",  1, &sem_plate },
      { "Rice",        1, &sem_ingr  },
      { "Cook Rice",   5, &sem_stove },
      { "Plate Dish 1",  1, &sem_plate },
      { "Seaweed",     1, &sem_ingr  },
      { "Plate Dish 1",  1, &sem_plate },
      { NULL,          0, NULL       },
    },
    /* dish_id=4 (menu_idx=3) 黃瓜沙拉 */
    {
      { "Cucumber",    1, &sem_ingr  },
      { "Chop 2",        2, &sem_cut   },
      { "Plate Dish 2",  1, &sem_plate },
      { "Cabbage",     1, &sem_ingr  },
      { "Chop 2",        2, &sem_cut   },
      { "Plate Dish 2",  1, &sem_plate },
      { NULL,          0, NULL       },
    },
    /* dish_id=5 (menu_idx=4) 蕃茄沙拉 */
     {
      { "Tomato",       1, &sem_ingr },
      { "Chop 2",         2, &sem_cut },
      { "Plate Dish 2", 1, &sem_plate },
      { "Cabbage",      1, &sem_ingr },
      { "Chop 2",         2, &sem_cut },
      { "Plate Dish 2", 1, &sem_plate},
      { NULL,           0, NULL       },
    },
    /* dish_id=6 (menu_idx=5) 蕃茄黃瓜沙拉 */
    {
        { "Tomato",       1, &sem_ingr },
        { "Chop 2",         2, &sem_cut },
        { "Plate Dish 2", 1, &sem_plate },
        { "Cucumber",     1, &sem_ingr },
        { "Chop 2",         2, &sem_cut },
        { "Plate Dish 2", 1, &sem_plate},
        { "Cabbage",      1, &sem_ingr },
        { "Chop 2",         2, &sem_cut },
        { "Plate Dish 2", 1, &sem_plate },
     },
     /* dish_id=7 (menu_idx=6) 綠茶 */ 
     {
        { "greentea",       1, &sem_ingr },
        { "Plate Dish 3",   1, &sem_plate },
        { NULL,           0, NULL       },
     },
     /* dish_id=7 (menu_idx=6) 紅茶 */ 
     {
        { "blacktea",       1, &sem_ingr },
        { "Plate Dish 3",   1, &sem_plate },
        { NULL,           0, NULL       },
     },
     /* dish_id=7 (menu_idx=6) 烏龍茶 */ 
     {
        { "oolongtea",       1, &sem_ingr },
        { "Plate Dish 3",    1, &sem_plate },
        { NULL,           0, NULL       },
     },
};
static const int recipe_len[] = { 8, 3, 8, 6, 6, 9, 2, 2, 2};

static const char *menu[] = {
    "Cucuber roll", "Sashimi", "Fish roll", "Cucumber Salad", "Tomato Salad", "Tomato Cucumber Salad", "greentea", "blacktea", "oolongtea"
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
    { "greentea",      28, 26 },
    { "blacktea",      28, 26 },
    { "oolongtea",     28, 26 },
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

#define NUM_CHEFS 3
struct chef_ack_slot {
    struct order_ack ack;
    int ready;  // 1 表示該 slot 中有資料可用
};

static struct chef_ack_slot ack_slots[NUM_CHEFS];
static DECLARE_WAIT_QUEUE_HEAD(ack_slot_wq);

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

    pr_info("notify_event: %s", buf);  // ✅ 加這一行看看 kernel log 有沒有
    kfifo_in(&event_log_fifo, buf, len);
    wake_up_interruptible(&kitchen_wq);
}

/* 用戶空間 read() */
static ssize_t kitchen_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct order_event ev;

    // 若是非阻塞模式且 FIFO 為空，直接返回 EAGAIN
    if ((file->f_flags & O_NONBLOCK) && kfifo_is_empty(&kitchen_fifo)) {
        pr_info("kitchen_read (nonblock): no data\n");
        return -EAGAIN;
    }

    // 阻塞等待直到 FIFO 非空
    if (wait_event_interruptible(kitchen_wq, !kfifo_is_empty(&kitchen_fifo))) {
        pr_info("kitchen_read interrupted\n");
        return -ERESTARTSYS; // 被 signal 中斷
    }

    // 從 FIFO 讀出一筆 event
    if (kfifo_out(&kitchen_fifo, &ev, 1) != 1) {
        pr_warn("kitchen_read: kfifo_out failed\n");
        return -EFAULT;
    }

    // 複製給 user space
    if (copy_to_user(buf, &ev, sizeof(ev))) {
        pr_warn("kitchen_read: copy_to_user failed\n");
        return -EFAULT;
    }

    pr_info("kitchen_read: sent event to user\n");

    return sizeof(ev);
}

/* 用戶空間 write()，接收 ACK 字串 */
static ssize_t kitchen_write(struct file *file,
                             const char __user *buf,
                             size_t count, loff_t *ppos)
{
    struct order_ack ack;
    if (count != sizeof(ack))
        return -EINVAL;

    if (copy_from_user(&ack, buf, sizeof(ack)))
        return -EFAULT;

    if (ack.chef_id < 0 || ack.chef_id >= NUM_CHEFS)
        return -EINVAL;

    ack_slots[ack.chef_id].ack = ack;
    ack_slots[ack.chef_id].ready = 1;

    wake_up_interruptible(&ack_slot_wq);
    pr_info("kitchen_write: received ack chef=%d step=%d\n",
            ack.chef_id, ack.step_idx);

    return sizeof(ack);
}


static unsigned int kitchen_poll(struct file *file, poll_table *wait)
{
    unsigned int mask = 0;
    pr_info("kitchen_poll: FIFO empty? %d\n", kfifo_is_empty(&kitchen_fifo));
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
    pr_info("[kernel] Got GUI ACK: completed = %d\n", completed);
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
    pr_warn("⚠️ get_step_position: unknown step '%s', defaulting to (50,12)\n", step_name);
    *x = 50;  // fallback 預設座標
    *y = 12;
}

static int chef_fn(void *data) {
    int id = (int)(long)data;
    struct order *o;
    pr_info("Chef[%d] waiting for task...\n", id);

    while (!kthread_should_stop()) {
        while (chef_queue[id] == NULL && !kthread_should_stop())
            ssleep(1);  // 等待任務派送

        if (kthread_should_stop())
            break;
        pr_info("Chef[%d] calling recv_order...\n", id);

        o = chef_queue[id];
        chef_queue[id] = NULL;  // 立即清空 slot

        if (!o) {
            pr_warn("Chef[%d] recv_order returned NULL\n", id);
            continue;
        }

        pr_info("Chef[%d] received order %d [%s]\n", id, o->order_id, (o->menu_idx < NMENU ? menu[o->menu_idx] : "Unknown"));
        notify_event("%d:ORDER_START:%d:%s\n", id, o->order_id, (o->menu_idx < NMENU ? menu[o->menu_idx] : "Unknown"));

        int idx = o->menu_idx;
        int steps = recipe_len[idx];
        int assigned_plate = -1;

        for (int s = 0; s < steps; s++) {
            step_t *st = &recipes[idx][s];
            int x, y;
            get_step_position(st->name, &x, &y);

            struct order_event ev = {
                .chef_id   = id,
                .step_idx  = s,
                .x         = x,
                .y         = y,
                .pause_sec = st->duration
            };

            if (kfifo_in(&kitchen_fifo, &ev, 1) != 1) {
                pr_warn("kitchen: kfifo_in failed\n");
            } else {
                pr_info("kitchen: push event chef=%d step=%d pos=(%d,%d)\n", ev.chef_id, ev.step_idx, ev.x, ev.y);
            }
            wake_up_interruptible(&kitchen_wq);

            // ✅ 模擬 GUI 完成任務的等待時間（取代 ACK 機制）
            ssleep(1);

            // 分配盤子邏輯
            if (st->sem == &sem_plate && assigned_plate == -1) {
                while ((assigned_plate = assign_plate_to_chef(id)) == -1)
                    ssleep(1);
            }

            if (st->sem == &sem_plate && assigned_plate >= 0) {
            update_plate_usage(id, assigned_plate);
            }
            
            // 執行步驟
            down(st->sem);
            ssleep(st->duration);

            if (st->sem != &sem_plate) up(st->sem);

            notify_event("%d:STEP_DONE:%d:%s\n", id, o->order_id, st->name);
        }

        notify_event("%d:ORDER_DONE:%d\n", id, o->order_id);
        pr_info("Chef[%d] done  order %d\n", id, o->order_id);

        notify_event("%d:STEP_START:%d:WASH_DISHES\n", id, o->order_id);
        down(&sem_sink);
        ssleep(3);
        up(&sem_sink);
        notify_event("%d:STEP_DONE:%d:WASH_DISHES\n", id, o->order_id);

        //release_chef_plate(id);

        // ✅ 回報完成（例如更新完成計數器）
        report_order_completion(o->order_id);

        kfree(o);
    }
    return 0;
}

static int dispatch_thread_fn(void *data) {
    int current_order_id = 1;
     /* —— 第一步：立刻拿第一份套餐（三道菜）並分派 —— */
    for (int meal_i = 0; meal_i < MEAL_SIZE; meal_i++) {
        struct order *o = recv_order();
        if (!o) {
            pr_warn("Dispatcher: failed to recv first meal item %d\n", meal_i);
            return 0;
        }

        /* 分派給空閒的 Chef */
        mutex_lock(&dispatch_lock);
        int assigned = 0;
        while (!assigned && !kthread_should_stop()) {
            for (int i = 0; i < MAX_CHEFS; i++) {
                if (chef_queue[i] == NULL) {
                    chef_queue[i] = o;
                    pr_info("Dispatcher: assigned first meal order %d [%s] to chef %d\n",
                            o->order_id, menu[o->menu_idx], i);
                    assigned = 1;
                    break;
                }
            }
            if (!assigned)
                ssleep(1);
        }
        mutex_unlock(&dispatch_lock);
    }

    while (!kthread_should_stop()) {
        // 等待當前套餐完成
        wait_event(order_done_wq,
                   atomic_read(&order_done_count[current_order_id]) >= MEAL_SIZE ||
                   kthread_should_stop());

        if (kthread_should_stop()) break;
        
        /* 重設完成計數，準備記錄下一份 */
        atomic_set(&order_done_count[current_order_id], 0);
        current_order_id++;

        for (int meal_i = 0; meal_i < MEAL_SIZE; meal_i++) {
            struct order *o = recv_order();
            if (!o) break;

            mutex_lock(&dispatch_lock);  // 避免多重 dispatch 競爭

            int assigned = 0;
            while (!assigned && !kthread_should_stop()) {
                for (int i = 0; i < MAX_CHEFS; i++) {
                    if (chef_queue[i] == NULL) {
                        chef_queue[i] = o;
                        pr_info("Dispatcher: assigned order %d [%s] to chef %d\n",
                                o->order_id, menu[o->menu_idx], i);
                        assigned = 1;
                        break;
                    }
                }
                if (!assigned)
                    ssleep(1);  // 等待空位
            }
            mutex_unlock(&dispatch_lock);
        }
    }
    return 0;
}

static int __init kitchen_init(void)
{
    connect_to_vm_monitor("192.168.222.150", 8888);  // ← 替換成 VM IP
    mutex_init(&dispatch_lock);
    dispatch_thread = kthread_run(dispatch_thread_fn, NULL, "dispatch_thread");

    int i, ret;
    pr_info("recv_order symbol address: %px\n", recv_order);
    /* 初始化工作區信號量 */
    sema_init(&sem_ingr,   1);
    sema_init(&sem_cut,    2);
    sema_init(&sem_stove,  1);
    sema_init(&sem_plate,  3);
    sema_init(&sem_return, 1);
    sema_init(&sem_sink,   1);

    // 初始化 plate pool
    for (i = 0; i < MAX_PLATES; i++) {
        plates[i].id = i;
        plates[i].in_use = 0;
        sema_init(&plates[i].sem, 1);
        mutex_init(&plates[i].lock);
    }

    for (i = 0; i < MAX_CHEFS; i++) {
        chef_assigned_plate[i] = -1;
        ack_slots[i].ready = 0;
    }

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
    chef_thread[i] = kthread_run(chef_fn, (void *)(long)i, "chef_%d", i);
    if (IS_ERR(chef_thread[i])) {
        pr_err("kitchen: failed to start chef_%d (error %ld)\n",
               i, PTR_ERR(chef_thread[i]));
        chef_thread[i] = NULL;
    } else {
        pr_info("kitchen: chef_%d thread started\n", i);
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
