#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int write_all_fd(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static int render_via_api(const char *data, size_t len, int out_fd) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "POST /render HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", len);
    write_all_fd(sock, header, (size_t)hlen);
    write_all_fd(sock, data, len);

    char buf[4096];
    char *hdr = malloc(8193);
    size_t hdr_len = 0;
    int is_body = 0;
    int saw_ok = 0;
    if (!hdr) { close(sock); return -1; }
    ssize_t n;
    while ((n = read(sock, buf, sizeof(buf))) > 0) {
        if (!is_body) {
            if (hdr_len + (size_t)n > 8192) { free(hdr); close(sock); return -1; }
            memcpy(hdr + hdr_len, buf, (size_t)n);
            hdr_len += (size_t)n;
            hdr[hdr_len] = '\0';
            char *body = strstr(hdr, "\r\n\r\n");
            if (body) {
                is_body = 1;
                if (strncmp(hdr, "HTTP/1.1 200", 12) == 0 ||
                    strncmp(hdr, "HTTP/1.0 200", 12) == 0) saw_ok = 1;
                body += 4;
                size_t body_len = hdr_len - (size_t)(body - hdr);
                if (body_len > 0) write_all_fd(out_fd, body, body_len);
            }
        } else {
            write_all_fd(out_fd, buf, (size_t)n);
        }
    }
    free(hdr);
    close(sock);
    return saw_ok ? 0 : -1;
}

static int cat_fd(int fd, const char *name) {
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fprintf(stderr, "cat: %s: out of memory\n", name);
        return 1;
    }
    ssize_t n;
    
    while ((n = read(fd, buf + len, cap - len)) > 0) {
        len += n;
        if (len == cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                fprintf(stderr, "cat: %s: out of memory\n", name);
                free(buf);
                return 1;
            }
            buf = nb;
            cap = ncap;
        }
    }
    
    if (n < 0) {
        fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
        free(buf);
        return 1;
    }

    if (render_via_api(buf, len, STDOUT_FILENO) < 0) {
        write_all_fd(STDOUT_FILENO, buf, len);
    }
    
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    int status = 0;
    if (argc == 1) return cat_fd(STDIN_FILENO, "-");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) { status |= cat_fd(STDIN_FILENO, "-"); continue; }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno)); status = 1; continue; }
        status |= cat_fd(fd, argv[i]);
        close(fd);
    }
    return status;
}
