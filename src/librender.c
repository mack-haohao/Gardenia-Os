#define _GNU_SOURCE
#include "librender.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define MAX_PASS (1024 * 1024)

static pid_t pump_pid = -1;
static int saved_stdout = -1;

static char *api_render(const char *data, size_t len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(1);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) { close(fd); return NULL; }
    fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
    struct timeval tv = {0, 100000};
    if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) { close(fd); return NULL; }
    {
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            close(fd); return NULL;
        }
    }
    fcntl(fd, F_SETFL, fl);
    tv.tv_sec = 0; tv.tv_usec = 100000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char h[256];
    int hl = snprintf(h, sizeof(h),
        "POST /render HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: text/plain\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", len);
    if (write(fd, h, hl) != hl || (len && write(fd, data, len) != (ssize_t)len)) {
        close(fd); return NULL;
    }
    shutdown(fd, SHUT_WR);
    size_t cap = 8192, rl = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (rl + 1 >= cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb; cap *= 2;
        }
        ssize_t n = read(fd, buf + rl, cap - 1 - rl);
        if (n <= 0) break;
        rl += (size_t)n;
    }
    buf[rl] = 0; close(fd);
    char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) { free(buf); return NULL; }
    long exp = -1;
    char *cl = strstr(buf, "Content-Length: ");
    if (cl) exp = atol(cl + 16);
    char *out = strdup(sep + 4);
    free(buf);
    if (exp >= 0 && out && (long)strlen(out) > exp) out[exp] = 0;
    return out;
}

static void pump(int fd) {
    size_t cap = 8192, len = 0;
    char *data = malloc(cap);
    if (!data) { close(fd); return; }
    char tmp[4096]; ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n > cap) {
            cap *= 2;
            char *nd = realloc(data, cap);
            if (!nd) { free(data); close(fd); return; }
            data = nd;
        }
        memcpy(data + len, tmp, (size_t)n);
        len += (size_t)n;
        if (len > MAX_PASS) {
            fwrite(data, 1, len, stdout);
            free(data);
            while ((n = read(fd, tmp, sizeof(tmp))) > 0)
                fwrite(tmp, 1, (size_t)n, stdout);
            close(fd); return;
        }
    }
    close(fd);
    if (!len) { free(data); return; }
    char *r = api_render(data, len);
    if (r) { fputs(r, stdout); free(r); }
    else fwrite(data, 1, len, stdout);
    free(data);
}

void render_end(void) {
    if (pump_pid <= 0) return;
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    saved_stdout = -1;
    int st; waitpid(pump_pid, &st, 0);
    pump_pid = -1;
}

void render_begin(void) {
    int fds[2];
    if (pipe(fds) < 0) return;
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) { close(fds[0]); close(fds[1]); return; }
    pump_pid = fork();
    if (pump_pid < 0) {
        close(fds[0]); close(fds[1]);
        close(saved_stdout); saved_stdout = -1;
        return;
    }
    if (pump_pid == 0) {
        close(fds[1]);
        pump(fds[0]);
        _exit(0);
    }
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    atexit(render_end);
}
