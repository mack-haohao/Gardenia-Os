#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* 0. Constants and global configuration */
#define LISTEN_PORT     1
#define MAX_BODY_SIZE   (1024 * 1024)
#define MAX_HEADER_SIZE 8192
#define IO_TIMEOUT_SEC  5

typedef struct {
    char   *code;
    size_t  code_len;
    char   *color;
    size_t  color_len;
} rule_t;

static rule_t *rules = NULL;
static size_t  n_rules = 0;
static size_t  rules_cap = 0;

/* First-byte index: bucket by code[0], each bucket sorted by code_len descending */
typedef struct {
    rule_t **v;
    size_t   n;
    size_t   cap;
} bucket_t;

static bucket_t buckets[256];

static volatile sig_atomic_t reload_flag = 0;
static volatile sig_atomic_t stop_flag   = 0;

static void load_configs(void);
static void handle_client(int client_fd);
static void install_handlers(void);
static void free_rules(void);

/* 3. PID file (safe version) */
static const char *runtime_dir(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg && access(xdg, W_OK) == 0) return xdg;
    return "/tmp";
}

static const char *pid_file_path(void) {
    static char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/gardeniaApi-%d.pid",
             runtime_dir(), (int)getuid());
    return path;
}

static int write_pid_file(void) {
    const char *p = pid_file_path();
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) {
        fd = open(p, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return -1;
        struct stat st;
        if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
            close(fd);
            return -1;
        }
        if (ftruncate(fd, 0) < 0) { close(fd); return -1; }
    }
    if (fd < 0) return -1;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    return (w == (ssize_t)len) ? 0 : -1;
}

static pid_t read_pid_file(void) {
    const char *p = pid_file_path();
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        return -1;
    }
    char buf[64];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = '\0';
    char *end = NULL;
    long pid = strtol(buf, &end, 10);
    if (end == buf || pid <= 0) return -1;
    return (pid_t)pid;
}

static void remove_pid_file(void) {
    unlink(pid_file_path());
}

/* Data write lock: keeps forked children from racing on read-modify-write */
static int acquire_data_lock(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/gardeniaApi-data-%d.lock",
             runtime_dir(), (int)getuid());
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        return -1;
    }
    while (flock(fd, LOCK_EX) < 0) {
        if (errno != EINTR) { close(fd); return -1; }
    }
    return fd;
}

/* 4. Rule management */
static void clear_index(void) {
    for (int i = 0; i < 256; i++) {
        free(buckets[i].v);
        buckets[i].v = NULL;
        buckets[i].n = 0;
        buckets[i].cap = 0;
    }
}

static void free_rules(void) {
    clear_index();
    if (!rules) return;
    for (size_t i = 0; i < n_rules; i++) {
        free(rules[i].code);
        free(rules[i].color);
    }
    free(rules);
    rules = NULL;
    n_rules = 0;
    rules_cap = 0;
}

static int add_rule(const char *code, size_t code_len, const char *color) {
    if (code_len == 0) return 0;
    for (size_t i = 0; i < n_rules; i++) {
        if (rules[i].code_len == code_len && memcmp(rules[i].code, code, code_len) == 0) {
            char *nc = strdup(color);
            if (!nc) return -1;
            free(rules[i].color);
            rules[i].color = nc;
            rules[i].color_len = strlen(nc);
            return 0;
        }
    }
    if (n_rules == rules_cap) {
        size_t nc = rules_cap ? rules_cap * 2 : 8;
        rule_t *nr = realloc(rules, nc * sizeof(*nr));
        if (!nr) return -1;
        rules = nr;
        rules_cap = nc;
    }
    char *c   = strndup(code, code_len);
    char *col = strdup(color);
    if (!c || !col) { free(c); free(col); return -1; }
    rules[n_rules].code      = c;
    rules[n_rules].code_len  = code_len;
    rules[n_rules].color     = col;
    rules[n_rules].color_len = strlen(col);
    n_rules++;
    return 0;
}

static int bucket_push(bucket_t *b, rule_t *r) {
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8;
        rule_t **nv = realloc(b->v, nc * sizeof(*nv));
        if (!nv) return -1;
        b->v = nv;
        b->cap = nc;
    }
    b->v[b->n++] = r;
    return 0;
}

static int cmp_rule_len_desc(const void *a, const void *b) {
    const rule_t *ra = *(rule_t * const *)a;
    const rule_t *rb = *(rule_t * const *)b;
    if (ra->code_len < rb->code_len) return 1;
    if (ra->code_len > rb->code_len) return -1;
    return 0;
}

static void rebuild_index(void) {
    clear_index();
    for (size_t i = 0; i < n_rules; i++) {
        unsigned char c = (unsigned char)rules[i].code[0];
        bucket_push(&buckets[c], &rules[i]);
    }
    for (int i = 0; i < 256; i++) {
        if (buckets[i].n > 1)
            qsort(buckets[i].v, buckets[i].n, sizeof(rule_t *), cmp_rule_len_desc);
    }
}

/* 5. Color parsing */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const struct { const char *name; int value; int as_256; } color_names[] = {
    {"black",30,0},{"red",31,0},{"green",32,0},{"yellow",33,0},
    {"blue",34,0},{"magenta",35,0},{"cyan",36,0},{"white",37,0},
    {"bright_red",91,0},{"bright_yellow",93,0},{"bright_cyan",96,0},
    {"orange",214,1}, {"purple",141,1}, {NULL,0,0}
};

static char *parse_color(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    const char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t len = (size_t)(e - s);
    if (len == 0) return NULL;

    for (size_t i = 0; color_names[i].name; i++) {
        if (strlen(color_names[i].name) == len && strncasecmp(s, color_names[i].name, len) == 0) {
            char *c = malloc(32);
            if (!c) return NULL;
            if (color_names[i].as_256) snprintf(c, 32, "\033[38;5;%dm", color_names[i].value);
            else snprintf(c, 32, "\033[%dm", color_names[i].value);
            return c;
        }
    }
    if (len >= 7 && s[0] == '#') {
        int r1 = hexval(s[1]), r2 = hexval(s[2]);
        int g1 = hexval(s[3]), g2 = hexval(s[4]);
        int b1 = hexval(s[5]), b2 = hexval(s[6]);
        if (r1>=0 && r2>=0 && g1>=0 && g2>=0 && b1>=0 && b2>=0) {
            char *c = malloc(64);
            if (!c) return NULL;
            snprintf(c, 64, "\033[38;2;%d;%d;%dm", r1*16+r2, g1*16+g2, b1*16+b2);
            return c;
        }
    }
    return NULL;
}

static char *parse_quoted(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while (*p && *p != '"') {
        char ch;
        if (*p == '\\' && p[1]) {
            p++;
            ch = (*p == 'n') ? '\n' : (*p == 't') ? '\t' : *p;
            p++;
        } else ch = *p++;
        if (len + 2 > cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) { free(buf); return NULL; }
            buf = nb; cap = ncap;
        }
        buf[len++] = ch;
    }
    if (*p != '"') { free(buf); return NULL; }
    p++;
    buf[len] = '\0';
    *pp = p;
    return buf;
}

static void load_configs_from(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char fpath[8192];
        int w = snprintf(fpath, sizeof(fpath), "%s/%s", path, e->d_name);
        if (w < 0 || (size_t)w >= sizeof(fpath)) continue;
        FILE *fp = fopen(fpath, "r");
        if (!fp) continue;
        if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); continue; }
        long sz = ftell(fp);
        if (sz < 0) { fclose(fp); continue; }
        if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); continue; }
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(fp); continue; }
        size_t got = fread(buf, 1, (size_t)sz, fp);
        buf[got] = '\0';
        fclose(fp);
        const char *p = buf;
        while (*p) {
            while (*p && *p != '{') p++;
            if (!*p) break;
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            char *code = parse_quoted(&p);
            if (!code) break;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p != ',') { free(code); continue; }
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            char *colstr = parse_quoted(&p);
            if (colstr) {
                char *color = parse_color(colstr);
                if (color) { add_rule(code, strlen(code), color); free(color); }
                free(colstr);
            }
            free(code);
        }
        free(buf);
    }
    closedir(d);
}

static void load_configs(void) {
    free_rules();
    load_configs_from("/etc/gardenia/syntax");
    const char *home = getenv("HOME");
    if (home) {
        char user_path[1024];
        int w = snprintf(user_path, sizeof(user_path), "%s/.config/gardenia/syntax", home);
        if (w > 0 && (size_t)w < sizeof(user_path))
            load_configs_from(user_path);
    }
    rebuild_index();
}

/* 6. Highlight engine */
static int is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}
static int is_boundary_match(const char *input, size_t in_len, size_t i, const rule_t *rule) {
    if (memcmp(input + i, rule->code, rule->code_len) != 0) return 0;
    if (is_word_char(rule->code[0]) && i > 0 && is_word_char(input[i - 1])) return 0;
    char last_char = rule->code[rule->code_len - 1];
    size_t next_idx = i + rule->code_len;
    if (is_word_char(last_char) && next_idx < in_len && is_word_char(input[next_idx])) return 0;
    return 1;
}

/* Growable output buffer; avoids boundary-case size estimation */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} obuf;

static int obuf_grow(obuf *o, size_t need) {
    if (need > SIZE_MAX - o->len) return -1;
    size_t want = o->len + need;
    if (want <= o->cap) return 0;
    size_t nc = o->cap ? o->cap : 256;
    while (nc < want) {
        if (nc > SIZE_MAX / 2) { nc = want; break; }
        nc *= 2;
    }
    char *np = realloc(o->data, nc);
    if (!np) return -1;
    o->data = np;
    o->cap  = nc;
    return 0;
}

static int obuf_append(obuf *o, const char *src, size_t n) {
    if (obuf_grow(o, n) < 0) return -1;
    if (n) memcpy(o->data + o->len, src, n);
    o->len += n;
    return 0;
}

static int obuf_append_escaped(obuf *o, const char *src, size_t n) {
    /* Append verbatim, no escaping. ANSI from upstream tools (e.g. ls)
     * must pass through unchanged, otherwise it renders as literals. */
    return obuf_append(o, src, n);
}


static char *highlight(const char *input) {
    size_t in_len = strlen(input);
    obuf o = {0};
    if (obuf_grow(&o, 1024) < 0) return NULL;

    size_t i = 0;
    while (i < in_len) {
        unsigned char c = (unsigned char)input[i];
        bucket_t *b = &buckets[c];
        rule_t *best = NULL;
        for (size_t k = 0; k < b->n; k++) {
            rule_t *rule = b->v[k];
            if (rule->code_len > in_len - i) continue;
            if (is_boundary_match(input, in_len, i, rule)) {
                best = rule;
                break; /* Descending by length; first hit is the longest match */
            }
        }
        if (best) {
            if (obuf_append(&o, best->color, best->color_len) < 0 ||
                obuf_append_escaped(&o, input + i, best->code_len) < 0 ||
                obuf_append(&o, "\033[0m", 4) < 0) {
                free(o.data);
                return NULL;
            }
            i += best->code_len;
        } else {
            if (obuf_append_escaped(&o, input + i, 1) < 0) {
                free(o.data);
                return NULL;
            }
            i++;
        }
    }
    if (obuf_grow(&o, 1) < 0) { free(o.data); return NULL; }
    o.data[o.len] = '\0';
    return o.data;
}

/* 7. Network helpers */
static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += w; len -= (size_t)w;
    }
    return 0;
}
static void send_simple_response(int fd, int code, const char *reason) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n\r\n", code, reason);
    if (len > 0) write_all(fd, buf, (size_t)len);
}
static void send_body_response(int fd, const char *body, size_t len) {
    char header[256];
    int hl = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n", len);
    if (hl > 0) write_all(fd, header, (size_t)hl);
    if (len) write_all(fd, body, len);
}

/* 8. Minimal JSON parser/serializer */
typedef struct jvalue jvalue;
typedef struct { char *key; jvalue *val; } jmember;

struct jvalue {
    int type; /* 0 = number, 1 = object, 2 = string */
    long long num;
    char *str;
    size_t n_members, cap;
    jmember *members;
};

static jvalue *json_new(int type) {
    jvalue *v = calloc(1, sizeof(*v));
    if (v) v->type = type;
    return v;
}
static void json_free(jvalue *v) {
    if (!v) return;
    if (v->type == 1) {
        for (size_t i = 0; i < v->n_members; i++) {
            free(v->members[i].key);
            json_free(v->members[i].val);
        }
        free(v->members);
    } else if (v->type == 2) {
        free(v->str);
    }
    free(v);
}
static const char *json_skip_ws(const char *p) {
    while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++;
    return p;
}

static char *json_parse_string(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while (*p && *p != '"') {
        char ch;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                default: free(buf); return NULL;
            }
            p++;
        } else ch = *p++;
        if (len + 2 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = ch;
    }
    if (*p != '"') { free(buf); return NULL; }
    p++;
    buf[len] = 0;
    *pp = p;
    return buf;
}

static jvalue *json_parse_value(const char **pp);

static jvalue *json_parse_object(const char **pp) {
    const char *p = *pp;
    if (*p != '{') return NULL;
    p++;
    jvalue *v = json_new(1);
    if (!v) return NULL;
    p = json_skip_ws(p);
    if (*p == '}') { p++; *pp = p; return v; }
    for (;;) {
        p = json_skip_ws(p);
        char *key = json_parse_string(&p);
        if (!key) goto fail;
        p = json_skip_ws(p);
        if (*p != ':') { free(key); goto fail; }
        p++;
        p = json_skip_ws(p);
        jvalue *val = json_parse_value(&p);
        if (!val) { free(key); goto fail; }
        if (v->n_members == v->cap) {
            size_t nc = v->cap ? v->cap * 2 : 8;
            jmember *nm = realloc(v->members, nc * sizeof(jmember));
            if (!nm) { free(key); json_free(val); goto fail; }
            v->members = nm; v->cap = nc;
        }
        v->members[v->n_members].key = key;
        v->members[v->n_members].val = val;
        v->n_members++;
        p = json_skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; *pp = p; return v; }
        goto fail;
    }
    fail:
    json_free(v);
    return NULL;
}

static jvalue *json_parse_value(const char **pp) {
    const char *p = json_skip_ws(*pp);
    if (*p == '{') {
        jvalue *v = json_parse_object(&p);
        if (v) *pp = p;
        return v;
    }
    if (*p == '"') {
        char *s = json_parse_string(&p);
        if (!s) return NULL;
        jvalue *v = json_new(2);
        if (v) v->str = s;
        else free(s);
        *pp = p;
        return v;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *end = NULL;
        long long num = strtoll(p, &end, 10);
        if (end == p) return NULL;
        jvalue *v = json_new(0);
        if (v) v->num = num;
        *pp = end;
        return v;
    }
    return NULL;
}

static jvalue *json_get(jvalue *obj, const char *key) {
    if (!obj || obj->type != 1) return NULL;
    for (size_t i = 0; i < obj->n_members; i++)
        if (strcmp(obj->members[i].key, key) == 0) return obj->members[i].val;
    return NULL;
}
static int json_add(jvalue *obj, const char *key, jvalue *val) {
    if (!obj || obj->type != 1) return -1;
    if (obj->n_members == obj->cap) {
        size_t nc = obj->cap ? obj->cap * 2 : 8;
        jmember *nm = realloc(obj->members, nc * sizeof(jmember));
        if (!nm) return -1;
        obj->members = nm; obj->cap = nc;
    }
    char *k = strdup(key);
    if (!k) return -1;
    obj->members[obj->n_members].key = k;
    obj->members[obj->n_members].val = val;
    obj->n_members++;
    return 0;
}

/* Replace the value if the key exists (freeing the old one), otherwise append */
static int json_set(jvalue *obj, const char *key, jvalue *val) {
    if (!obj || obj->type != 1) return -1;
    for (size_t i = 0; i < obj->n_members; i++) {
        if (strcmp(obj->members[i].key, key) == 0) {
            json_free(obj->members[i].val);
            obj->members[i].val = val;
            return 0;
        }
    }
    return json_add(obj, key, val);
}

static void json_write_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            case '\b': fputs("\\b", f); break;
            case '\f': fputs("\\f", f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                else fputc(*p, f);
        }
    }
    fputc('"', f);
}
static void json_write(FILE *f, jvalue *v) {
    if (v->type == 0) fprintf(f, "%lld", v->num);
    else if (v->type == 2) json_write_string(f, v->str);
    else {
        fputc('{', f);
        for (size_t i = 0; i < v->n_members; i++) {
            if (i) fputc(',', f);
            json_write_string(f, v->members[i].key);
            fputc(':', f);
            json_write(f, v->members[i].val);
        }
        fputc('}', f);
    }
}

/* 9. Autocomplete storage layer */
static const char *get_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_dir) ? pw->pw_dir : NULL;
}

static int mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t l = strlen(path);
    if (l == 0 || l >= sizeof(buf)) return -1;
    memcpy(buf, path, l + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int valid_lang(const char *lang) {
    if (!lang || !*lang) return 0;
    for (const char *p = lang; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
            return 0;
    return 1;
}

static int user_lang_path(const char *lang, char *out, size_t out_cap) {
    if (!valid_lang(lang)) return -1;
    const char *home = get_home();
    if (!home) return -1;
    int w = snprintf(out, out_cap, "%s/.config/gardenia/completions/%s.json", home, lang);
    return (w < 0 || (size_t)w >= out_cap) ? -1 : 0;
}

static int sys_lang_path(const char *lang, char *out, size_t out_cap) {
    if (!valid_lang(lang)) return -1;
    int w = snprintf(out, out_cap, "/etc/gardenia/completions/%s.json", lang);
    return (w < 0 || (size_t)w >= out_cap) ? -1 : 0;
}

static int is_regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Read path: user file first, fall back to system */
static int find_read_path(const char *lang, char *out, size_t out_cap) {
    char p[PATH_MAX];
    if (user_lang_path(lang, p, sizeof(p)) == 0 && is_regular_file(p)) {
        snprintf(out, out_cap, "%s", p);
        return 0;
    }
    if (sys_lang_path(lang, p, sizeof(p)) == 0 && is_regular_file(p)) {
        snprintf(out, out_cap, "%s", p);
        return 0;
    }
    return -1;
}

static jvalue *load_lang_data(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return json_new(1); /* File missing: return an empty object */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return json_new(1); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return json_new(1); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return json_new(1); }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return json_new(1); }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    const char *p = json_skip_ws(buf);
    jvalue *root = NULL;
    if (*p != 0) root = json_parse_value(&p);
    else         root = json_new(1);   /* Empty file: treat as an empty object */
    free(buf);
    if (!root) {
        /* Parse failure (corrupt file): do not overwrite, return NULL so the caller can report */
        return NULL;
    }
    return root;
}

static int save_lang_data(const char *path, jvalue *root) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    json_write(f, root);
    fputc('\n', f);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Field must be an object; if present with the wrong type, replace with a new empty object */
static jvalue *get_or_create_object(jvalue *parent, const char *key) {
    if (!parent || parent->type != 1) return NULL;
    jvalue *v = json_get(parent, key);
    if (v && v->type == 1) return v;
    jvalue *nv = json_new(1);
    if (!nv) return NULL;
    if (v) {
        if (json_set(parent, key, nv) != 0) { json_free(nv); return NULL; }
    } else {
        if (json_add(parent, key, nv) != 0) { json_free(nv); return NULL; }
    }
    return nv;
}

/* Increment count; if the field exists with the wrong type, replace it with 1 */
static int inc_count(jvalue *obj, const char *key) {
    if (!obj || obj->type != 1) return -1;
    jvalue *v = json_get(obj, key);
    if (v) {
        if (v->type == 0) { v->num++; return 0; }
        jvalue *nv = json_new(0);
        if (!nv) return -1;
        nv->num = 1;
        return json_set(obj, key, nv);
    }
    jvalue *nv = json_new(0);
    if (!nv) return -1;
    nv->num = 1;
    return json_add(obj, key, nv);
}

static int record_usage(jvalue *root, const char *cmd, const char *line) {
    if (!root || root->type != 1) return -1;

    jvalue *commands = get_or_create_object(root, "commands");
    if (!commands) return -1;
    if (inc_count(commands, cmd) != 0) return -1;

    jvalue *lines = get_or_create_object(root, "lines");
    if (!lines) return -1;
    jvalue *line_obj = get_or_create_object(lines, line);
    if (!line_obj) return -1;
    if (inc_count(line_obj, cmd) != 0) return -1;

    return 0;
}

/* 10. Parameter decoding */
static char *url_decode(const char *s, size_t len) {
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '+') buf[j++] = ' ';
        else if (s[i] == '%' && i + 2 < len) {
            int h = hexval(s[i+1]);
            int l = hexval(s[i+2]);
            if (h < 0 || l < 0) { buf[j++] = s[i]; continue; }
            buf[j++] = (char)((h << 4) | l);
            i += 2;
        } else buf[j++] = s[i];
    }
    buf[j] = 0;
    return buf;
}

static char *urlenc_get(const char *s, size_t len, const char *key) {
    if (!s || !len) return NULL;
    size_t klen = strlen(key);
    const char *p = s;
    const char *end = s + len;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        if (!amp) amp = end;
        if ((size_t)(amp - p) > klen && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            return url_decode(p + klen + 1, (size_t)(amp - (p + klen + 1)));
        }
        p = amp + 1;
    }
    return NULL;
}

static char *extract_param(const char *qs, const char *body, size_t body_len, const char *name) {
    if (qs) {
        char *v = urlenc_get(qs, strlen(qs), name);
        if (v) return v;
    }
    if (body && body_len)
        return urlenc_get(body, body_len, name);
    return NULL;
}

/* 11. /autoconf endpoints */
typedef struct { const char *cmd; long long score; } scored_t;

static int cmp_scored(const void *a, const void *b) {
    const scored_t *sa = a, *sb = b;
    if (sa->score < sb->score) return 1;
    if (sa->score > sb->score) return -1;
    return strcmp(sa->cmd, sb->cmd);
}

static void do_autoconf_query(int fd, const char *lang, const char *prefix, const char *line) {
    char path[PATH_MAX];
    if (find_read_path(lang, path, sizeof(path)) < 0) {
        send_simple_response(fd, 404, "Not Found");
        return;
    }
    jvalue *root = load_lang_data(path);
    if (!root) { send_simple_response(fd, 500, "Internal Server Error (Invalid JSON)"); return; }

    jvalue *commands = json_get(root, "commands");
    jvalue *lines    = json_get(root, "lines");
    jvalue *line_obj = lines ? json_get(lines, line) : NULL;

    size_t cap = 32, n = 0;
    scored_t *res = malloc(cap * sizeof(scored_t));
    if (!res) {
        json_free(root);
        send_simple_response(fd, 500, "Internal Server Error");
        return;
    }

    size_t plen = prefix ? strlen(prefix) : 0;

    if (commands && commands->type == 1) {
        for (size_t i = 0; i < commands->n_members; i++) {
            const char *cmd = commands->members[i].key;
            if (plen && (strlen(cmd) < plen || memcmp(cmd, prefix, plen) != 0)) continue;
            long long g = commands->members[i].val->type == 0 ? commands->members[i].val->num : 0;
            long long ctx = 0;
            if (line_obj) {
                jvalue *lv = json_get(line_obj, cmd);
                if (lv && lv->type == 0) ctx = lv->num;
            }
            if (n == cap) {
                size_t nc = cap * 2;
                scored_t *nr = realloc(res, nc * sizeof(scored_t));
                if (!nr) break;
                res = nr; cap = nc;
            }
            res[n].cmd = cmd;
            res[n].score = g + ctx;
            n++;
        }
    }

    if (line_obj && line_obj->type == 1) {
        for (size_t i = 0; i < line_obj->n_members; i++) {
            const char *cmd = line_obj->members[i].key;
            if (plen && (strlen(cmd) < plen || memcmp(cmd, prefix, plen) != 0)) continue;
            int found = 0;
            for (size_t j = 0; j < n; j++)
                if (strcmp(res[j].cmd, cmd) == 0) { found = 1; break; }
            if (found) continue;
            long long ctx = line_obj->members[i].val->type == 0 ? line_obj->members[i].val->num : 0;
            if (n == cap) {
                size_t nc = cap * 2;
                scored_t *nr = realloc(res, nc * sizeof(scored_t));
                if (!nr) break;
                res = nr; cap = nc;
            }
            res[n].cmd = cmd;
            res[n].score = ctx;
            n++;
        }
    }

    qsort(res, n, sizeof(scored_t), cmp_scored);

    size_t out_cap = 64;
    for (size_t i = 0; i < n; i++) out_cap += strlen(res[i].cmd) + 2;
    char *out = malloc(out_cap);
    if (!out) {
        free(res); json_free(root);
        send_simple_response(fd, 500, "Internal Server Error");
        return;
    }
    size_t out_len = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(res[i].cmd);
        memcpy(out + out_len, res[i].cmd, l);
        out_len += l;
        out[out_len++] = '\n';
    }

    send_body_response(fd, out, out_len);
    free(out);
    free(res);
    json_free(root);
}

static void handle_autoconf_data(int fd, const char *qs, const char *body, size_t body_len) {
    char *lang  = extract_param(qs, body, body_len, "lang");
    char *param = extract_param(qs, body, body_len, "param");
    char *line  = extract_param(qs, body, body_len, "line");

    if (!lang || !param || !line) {
        free(lang); free(param); free(line);
        send_simple_response(fd, 400, "Bad Request");
        return;
    }

    if (strcmp(param, "exist") == 0) {
        do_autoconf_query(fd, lang, NULL, line);
        free(lang); free(param); free(line);
        return;
    }

    if (!valid_lang(lang)) {
        free(lang); free(param); free(line);
        send_simple_response(fd, 500, "Internal Server Error");
        return;
    }

    char upath[PATH_MAX], spath[PATH_MAX];
    int has_user = user_lang_path(lang, upath, sizeof(upath)) == 0;
    int has_sys  = sys_lang_path(lang, spath, sizeof(spath)) == 0;

    if (!has_user && !has_sys) {
        free(lang); free(param); free(line);
        send_simple_response(fd, 500, "Internal Server Error");
        return;
    }

    int user_exists = has_user && is_regular_file(upath);
    int sys_exists  = has_sys  && is_regular_file(spath);

    /* Choose the write target: prefer the user path */
    char wpath[PATH_MAX];
    int use_user = 0;
    wpath[0] = '\0';

    if (has_user) {
        if (user_exists) use_user = 1;
        else {
            char dir[PATH_MAX];
            snprintf(dir, sizeof(dir), "%s", upath);
            char *slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
                if (mkdir_p(dir) == 0) use_user = 1;
            }
        }
    }

    if (use_user) {
        snprintf(wpath, sizeof(wpath), "%s", upath);
    } else {
        free(lang); free(param); free(line);
        send_simple_response(fd, 500, "Internal Server Error");
        return;
    }

    /* Hold the lock across read-modify-write to prevent lost updates */
    int lock_fd = acquire_data_lock();

    jvalue *root = NULL;
    int fatal = 0;

    if (use_user) {
        if (user_exists) {
            /* User file exists: trust it; error out on corruption instead of overwriting */
            root = load_lang_data(upath);
            if (!root) fatal = 1;
        } else if (sys_exists) {
            /* User file missing: use the system file as baseline, write back to the user path */
            root = load_lang_data(spath);
            if (!root) fatal = 1;
        }
    } else {
        if (sys_exists) {
            root = load_lang_data(spath);
            if (!root) fatal = 1;
        }
    }

    if (fatal) {
        if (lock_fd >= 0) close(lock_fd);
        free(lang); free(param); free(line);
        send_simple_response(fd, 500, "Internal Server Error (Invalid JSON)");
        return;
    }

    if (!root) root = json_new(1);
    if (!root) {
        if (lock_fd >= 0) close(lock_fd);
        free(lang); free(param); free(line);
        send_simple_response(fd, 500, "Internal Server Error");
        return;
    }

    if (record_usage(root, param, line) == 0 && save_lang_data(wpath, root) == 0)
        send_simple_response(fd, 200, "OK");
    else
        send_simple_response(fd, 500, "Internal Server Error");

    json_free(root);
    if (lock_fd >= 0) close(lock_fd);
    free(lang); free(param); free(line);
}

static void handle_autoconf_get(int fd, const char *qs, const char *body, size_t body_len) {
    char *lang  = extract_param(qs, body, body_len, "lang");
    char *param = extract_param(qs, body, body_len, "param");
    char *line  = extract_param(qs, body, body_len, "line");

    if (!lang || !param || !line) {
        free(lang); free(param); free(line);
        send_simple_response(fd, 400, "Bad Request");
        return;
    }
    const char *prefix = (strcmp(param, "exist") == 0) ? NULL : param;
    do_autoconf_query(fd, lang, prefix, line);
    free(lang); free(param); free(line);
}

/* 12. Request parsing and HTTP dispatch */
static int parse_request_line(const char *line, char *method, size_t mcap,
                              char *path, size_t pcap, char *qs, size_t qcap) {
    method[0] = path[0] = qs[0] = 0;
    const char *p = line;
    size_t ml = 0;
    while (*p && *p != ' ' && ml + 1 < mcap) method[ml++] = *p++;
    method[ml] = 0;
    if (*p != ' ') return -1;
    p++;
    size_t pl = 0;
    while (*p && *p != ' ' && pl + 1 < pcap) {
        if (*p == '?') break;
        path[pl++] = *p++;
    }
    path[pl] = 0;
    if (*p == '?') {
        p++;
        size_t ql = 0;
        while (*p && *p != ' ' && ql + 1 < qcap) qs[ql++] = *p++;
        qs[ql] = 0;
    }
    return 0;
}

/* Strictly parse the header area (req .. header_len, excluding the final blank line).
 * Returns 0 on success; -1 on malformed or illegal header. */
static int parse_headers(const char *req, size_t header_len,
                         long *content_length, int *bad) {
    *content_length = 0;
    *bad = 0;
    int has_cl = 0;
    long cl_val = 0;

    const char *p   = req;
    const char *end = req + header_len;

    /* Skip the request line */
    const char *eol = memmem(p, (size_t)(end - p), "\r\n", 2);
    if (!eol) { eol = end; }
    p = eol + 2;

    while (p < end) {
        eol = memmem(p, (size_t)(end - p), "\r\n", 2);
        if (!eol) { eol = end; }
        if (eol == p) break;

        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (!colon) { if (eol == end) break; p = eol + 2; continue; }

        size_t name_len = (size_t)(colon - p);
        while (name_len > 0 && (p[name_len-1] == ' ' || p[name_len-1] == '\t'))
            name_len--;

        if (name_len == 14 && strncasecmp(p, "Content-Length", 14) == 0) {
            const char *v  = colon + 1;
            const char *ve = eol;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            if (v == ve) { *bad = 1; return -1; }
            long n = 0;
            for (const char *q = v; q < ve; q++) {
                if (*q < '0' || *q > '9') { *bad = 1; return -1; }
                n = n * 10 + (*q - '0');
                if (n > MAX_BODY_SIZE) { *bad = 1; return -1; }
            }
            if (has_cl && n != cl_val) { *bad = 1; return -1; }
            has_cl = 1;
            cl_val = n;
        } else if (name_len == 17 && strncasecmp(p, "Transfer-Encoding", 17) == 0) {
            *bad = 1;
            return -1;
        }

        if (eol == end) break;
        p = eol + 2;
    }

    *content_length = cl_val;
    return 0;
}

static void handle_client(int client_fd) {
    char   req[MAX_HEADER_SIZE + 1];
    size_t n = 0;
    int    header_done = 0;

    while (n < MAX_HEADER_SIZE) {
        ssize_t r = read(client_fd, req + n, MAX_HEADER_SIZE - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                send_simple_response(client_fd, 408, "Request Timeout");
                return;
            }
            return;
        }
        if (r == 0) break;
        n += (size_t)r;
        req[n] = '\0';
        if (memmem(req, n, "\r\n\r\n", 4) != NULL) { header_done = 1; break; }
    }
    if (!header_done) {
        send_simple_response(client_fd, 431, "Request Header Fields Too Large");
        return;
    }

    char *hdrend = memmem(req, n, "\r\n\r\n", 4);
    size_t header_len = (size_t)(hdrend - req);

    /* Request line */
    char method[16], path[1024], qs[4096];
    if (parse_request_line(req, method, sizeof(method), path, sizeof(path), qs, sizeof(qs)) < 0) {
        send_simple_response(client_fd, 400, "Bad Request");
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        send_simple_response(client_fd, 405, "Method Not Allowed");
        return;
    }

    /* Content-Length: header area only; reject conflicting duplicates and Transfer-Encoding */
    long content_length = 0;
    int  bad_length     = 0;
    if (parse_headers(req, header_len, &content_length, &bad_length) < 0 || bad_length) {
        send_simple_response(client_fd, 400, "Bad Content-Length");
        return;
    }

    char *body = hdrend + 4;

    /* body */
    char *payload = calloc((size_t)content_length + 1, 1);
    if (!payload) {
        send_simple_response(client_fd, 500, "Internal Server Error");
        return;
    }
    size_t body_avail = n - (size_t)(body - req);
    size_t to_copy = body_avail < (size_t)content_length ? body_avail : (size_t)content_length;
    if (to_copy) memcpy(payload, body, to_copy);
    size_t total = to_copy;
    while (total < (size_t)content_length) {
        ssize_t r = read(client_fd, payload + total, (size_t)content_length - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                free(payload);
                send_simple_response(client_fd, 408, "Request Timeout");
                return;
            }
            free(payload);
            return;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    if (total < (size_t)content_length) {
        free(payload);
        send_simple_response(client_fd, 400, "Incomplete Body");
        return;
    }
    payload[(size_t)content_length] = '\0';

    /* Dispatch */
    if (strcmp(path, "/render") == 0 || strcmp(path, "/render/") == 0) {
        char *result = highlight(payload);
        if (!result) send_simple_response(client_fd, 500, "Internal Server Error");
        else { send_body_response(client_fd, result, strlen(result)); free(result); }
    } else if (strcmp(path, "/autoconf/data") == 0) {
        handle_autoconf_data(client_fd, qs, payload, total);
    } else if (strcmp(path, "/autoconf/get") == 0) {
        handle_autoconf_get(client_fd, qs, payload, total);
    } else {
        send_simple_response(client_fd, 404, "Not Found");
    }
    free(payload);
}

/* 13. Signal handling */
static void on_sighup(int sig) { (void)sig; reload_flag = 1; }
static void on_sigterm(int sig) { (void)sig; stop_flag = 1; }

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sighup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, NULL);

    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);

    /* Auto-reap forked children to avoid zombies */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa, NULL);
}

static void set_io_timeout(int fd) {
    struct timeval tv;
    tv.tv_sec  = IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* 14. Entry point */
static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "Options:\n"
            "  -r, --restart   Send SIGHUP to reload config, or start fresh\n"
            "  -h, --help      Show this help.\n", prog);
}

int main(int argc, char **argv) {
    int restart = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--restart") == 0 || strcmp(argv[i], "-r") == 0) restart = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { print_usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (restart) {
        pid_t pid = read_pid_file();
        if (pid > 0 && kill(pid, 0) == 0) {
            if (kill(pid, SIGHUP) == 0) {
                fprintf(stderr, "gardeniaApi: reload sent to pid %d\n", (int)pid);
                return 0;
            }
            perror("kill(SIGHUP)");
            return 1;
        }
        fprintf(stderr, "gardeniaApi: no running instance, starting new\n");
    }

    install_handlers();

    if (!restart) {
        pid_t pid = read_pid_file();
        if (pid > 0 && kill(pid, 0) == 0) {
            fprintf(stderr, "gardeniaApi: already running (pid %d); use -r\n", (int)pid);
            return 1;
        }
    }

    load_configs();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(LISTEN_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "gardeniaApi: bind %d failed: %s\n", LISTEN_PORT, strerror(errno));
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 64) < 0) { perror("listen"); close(server_fd); return 1; }

    if (write_pid_file() != 0)
        fprintf(stderr, "gardeniaApi: warn: pid file failed: %s\n", strerror(errno));
    atexit(remove_pid_file);

    fprintf(stderr, "gardeniaApi: listening on %d (pid %d, %zu rules)\n",
            LISTEN_PORT, (int)getpid(), n_rules);

    for (;;) {
        if (reload_flag) {
            reload_flag = 0;
            load_configs();
            fprintf(stderr, "gardeniaApi: reloaded (%zu rules)\n", n_rules);
        }
        if (stop_flag) break;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) usleep(10000);
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            /* Child: handle exactly one connection */
            close(server_fd);
            set_io_timeout(client_fd);
            handle_client(client_fd);
            close(client_fd);
            _exit(0);
        } else if (pid > 0) {
            close(client_fd);
        } else {
            /* fork failed: fall back to in-process synchronous handling */
            set_io_timeout(client_fd);
            handle_client(client_fd);
            close(client_fd);
        }
    }

    close(server_fd);
    fprintf(stderr, "gardeniaApi: shutting down\n");
    return 0;
}
