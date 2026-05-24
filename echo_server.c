// echo_server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define PORT 8080
#define BACKLOG 16
#define BUF_SIZE 1024

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
    struct sockaddr_in client_addr;
    
    while (1)
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
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        printf("New connection from %s:%d\n", ip, ntohs(client_addr.sin_port));

        while (1) {
            char buf[BUF_SIZE];
            ssize_t n = read(client_fd, buf, BUF_SIZE);
            if (n == 0) break;
            if (n  < 0) { perror("read"); break; }
            if (write(client_fd, buf, n) < 0) { perror("write"); break; }
        }
        close(client_fd);
    }
    
    
    
    


    return 0;
}