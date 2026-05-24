// echo_server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>

#define PORT 8080
#define BACKLOG 16
#define BUF_SIZE 1024
#define MAX_EVENTS 1024

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0); //server_file_describer
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) //socket_option configuration
    {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // struct sockaddr_in {
    //            sa_family_t    sin_family; /* address family: AF_INET */
    //            in_port_t      sin_port;   /* port in network byte order */
    //            struct in_addr sin_addr;   /* internet address */
    //        };
    
    struct sockaddr_in server_addr; //set IP and port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd,BACKLOG) < 0)
    {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_fd))
    {
        perror("set_nonblocking");
        close(server_fd);
    }
    
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0)
    {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[MAX_EVENTS];
    while (1)
    {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("epoll_wait");
            break;
        }
        struct sockaddr_in client_addr;
        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            if (fd == server_fd)
            {
                socklen_t client_addr_len = sizeof(client_addr);
                int client_fd = accept(server_fd,
                                    (struct sockaddr *)&client_addr,
                                    &client_addr_len);
                if (client_fd < 0)
                {
                    perror("accept");
                    continue;
                }
                if (set_nonblocking(client_fd))
                {
                    perror("set_nonblocking");
                    close(client_fd);
                    continue;
                }
                ev.events  = EPOLLIN;
                ev.data.fd = client_fd;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
                {
                    perror("epoll_ctl: client_fd");
                    close(client_fd);
                    continue;
                }
            }
            else{
                char buf[BUF_SIZE];
                ssize_t nread = read(fd, buf, BUF_SIZE);
                if (nread == 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else if (nread < 0) {
                    perror("read");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                }
                else {
                    write(fd, buf, nread);
                }
                
            }
            
        }
        
    }
    return 0;
}