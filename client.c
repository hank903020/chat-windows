/* client.c
 * 功能：TCP Chat Client（可自訂暱稱）
 * 行為：
 *   1) 啟動後先詢問使用者暱稱，連線成功即送出 "NICK <name>"
 *   2) 之後用 select() 同時讀鍵盤與 socket
 *   3) 鍵盤輸入：
 *       - "/quit" 離線
 *       - "/name 新名" 會送出 "NICK 新名" 給 Server
 *       - 其他文字原樣送出（含換行也沒關係，Server 會去掉 \r\n）
 *   4) 伺服器傳來的訊息「原樣印出、不自動加換行」
 *
 * 編譯：gcc -O2 -Wall -Wextra -o client client.c
 * 使用：./client <server-host> <port>  ./client_named 127.0.0.1 12345
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#define BUFSIZE 4096
#define NAMELEN 32

/* 同 server 的 trim_crlf 功能 */
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server-host> <port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    const char *port = argv[2];

    // 取得暱稱
    char myname[NAMELEN];
    printf("Enter your name: ");
    fflush(stdout);
    if (!fgets(myname, sizeof(myname), stdin)) {
        fprintf(stderr, "no name input\n");
        return 1;
    }
    trim_crlf(myname);
    if (myname[0] == '\0') snprintf(myname, sizeof(myname), "anon");

    // 解析位址
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return 1;
    }

    int sockfd = -1;
    for (p = res; p; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sockfd); sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        fprintf(stderr, "Unable to connect\n");
        return 1;
    }

    printf("Connected to %s:%s as '%s'\n", host, port, myname);
    fflush(stdout);

    // 連線成功先把暱稱送給 Server
    {
        char nickbuf[BUFSIZE];
        int n = snprintf(nickbuf, sizeof(nickbuf), "NICK %s", myname);
        send(sockfd, nickbuf, (size_t)n, 0);
    }

    fd_set readfds;
    char buf[BUFSIZE];

    for (;;) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sockfd, &readfds);
        int maxfd = (sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO);

        int nready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (nready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // 伺服器傳來的資料：原樣印出（不自動加換行）
        if (FD_ISSET(sockfd, &readfds)) {
            ssize_t n = recv(sockfd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                if (n == 0) printf("\nServer closed connection\n");
                else perror("recv");
                break;
            }
            buf[n] = '\0';
            printf("%s", buf);  // 不加 '\n'
            fflush(stdout);
        }

        // 鍵盤輸入：轉送給 Server
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(buf, sizeof(buf), stdin)) {
                printf("\nstdin EOF, closing\n");
                break;
            }
            // 指令處理
            if (strncmp(buf, "/quit", 5) == 0) {
                printf("bye\n");
                break;
            }
            if (strncmp(buf, "/name ", 6) == 0) {
                // /name 新名 -> 轉成 NICK 新名
                char *newname = buf + 6;
                trim_crlf(newname);
                if (*newname) {
                    char out[BUFSIZE];
                    int m = snprintf(out, sizeof(out), "NICK %s", newname);
                    send(sockfd, out, (size_t)m, 0);
                    snprintf(myname, sizeof(myname), "%s", newname);
                }
                continue;
            }

            // 一般聊天訊息：原樣傳送（含換行也沒關係，Server 會去掉）
            ssize_t wn = send(sockfd, buf, strlen(buf), 0);
            if (wn < 0) { perror("send"); break; }
        }
    }

    close(sockfd);
    return 0;
}
