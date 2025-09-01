// server_multi_fixed.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define PORT 12345
#define MAX_CLIENTS 10
#define BUF_SIZE 1024

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_sockets[MAX_CLIENTS] = {0};
    char buffer[BUF_SIZE];
    char client_names[MAX_CLIENTS][32]; // 每個 client 名稱

    fd_set readfds;
    int max_sd, activity, i, sd;

    // 1️⃣ 建立 server socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket failed"); exit(EXIT_FAILURE); }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed"); exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen failed"); exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);

    while(1) {
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);
        max_sd = server_fd;

        for (i = 0; i < MAX_CLIENTS; i++) {
            sd = client_sockets[i];
            if (sd > 0) FD_SET(sd, &readfds);
            if (sd > max_sd) max_sd = sd;
        }

        activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) { perror("select error"); continue; }

        // 🔹 新 client 連線
        if (FD_ISSET(server_fd, &readfds)) {
            client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (client_fd < 0) { perror("accept"); exit(EXIT_FAILURE); }
            printf("New client connected: %d\n", client_fd);

            for (i = 0; i < MAX_CLIENTS; i++) {
                if (client_sockets[i] == 0) {
                    client_sockets[i] = client_fd;
                    snprintf(client_names[i], sizeof(client_names[i]), "Client%d", client_fd);
                    break;
                }
            }
        }

        // 🔹 server 終端輸入
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(buffer, BUF_SIZE, stdin) != NULL) {
                buffer[strcspn(buffer, "\n")] = 0;
                if (strcmp(buffer, "/quit") == 0) break;

                // 廣播 server 訊息
                for (i = 0; i < MAX_CLIENTS; i++) {
                    sd = client_sockets[i];
                    if (sd > 0) send(sd, buffer, strlen(buffer), 0);
                }
            } else {
                printf("Input error or EOF detected. Exiting server.\n");
                break;
            }
        }

        // 🔹 client 發送訊息
        for (i = 0; i < MAX_CLIENTS; i++) {
            sd = client_sockets[i];
            if (sd > 0 && FD_ISSET(sd, &readfds)) {
                int valread = recv(sd, buffer, BUF_SIZE, 0);
                if (valread <= 0) {
                    printf("%s disconnected\n", client_names[i]);
                    close(sd);
                    client_sockets[i] = 0;
                } else {
                    buffer[valread] = '\0';
                    printf("[%s] %s\n", client_names[i], buffer);

                    // 廣播給其他 client
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (client_sockets[j] > 0 && j != i) {
                            char msg_with_name[BUF_SIZE + 64];
                            snprintf(msg_with_name, sizeof(msg_with_name), "[%s] %s", client_names[i], buffer);
                            send(client_sockets[j], msg_with_name, strlen(msg_with_name), 0);
                        }
                    }
                }
            }
        }
    }

    // 🔹 關閉所有 client socket
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] > 0) close(client_sockets[i]);
    }
    close(server_fd);
    printf("Server exited.\n");
    return 0;
}

