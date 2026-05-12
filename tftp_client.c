#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define DATA_SIZE 512
#define MAXBUF (4 + DATA_SIZE)
#define TIMEOUT_SEC 3
#define MAX_RETRIES 5

#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5

int sock;
struct sockaddr_in server_addr;
socklen_t server_len;

struct sockaddr_in tid_addr;
socklen_t tid_len;
int tid_known = 0;

void set_socket_timeout(int sec) {
    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void send_request(int opcode, const char *filename) {
    unsigned char buf[DATA_SIZE];
    int pos = 0;

    buf[pos++] = 0;
    buf[pos++] = (unsigned char)opcode;

    strcpy((char *)&buf[pos], filename);
    pos += strlen(filename);
    buf[pos++] = 0;

    strcpy((char *)&buf[pos], "octet");
    pos += strlen("octet");
    buf[pos++] = 0;

    sendto(sock, buf, pos, 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
}

void send_ack(unsigned short block) {
    unsigned char ack[4];
    ack[0] = 0;
    ack[1] = OP_ACK;
    ack[2] = (block >> 8) & 0xFF;
    ack[3] = block & 0xFF;
    sendto(sock, ack, 4, 0, (struct sockaddr *)&tid_addr, tid_len);
    printf("[ACK] Sent ACK for block %d\n", block);
}

void send_data(unsigned short block, unsigned char *data, int data_len) {
    unsigned char buf[MAXBUF];
    buf[0] = 0;
    buf[1] = OP_DATA;
    buf[2] = (block >> 8) & 0xFF;
    buf[3] = block & 0xFF;
    memcpy(&buf[4], data, data_len);
    sendto(sock, buf, data_len + 4, 0, (struct sockaddr *)&tid_addr, tid_len);
    printf("[DATA] Sent block %d (%d bytes)\n", block, data_len);
}

void send_error(unsigned short error_code, const char *msg) {
    unsigned char buf[DATA_SIZE];
    buf[0] = 0;
    buf[1] = OP_ERROR;
    buf[2] = (error_code >> 8) & 0xFF;
    buf[3] = error_code & 0xFF;
    int pos = 4;
    strcpy((char *)&buf[pos], msg);
    pos += strlen(msg);
    buf[pos++] = 0;
    sendto(sock, buf, pos, 0, (struct sockaddr *)&tid_addr, tid_len);
    printf("[ERROR] Sent error %d: %s\n", error_code, msg);
}

void do_get(const char *filename) {
    unsigned char recv_buf[MAXBUF];
    unsigned short expected_block = 1;
    int retries = 0;
    int file_fd = -1;
    int file_opened = 0;

    send_request(OP_RRQ, filename);
    printf("[RRQ] Requested file: %s\n", filename);

    set_socket_timeout(TIMEOUT_SEC);

    while (1) {
        tid_len = sizeof(tid_addr);
        int n = recvfrom(sock, recv_buf, MAXBUF, 0, (struct sockaddr *)&tid_addr, &tid_len);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                retries++;
                if (retries > MAX_RETRIES) {
                    printf("[!] Timeout. Server not responding.\n");
                    break;
                }
                printf("[!] Timeout. Resending RRQ...\n");
                send_request(OP_RRQ, filename);
                continue;
            } else {
                perror("recvfrom");
                break;
            }
        }

        tid_known = 1;
        retries = 0;

        unsigned short op = (recv_buf[0] << 8) | recv_buf[1];

        if (op == OP_DATA) {
            unsigned short block = (recv_buf[2] << 8) | recv_buf[3];
            int data_len = n - 4;

            if (!file_opened) {
                file_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (file_fd < 0) {
                    perror("open");
                    send_error(0, "Local file error");
                    return;
                }
                file_opened = 1;
            }

            if (block == expected_block) {
                if (write(file_fd, &recv_buf[4], data_len) < 0) {
                    perror("write");
                    send_error(0, "Local write error");
                    break;
                }
                printf("[DATA] Received block %d (%d bytes)\n", block, data_len);
                expected_block++;
            }

            send_ack(block);

            if (data_len < DATA_SIZE) {
                printf("[*] Transfer complete. File saved.\n");
                break;
            }
        } else if (op == OP_ERROR) {
            printf("[ERROR] Server error: %.*s\n", n - 4, &recv_buf[4]);
            break;
        } else {
            printf("[!] Unexpected opcode: %d\n", op);
        }
    }

    if (file_opened) close(file_fd);
}

void do_put(const char *filename) {
    unsigned char recv_buf[MAXBUF];
    unsigned char send_buf[DATA_SIZE];
    unsigned short block = 0;
    int retries = 0;
    int file_fd = -1;
    int bytes_read = 0;
    int transfer_complete = 0;

    file_fd = open(filename, O_RDONLY);
    if (file_fd < 0) {
        perror("open");
        printf("[!] Cannot open local file '%s'\n", filename);
        return;
    }

    send_request(OP_WRQ, filename);
    printf("[WRQ] Sending file: %s\n", filename);

    set_socket_timeout(TIMEOUT_SEC);

    while (!transfer_complete) {
        tid_len = sizeof(tid_addr);
        int n = recvfrom(sock, recv_buf, MAXBUF, 0, (struct sockaddr *)&tid_addr, &tid_len);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                retries++;
                if (retries > MAX_RETRIES) {
                    printf("[!] Timeout. Server not responding.\n");
                    break;
                }
                printf("[!] Timeout. Resending WRQ...\n");
                send_request(OP_WRQ, filename);
                continue;
            } else {
                perror("recvfrom");
                break;
            }
        }

        tid_known = 1;
        retries = 0;

        unsigned short op = (recv_buf[0] << 8) | recv_buf[1];

        if (op == OP_ACK) {
            unsigned short ack_block = (recv_buf[2] << 8) | recv_buf[3];
            printf("[ACK] Received ACK for block %d\n", ack_block);

            if (ack_block == block) {
                bytes_read = read(file_fd, send_buf, DATA_SIZE);
                if (bytes_read < 0) {
                    perror("read");
                    send_error(0, "Local read error");
                    break;
                }
                block++;
                send_data(block, send_buf, bytes_read);

                if (bytes_read < DATA_SIZE) {
                    printf("[*] Last block sent. Waiting for final ACK...\n");
                    transfer_complete = 1;
                }
            } else {
                printf("[!] Unexpected ACK block: %d (expected %d)\n", ack_block, block);
            }
        } else if (op == OP_ERROR) {
            printf("[ERROR] Server error: %.*s\n", n - 4, &recv_buf[4]);
            break;
        } else {
            printf("[!] Unexpected opcode: %d\n", op);
        }
    }

    close(file_fd);
    printf("[*] PUT operation finished.\n");
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <server_ip> <get|put> <filename>\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    char *mode = argv[2];
    char *filename = argv[3];

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(69);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        printf("[!] Invalid IP address: %s\n", server_ip);
        close(sock);
        exit(1);
    }

    printf("=== TFTP Client ===\n");
    printf("Server IP: %s\n", server_ip);
    printf("Mode: %s\n", mode);
    printf("Filename: %s\n", filename);
    printf("====================\n\n");

    if (strcmp(mode, "get") == 0) {
        do_get(filename);
    } else if (strcmp(mode, "put") == 0) {
        do_put(filename);
    } else {
        printf("[!] Unknown mode. Use 'get' or 'put'.\n");
    }

    close(sock);
    return 0;
}