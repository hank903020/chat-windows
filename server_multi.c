// server_multi_serverlabel.c

// 多人聊天室 server（select 版本）

// 變更重點：

//  - Server 終端輸入廣播時，會以 "[server] " 為前綴，並且尾端加上 '\n'，所以 client 顯示會看到 server 標示且換行。

//  - Client 訊息廣播也加上 '\n'，以確保 client 顯示時換行整齊。

// 其餘功能（NICK 設定、多人連線管理、/quit）不變。



#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <unistd.h>

#include <ctype.h>

#include <errno.h>

#include <arpa/inet.h>

#include <sys/socket.h>

#include <sys/select.h>



#define DEFAULT_PORT   12345

#define MAX_CLIENTS    64

#define BUF_SIZE       2048

#define NAME_LEN       32



static void trim_crlf(char *s) {

    size_t n = strlen(s);

    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {

        s[--n] = '\0';

    }

}



static void broadcast_to_all(int *socks, int except_idx, const char *data, size_t len) {

    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (socks[i] > 0 && i != except_idx) {

            ssize_t sent = send(socks[i], data, len, 0);

            (void)sent; // 忽略部分傳送處理（可視情況加重送或錯誤處理）

        }

    }

}



int main(int argc, char **argv) {

    int port = (argc >= 2) ? atoi(argv[1]) : DEFAULT_PORT;



    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) { perror("socket"); return 1; }



    int yes = 1;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));



    struct sockaddr_in addr;

    socklen_t addrlen = sizeof(addr);

    memset(&addr, 0, sizeof(addr));

    addr.sin_family      = AF_INET;

    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    addr.sin_port        = htons(port);



    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {

        perror("bind");

        close(server_fd);

        return 1;

    }

    if (listen(server_fd, 16) < 0) {

        perror("listen");

        close(server_fd);

        return 1;

    }



    int clients[MAX_CLIENTS] = {0};

    char names[MAX_CLIENTS][NAME_LEN];

    for (int i = 0; i < MAX_CLIENTS; i++) names[i][0] = '\0';



    printf("Server listening on port %d ... (/quit to stop)\n", port);



    fd_set readfds;

    int maxfd;

    char buf[BUF_SIZE];



    for (;;) {

        FD_ZERO(&readfds);

        FD_SET(server_fd, &readfds);

        FD_SET(STDIN_FILENO, &readfds);

        maxfd = server_fd;



        for (int i = 0; i < MAX_CLIENTS; i++) {

            if (clients[i] > 0) {

                FD_SET(clients[i], &readfds);

                if (clients[i] > maxfd) maxfd = clients[i];

            }

        }



        int nready = select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if (nready < 0) {

            if (errno == EINTR) continue;

            perror("select");

            break;

        }



        // 新連線

        if (FD_ISSET(server_fd, &readfds)) {

            struct sockaddr_in caddr;

            socklen_t clen = sizeof(caddr);

            int cfd = accept(server_fd, (struct sockaddr*)&caddr, &clen);

            if (cfd < 0) { perror("accept"); continue; }



            int slot = -1;

            for (int i = 0; i < MAX_CLIENTS; i++) {

                if (clients[i] == 0) { slot = i; break; }

            }

            if (slot < 0) {

                const char *msg = "Server full.\n";

                send(cfd, msg, strlen(msg), 0);

                close(cfd);

            } else {

                clients[slot] = cfd;

                snprintf(names[slot], NAME_LEN, "anon%d", cfd);

                printf("New client fd=%d at slot=%d name=%s\n", cfd, slot, names[slot]);

            }

        }



        // Server 終端輸入（**改為帶 [server] 前綴 並加換行**）

        if (FD_ISSET(STDIN_FILENO, &readfds)) {

            if (!fgets(buf, sizeof(buf), stdin)) {

                printf("stdin EOF. shutting down.\n");

                break;

            }

            trim_crlf(buf);

            if (strcmp(buf, "/quit") == 0) break;



            // 這裡把伺服器輸入包成 "[server] <msg>\n" 再廣播

            char out[BUF_SIZE + 16];

            int m = snprintf(out, sizeof(out), "[server] %s\n", buf); // <-- 這是關鍵改動：加上 [server] 與換行

            if (m < 0) m = 0;

            broadcast_to_all(clients, -1, out, (size_t)m);

        }



        // 處理 client 資料

        for (int i = 0; i < MAX_CLIENTS; i++) {

            int sd = clients[i];

            if (sd <= 0) continue;

            if (!FD_ISSET(sd, &readfds)) continue;



            int n = recv(sd, buf, sizeof(buf) - 1, 0);

            if (n <= 0) {

                printf("Client %s (fd=%d) disconnected.\n", names[i], sd);

                close(sd);

                clients[i] = 0;

                names[i][0] = '\0';

                continue;

            }

            buf[n] = '\0';

            trim_crlf(buf); // 移除傳入的換行



            // 處理 NICK 協定

            if (strncmp(buf, "NICK ", 5) == 0) {

                const char *newname = buf + 5;

                if (*newname == '\0') {

                    const char *msg = "Name cannot be empty";

                    send(sd, msg, strlen(msg), 0);

                    continue;

                }

                char clean[NAME_LEN];

                int k = 0;

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

                continue;

            }



            // 一般訊息：server 內印出並廣播給其他 client（**加上換行**）

            printf("[%s] %s\n", names[i], buf);



            char out[BUF_SIZE + NAME_LEN + 8];

            int m = snprintf(out, sizeof(out), "[%s] %s\n", names[i], buf); // <-- 這裡加上 '\n'，確保 client 顯示換行

            if (m < 0) m = 0;

            broadcast_to_all(clients, i, out, (size_t)m);

        }

    }



    // 收尾

    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (clients[i] > 0) close(clients[i]);

    }

    close(server_fd);

    printf("Server exited.\n");

    return 0;

}

