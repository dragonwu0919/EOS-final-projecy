#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include "order_comm.h"
#include <linux/kthread.h>

#define MAX_ORDERS    4
#define ORDER_BUFSIZE 16

static struct order *buf[MAX_ORDERS];
static int head, tail;
static DEFINE_MUTEX(buf_mutex);
static struct semaphore order_sem;
static DECLARE_WAIT_QUEUE_HEAD(order_wq);

/*
 * add_order: 將一筆 order 推入環形佇列，成功回傳 0，滿了回傳 -ENOSPC
 */
int add_order(struct order *o)
{
    int next;

    mutex_lock(&buf_mutex);
    next = (tail + 1) % MAX_ORDERS;
    if (next == head) {
        mutex_unlock(&buf_mutex);
        return -ENOSPC;
    }
    buf[tail] = o;
    tail = next;
    up(&order_sem);  /* 通知有新單 */
    mutex_unlock(&buf_mutex);
    return 0;
}
EXPORT_SYMBOL(add_order);

/*
 * recv_order: 可中斷等待直到有新單，然後從環形佇列取出一筆。
 */
struct order *recv_order(void)
{
    struct order *o;

    // ✅ 改用 down_interruptible：可被 signal 中斷（例如 kthread_stop）
    if (down_interruptible(&order_sem)) {
        // ⚠️ 若中斷且 thread 準備退出，就不取單
        if (kthread_should_stop())
            return NULL;
        else
            return NULL;  // 或視情況重試
    }

    mutex_lock(&buf_mutex);
    o = buf[head];
    head = (head + 1) % MAX_ORDERS;
    mutex_unlock(&buf_mutex);
    return o;
}
EXPORT_SYMBOL(recv_order);


/* misc device: /dev/order 用於用戶空間讀取 order_id */
static struct miscdevice order_dev;

static ssize_t order_dev_read(struct file *file, char __user *buf_user,
                              size_t count, loff_t *ppos)
{
    struct order *o;
    char kbuf[ORDER_BUFSIZE];
    int len;

    /* 非阻塞模式下若無訂單則立即返回 EAGAIN */
    if ((file->f_flags & O_NONBLOCK) &&
        !down_trylock(&order_sem))
        return -EAGAIN;

    /* 阻塞直到有新訂單 */
    down(&order_sem);
    mutex_lock(&buf_mutex);
    o = buf[head];
    head = (head + 1) % MAX_ORDERS;
    mutex_unlock(&buf_mutex);

    /* 格式化成 "<order_id>\n" */
    len = snprintf(kbuf, ORDER_BUFSIZE, "%d\n", o->order_id);
    if (len > count)
        len = count;

    if (copy_to_user(buf_user, kbuf, len)) {
        kfree(o);
        return -EFAULT;
    }
    kfree(o);
    return len;
}

static unsigned int order_dev_poll(struct file *file, poll_table *wait)
{
    unsigned int mask = 0;

    /* 註冊等待隊列 */
    //poll_wait(file, &order_sem.wait, wait);
    poll_wait(file, &order_wq, wait);
    /* 嘗試不阻塞取一部份 sema，檢查是否有訂單 */
    
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

static int __init order_receive_init(void)
{
    int ret;

    /* 初始化環形佇列與信號量 */
    head = tail = 0;
    sema_init(&order_sem, 0);

    /* 註冊 misc device，創建 /dev/order */
    order_dev.minor = MISC_DYNAMIC_MINOR;
    order_dev.name  = "order";
    order_dev.fops  = &order_fops;
    ret = misc_register(&order_dev);
    if (ret) {
        pr_err("order_receive: misc_register failed (%d)\n", ret);
        return ret;
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
MODULE_DESCRIPTION("Order Receive Module with /dev/order character device (non-blocking and poll support)");

