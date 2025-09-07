// server_multi.c
// 多人聊天室 Server（使用 select() 同時處理多個 client）
//
// 功能說明：
//   1. 使用者 (client) 連上 server 後，可以傳送訊息給其他所有 client。
//   2. server 端輸入的文字會廣播給所有 client，並以 "[server]" 作為前綴，最後加上換行。
//   3. client 也可以用 "NICK <name>" 設定暱稱，server 廣播時會顯示為 "[name]"。
//   4. 支援多人連線，最大數量由 MAX_CLIENTS 控制。
//   5. 在 server 端輸入 "/quit" 可以關閉 server。
//
// 技術重點：
//   - 使用 select() 同時監聽多個 fd：server socket、標準輸入、以及所有 client socket。
//   - 使用陣列 clients[] 來管理所有 client socket，names[][] 來存暱稱。
//   - 每則訊息會在廣播前加上前綴並且補上 '\n'，確保 client 顯示時換行整齊。
//   - "NICK " 協定用來讓 client 設定或更改暱稱。
//   - 如果 client 離線或出錯，server 會清除該連線資源。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define DEFAULT_PORT   12345   // 預設監聽的 TCP port
#define MAX_CLIENTS    64      // 可同時接入的最大 client 數
#define BUF_SIZE       2048    // 訊息緩衝區大小
#define NAME_LEN       32      // 暱稱的最大長度

// 去除字串結尾的 CR/LF (\r 或 \n)，避免處理命令或訊息時出現多餘換行
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[--n] = '\0';
    }
}

// 廣播訊息給所有 client
// except_idx 表示排除某個 client（例如訊息來源者不需要收到回送）
static void broadcast_to_all(int *socks, int except_idx, const char *data, size_t len) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (socks[i] > 0 && i != except_idx) {
            ssize_t sent = send(socks[i], data, len, 0);
            (void)sent; // 這裡忽略部分傳送與錯誤處理，簡化版本
        }
    }
}

int main(int argc, char **argv) {
    // 讀取參數指定的 port，若無則使用 DEFAULT_PORT
    int port = (argc >= 2) ? atoi(argv[1]) : DEFAULT_PORT;

    // 建立 TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    // 設定 SO_REUSEADDR，避免 server 重啟時 bind 失敗
    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // 設定 server 端地址結構
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;           // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 接收所有網卡
    addr.sin_port        = htons(port);       // 監聽的 port

    // 綁定 socket 到指定 port
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    // 開始監聽
    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    // 儲存 client socket 與暱稱
    int clients[MAX_CLIENTS] = {0};
    char names[MAX_CLIENTS][NAME_LEN];
    for (int i = 0; i < MAX_CLIENTS; i++) names[i][0] = '\0';

    printf("Server listening on port %d ... (/quit to stop)\n", port);

    fd_set readfds;
    int maxfd;
    char buf[BUF_SIZE];

    for (;;) {
        // 每次迴圈都要重設 fd_set
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);  // 監聽新連線
        FD_SET(STDIN_FILENO, &readfds); // 監聽鍵盤輸入
        maxfd = server_fd;

        // 把所有 client socket 加入監聽集合
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] > 0) {
                FD_SET(clients[i], &readfds);
                if (clients[i] > maxfd) maxfd = clients[i];
            }
        }

        // 使用 select 等待事件（新連線 / 有資料可讀）
        int nready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (nready < 0) {
            if (errno == EINTR) continue; // 如果被 signal 中斷則重試
            perror("select");
            break;
        }

        // --- 1. 處理新 client 連線 ---
        if (FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(server_fd, (struct sockaddr*)&caddr, &clen);
            if (cfd < 0) { perror("accept"); continue; }

            // 找一個空槽存放新的 client
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] == 0) { slot = i; break; }
            }
            if (slot < 0) {
                // 已達最大人數，拒絕連線
                const char *msg = "Server full.\n";
                send(cfd, msg, strlen(msg), 0);
                close(cfd);
            } else {
                // 接受新連線，預設名稱 anon<fd>
                clients[slot] = cfd;
                snprintf(names[slot], NAME_LEN, "anon%d", cfd);
                printf("New client fd=%d at slot=%d name=%s\n", cfd, slot, names[slot]);
            }
        }

        // --- 2. 處理 server 端輸入 ---
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(buf, sizeof(buf), stdin)) {
                // EOF（例如 Ctrl+D），直接關閉 server
                printf("stdin EOF. shutting down.\n");
                break;
            }
            trim_crlf(buf);
            if (strcmp(buf, "/quit") == 0) break; // "/quit" 指令關閉 server

            // 廣播訊息，格式為 [server] <msg>\n
            char out[BUF_SIZE + 16];
            int m = snprintf(out, sizeof(out), "[server] %s\n", buf);
            if (m < 0) m = 0;
            broadcast_to_all(clients, -1, out, (size_t)m);
        }

        // --- 3. 處理 client 傳來的資料 ---
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int sd = clients[i];
            if (sd <= 0) continue;
            if (!FD_ISSET(sd, &readfds)) continue;

            // 收資料
            int n = recv(sd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                // client 離線或錯誤
                printf("Client %s (fd=%d) disconnected.\n", names[i], sd);
                close(sd);
                clients[i] = 0;
                names[i][0] = '\0';
                continue;
            }
            buf[n] = '\0';
            trim_crlf(buf); // 移除換行

            // 協定：NICK <name> -> 設定暱稱
            if (strncmp(buf, "NICK ", 5) == 0) {
                const char *newname = buf + 5;
                if (*newname == '\0') {
                    const char *msg = "Name cannot be empty";
                    send(sd, msg, strlen(msg), 0);
                    continue;
                }
                char clean[NAME_LEN];
                int k = 0;
                // 過濾掉非印字元與 '['、']'
                for (; *newname && k < NAME_LEN - 1; newname++) {
                    if (isprint((unsigned char)*newname) && *newname != '[' && *newname != ']') {
                        clean[k++] = *newname;
                    }
                }
                clean[k] = '\0';
                if (k == 0) {
                    const char *msg = "Invalid name";
                    send(sd, msg, strlen(msg), 0);
                    continue;
                }
                printf("Client fd=%d set name: %s -> %s\n", sd, names[i], clean);
                snprintf(names[i], NAME_LEN, "%s", clean);
                continue; // 改名不廣播
            }

            // 一般訊息：印在 server 終端，並廣播給其他 client
            printf("[%s] %s\n", names[i], buf);

            char out[BUF_SIZE + NAME_LEN + 8];
            int m = snprintf(out, sizeof(out), "[%s] %s\n", names[i], buf); // 廣播格式
            if (m < 0) m = 0;
            broadcast_to_all(clients, i, out, (size_t)m);
        }
    }

    // --- 收尾，關閉所有 client 與 server socket ---
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] > 0) close(clients[i]);
    }
    close(server_fd);
    printf("Server exited.\n");
    return 0;
}
