/* client.c
 * 功能：TCP Chat Client（可自訂暱稱）
 *
 * 行為：
 *   1) 啟動後先詢問使用者暱稱，連線成功即送出 "NICK <name>"。
 *   2) 使用 select() 同時監聽 socket（伺服器訊息）和 stdin（鍵盤輸入）。
 *   3) 鍵盤輸入：
 *       - "/quit" -> 主動斷線並結束程式
 *       - "/name 新名" -> 會轉換成 "NICK 新名" 送給 Server，用來更改暱稱
 *       - 其他文字 -> 原樣送出給 Server（Server 會處理換行與格式化）
 *   4) 伺服器傳來的訊息：原樣印出，不自動加換行
 *
 * 編譯： gcc -O2 -Wall -Wextra -o client client.c
 * 使用： ./client <server-host> <port>
 *
 * 範例：
 *   ./client 127.0.0.1 12345
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

#define BUFSIZE 4096   // 緩衝區大小（接收/傳送訊息的暫存空間）
#define NAMELEN 32     // 暱稱最大長度

/* 
 * 功能：移除字串末尾的 '\n' 或 '\r'
 * 用途：處理 fgets() 讀入的輸入字串，避免多餘換行影響訊息格式。
 */
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[--n] = '\0';
    }
}

int main(int argc, char *argv[]) {
    // 驗證參數，必須有 server-host 與 port
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server-host> <port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    const char *port = argv[2];

    // 取得使用者輸入的暱稱
    char myname[NAMELEN];
    printf("Enter your name: ");
    fflush(stdout);
    if (!fgets(myname, sizeof(myname), stdin)) {
        fprintf(stderr, "no name input\n");
        return 1;
    }
    trim_crlf(myname);
    if (myname[0] == '\0') snprintf(myname, sizeof(myname), "anon"); // 如果沒輸入，給匿名名 anon

    // ----------- 建立 TCP 連線 -----------

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;      // 強制使用 IPv4（可改 AF_UNSPEC 支援 IPv6）
    hints.ai_socktype = SOCK_STREAM;  // TCP socket

    // getaddrinfo 解析 host 與 port
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return 1;
    }

    int sockfd = -1;
    for (p = res; p; p = p->ai_next) {
        // 嘗試建立 socket
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;
        // 嘗試連線
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) break; // 成功
        // 失敗則關閉並繼續下一個候選
        close(sockfd); sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        fprintf(stderr, "Unable to connect\n");
        return 1;
    }

    printf("Connected to %s:%s as '%s'\n", host, port, myname);
    fflush(stdout);

    // 連線成功後，先告訴 Server 我的暱稱
    {
        char nickbuf[BUFSIZE];
        int n = snprintf(nickbuf, sizeof(nickbuf), "NICK %s", myname);
        send(sockfd, nickbuf, (size_t)n, 0);
    }

    // ----------- 進入主迴圈，使用 select 監聽 socket 與 stdin -----------

    fd_set readfds;
    char buf[BUFSIZE];

    for (;;) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds); // 監聽鍵盤輸入
        FD_SET(sockfd, &readfds);       // 監聽 server 訊息
        int maxfd = (sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO);

        // select 等待任一事件發生
        int nready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (nready < 0) {
            if (errno == EINTR) continue; // 若被訊號中斷，重試
            perror("select");
            break;
        }

        // ----------- Case 1: Server 傳來訊息 -----------
        if (FD_ISSET(sockfd, &readfds)) {
            ssize_t n = recv(sockfd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                // n == 0 -> server 關閉連線
                if (n == 0) printf("\nServer closed connection\n");
                else perror("recv");
                break;
            }
            buf[n] = '\0';
            // 伺服器的訊息已包含換行，因此 client 直接印出即可
            printf("%s", buf);  // 注意：不額外加 '\n'
            fflush(stdout);
        }

        // ----------- Case 2: 使用者鍵盤輸入 -----------
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(buf, sizeof(buf), stdin)) {
                // stdin EOF（例如 Ctrl+D）
                printf("\nstdin EOF, closing\n");
                break;
            }

            // --- 處理特殊指令 ---
            if (strncmp(buf, "/quit", 5) == 0) {
                printf("bye\n");
                break;
            }
            if (strncmp(buf, "/name ", 6) == 0) {
                // 將 "/name 新名" 轉換為 "NICK 新名" 送給 server
                char *newname = buf + 6;
                trim_crlf(newname);
                if (*newname) {
                    char out[BUFSIZE];
                    int m = snprintf(out, sizeof(out), "NICK %s", newname);
                    send(sockfd, out, (size_t)m, 0);
                    snprintf(myname, sizeof(myname), "%s", newname); // 本地也更新
                }
                continue; // 處理完指令後跳過
            }

            // --- 一般聊天訊息 ---
            // 這裡不移除換行，直接送出，server 收到後會處理換行
            ssize_t wn = send(sockfd, buf, strlen(buf), 0);
            if (wn < 0) { perror("send"); break; }
        }
    }

    // ----------- 收尾，關閉 socket -----------

    close(sockfd);
    return 0;
}
