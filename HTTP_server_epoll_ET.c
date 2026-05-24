// HTTP_server_epoll_ET.c
#define _GNU_SOURCE
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
#include <sys/stat.h>
#include <sys/sendfile.h>

#define PORT 8080
#define BACKLOG 16
#define BUF_SIZE 1024
#define MAX_EVENTS 1024

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

enum parse_state { PARSE_REQUEST_LINE, PARSE_HEADERS, PARSE_DONE };
enum parse_result {
    PARSE_OK,
    PARSE_INCOMPLETE,
    PARSE_ERROR,
};
struct header {
    char name[64];
    char value[256];
};

struct request {
    enum parse_state state;        // current parse state
    size_t parsed;
    char method[8];                // "GET"
    char path[1024];               // "/"
    char version[16];              // "HTTP/1.1"
    struct header headers[32];     // header key:value pair
    int header_count;
};

static int copy_field(char *dst, size_t dst_size,
                      const char *src, size_t src_len)
{
    if (src_len >= dst_size) return -1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return 0;
}

enum parse_result parse_request(struct request *req,
                                const char *buf, size_t len)
    {
        while (1)
        {
            const char *crlf = memmem(buf + req->parsed,
                          len - req->parsed,
                          "\r\n", 2);
            if (crlf == NULL) return PARSE_INCOMPLETE;
            size_t line_len = crlf - (buf + req->parsed);
            const char *line = buf + req->parsed;
            switch (req->state) {
                case PARSE_REQUEST_LINE: {
                    const char *sp1 = memchr(line, ' ', line_len);
                    if (!sp1) return PARSE_ERROR;
                    const char *sp2 = memchr(sp1 + 1, ' ', (line + line_len) - (sp1 + 1));
                    if (!sp2) return PARSE_ERROR;

                    if (copy_field(req->method,  sizeof(req->method),
                                line,    sp1 - line)                    < 0) return PARSE_ERROR;
                    if (copy_field(req->path,    sizeof(req->path),
                                sp1 + 1, sp2 - sp1 - 1)                 < 0) return PARSE_ERROR;
                    if (copy_field(req->version, sizeof(req->version),
                                sp2 + 1, (line + line_len) - sp2 - 1)   < 0) return PARSE_ERROR;

                    req->state = PARSE_HEADERS;
                    break;
                }
                case PARSE_HEADERS: {
                    if (line_len == 0) {
                        req->state = PARSE_DONE;
                        return PARSE_OK;
                    }

                    const char *colon = memchr(line, ':', line_len);
                    if (colon == NULL) return PARSE_ERROR;

                    // name: [line, colon)
                    size_t name_len = colon - line;

                    // value: 
                    const char *value_start = colon + 1;
                    const char *line_end = line + line_len;
                    while (value_start < line_end &&
                        (*value_start == ' ' || *value_start == '\t')) {
                        value_start++;
                    }
                    size_t value_len = line_end - value_start;

                    if (req->header_count >= 32) return PARSE_ERROR;

                    struct header *h = &req->headers[req->header_count];
                    if (copy_field(h->name, sizeof(h->name), line, name_len)  < 0)
                        return PARSE_ERROR;
                    if (copy_field(h->value, sizeof(h->value), value_start, value_len) < 0)
                        return PARSE_ERROR;
                    req->header_count++;
                    break;
                }
                case PARSE_DONE:{
                    return PARSE_OK;
                }
            }
            req->parsed += line_len + 2;
        }
        
    }


    struct connection {
    int fd;
    char read_buf[8192];
    size_t read_len;
    struct request req;

    int sending_fd;
    off_t send_offset;
    size_t send_total;
};

static void close_connection(int epfd, struct connection *conn) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    free(conn);
}

static void send_response(int fd, int status, const char *reason,
                          const char *content_type, const char *body)
{
    size_t body_len = strlen(body);
    char response[8192];
    int n = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, reason, content_type, body_len, body);
    write(fd, response, n);
}

static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";

    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".htm")  == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".gif")  == 0) return "image/gif";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";
}

static void try_send_file(int epfd, struct connection *conn) {
    while (conn->send_offset < (off_t)conn->send_total) {
        ssize_t n = sendfile(conn->fd, conn->sending_fd,
                             &conn->send_offset,
                             conn->send_total - conn->send_offset);
        if (n > 0) continue;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("[EAGAIN] offset=%ld/%zu, MOD EPOLLOUT\n",
                    (long)conn->send_offset, conn->send_total);
                struct epoll_event ev;
                ev.events  = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.ptr = conn;
                epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
                return;
            }
            if (errno == EINTR) continue;
            perror("sendfile");
            close(conn->sending_fd);
            close_connection(epfd, conn);
            return;
        }
    }
    close(conn->sending_fd);
    close_connection(epfd, conn);
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
        exit(EXIT_FAILURE); 
    }
    
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct connection *server_conn = calloc(1, sizeof(*server_conn));
    if (server_conn == NULL) { perror("calloc"); exit(EXIT_FAILURE); }
    server_conn->fd = server_fd;

    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET; //key difference to LT
    ev.data.ptr = server_conn;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0)
    {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[MAX_EVENTS];
    while (1)
    {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        printf("=== epoll_wait wake up ===\n");
        if (n < 0) {
        if (errno == EINTR) continue;
        perror("epoll_wait");
        break;
        }
        struct sockaddr_in client_addr;
        for (int i = 0; i < n; i++)
        {
            struct connection *conn = events[i].data.ptr;
            uint32_t flags = events[i].events; 
            if (conn->fd == server_fd) {
                while (1) //process till queue is drained
                {
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_fd = accept(server_fd,
                                        (struct sockaddr *)&client_addr,
                                        &client_addr_len);
                    if (client_fd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("accept");
                        break;
                    }
                    printf("  >>> new connection,accept\n");
                    if (set_nonblocking(client_fd) < 0)
                    {
                        perror("set_nonblocking");
                        close(client_fd);
                        continue;
                    }
                    struct connection *client_conn  = calloc(1, sizeof(*client_conn));
                    if (client_conn  == NULL) {
                        perror("calloc");
                        close(client_fd);
                        continue;
                    }
                    client_conn ->fd = client_fd;
                    client_conn->sending_fd = -1;
                    ev.events  = EPOLLIN | EPOLLET;
                    ev.data.ptr = client_conn ;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
                    {
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                        free(client_conn );
                        continue;
                    }
                }
            } else {
                if (flags & EPOLLOUT) {
                    try_send_file(epfd, conn);
                    continue;
                }
                if (flags & EPOLLIN)
                {
                    while (1) {
                        ssize_t cnt = read(conn->fd,
                                        conn->read_buf + conn->read_len,
                                        sizeof(conn->read_buf) - conn->read_len);
                        if (cnt > 0) {
                            conn->read_len += cnt;
                            enum parse_result r = parse_request(&conn->req,
                                                                conn->read_buf,
                                                                conn->read_len);
                            if (r == PARSE_OK) {
                                const char *url_path = conn->req.path;
                                if (strcmp(url_path, "/") == 0) url_path = "/index.html";

                                if (strstr(url_path, "..") != NULL) {
                                    send_response(conn->fd, 400, "Bad Request", "text/plain", "Bad Request");
                                    close_connection(epfd, conn);
                                    break;
                                }

                                char file_path[1100];
                                snprintf(file_path, sizeof(file_path), "./www%s", url_path);

                                int file_fd = open(file_path, O_RDONLY);
                                if (file_fd < 0) {
                                    if (errno == ENOENT)
                                        send_response(conn->fd, 404, "Not Found", "text/plain", "Not Found");
                                    else
                                        send_response(conn->fd, 500, "Internal Server Error", "text/plain", "Internal Server Error");
                                    close_connection(epfd, conn);
                                    break;
                                }

                                struct stat st;
                                if (fstat(file_fd, &st) < 0) {
                                    send_response(conn->fd, 500, "Internal Server Error", "text/plain", "Internal Server Error");
                                    close(file_fd);
                                    close_connection(epfd, conn);
                                    break;
                                }
                                size_t file_size = st.st_size;

                                char header[512];
                                const char *mime = get_mime_type(file_path);

                                int hlen = snprintf(header, sizeof(header),
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: %s\r\n"
                                    "Content-Length: %zu\r\n"
                                    "Connection: close\r\n"
                                    "\r\n",
                                    mime, file_size);

                                write(conn->fd, header, hlen);

                                conn->sending_fd  = file_fd;
                                conn->send_offset = 0;
                                conn->send_total  = file_size;

                                try_send_file(epfd, conn);
                                break;
                            }
                            if (r == PARSE_ERROR) {
                                send_response(conn->fd, 400, "Bad Request", "text/plain", "Bad Request");
                                close_connection(epfd, conn);
                                break;
                            }
                            // PARSE_INCOMPLETE: continue draining
                        } else if (cnt == 0) {
                            close_connection(epfd, conn);
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (errno == EINTR) continue;
                            perror("read");
                            close_connection(epfd, conn);
                            break;
                        }
                    }
                }
            }
        }
        
    }
    return 0;
}