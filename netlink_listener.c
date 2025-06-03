#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

#define PORT 8888

static int server_fd = -1; // 用於追蹤 server socket
static int client_fd = -1; // 用於追蹤 client socket

void handle_sigint(int sig) {
    printf("\n捕捉到 Ctrl+C 信號，正在關閉所有資源...\n");
    if (client_fd != -1) close(client_fd);
    if (server_fd != -1) close(server_fd);
    exit(EXIT_SUCCESS);
}

int main() {
    // 設置信號處理函數
    signal(SIGINT, handle_sigint);

    // 創建 socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("無法創建 socket");
        exit(EXIT_FAILURE);
    }

    // 設置 SO_REUSEADDR 選項，移除冷卻時間
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("無法設置 SO_REUSEADDR");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 配置地址
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    // 綁定地址
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("無法綁定地址");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 開始監聽
    if (listen(server_fd, 1) == -1) {
        perror("無法開始監聽");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("📡 Listening on port %d...\n", PORT);

    // 接受客戶端連線
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) {
        perror("無法接受客戶端連線");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("✅ Client connected!\n");

    // 接收資料
    char buf[128];
    while (1) {
        ssize_t len = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (len <= 0) break;
        buf[len] = '\0';
        printf("📦 %s", buf);  // 顯示來自 kernel 的訊息
    }

    // 關閉資源
    close(client_fd);
    close(server_fd);
    return 0;
}
