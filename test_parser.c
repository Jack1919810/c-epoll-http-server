// test_parser.c
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stddef.h>     // size_t

// ===== 解析器相关定义（从你的 server 文件复制过来）=====
enum parse_state { PARSE_REQUEST_LINE, PARSE_HEADERS, PARSE_DONE };
enum parse_result { PARSE_OK, PARSE_INCOMPLETE, PARSE_ERROR };

struct header { char name[64]; char value[256]; };
struct request {
    enum parse_state state;
    size_t parsed;
    char method[8];
    char path[1024];
    char version[16];
    struct header headers[32];
    int header_count;
};

static int copy_field(char *dst, size_t dst_size,
                      const char *src, size_t src_len) {
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
            }
            req->parsed += line_len + 2;
        }
        
    }

// ===== 测试 =====
static const char *result_str(enum parse_result r) {
    switch (r) {
        case PARSE_OK:         return "OK";
        case PARSE_INCOMPLETE: return "INCOMPLETE";
        case PARSE_ERROR:      return "ERROR";
        default:               return "???";
    }
}

static void run_test(const char *name, const char *req_text) {
    struct request req = {0};
    enum parse_result r = parse_request(&req, req_text, strlen(req_text));
    printf("--- %s ---\n", name);
    printf("  result  = %s\n", result_str(r));
    printf("  method  = [%s]\n", req.method);
    printf("  path    = [%s]\n", req.path);
    printf("  version = [%s]\n", req.version);
    printf("\n");
    for (int i = 0; i < req.header_count; i++) {
    printf("  header[%d] = [%s] = [%s]\n",
           i, req.headers[i].name, req.headers[i].value);
}
}

int main(void) {
    run_test("happy: GET /index.html",
             "GET /index.html HTTP/1.1\r\n\r\n");

    run_test("happy: GET /",
             "GET / HTTP/1.1\r\n\r\n");

    run_test("happy: POST",
             "POST /api/users HTTP/1.1\r\n\r\n");

    run_test("incomplete: no CRLF yet",
             "GET /index.html HTTP/1.1");   // 期望 INCOMPLETE

    run_test("error: no spaces",
             "GETNOTHING\r\n\r\n");          // 期望 ERROR
    run_test("with headers",
    "GET /index.html HTTP/1.1\r\n"
    "Host: localhost:8080\r\n"
    "User-Agent: curl/8.0\r\n"
    "Accept: */*\r\n"
    "\r\n");
    return 0;
}