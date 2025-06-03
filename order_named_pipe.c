#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "order_comm.h"

static char pipe_path[128]; // Named Pipe 的路徑
static int pipe_fd = -1;    // Named Pipe 的文件描述符

// 餐點名稱表
static const char *menu_names[] = {
    "Fried Rice", "Noodles", "Soup", "Salad", "Pizza", "Burger", "Steak", "Sushi", "Pasta", "Curry"
};

// 信號處理函數，用於清理 Named Pipe
void cleanup_pipe(int sig) {
    if (pipe_fd != -1) {
        close(pipe_fd);
    }
    unlink(pipe_path); // 刪除 Named Pipe
    printf("Named Pipe %s 已刪除\n", pipe_path);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "使用方式: %s <PIPE_NAME>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 根據用戶指定的名稱生成 Named Pipe 路徑
    snprintf(pipe_path, sizeof(pipe_path), "/tmp/%s", argv[1]);

    // 設置信號處理函數，捕捉 Ctrl+C (SIGINT)
    signal(SIGINT, cleanup_pipe);

    // 創建 Named Pipe
    if (mkfifo(pipe_path, 0666) == -1) {
        perror("無法創建 Named Pipe");
        return EXIT_FAILURE;
    }
    printf("Named Pipe %s 已創建\n", pipe_path);

    // 打開 Named Pipe 以進行阻塞式寫入
    pipe_fd = open(pipe_path, O_WRONLY);
    if (pipe_fd == -1) {
        perror("無法打開 Named Pipe");
        unlink(pipe_path); // 清理 Named Pipe
        return EXIT_FAILURE;
    }

    // 初始化隨機數生成器
    srand(time(NULL));

    // 不斷傳遞 struct order
    int order_id = 0;
    while (1) {
        int num_menu_items = rand() % 3 + 1; // 隨機生成 1~3 個 menu_id
        
        for (int i = 0; i < num_menu_items; i++) {
            struct order o = {
                .order_id = order_id,
                .menu_idx = rand() % 10, // 隨機生成 menu_id (0~9)
            };
            strncpy(o.name, menu_names[o.menu_idx], sizeof(o.name) - 1);

            printf("向 Named Pipe 傳遞訂單: order_id=%d, menu_idx=%d, name=%s\n",
                   o.order_id, o.menu_idx, o.name);

            // 阻塞式寫入
            ssize_t bytes_written = write(pipe_fd, &o, sizeof(o));
            if (bytes_written != sizeof(o)) {
                perror("寫入 Named Pipe 失敗");
                cleanup_pipe(SIGINT); // 清理 Named Pipe
            }
        }

        order_id++; // 訂單 ID 遞增
        sleep(5);   // 每次迴圈等待 5 秒
    }

    // 關閉 Named Pipe
    close(pipe_fd);
    unlink(pipe_path); // 刪除 Named Pipe
    printf("Named Pipe %s 已刪除\n", pipe_path);

    return EXIT_SUCCESS;
}