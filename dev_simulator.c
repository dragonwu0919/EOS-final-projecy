#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>

#define KITCHEN_FIFO "/tmp/dev_kitchen"
#define CHEF_TASKS_FIFO_READ "/tmp/dev_chef_tasks_read"
#define CHEF_TASKS_FIFO_WRITE "/tmp/dev_chef_tasks_write"

pthread_mutex_t screen_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_t kitchen_thread, chef_tasks_thread;
volatile int running = 1; // 用於控制執行緒的運行狀態

void handle_sigint(int sig) {
    if (sig != SIGINT) {
        return; // 忽略非 SIGINT 信號
    }

    printf("\nReceived SIGINT (Ctrl+C), shutting down...\n");
    running = 0; // 設置運行標誌為 0，通知執行緒退出
    pthread_cancel(kitchen_thread); // 強制取消執行緒
    pthread_cancel(chef_tasks_thread);
    unlink(KITCHEN_FIFO); // 刪除 FIFO
    unlink(CHEF_TASKS_FIFO_READ);
    unlink(CHEF_TASKS_FIFO_WRITE);
    exit(0); // 結束程式
}

void *kitchen_simulator(void *arg) {
    int fd;

    printf("Kitchen simulator started. Writing data...\n");
    while (running) {
        fd = open(KITCHEN_FIFO, O_RDWR);
        if (fd < 0) {
            perror("Failed to open /dev/kitchen");
            sleep(1); // 如果打開失敗，稍作等待再重試
            continue;
        }

        // 模擬輸出符合格式的資料
        for (int i = 0; i < 3 && running; i++) {
            int id = i; // 廚師 ID
            int order_id = 100 + i; // 訂單 ID
            const char *type = (i % 2 == 0) ? "START" : "DONE"; // 模擬 START 或 DONE
            const char *menu = (i % 2 == 0) ? "Spaghetti" : "Pizza"; // 模擬菜單名稱

            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%d:%s:%d:%s", id, type, order_id, menu);

            printf("[/dev/kitchen] Writing: %s\n", buffer);

            // 將資料寫入 FIFO
            write(fd, buffer, strlen(buffer));
            sleep(5); // 模擬間隔時間
        }

        close(fd);
    }
    return NULL;
}

struct step_location {
    const char *name;
    int x, y;
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

static struct step_location temp_step_positions[] = {
    { "Cucumber",      28, 26 },
    { "Chop 1",        15, 15 },
    { "Chop 2",        15, 20 },
    { "Cook Rice",     60, 28 },
    { "Plate Dish 1",  91, 12 },
    { "WASH_DISHES",   28,  9 },
    { "Return_DISHES", 60,  7 },
};

void *chef_tasks_simulator(void *arg) {
    int fd_read, fd_write;

    printf("Chef tasks simulator started. Writing tasks...\n");

    // 打開 FIFO，分別用於讀取和寫入
    fd_read = open(CHEF_TASKS_FIFO_READ, O_RDONLY | O_NONBLOCK);
    if (fd_read < 0) {
        perror("Failed to open /dev/chef_tasks_read");
        return NULL;
    }

    fd_write = open(CHEF_TASKS_FIFO_WRITE, O_RDWR);
    if (fd_write < 0) {
        perror("Failed to open /dev/chef_tasks_write");
        close(fd_read);
        return NULL;
    }

    while (running) {
        // 模擬輸出符合格式的任務資料
        for (int i = 0; i < 5 && running; i++) {
            int order_id = 200 + i; // 訂單 ID
            int dish_id = 10 + i;   // 菜品 ID
            int step = i + 1;       // 步驟編號
            const char *step_name = temp_step_positions[
                i % (sizeof(temp_step_positions) / sizeof(temp_step_positions[0]) - 1)
            ].name;

            int x = temp_step_positions[
                i % (sizeof(temp_step_positions) / sizeof(temp_step_positions[0]) - 1)
            ].x;

            int y = temp_step_positions[
                i % (sizeof(temp_step_positions) / sizeof(temp_step_positions[0]) - 1)
            ].y;

            char task_buf[256];
            snprintf(task_buf, sizeof(task_buf), "%d %d %d %d %d", order_id, dish_id, step, x, y);

            // 打印任務資訊
            printf("[/dev/chef_tasks] Writing task: %s (Step: %s)\n", task_buf, step_name);

            // 將任務資料寫入 FIFO
            write(fd_write, task_buf, strlen(task_buf));

            // 不斷等待 ACK
            char ack_buf[32];
            while (1) {
                memset(ack_buf, 0, sizeof(ack_buf));
                int n = read(fd_read, ack_buf, sizeof(ack_buf) - 1);
                if (n > 0) {
                    if(ack_buf[0] == '1' && ack_buf[1] == '0' && ack_buf[2] == '0') {
                        ack_buf[n] = '\0'; // 確保字串結束

                        printf("[/dev/chef_tasks] Received ACK: %s\n", ack_buf);
                        break; // 收到 ACK，跳出等待循環
                    } else{
                        printf("[/dev/chef_tasks] Received unexpected ACK: %s\n", ack_buf);
                    }
                    
                } else {
                    // 打印未收到 ACK 的重試訊息
                    printf("[/dev/chef_tasks] No ACK received, retrying...\n");
                    sleep(5); // 等待一段時間後重試
                }
            }

            sleep(1); // 模擬間隔時間
        }
    }

    close(fd_read);
    close(fd_write);
    return NULL;
}

int main() {
    // 設置 SIGINT 信號處理器
    signal(SIGINT, handle_sigint);

    unlink(KITCHEN_FIFO);
    unlink(CHEF_TASKS_FIFO_READ);
    unlink(CHEF_TASKS_FIFO_WRITE);

    // Create FIFO for KITCHEN_FIFO
    if (mkfifo(KITCHEN_FIFO, 0666) == -1) {
        perror("Failed to create FIFO for /dev/kitchen");
        exit(EXIT_FAILURE);
    }
    if (mkfifo(CHEF_TASKS_FIFO_READ, 0666) == -1) {
        perror("Failed to create FIFO for /dev/chef_tasks_read");
        unlink(KITCHEN_FIFO);
        exit(EXIT_FAILURE);
    }
    if (mkfifo(CHEF_TASKS_FIFO_WRITE, 0666) == -1) {
        perror("Failed to create FIFO for /dev/chef_tasks_write");
        unlink(KITCHEN_FIFO);
        unlink(CHEF_TASKS_FIFO_READ);
        exit(EXIT_FAILURE);
    }

    // Start kitchen simulator thread
    if (pthread_create(&kitchen_thread, NULL, kitchen_simulator, NULL) != 0) {
        perror("Failed to create kitchen simulator thread");
        unlink(KITCHEN_FIFO);
        unlink(CHEF_TASKS_FIFO_READ);
        unlink(CHEF_TASKS_FIFO_WRITE);
        exit(EXIT_FAILURE);
    }

    // Start chef tasks simulator thread
    if (pthread_create(&chef_tasks_thread, NULL, chef_tasks_simulator, NULL) != 0) {
        perror("Failed to create chef tasks simulator thread");
        unlink(KITCHEN_FIFO);
        unlink(CHEF_TASKS_FIFO_READ);
        unlink(CHEF_TASKS_FIFO_WRITE);
        exit(EXIT_FAILURE);
    }

    // Wait for threads to finish
    pthread_join(kitchen_thread, NULL);
    pthread_join(chef_tasks_thread, NULL);

    // 清理 FIFO
    unlink(KITCHEN_FIFO);
    unlink(CHEF_TASKS_FIFO_READ);
    unlink(CHEF_TASKS_FIFO_WRITE);

    return 0;
}