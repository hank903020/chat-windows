/* client.c
 *
 * 功能：簡單 TCP chat client
 * 說明：
 *  - 連線到 server
 *  - 使用 select() 同時監聽 stdin（鍵盤輸入）與 server socket
 *  - stdin 輸入會送給 server
 *  - server 傳來的訊息會印在終端
 *
 * 編譯：
 *   gcc -O2 -o client client.c
 *
 * 使用：
 *   ./client <server-host> <port>
 * 範例：
 *   ./client chat-server 12345
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* 網路相關 */
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

/* select() */
#include <sys/select.h>

#define BUFSIZE 4096  /* 緩衝區大小 */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server-host> <port>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];  // server 主機名或 IP
    const char *port = argv[2];  // server 埠號

    /* 1) 使用 getaddrinfo 解析 server hostname / port */
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        perror("getaddrinfo");
        return 1;
    }

    /* 2) 嘗試逐一建立 socket 並 connect */
    int sockfd = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;  // socket 建立失敗，嘗試下一個

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) break; // 成功連線
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        fprintf(stderr, "Unable to connect\n");
        return 1;
    }

    printf("Connected to %s:%s\n", host, port);
    fflush(stdout);

    fd_set readfds;   // select 用的 fd 集合
    char buf[BUFSIZE];

    for (;;) {
        FD_ZERO(&readfds);

        /* 3) 監聽 stdin（鍵盤輸入） */
        FD_SET(STDIN_FILENO, &readfds);

        /* 4) 監聽 socket（來自 server 的訊息） */
        FD_SET(sockfd, &readfds);

        int maxfd = (STDIN_FILENO > sockfd ? STDIN_FILENO : sockfd) + 1;

        /* 5) select 阻塞直到有 fd 可讀 */
        int nready = select(maxfd, &readfds, NULL, NULL, NULL);
        if (nready < 0) {
            if (errno == EINTR) continue; // 被訊號中斷，重試
            perror("select");
            break;
        }

        /* 6) server socket 可讀：接收訊息 */
        if (FD_ISSET(sockfd, &readfds)) {
            ssize_t n = recv(sockfd, buf, sizeof(buf)-1, 0);
            if (n <= 0) {
                if (n == 0) printf("Server closed connection\n");
                else perror("recv");
                break;  // 連線關閉或錯誤，跳出迴圈
            }
            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);
        }

        /* 7) stdin 可讀：輸入訊息送給 server */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(buf, sizeof(buf), stdin)) {
                printf("stdin EOF, closing\n");
                break;
            }
            /* 支援離開命令 /quit */
            if (strcmp(buf, "/quit\n") == 0) {
                printf("Quitting (requested)\n");
                break;
            }
            ssize_t wn = send(sockfd, buf, strlen(buf), 0);
            if (wn < 0) {
                perror("send");
                break;
            }
        }
    }

    /* 8) 清理 socket */
    close(sockfd);
    return 0;
}

