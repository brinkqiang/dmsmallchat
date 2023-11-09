#ifndef CHATLIB_H
#define CHATLIB_H

/* Networking. */
int create_socket();
void close_socket(int sockfd);
int send_data(int sockfd, const void* buf, int len);
int receive_data(int sockfd, void* buf, int len);

int createTCPServer(int port);
int socketSetNonBlockNoDelay(int fd);
int acceptClient(int server_socket);
int TCPConnect(char *addr, int port, int nonblock);

/* Allocation. */
void *chatMalloc(size_t size);
void *chatRealloc(void *ptr, size_t size);

#endif // CHATLIB_H
