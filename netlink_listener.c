#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8888

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 1);
    printf("📡 Listening on port %d...\n", PORT);

    int client_fd = accept(server_fd, NULL, NULL);
    printf("✅ Client connected!\n");

    char buf[128];
    while (1) {
        ssize_t len = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (len <= 0) break;
        buf[len] = '\0';
        printf("📦 %s", buf);  // 顯示來自 kernel 的訊息
    }

    close(client_fd);
    close(server_fd);
    return 0;
}
