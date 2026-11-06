#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* API declarations and implementation (formerly api.h / api.c) */
char *api_render(const char *text, size_t len);
char *api_get_completions(const char *lang, const char *prefix, const char *line);
int   api_record_usage(const char *lang, const char *cmd, const char *line);

static int connect_api(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(1);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
    struct timeval tv = {0, 100000};
    if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) { close(fd); return -1; }
    int soerr = 0; socklen_t sl = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
        close(fd); return -1;
    }
    fcntl(fd, F_SETFL, fl);
    tv.tv_sec = 0; tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static char *send_request(int fd, const char *method, const char *path, const char *content_type, const char *data, size_t len) {
    char h[1024];
    int hl;
    if (data) {
        hl = snprintf(h, sizeof(h),
            "%s %s HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: %s\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n", method, path, content_type, len);
    } else {
        hl = snprintf(h, sizeof(h),
            "%s %s HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: close\r\n\r\n", method, path);
    }
    if (write(fd, h, hl) != hl || (len && write(fd, data, len) != (ssize_t)len)) {
        return NULL;
    }
    shutdown(fd, SHUT_WR);
    size_t cap = 8192, rl = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
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
    buf[rl] = 0;
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

static char *url_encode(const char *s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 1);
    if (!out) return NULL;
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = (char)c;
        } else if (c == ' ') {
            *p++ = '+';
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = 0;
    return out;
}

char *api_render(const char *text, size_t len) {
    if (!text || len == 0) return NULL;
    int fd = connect_api();
    if (fd < 0) return NULL;
    char *res = send_request(fd, "POST", "/render", "text/plain", text, len);
    close(fd);
    return res;
}

char *api_get_completions(const char *lang, const char *prefix, const char *line) {
    int fd = connect_api();
    if (fd < 0) return NULL;
    char *elang = url_encode(lang);
    char *eprefix = url_encode(prefix);
    char *eline = url_encode(line);
    char path[4096];
    snprintf(path, sizeof(path), "/autoconf/get?lang=%s&param=%s&line=%s", elang, eprefix, eline);
    free(elang); free(eprefix); free(eline);
    char *res = send_request(fd, "GET", path, NULL, NULL, 0);
    close(fd);
    return res;
}

int api_record_usage(const char *lang, const char *cmd, const char *line) {
    int fd = connect_api();
    if (fd < 0) return -1;
    char *elang = url_encode(lang);
    char *ecmd = url_encode(cmd);
    char *eline = url_encode(line);
    char body[4096];
    snprintf(body, sizeof(body), "lang=%s&param=%s&line=%s", elang, ecmd, eline);
    free(elang); free(ecmd); free(eline);
    char *res = send_request(fd, "POST", "/autoconf/data", "application/x-www-form-urlencoded", body, strlen(body));
    close(fd);
    free(res);
    return 0;
}

/* Editor core (formerly gte.c) */
enum {
    K_NONE = 0,
    K_ESC,
    K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_DEL,
    K_ENTER, K_BACKSPACE, K_TAB,
    K_CTRL_O, K_CTRL_X, K_CTRL_K, K_CTRL_U,
    K_CTRL_G, K_CTRL_C, K_CTRL_S, K_CTRL_F,
    K_CHAR
};

typedef struct { char *s; int len, cap; } line_t;

typedef struct {
    line_t *lines;
    int     n, cap;
    int     cx, cy;
    int     sy;
    char    path[PATH_MAX];
    int     dirty;
    char    msg[256];
    int     raw_on;
    struct termios orig_term;
} ed_t;

static void get_lang(char *lang, size_t cap);
static void api_worker_init(void);
static void api_async_record(const char *lang, const char *cmd, const char *line);


static int api_pipe_fd = -1;
static pid_t api_worker_pid = -1;

static ed_t ed;
static int  tty_rows = 24, tty_cols = 80;

static volatile sig_atomic_t got_term  = 0;
static volatile sig_atomic_t got_winch = 0;

static void on_term(int sig)  { (void)sig; got_term  = 1; }
static void on_winch(int sig) { (void)sig; got_winch = 1; }

typedef struct { char *p; size_t len, cap; } obuf;

static int ob_need(obuf *o, size_t n) {
    if (o->len + n + 1 <= o->cap) return 0;
    size_t nc = o->cap ? o->cap : 1024;
    while (nc < o->len + n + 1) {
        if (nc > (size_t)-1 / 2) { nc = o->len + n + 1; break; }
        nc *= 2;
    }
    char *np = realloc(o->p, nc);
    if (!np) return -1;
    o->p = np; o->cap = nc;
    return 0;
}
static int ob_putn(obuf *o, const char *s, size_t n) {
    if (ob_need(o, n) < 0) return -1;
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = 0;
    return 0;
}
static int ob_puts(obuf *o, const char *s) { return ob_putn(o, s, strlen(s)); }
static int ob_printf(obuf *o, const char *fmt, ...) {
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return ob_putn(o, tmp, (size_t)n);
    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = ob_putn(o, big, (size_t)n);
    free(big);
    return r;
}

static int ed_ensure_lines(int need) {
    if (need <= ed.cap) return 0;
    int nc = ed.cap ? ed.cap * 2 : 16;
    while (nc < need) nc *= 2;
    line_t *nl = realloc(ed.lines, (size_t)nc * sizeof(line_t));
    if (!nl) return -1;
    ed.lines = nl; ed.cap = nc;
    return 0;
}

static int line_ensure(line_t *l, int need) {
    if (l->cap >= need + 1) return 0;
    int nc = l->cap ? l->cap * 2 : 64;
    while (nc < need + 1) nc *= 2;
    char *ns = realloc(l->s, (size_t)nc);
    if (!ns) return -1;
    l->s = ns; l->cap = nc;
    return 0;
}

static int ed_insert_line_at(int at, const char *s, int len) {
    if (ed_ensure_lines(ed.n + 1) < 0) return -1;
    memmove(&ed.lines[at + 1], &ed.lines[at], (size_t)(ed.n - at) * sizeof(line_t));
    line_t *l = &ed.lines[at];
    l->s = NULL; l->len = 0; l->cap = 0;
    if (line_ensure(l, len) < 0) return -1;
    if (len > 0) memcpy(l->s, s, (size_t)len);
    l->len = len;
    l->s[len] = 0;
    ed.n++;
    return 0;
}

static void ed_delete_line_at(int at) {
    if (at < 0 || at >= ed.n) return;
    free(ed.lines[at].s);
    memmove(&ed.lines[at], &ed.lines[at + 1], (size_t)(ed.n - at - 1) * sizeof(line_t));
    ed.n--;
}

static void ed_new_buffer(void) {
    ed.n = 0;
    ed_insert_line_at(0, "", 0);
}

static void ed_ins_char(char c) {
    line_t *l = &ed.lines[ed.cy];
    if (line_ensure(l, l->len + 1) < 0) return;
    memmove(l->s + ed.cx + 1, l->s + ed.cx, (size_t)(l->len - ed.cx));
    l->s[ed.cx] = c;
    l->len++;
    l->s[l->len] = 0;
    ed.cx++;
    ed.dirty = 1;

    if (!isalnum((unsigned char)c) && c != '_') {
        if (ed.path[0] != 0) {
            const char *ext = strrchr(ed.path, '.');
            if (ext && strlen(ext) > 1) {
                int end = ed.cx - 1;
                int start = end;
                while (start >= 0 && (isalnum((unsigned char)l->s[start]) || l->s[start] == '_')) {
                    start--;
                }
                start++;
                if (end > start) {
                    char word[64] = {0};
                    int wlen = end - start;
                    if (wlen > 63) wlen = 63;
                    memcpy(word, l->s + start, wlen);
                    char lang[32];
                    get_lang(lang, sizeof(lang));
                    api_async_record(lang, word, l->s);
                }
            }
        }
    }
}

static void ed_split_line(void) {
    line_t *l = &ed.lines[ed.cy];
    int right = l->len - ed.cx;
    if (ed_insert_line_at(ed.cy + 1, l->s + ed.cx, right) < 0) return;
    l = &ed.lines[ed.cy];
    l->len = ed.cx;
    l->s[l->len] = 0;
    ed.cy++;
    ed.cx = 0;
    ed.dirty = 1;
}

static void ed_backspace(void) {
    if (ed.cx > 0) {
        line_t *l = &ed.lines[ed.cy];
        memmove(l->s + ed.cx - 1, l->s + ed.cx, (size_t)(l->len - ed.cx));
        l->len--;
        l->s[l->len] = 0;
        ed.cx--;
        ed.dirty = 1;
    } else if (ed.cy > 0) {
        line_t *prev = &ed.lines[ed.cy - 1];
        line_t *cur  = &ed.lines[ed.cy];
        int old_prev = prev->len;
        int new_len  = prev->len + cur->len;
        if (line_ensure(prev, new_len) < 0) return;
        memcpy(prev->s + prev->len, cur->s, (size_t)cur->len);
        prev->len = new_len;
        prev->s[prev->len] = 0;
        ed.cx = old_prev;
        ed_delete_line_at(ed.cy);
        ed.cy--;
        ed.dirty = 1;
    }
}

static void ed_delete(void) {
    line_t *l = &ed.lines[ed.cy];
    if (ed.cx < l->len) {
        memmove(l->s + ed.cx, l->s + ed.cx + 1, (size_t)(l->len - ed.cx - 1));
        l->len--;
        l->s[l->len] = 0;
        ed.dirty = 1;
    } else if (ed.cy < ed.n - 1) {
        line_t *next = &ed.lines[ed.cy + 1];
        int new_len = l->len + next->len;
        if (line_ensure(l, new_len) < 0) return;
        memcpy(l->s + l->len, next->s, (size_t)next->len);
        l->len = new_len;
        l->s[l->len] = 0;
        ed_delete_line_at(ed.cy + 1);
        ed.dirty = 1;
    }
}

static void ed_move_left(void) {
    if (ed.cx > 0) ed.cx--;
    else if (ed.cy > 0) { ed.cy--; ed.cx = ed.lines[ed.cy].len; }
}
static void ed_move_right(void) {
    if (ed.cx < ed.lines[ed.cy].len) ed.cx++;
    else if (ed.cy < ed.n - 1) { ed.cy++; ed.cx = 0; }
}
static void ed_move_up(void) {
    if (ed.cy > 0) {
        ed.cy--;
        if (ed.cx > ed.lines[ed.cy].len) ed.cx = ed.lines[ed.cy].len;
    }
}
static void ed_move_down(void) {
    if (ed.cy < ed.n - 1) {
        ed.cy++;
        if (ed.cx > ed.lines[ed.cy].len) ed.cx = ed.lines[ed.cy].len;
    }
}
static void ed_home(void) { ed.cx = 0; }
static void ed_end(void)  { ed.cx = ed.lines[ed.cy].len; }

static void ed_pgup(void) {
    int page = tty_rows - 2; if (page < 1) page = 1;
    ed.cy -= page; if (ed.cy < 0) ed.cy = 0;
    if (ed.cx > ed.lines[ed.cy].len) ed.cx = ed.lines[ed.cy].len;
}
static void ed_pgdn(void) {
    int page = tty_rows - 2; if (page < 1) page = 1;
    ed.cy += page; if (ed.cy >= ed.n) ed.cy = ed.n - 1;
    if (ed.cx > ed.lines[ed.cy].len) ed.cx = ed.lines[ed.cy].len;
}

static char  *clip_buf = NULL;
static size_t clip_len = 0;

static void ed_cut_line(void) {
    if (ed.n == 0) return;
    line_t *l = &ed.lines[ed.cy];
    free(clip_buf);
    clip_buf = malloc((size_t)l->len + 1);
    if (clip_buf) {
        memcpy(clip_buf, l->s, (size_t)l->len);
        clip_buf[l->len] = 0;
        clip_len = (size_t)l->len;
    }
    if (ed.n == 1) {
        l->len = 0; l->s[0] = 0;
    } else {
        ed_delete_line_at(ed.cy);
        if (ed.cy >= ed.n) ed.cy = ed.n - 1;
    }
    ed.cx = 0;
    ed.dirty = 1;
}

static void ed_paste(void) {
    if (!clip_buf) return;
    if (ed_insert_line_at(ed.cy, clip_buf, (int)clip_len) < 0) return;
    ed.cx = 0;
    ed.dirty = 1;
}

static void update_size(void) {
    struct winsize ws;
    // 1. Get the real size from the underlying terminal
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) tty_rows = ws.ws_row;
        if (ws.ws_col > 0) tty_cols = ws.ws_col;
    }
    // 2. Check env vars regardless of ioctl result (covers QEMU -nographic)
    char *l = getenv("LINES");
    char *c = getenv("COLUMNS");
    if (l) { int v = atoi(l); if (v > 0) tty_rows = v; }
    if (c) { int v = atoi(c); if (v > 0) tty_cols = v; }
}

static void raw_enable(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &ed.orig_term);
    t = ed.orig_term;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    ed.raw_on = 1;
    const char *enter = "\033[?1049h\033[2J\033[?25l";
    (void)!write(STDOUT_FILENO, enter, strlen(enter));
}

static void raw_disable(void) {
    if (!ed.raw_on) return;
    const char *leave = "\033[?25h\033[?1049l";
    (void)!write(STDOUT_FILENO, leave, strlen(leave));
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ed.orig_term);
    ed.raw_on = 0;
}

static int read_key(char *out_char) {
    unsigned char c;
    ssize_t r = read(STDIN_FILENO, &c, 1);
    if (r <= 0) return K_NONE;

    if (c == 0x1B) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 50000};
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) return K_ESC;
        unsigned char c2;
        if (read(STDIN_FILENO, &c2, 1) != 1) return K_NONE;
        if (c2 != '[' && c2 != 'O') return K_NONE;
        unsigned char c3;
        if (read(STDIN_FILENO, &c3, 1) != 1) return K_NONE;
        switch (c3) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            case '3': { unsigned char c4; if (read(STDIN_FILENO, &c4, 1) == 1 && c4 == '~') return K_DEL; return K_NONE; }
            case '5': { unsigned char c4; if (read(STDIN_FILENO, &c4, 1) == 1 && c4 == '~') return K_PGUP; return K_NONE; }
            case '6': { unsigned char c4; if (read(STDIN_FILENO, &c4, 1) == 1 && c4 == '~') return K_PGDN; return K_NONE; }
        }
        return K_NONE;
    }

    if (c == 0x0F) return K_CTRL_O;
    if (c == 0x18) return K_CTRL_X;
    if (c == 0x0B) return K_CTRL_K;
    if (c == 0x15) return K_CTRL_U;
    if (c == 0x07) return K_CTRL_G;
    if (c == 0x13) return K_CTRL_S;
    if (c == 0x06) return K_CTRL_F;
    if (c == 0x03) return K_CTRL_C;
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 0x7F || c == 0x08) return K_BACKSPACE;
    if (c == '\t') return K_TAB;

    *out_char = (char)c;
    return K_CHAR;
}

static int ed_prompt(const char *prompt, char *buf, int cap) {
    int len = (int)strlen(buf);
    if (len >= cap) len = cap - 1;
    for (;;) {
        int text_rows = tty_rows - 2;
        if (text_rows < 1) text_rows = 1;
        obuf o = {0};
        ob_printf(&o, "\033[%d;1H\033[0m\033[K", text_rows + 2);
        ob_puts(&o, prompt);
        ob_putn(&o, buf, (size_t)len);
        ob_printf(&o, "\033[%d;%dH\033[?25h", text_rows + 2, (int)(strlen(prompt) + (size_t)len + 1));
        (void)!write(STDOUT_FILENO, o.p, o.len);
        free(o.p);

        char c = 0;
        int k = read_key(&c);
        if (k == K_ENTER) return 0;
        if (k == K_ESC || k == K_CTRL_C || k == K_CTRL_X) return -1;
        if (k == K_BACKSPACE) {
            if (len > 0) buf[--len] = 0;
            continue;
        }
        if (k == K_CHAR) {
            if (len + 1 < cap) { buf[len++] = c; buf[len] = 0; }
            continue;
        }
    }
}

static int ed_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            snprintf(ed.path, sizeof(ed.path), "%s", path);
            snprintf(ed.msg, sizeof(ed.msg), "New file: %s", path);
            ed_new_buffer();
            return 0;
        }
        return -1;
    }
    char  *line = NULL;
    size_t cap  = 0;
    ssize_t n;
    int count = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n > 0 && line[n - 1] == '\n') n--;
        if (n > 0 && line[n - 1] == '\r') n--;
        if (ed_insert_line_at(count, line, (int)n) < 0) break;
        count++;
    }
    free(line);
    fclose(f);
    if (count == 0) ed_new_buffer();
    snprintf(ed.path, sizeof(ed.path), "%s", path);
    return 0;
}

static int ed_save_to(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(ed.msg, sizeof(ed.msg), "Save error: %s", strerror(errno));
        return -1;
    }
    for (int i = 0; i < ed.n; i++) {
        if (ed.lines[i].len &&
            fwrite(ed.lines[i].s, 1, (size_t)ed.lines[i].len, f)
                != (size_t)ed.lines[i].len) {
            snprintf(ed.msg, sizeof(ed.msg), "Write error");
            fclose(f);
            return -1;
        }
        fputc('\n', f);
    }
    if (fclose(f) != 0) {
        snprintf(ed.msg, sizeof(ed.msg), "Close error");
        return -1;
    }
    ed.dirty = 0;
    snprintf(ed.path, sizeof(ed.path), "%s", path);
    snprintf(ed.msg, sizeof(ed.msg), "Saved %s (%d lines)", path, ed.n);
    return 0;
}

static void ed_scroll(void) {
    int text_rows = tty_rows - 2;
    if (text_rows < 1) text_rows = 1;
    if (ed.cy < ed.sy) ed.sy = ed.cy;
    if (ed.cy >= ed.sy + text_rows) ed.sy = ed.cy - text_rows + 1;
    if (ed.sy < 0) ed.sy = 0;
}

static void ed_find_replace(void) {
    char find[256] = {0};
    char rep[256] = {0};
    if (ed_prompt("Search: ", find, sizeof(find)) < 0) return;
    if (find[0] == 0) return;
    if (ed_prompt("Replace with: ", rep, sizeof(rep)) < 0) return;

    int count = 0;
    for (int i = 0; i < ed.n; i++) {
        line_t *l = &ed.lines[i];
        char *pos = l->s;
        while ((pos = strstr(pos, find)) != NULL) {
            int offset = pos - l->s;
            int old_len = l->len;
            int new_len = old_len - strlen(find) + strlen(rep);
            if (line_ensure(l, new_len) < 0) break;
            memmove(l->s + offset + strlen(rep), l->s + offset + strlen(find),
                    old_len - offset - strlen(find) + 1);
            memcpy(l->s + offset, rep, strlen(rep));
            l->len = new_len;
            pos = l->s + offset + strlen(rep);
            count++;
        }
    }
    if (count > 0) {
        ed.dirty = 1;
        snprintf(ed.msg, sizeof(ed.msg), "Replaced %d occurrences", count);
    } else {
        snprintf(ed.msg, sizeof(ed.msg), "Not found: %s", find);
    }
}

static void get_lang(char *lang, size_t cap) {
    const char *ext = strrchr(ed.path, '.');
    if (ext && strlen(ext) > 1) {
        snprintf(lang, cap, "%s", ext + 1);
        return;
    }
    // Heuristic sniffing (only for unsaved files, not exact)
    int is_c = 0, is_py = 0, is_sh = 0;
    int limit = ed.n < 50 ? ed.n : 50;
    for (int i = 0; i < limit; i++) {
        const char *s = ed.lines[i].s;
        if (strstr(s, "#include")) is_c = 1;
        else if (strstr(s, "def ") && strstr(s, ":")) is_py = 1;
        else if (strstr(s, "#!/bin/")) is_sh = 1;
    }
    if (is_c) snprintf(lang, cap, "c");
    else if (is_py) snprintf(lang, cap, "py");
    else if (is_sh) snprintf(lang, cap, "sh");
    else snprintf(lang, cap, "txt");
}

static int side_win = 0;
static char *completions[32];
static int  comp_count = 0;
static int  comp_sel = 0;
static int  comp_start_x = 0;

static void trigger_completion(void) {
    line_t *l = &ed.lines[ed.cy];
    int i = ed.cx - 1;
    while (i >= 0 && (isalnum((unsigned char)l->s[i]) || l->s[i] == '_')) {
        i--;
    }
    int start = i + 1;
    int wlen = ed.cx - start;
    if (wlen == 0) return;

    char word[64] = {0};
    memcpy(word, l->s + start, wlen);
    word[wlen] = 0;

    char lang[32];
    get_lang(lang, sizeof(lang));

    char *result = api_get_completions(lang, word, l->s);
    if (!result) return;

    for (int j = 0; j < comp_count; j++) free(completions[j]);
    comp_count = 0;

    char *tok = strtok(result, "\n");
    while (tok && comp_count < 32) {
        completions[comp_count++] = strdup(tok);
        tok = strtok(NULL, "\n");
    }
    free(result);

    if (comp_count > 0) {
        side_win = 1;
        comp_sel = 0;
        comp_start_x = start;
    }
}

static void apply_completion(void) {
    if (!side_win || comp_count == 0) return;
    line_t *l = &ed.lines[ed.cy];
    const char *sel = completions[comp_sel];
    int sel_len = strlen(sel);
    int old_len = ed.cx - comp_start_x;
    int new_len = l->len - old_len + sel_len;

    if (line_ensure(l, new_len) < 0) return;

    memmove(l->s + comp_start_x + sel_len, l->s + ed.cx, l->len - ed.cx + 1);
    memcpy(l->s + comp_start_x, sel, sel_len);
    l->len = new_len;
    ed.cx = comp_start_x + sel_len;
    ed.dirty = 1;
    side_win = 0;

    char lang[32];
    get_lang(lang, sizeof(lang));
    api_record_usage(lang, sel, l->s);
}

static void ed_render(void) {
    update_size();
    int text_rows = tty_rows - 2;
    if (text_rows < 1) text_rows = 1;

    int digits = 1;
    int tmp = ed.n;
    while (tmp >= 10) { digits++; tmp /= 10; }
    if (digits < 3) digits = 3;

    int edit_cols = tty_cols;
    if (side_win && comp_count > 0) {
        edit_cols = tty_cols - 30 - 1;
        if (edit_cols < 20) {
            side_win = 0;
            edit_cols = tty_cols;
        }
    }

    obuf o = {0};
    ob_puts(&o, "\033[?2026h\033[?25l");

    for (int i = 0; i < text_rows; i++) {
        int li = ed.sy + i;
        ob_printf(&o, "\033[%d;1H\033[K", i + 1);
        if (li < ed.n) {
            ob_printf(&o, "\033[90m%*d \033[0m", digits, li + 1);
            if (ed.lines[li].len == 0) continue;
            char *h = api_render(ed.lines[li].s, ed.lines[li].len);
            if (h) { ob_puts(&o, h); free(h); }
            else   { ob_puts(&o, ed.lines[li].s); }
        }
    }

    if (side_win && comp_count > 0) {
        int side_x = edit_cols + 2;
        for (int i = 0; i < comp_count && i < text_rows; i++) {
            ob_printf(&o, "\033[%d;%dH", i + 1, side_x);
            if (i == comp_sel) ob_puts(&o, "\033[7m");
            else ob_puts(&o, "\033[0m");
            
            char *h = api_render(completions[i], strlen(completions[i]));
            if (h) { ob_puts(&o, h); free(h); }
            else   { ob_puts(&o, completions[i]); }
            ob_puts(&o, "\033[0m\033[K");
        }
        for (int i = 0; i < text_rows; i++) {
            ob_printf(&o, "\033[%d;%dH\033[90m|\033[0m", i + 1, edit_cols + 1);
        }
    }

    ob_printf(&o, "\033[%d;1H\033[7m", text_rows + 1);
    char stat[512];
    snprintf(stat, sizeof(stat), " GTE  %.250s%s  [%d/%d] col %d ",
             ed.path[0] ? ed.path : "[No Name]",
             ed.dirty ? " *" : "",
             ed.cy + 1, ed.n, ed.cx + 1);
    int sl = (int)strlen(stat);
    if (sl > tty_cols) sl = tty_cols;
    ob_putn(&o, stat, (size_t)sl);
    for (int i = sl; i < tty_cols; i++) ob_putn(&o, " ", 1);
    ob_puts(&o, "\033[0m");

    ob_printf(&o, "\033[%d;1H\033[0m\033[K", text_rows + 2);
    if (ed.msg[0]) ob_puts(&o, ed.msg);
    else ob_puts(&o, "Ctrl+O SaveAs  Ctrl+X Exit  Ctrl+F Find  Ctrl+S Save  Ctrl+G Help");

    int scr_y = ed.cy - ed.sy + 1;
    int scr_x = digits + 2 + ed.cx;
    if (scr_x > edit_cols) scr_x = edit_cols;
    if (scr_x < digits + 2) scr_x = digits + 2;
    if (scr_y < 1) scr_y = 1;
    if (scr_y > text_rows) scr_y = text_rows;
    ob_printf(&o, "\033[%d;%dH\033[?25h\033[?2026l", scr_y, scr_x);

    (void)!write(STDOUT_FILENO, o.p, o.len);
    free(o.p);
}

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

static void quit_sequence(void) {
    if (!ed.dirty) {
        got_term = 2;
        return;
    }
    int text_rows = tty_rows - 2;
    if (text_rows < 1) text_rows = 1;
    obuf o = {0};
    ob_printf(&o, "\033[%d;1H\033[0m\033[K", text_rows + 2);
    ob_puts(&o, "Save modified buffer? (Y/N/C) ");
    (void)!write(STDOUT_FILENO, o.p, o.len);
    free(o.p);

    char c = 0;
    int k = read_key(&c);
    if (k == K_CHAR && (c == 'y' || c == 'Y')) {
        if (ed.path[0]) {
            ed_save_to(ed.path);
        } else {
            char name[PATH_MAX] = {0};
            if (ed_prompt("File Name to Write: ", name, sizeof(name)) == 0) {
                if (name[0]) ed_save_to(name);
            }
        }
        if (!ed.dirty) got_term = 2;
    } else if (k == K_CHAR && (c == 'n' || c == 'N')) {
        got_term = 2;
    }
}

static void api_worker_process(void) {
    char buf[4096];
    size_t rl = 0;
    ssize_t n;
    while ((n = read(api_pipe_fd, buf + rl, sizeof(buf) - rl)) > 0) {
        rl += (size_t)n;
        char *p = buf;
        while (rl > 0) {
            char *lang = p;
            char *e1 = memchr(p, 0, rl);
            if (!e1) break;
            char *cmd = e1 + 1;
            char *e2 = memchr(cmd, 0, (size_t)(buf + rl - cmd));
            if (!e2) break;
            char *line = e2 + 1;
            char *e3 = memchr(line, 0, (size_t)(buf + rl - line));
            if (!e3) break;
            
            api_record_usage(lang, cmd, line);
            
            size_t used = (size_t)(e3 + 1 - buf);
            memmove(buf, e3 + 1, rl - used);
            rl -= used;
            p = buf;
        }
    }
    _exit(0);
}

static void api_worker_init(void) {
    int fds[2];
    if (pipe(fds) < 0) return;
    int fl = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, fl | O_NONBLOCK);

    api_worker_pid = fork();
    if (api_worker_pid == 0) {
        close(fds[1]);
        api_pipe_fd = fds[0];
        api_worker_process();
        _exit(0);
    } else if (api_worker_pid > 0) {
        close(fds[0]);
        api_pipe_fd = fds[1];
        signal(SIGCHLD, SIG_IGN);
    } else {
        close(fds[0]);
        close(fds[1]);
    }
}

static void api_async_record(const char *lang, const char *cmd, const char *line) {
    if (api_pipe_fd < 0) return;
    char buf[4096];
    size_t l1 = strlen(lang) + 1;
    size_t l2 = strlen(cmd) + 1;
    size_t l3 = strlen(line) + 1;
    if (l1 + l2 + l3 > sizeof(buf)) return;
    memcpy(buf, lang, l1);
    memcpy(buf + l1, cmd, l2);
    memcpy(buf + l1 + l2, line, l3);
    (void)!write(api_pipe_fd, buf, l1 + l2 + l3);
}

int main(int argc, char **argv) {
    if (argc >= 2 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("GTE - Gandenia Text Editor (basic)\n"
               "Usage: %s [filename]\n"
               "Keys:  Ctrl+S save  Ctrl+O save as  Ctrl+X exit\n"
               "       Ctrl+F find/replace  Tab complete\n"
               "       arrows / Home / End / PgUp / PgDn\n", argv[0]);
        return 0;
    }

    memset(&ed, 0, sizeof(ed));
    install_handlers();
    api_worker_init();

    if (argc >= 2) {
        if (ed_load(argv[1]) < 0) {
            fprintf(stderr, "gte: cannot open %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
    } else {
        ed_new_buffer();
    }

    raw_enable();
    atexit(raw_disable);

    int running = 1;
    while (running && got_term != 1) {
        if (got_winch) { got_winch = 0; update_size(); }
        ed_scroll();
        ed_render();
        ed.msg[0] = 0;

        char c = 0;
        int k = read_key(&c);

        if (side_win) {
            if (k == K_TAB || k == K_DOWN) {
                comp_sel = (comp_sel + 1) % comp_count;
                continue;
            } else if (k == K_UP) {
                comp_sel = (comp_sel - 1 + comp_count) % comp_count;
                continue;
            } else if (k == K_ENTER) {
                apply_completion();
                continue;
            } else if (k == K_ESC) {
                side_win = 0;
                continue;
            }
        }

        switch (k) {
            case K_CHAR:      ed_ins_char(c);  break;
            case K_ENTER:     ed_split_line(); break;
            case K_BACKSPACE: ed_backspace();  break;
            case K_TAB:       trigger_completion(); break;
            case K_DEL:       ed_delete();     break;
            case K_LEFT:      ed_move_left();  break;
            case K_RIGHT:     ed_move_right(); break;
            case K_UP:        ed_move_up();    break;
            case K_DOWN:      ed_move_down();  break;
            case K_HOME:      ed_home();       break;
            case K_END:       ed_end();        break;
            case K_PGUP:      ed_pgup();       break;
            case K_PGDN:      ed_pgdn();       break;
            case K_CTRL_K:    ed_cut_line();   break;
            case K_CTRL_U:    ed_paste();      break;
            case K_CTRL_F:    ed_find_replace(); break;
            case K_CTRL_S:
                if (ed.path[0]) {
                    ed_save_to(ed.path);
                } else {
                    snprintf(ed.msg, sizeof(ed.msg), "No filename; use Ctrl+O");
                }
                break;
            case K_CTRL_O: {
                char name[PATH_MAX];
                snprintf(name, sizeof(name), "%s", ed.path);
                if (ed_prompt("File Name to Write: ", name, (int)sizeof(name)) == 0) {
                    if (name[0]) ed_save_to(name);
                    else snprintf(ed.msg, sizeof(ed.msg), "Empty filename");
                } else {
                    snprintf(ed.msg, sizeof(ed.msg), "Save cancelled");
                }
                break;
            }
            case K_CTRL_X:
                quit_sequence();
                if (got_term == 2) running = 0;
                break;
            case K_CTRL_C:
                snprintf(ed.msg, sizeof(ed.msg), "Press Ctrl+X to exit, Ctrl+S to save");
                break;
            case K_CTRL_G:
                snprintf(ed.msg, sizeof(ed.msg), "Ctrl+O saveas  Ctrl+X exit  Ctrl+F find  Ctrl+S save  Tab complete");
                break;
            default:
                break;
        }
    }

    raw_disable();

    for (int i = 0; i < ed.n; i++) free(ed.lines[i].s);
    free(ed.lines);
    free(clip_buf);
    for (int i = 0; i < comp_count; i++) free(completions[i]);
    return 0;
}
