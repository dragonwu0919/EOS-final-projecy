#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "order_comm.h"
#include <time.h> // 用於 struct timespec 和 clock_gettime
#include <sys/select.h> // 用於 select 函數

#define MAX_CHEFS 3
#define NMENU 9
#define PIPE_TIMEOUT_SEC 5 // 超時秒數

static pthread_t chefs[MAX_CHEFS];
static pthread_t kitchen_thread;
static int chef_ids[MAX_CHEFS];
static int pipe_fd = -1; // Named Pipe 文件描述符
static const char *pipe_path; // Named Pipe 路徑
static volatile int running = 1; // 用於控制執行緒是否繼續運行
static int chef_pipe[2]; // Unnamed Pipe，用於 kitchen 和 chef 的通信
static pthread_mutex_t pipe_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex 保護 pipe 的讀寫操作
static int socket_fd = -1; // Socket 文件描述符
static pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex 保護 socket 的讀寫操作

// 配方表
typedef struct {
    const char *name;  // 步驟名稱
    int duration;      // 持續秒數
} step_t;

static step_t recipes[9] = {
    { "Cucumber roll", 2 }, // Cucumber roll
    { "Sashimi", 5 },     // Sashimi
    { "Fish roll", 2 },     // Fish roll
    { "Cucumber Salad", 3 }, // Cucumber Salad
    { "Tomato Salad", 3 },   // Tomato Salad
    { "Tomato Cucumber Salad", 3 },   // Tomato Cucumber Salad
    { "Green Tea", 1 },// Green Tea
    { "Black Tea", 1 },// Black Tea
    { "Oolong Tea", 1 }// Oolong Tea
};

// 信號處理函數
void handle_sigint(int sig) {
    printf("\n捕捉到 Ctrl+C 信號，正在關閉所有執行緒和資源...\n");
    running = 0; // 停止所有執行緒
    if (pipe_fd != -1) {
        close(pipe_fd); // 關閉 Named Pipe 文件描述符
    }
    unlink(pipe_path); // 刪除 Named Pipe
    printf("Named Pipe %s 已刪除\n", pipe_path);

    // 關閉 Unnamed Pipe
    close(chef_pipe[0]); // 關閉讀端
    close(chef_pipe[1]); // 關閉寫端

    // 關閉 Socket
    if (socket_fd != -1) {
        close(socket_fd);
    }

    exit(EXIT_SUCCESS);
}

// 廚師執行緒函數
void *chef_fn(void *arg) {
    int chef_id = *(int *)arg;
    struct order o;

    while (running) {
        // 設置超時時間
        struct timeval timeout;
        timeout.tv_sec = PIPE_TIMEOUT_SEC; // 超時秒數
        timeout.tv_usec = 0;

        struct timeval intertimeout;
        intertimeout.tv_sec = 0; // 超時秒數
        intertimeout.tv_usec = 100;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(chef_pipe[0], &read_fds);

        // 使用 select 函數等待管道可讀
        int ready = select(chef_pipe[0] + 1, &read_fds, NULL, NULL, &timeout);
        int inter_ready = 0;
        ssize_t bytes_read;

        if (ready > 0 && FD_ISSET(chef_pipe[0], &read_fds)) {
            // 管道可讀，嘗試讀取訂單
            pthread_mutex_lock(&pipe_mutex); // 保護讀操作
            inter_ready = select(chef_pipe[0] + 1, &read_fds, NULL, NULL, &intertimeout);
            if(inter_ready > 0) {
                bytes_read = read(chef_pipe[0], &o, sizeof(o));
                pthread_mutex_unlock(&pipe_mutex); // 解鎖
            } else {
                pthread_mutex_unlock(&pipe_mutex); // 解鎖
                printf("Chef[%d] Unnamed Pipe 超時，沒有可讀取內容\n", chef_id);
                sleep(1);
                continue; // 繼續下一次迴圈
            }

            if (bytes_read == sizeof(o)) {
                printf("Chef[%d] 接收到訂單: order_id=%d, menu_idx=%d, name=%s\n",
                       chef_id, o.order_id, o.menu_idx, o.name);

                // 執行訂單中的步驟
                step_t steps = recipes[o.menu_idx];
                printf("Chef[%d] 執行步驟: %s (持續 %d 秒)\n", chef_id, steps.name, steps.duration);
                sleep(steps.duration);  // 模擬步驟執行時間

                // 向 Socket 打印目前 order 和完成的 menu
                char message[256];
                snprintf(message, sizeof(message), "Chef[%d] 完成訂單: order_id=%d, step=%s\n",
                            chef_id, o.order_id, steps.name);

                pthread_mutex_lock(&socket_mutex); // 保護 socket 寫操作
                send(socket_fd, message, strlen(message), 0);
                pthread_mutex_unlock(&socket_mutex); // 解鎖
                
                

                printf("Chef[%d] 完成訂單: order_id=%d\n", chef_id, o.order_id);
            } else if (bytes_read == 0) {
                printf("Chef[%d] Unnamed Pipe 已關閉，結束執行\n", chef_id);
                break;
            } else {
                perror("Chef[%d] 讀取訂單失敗");
                break;
            }
        } else if (ready == 0) {
            // 超時，沒有可讀的資料
            printf("Chef[%d] 沒有可讀取內容\n", chef_id);
            sleep(1);
        } else {
            // select 函數出錯
            perror("select 函數失敗");
            break;
        }
    }

    pthread_exit(NULL);
}

// Kitchen 主執行緒函數
void *kitchen_fn(void *arg) {
    pipe_path = (const char *)arg;

    // 打開 Named Pipe 以進行阻塞式讀取
    pipe_fd = open(pipe_path, O_RDONLY);
    if (pipe_fd == -1) {
        perror("無法打開 Named Pipe");
        pthread_exit(NULL);
    }
    printf("Kitchen 已打開 Named Pipe %s，等待資料...\n", pipe_path);

    struct order o;
    while (running) {
        ssize_t bytes_read = read(pipe_fd, &o, sizeof(o));
        if (bytes_read == sizeof(o)) {
            pthread_mutex_lock(&pipe_mutex);
            write(chef_pipe[1], &o, sizeof(o));
            pthread_mutex_unlock(&pipe_mutex);

            printf("Kitchen 接收到訂單: order_id=%d, menu_idx=%d, name=%s\n",
                o.order_id, o.menu_idx, o.name);
        } else if (bytes_read == 0) {
            printf("Kitchen Named Pipe 已關閉，結束讀取\n");
            break;
        } else if (!running) {
            break;
        } else {
            perror("Kitchen 讀取 Named Pipe 失敗");
            break;
        }
    }

    close(pipe_fd); // 關閉 Named Pipe
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "使用方式: %s <PIPE_NAME> <IP> <PORT>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 設置信號處理函數
    signal(SIGINT, handle_sigint);

    // 初始化 Named Pipe 路徑
    char pipe_path[128];
    snprintf(pipe_path, sizeof(pipe_path), "/tmp/%s", argv[1]);

    // 檢查 Named Pipe 是否存在
    struct stat st;
    if (stat(pipe_path, &st) == -1) {
        fprintf(stderr, "Named Pipe %s 不存在，程序退出\n", pipe_path);
        return EXIT_FAILURE;
    }
    printf("Named Pipe %s 已存在，直接使用\n", pipe_path);

    // 創建 Socket
    const char *ip = argv[2];
    int port = atoi(argv[3]);
    struct sockaddr_in server_addr;
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        perror("無法創建 Socket");
        return EXIT_FAILURE;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("無效的 IP 地址");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("無法連接到 Socket");
        close(socket_fd);
        return EXIT_FAILURE;
    }
    printf("成功連接到 Socket，IP: %s, 端口: %d\n", ip, port);

    // 創建 Unnamed Pipe
    if (pipe(chef_pipe) == -1) {
        perror("無法創建 Unnamed Pipe");
        return EXIT_FAILURE;
    }

    // 創建廚師執行緒
    for (int i = 0; i < MAX_CHEFS; i++) {
        chef_ids[i] = i;
        if (pthread_create(&chefs[i], NULL, chef_fn, &chef_ids[i]) != 0) {
            perror("無法創建廚師執行緒");
            return EXIT_FAILURE;
        }
    }

    // 創建 Kitchen 主執行緒
    if (pthread_create(&kitchen_thread, NULL, kitchen_fn, (void *)pipe_path) != 0) {
        perror("無法創建 Kitchen 主執行緒");
        return EXIT_FAILURE;
    }

    // 等待 Kitchen 主執行緒完成
    pthread_join(kitchen_thread, NULL);

    // 等待所有廚師執行緒完成
    for (int i = 0; i < MAX_CHEFS; i++) {
        pthread_join(chefs[i], NULL);
    }

    // 刪除 Named Pipe
    unlink(pipe_path);
    printf("Named Pipe %s 已刪除\n", pipe_path);

    // 關閉 Socket
    close(socket_fd);

    return EXIT_SUCCESS;
}