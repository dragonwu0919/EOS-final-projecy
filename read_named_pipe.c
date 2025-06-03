#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "order_comm.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "使用方式: %s <PIPE_NAME>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 根據用戶指定的名稱生成 Named Pipe 路徑
    char pipe_path[128];
    snprintf(pipe_path, sizeof(pipe_path), "/tmp/%s", argv[1]);

    // 打開 Named Pipe 以進行阻塞式讀取
    int pipe_fd = open(pipe_path, O_RDONLY);
    if (pipe_fd == -1) {
        perror("無法打開 Named Pipe");
        return EXIT_FAILURE;
    }
    printf("Named Pipe %s 已打開，等待資料...\n", pipe_path);

    // 從 Named Pipe 中讀取 struct order
    struct order o;
    while (1) {
        ssize_t bytes_read = read(pipe_fd, &o, sizeof(o));
        if (bytes_read == sizeof(o)) {
            printf("讀取訂單: order_id=%d, menu_idx=%d, name=%s\n",
                   o.order_id, o.menu_idx, o.name);
        } else if (bytes_read == 0) {
            printf("Named Pipe 已關閉，結束讀取\n");
            break;
        } else {
            perror("讀取 Named Pipe 失敗");
            break;
        }
    }

    close(pipe_fd); // 關閉 Named Pipe
    return EXIT_SUCCESS;
}