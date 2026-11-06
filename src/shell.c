#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <termios.h>
#include <pwd.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <glob.h>
#include <pty.h>

#define GASH_NAME       "gash"
#define GASH_VERSION    "0.7.0"
#define GASH_BUILD      "2026-11-03"
#define GASH_AUTHOR     "mack-haohao"
#define GASH_LICENSE    "MIT"
#define GASH_DESC       "A small GNU-ish shell with per-directory history"

#define GARDENIA_ART \
"     ⣠⣤⣄\n" \
" ⣠⣾⣿⣿⣷⣄\n" \
"⣾⣿⣿⣿⣿⣿⣷\n" \
" ⣾⣿⣿⣿⣿⣷\n" \
"   ⣠⣤⣄\n"

#define MAX_LINE          1024
#define MAX_ARGS          64
#define HIST_FILE         ".gash_history"
#define MAX_HIST          2000
#define MAX_MATCHES       512
#define CHOICE_ROWS       5
#define CHOICE_COLS       7
#define PASTE_MAX         4096
#define PASTE_READ_LIMIT  (1024 * 1024)
#define MAX_JOBS          256

#define CLR_RESET     "\033[0m"
#define CLR_USERHOST  "\033[38;5;141m"
#define CLR_PATH      "\033[38;5;147m"
#define CLR_MATCH     "\033[38;5;177m"
#define CLR_NOTICE    "\033[38;5;245m"
#define CLR_PANE      "\033[38;5;214m"

/* forward decls */
typedef struct {
    char *argv[MAX_ARGS];
    int   argv_quoted[MAX_ARGS];
    int   argc;
    char *in_path;
    char *out_path;
    int   out_append;
} cmd_t;

static char *gardenia_render(const char *text);
static void  free_matches(void);
static int   run_builtin(cmd_t *c, int *last_status);
static void  term_raw_mode(void);
static int   is_builtin_name(const char *name);
static void  print_help(void);
static void  print_version(void);
static int   execute_line(const char *line_in, int *last_status);
static int   try_builtin_redirs(cmd_t *c, int *last_status);
static int   run_script(const char *path, int argn, char **args);
static int   try_run_script_file(const char *path, int argc, char **argv);

/* globals */
static char **history    = NULL;
static int    hist_count = 0;
static int    hist_cap   = 0;
static char   current_dir[MAX_LINE];

static int    script_argc = 0;
static char **script_argv = NULL;

static pid_t job_pids[MAX_JOBS];
static int   njobs = 0;

static const char *aliases[][2] = { {NULL, NULL} };

/* Jobs */
static void job_add(pid_t p) {
    if (njobs < MAX_JOBS) job_pids[njobs++] = p;
}

static void reap_jobs(void) {
    int i = 0;
    while (i < njobs) {
        int st;
        pid_t r = waitpid(job_pids[i], &st, WNOHANG);
        if (r == job_pids[i]) {
            job_pids[i] = job_pids[--njobs];
        } else if (r < 0 && errno == ECHILD) {
            job_pids[i] = job_pids[--njobs];
        } else {
            i++;
        }
    }
}

/* Panes (screen-like) */
#define MAX_PANES      32
#define PANE_NAME_MAX  64

typedef struct {
    char  name[PANE_NAME_MAX];
    pid_t pid;
    int   fd;
    int   alive;
} pane_t;

static pane_t panes[MAX_PANES];
static int    np = 0;

static int attach_pane(int idx);
static int kill_pane(int idx);

static void write_all_fd(int fd, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += (size_t)w;
    }
}

static void reap_panes(void) {
    for (int i = 0; i < np; i++) {
        if (!panes[i].alive) continue;
        int st;
        pid_t r = waitpid(panes[i].pid, &st, WNOHANG);
        if (r == panes[i].pid || (r < 0 && errno == ECHILD)) {
            panes[i].alive = 0;
            if (panes[i].fd >= 0) { close(panes[i].fd); panes[i].fd = -1; }
        }
    }
}

static int find_pane(const char *name) {
    for (int i = 0; i < np; i++)
        if (panes[i].alive && strcmp(panes[i].name, name) == 0) return i;
    return -1;
}

static int new_pane(const char *name) {
    if (!name || !*name) {
        fprintf(stderr, "gash-process-new: pane name required\n");
        return 1;
    }
    if (strlen(name) >= PANE_NAME_MAX) {
        fprintf(stderr, "gash-process-new: pane name too long\n");
        return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "gash-process-new: requires a terminal\n");
        return 1;
    }
    if (find_pane(name) >= 0) {
        fprintf(stderr, "gash-process-new: pane '%s' already exists\n", name);
        return 1;
    }

    int slot = -1;
    for (int i = 0; i < np; i++) if (!panes[i].alive) { slot = i; break; }
    int extended = 0;
    if (slot < 0) {
        if (np >= MAX_PANES) {
            fprintf(stderr, "gash-process-new: too many panes (max %d)\n", MAX_PANES);
            return 1;
        }
        slot = np++;
        extended = 1;
    }

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        ws.ws_row = 24; ws.ws_col = 80;
        ws.ws_xpixel = ws.ws_ypixel = 0;
    }

    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        if (extended) np--;
        return 1;
    }
    if (pid == 0) {
        setenv("GASH_PANE", name, 1);
        execl("/proc/self/exe", "gash", (char *)NULL);
        execlp("gash", "gash", (char *)NULL);
        _exit(127);
    }

    fcntl(master, F_SETFD, FD_CLOEXEC);
    strncpy(panes[slot].name, name, PANE_NAME_MAX - 1);
    panes[slot].name[PANE_NAME_MAX - 1] = '\0';
    panes[slot].pid   = pid;
    panes[slot].fd    = master;
    panes[slot].alive = 1;

    int ar = attach_pane(slot);
    if (ar < 0) {
        kill_pane(slot);
        return 1;
    }
    return ar;
}

static volatile sig_atomic_t pane_detach_req = 0;
static void sigusr1_handler(int sig) {
    (void)sig;
    pane_detach_req = 1;
}

static int attach_pane(int idx) {
    if (idx < 0 || idx >= np) return -1;
    pane_t *p = &panes[idx];
    if (!p->alive) {
        fprintf(stderr, "gash-process-join: pane '%s' is not running\n", p->name);
        return -1;
    }

    pane_detach_req = 0;
    /* SIGUSR1 handler is installed once in main; no switching here */

    kill(p->pid, SIGCONT);
    kill(p->pid, SIGWINCH);
    usleep(10000);

    int st;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid) {
        p->alive = 0;
        if (p->fd >= 0) { close(p->fd); p->fd = -1; }
        fprintf(stderr, "gash-process-join: pane '%s' has exited\n", p->name);
        return -1;
    }

    struct termios orig;
    if (tcgetattr(STDIN_FILENO, &orig) != 0) {
        perror("tcgetattr");
        return -1;
    }

    struct termios raw = orig;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    /* No screen clear: keep scrollback so switching panes does not wipe the prompt */

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(p->fd, TIOCSWINSZ, &ws);

    int detach = 0;
    int ctrl_e = 0;
    struct timeval ctrl_e_deadline;
    memset(&ctrl_e_deadline, 0, sizeof(ctrl_e_deadline));

    while (!detach) {
        /* Fast path: SIGUSR1 may arrive outside the previous select() */
        if (pane_detach_req) { detach = 1; break; }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(p->fd, &rfds);
        int maxfd = p->fd > STDIN_FILENO ? p->fd : STDIN_FILENO;

        struct timeval tv, *tvp = NULL;
        if (ctrl_e) {
            struct timeval now, remain;
            gettimeofday(&now, NULL);
            if (timercmp(&now, &ctrl_e_deadline, >=)) {
                tv.tv_sec = 0; tv.tv_usec = 0;
            } else {
                timersub(&ctrl_e_deadline, &now, &remain);
                tv = remain;
            }
            tvp = &tv;
        } else {
            /* Wake up periodically to check pane_detach_req so a SIGUSR1 that lands
             * outside select() cannot block us forever. */
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            tvp = &tv;
        }

        int rr = select(maxfd + 1, &rfds, NULL, NULL, tvp);
        if (rr < 0) {
            if (errno == EINTR) {
                if (pane_detach_req) { detach = 1; break; }
                continue;
            }
            break;
        }

        if (pane_detach_req) { detach = 1; break; }

        if (rr == 0) {
            if (ctrl_e) {
                char ce = 0x05;
                write_all_fd(p->fd, &ce, 1);
                ctrl_e = 0;
            }
            continue;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                detach = 1; break;
            }
            /* Pane is detaching: drop this round of input instead of forwarding to a stopped pane */
            if (pane_detach_req) { detach = 1; break; }

            unsigned char outbuf[sizeof(buf) * 2];
            size_t on = 0;
            for (ssize_t i = 0; i < n; i++) {
                unsigned char c = buf[i];
                if (ctrl_e) {
                    if (c == 'x' || c == 'X' ||
                        c == 'd' || c == 'D') { detach = 1; break; }
                    outbuf[on++] = 0x05;
                    ctrl_e = 0;
                }
                if (c == 0x05) {
                    ctrl_e = 1;
                    gettimeofday(&ctrl_e_deadline, NULL);
                    ctrl_e_deadline.tv_usec += 500000;
                    if (ctrl_e_deadline.tv_usec >= 1000000) {
                        ctrl_e_deadline.tv_usec -= 1000000;
                        ctrl_e_deadline.tv_sec  += 1;
                    }
                    continue;
                }
                outbuf[on++] = c;
            }
            if (on > 0) write_all_fd(p->fd, (const char *)outbuf, on);
            if (detach) break;
        }

        if (FD_ISSET(p->fd, &rfds)) {
            char buf[4096];
            ssize_t n = read(p->fd, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                p->alive = 0;
                if (p->fd >= 0) { close(p->fd); p->fd = -1; }
                waitpid(p->pid, NULL, 0);
                detach = 1;
                break;
            }
            write_all_fd(STDOUT_FILENO, buf, (size_t)n);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);

    if (pane_detach_req) {
        /* Triggered by exit / gash-process-exit inside a pane.
         *
         * Do not kill(p->pid, SIGSTOP) here:
         * the pane child calls raise(SIGSTOP) right after kill(getppid, SIGUSR1).
         * If the main shell sends SIGSTOP before that raise(SIGSTOP), the child is stopped by
         * the main shell's SIGSTOP; the next SIGCONT wakes it, the child continues to
         * raise(SIGSTOP) and stops again -- after join the pane is unresponsive (hang).
         *
         * Correct approach: let the child raise(SIGSTOP) itself and have the main shell waitpid
         * for the STOP. Add a 2s fallback that force-sends SIGSTOP on timeout. */
        int status = 0;
        pid_t w = 0;

        for (int k = 0; k < 200; k++) {     /* up to 2 seconds */
            w = waitpid(p->pid, &status, WUNTRACED | WNOHANG);
            if (w == p->pid) break;
            if (w < 0 && errno != EINTR) break;
            usleep(10000);
        }
        if (w != p->pid) {
            /* Fallback: the child did not stop on its own, force it */
            kill(p->pid, SIGSTOP);
            do {
                w = waitpid(p->pid, &status, WUNTRACED);
            } while (w < 0 && errno == EINTR);
        }

        if (w == p->pid && WIFSTOPPED(status)) {
            printf("\r\033[K[detached from %s]\n", p->name);
        } else if (w == p->pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
            p->alive = 0;
            if (p->fd >= 0) { close(p->fd); p->fd = -1; }
            printf("\r\033[K[pane %s exited]\n", p->name);
        } else if (w < 0 && errno == ECHILD) {
            p->alive = 0;
            if (p->fd >= 0) { close(p->fd); p->fd = -1; }
            printf("\r\033[K[pane %s exited]\n", p->name);
        } else {
            printf("\r\033[K[detached from %s]\n", p->name);
        }
        fflush(stdout);
    } else {
        /* Ctrl+E d/x local detach: the pane process keeps running */
        printf("\r\033[K[detached from %s]\n", p->name);
        fflush(stdout);
    }

    return 0;
}

static int kill_pane(int idx) {
    if (idx < 0 || idx >= np) return -1;

    pane_t *p = &panes[idx];
    if (!p->alive) return 0;

    kill(-p->pid, SIGHUP);
    kill(p->pid, SIGHUP);

    int reaped = 0;
    for (int k = 0; k < 50; k++) {
        int st;
        if (waitpid(p->pid, &st, WNOHANG) == p->pid) {
            reaped = 1;
            break;
        }
        usleep(10000);
    }
    if (!reaped) {
        kill(-p->pid, SIGKILL);
        kill(p->pid, SIGKILL);
        waitpid(p->pid, NULL, 0);
    }

    p->alive = 0;
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    return 0;
}

/* History */
static void hist_free(void) {
    if (!history) { hist_count = hist_cap = 0; return; }
    for (int i = 0; i < hist_count; i++) free(history[i]);
    free(history); history = NULL;
    hist_count = hist_cap = 0;
}
static void hist_add(const char *line) {
    if (!line || !*line) return;
    if (hist_count >= MAX_HIST) {
        free(history[0]);
        memmove(history, history + 1, (hist_count - 1) * sizeof(char *));
        hist_count--;
    }
    if (hist_count >= hist_cap) {
        int nc = hist_cap ? hist_cap * 2 : 64;
        char **nh = realloc(history, (size_t)nc * sizeof(char *));
        if (!nh) return;
        history = nh; hist_cap = nc;
    }
    history[hist_count++] = strdup(line);
}
static void hist_load_file(const char *path) {
    hist_free();
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        if (*buf) hist_add(buf);
    }
    fclose(f);
}
static void hist_append_file(const char *path, const char *line) {
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}
static void ensure_hist_file(const char *dir) {
    char path[MAX_LINE + 64];
    snprintf(path, sizeof(path), "%s/%s", dir, HIST_FILE);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
}
static void hist_switch_dir(const char *dir) {
    char path[MAX_LINE + 64];
    snprintf(path, sizeof(path), "%s/%s", dir, HIST_FILE);
    ensure_hist_file(dir);
    hist_load_file(path);
}
static void hist_append_current(const char *line) {
    char path[MAX_LINE + 64];
    snprintf(path, sizeof(path), "%s/%s", current_dir, HIST_FILE);
    hist_append_file(path, line);
}

/* Terminal raw mode */
static struct termios orig_termios;
static int            term_raw = 0;
static void term_restore(void) {
    if (term_raw) {
        fputs("\033[?2004l", stdout);
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        term_raw = 0;
    }
}
static void term_raw_mode(void) {
    if (term_raw) return;
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0) return;
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    /* TCSANOW: keep the stdin buffer so fast input before the prompt is not lost */
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        term_raw = 1;
        fputs("\033[?2004h", stdout);
        fflush(stdout);
    }
}

/* Prompt */
static void format_path(char *out, size_t outsz, const char *cwd) {
    const char *home = getenv("HOME");
    if (home && *home) {
        size_t hl = strlen(home);
        if (strncmp(cwd, home, hl) == 0) {
            const char *rest = cwd + hl;
            if (*rest == '\0' || *rest == '/') { snprintf(out, outsz, "~%s", rest); return; }
        }
    }
    snprintf(out, outsz, "%s", cwd);
}
static void build_prompt(char *out, size_t outsz, const char *cwd) {
    const struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : getenv("USER");
    if (!user) user = "user";
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) strcpy(host, "localhost");
    host[sizeof(host) - 1] = '\0';
    char *dot = strchr(host, '.');
    if (dot) *dot = '\0';
    char path[MAX_LINE + 64];
    format_path(path, sizeof(path), cwd);
    char mark = (geteuid() == 0) ? '#' : '$';

    char pane_prefix[128] = "";
    const char *pane = getenv("GASH_PANE");
    if (pane && *pane) {
        snprintf(pane_prefix, sizeof(pane_prefix),
                 CLR_PANE "[%s] " CLR_RESET, pane);
    }

    snprintf(out, outsz,
             "%s" CLR_USERHOST "%s@%s" CLR_RESET ":" CLR_PATH "%s" CLR_RESET "%c ",
             pane_prefix, user, host, path, mark);
}

/* Completion */
static int   match_count;
static char *matches[MAX_MATCHES];
static void add_match(const char *m) {
    for (int i = 0; i < match_count; i++)
        if (strcmp(matches[i], m) == 0) return;
    if (match_count >= MAX_MATCHES) return;
    matches[match_count++] = strdup(m);
}
static void free_matches(void) {
    for (int i = 0; i < match_count; i++) free(matches[i]);
    match_count = 0;
}
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}
static int is_exec_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
}
static void complete_commands(const char *prefix) {
    static const char *builtins[] = {
        "cd", "exit", "gash-clean", "gash-history",
        "gash-help", "gash-version", "clear", "true", "false",
        "jobs", "pwd",
        "gash-process-new", "gash-process-join", "gash-process-exit",
        "gash-process-kill", "gash-process-list", NULL
    };
    size_t pl = strlen(prefix);
    for (int i = 0; builtins[i]; i++)
        if (strncmp(builtins[i], prefix, pl) == 0) add_match(builtins[i]);
    for (int i = 0; aliases[i][0]; i++)
        if (strncmp(aliases[i][0], prefix, pl) == 0) add_match(aliases[i][0]);
    const char *path = getenv("PATH");
    if (path) {
        char *copy = strdup(path);
        if (copy) {
            char *save = NULL;
            for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
                const char *dir = *d ? d : ".";
                DIR *dh = opendir(dir);
                if (!dh) continue;
                struct dirent *ent;
                char full[MAX_LINE];
                while ((ent = readdir(dh))) {
                    if (ent->d_name[0] == '.') continue;
                    if (strncmp(ent->d_name, prefix, pl) != 0) continue;
                    snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
                    if (is_exec_file(full)) add_match(ent->d_name);
                }
                closedir(dh);
            }
            free(copy);
        }
    }
    for (int i = 0; i < hist_count; i++) {
        const char *h = history[i];
        char w[MAX_LINE]; int n = 0;
        while (h[n] && !isspace((unsigned char)h[n]) && n < MAX_LINE - 1) {
            w[n] = h[n];
            n++;
        }
        w[n] = '\0';
        if (n > 0 && strncmp(w, prefix, pl) == 0) add_match(w);
    }
}
static void complete_files(const char *prefix) {
    char dir[MAX_LINE], base[MAX_LINE], expanded[MAX_LINE];
    const char *slash = strrchr(prefix, '/');
    int dir_len = 0;
    if (slash) {
        dir_len = (int)(slash - prefix) + 1;
        if (dir_len >= MAX_LINE) return;
        memcpy(dir, prefix, (size_t)dir_len);
        dir[dir_len] = '\0';
        strcpy(base, prefix + dir_len);
    } else {
        strcpy(dir, ".");
        strcpy(base, prefix);
    }
    if (dir[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "/";
        if (dir[1] == '\0')     snprintf(expanded, sizeof(expanded), "%s", home);
        else if (dir[1] == '/') snprintf(expanded, sizeof(expanded), "%s%s", home, dir + 1);
        else                    snprintf(expanded, sizeof(expanded), "%s", dir);
    } else {
        snprintf(expanded, sizeof(expanded), "%s", dir);
    }
    DIR *dh = opendir(expanded);
    if (!dh) return;
    struct dirent *ent;
    char check[MAX_LINE + 300];
    char disp[MAX_LINE + 300];
    size_t blen = strlen(base);
    while ((ent = readdir(dh))) {
        if (strncmp(ent->d_name, base, blen) != 0) continue;
        if (ent->d_name[0] == '.' && base[0] != '.') continue;
        int n;
        if (slash) {
            size_t el = strlen(expanded);
            int need_slash = !(el > 0 && expanded[el - 1] == '/');
            n = snprintf(check, sizeof(check), "%s%s%s", expanded, need_slash ? "/" : "", ent->d_name);
            if (n < 0 || n >= (int)sizeof(check)) continue;
            n = snprintf(disp, sizeof(disp), "%.*s%s", dir_len, prefix, ent->d_name);
        } else {
            n = snprintf(check, sizeof(check), "%s/%s", expanded, ent->d_name);
            if (n < 0 || n >= (int)sizeof(check)) continue;
            n = snprintf(disp, sizeof(disp), "%s", ent->d_name);
        }
        if (n < 0 || n >= (int)sizeof(disp)) continue;
        struct stat st;
        if (stat(check, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t L = strlen(disp);
            if (L + 1 < sizeof(disp)) { disp[L] = '/'; disp[L + 1] = '\0'; }
        }
        add_match(disp);
    }
    closedir(dh);
}
static char *path_suggestion(const char *prefix, size_t pl) {
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *copy = strdup(path);
    if (!copy) return NULL;
    char *save = NULL;
    char *best = NULL;
    size_t best_len = (size_t)-1;
    for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        const char *dir = *d ? d : ".";
        DIR *dh = opendir(dir);
        if (!dh) continue;
        struct dirent *ent;
        char full[MAX_LINE];
        while ((ent = readdir(dh))) {
            if (ent->d_name[0] == '.') continue;
            if (strncmp(ent->d_name, prefix, pl) != 0) continue;
            size_t el = strlen(ent->d_name);
            if (el <= pl || el - pl >= best_len) continue;
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
            if (!is_exec_file(full)) continue;
            char *cand = strdup(ent->d_name + pl);
            if (!cand) continue;
            free(best); best = cand; best_len = el - pl;
        }
        closedir(dh);
    }
    free(copy);
    return best;
}
static char *compute_suggestion(const char *buf, int len, int pos) {
    if (pos != len || len == 0) return NULL;
    if (buf[len - 1] == ' ' || buf[len - 1] == '\t') return NULL;
    for (int i = hist_count - 1; i >= 0; i--) {
        const char *h = history[i];
        if (strncmp(h, buf, (size_t)len) == 0 && h[len] != '\0')
            return strdup(h + len);
    }
    int is_first = 1;
    for (int i = 0; i < len; i++)
        if (buf[i] == ' ' || buf[i] == '\t') { is_first = 0; break; }
    if (is_first) {
        static const char *bi[] = {
            "cd", "exit", "gash-clean", "gash-history",
            "gash-help", "gash-version", "clear", "true", "false",
            "jobs", "pwd",
            "gash-process-new", "gash-process-join", "gash-process-exit",
            "gash-process-kill", "gash-process-list", NULL
        };
        for (int i = 0; bi[i]; i++)
            if (strncmp(bi[i], buf, (size_t)len) == 0 && bi[i][len] != '\0')
                return strdup(bi[i] + len);
        return path_suggestion(buf, (size_t)len);
    }
    return NULL;
}

/* Prompt redraw */
static void redraw_input(const char *prompt, char *buf, int len, int pos, const char *sugg) {
    fputc('\r', stdout);
    fputs("\033[K", stdout);
    fputs(prompt, stdout);

    char saved = buf[len];
    buf[len] = '\0';
    char *rendered = gardenia_render(buf);
    buf[len] = saved;

    int ok_render = 0;
    if (rendered && *rendered) {
        ok_render = 1;
        for (char *p = rendered; *p; p++)
            if (*p == '\n' || *p == '\r') { ok_render = 0; break; }
    }

    if (ok_render) {
        fputs(rendered, stdout);
        fputs(CLR_RESET, stdout);
        free(rendered);
    } else {
        fwrite(buf, 1, (size_t)len, stdout);
        free(rendered);
    }

    int sugglen = 0;
    if (sugg && *sugg) {
        fputs(CLR_NOTICE, stdout);
        fputs(sugg, stdout);
        fputs(CLR_RESET, stdout);
        sugglen = (int)strlen(sugg);
    }
    int tail = len + sugglen - pos;
    for (int i = 0; i < tail; i++) fputc('\b', stdout);
    fflush(stdout);
}

/* Completion popup */
static int choice_cols = 1;
static int choice_rows = 1;
static int list_lines  = 0;
static void erase_choice_list(void) {
    if (list_lines > 0) {
        printf("\033[%dA\r\033[J", list_lines + 1);
        list_lines = 0;
        fflush(stdout);
    }
}
static void draw_choice_list(int sel, int off) {
    int maxw = 0;
    for (int i = 0; i < match_count; i++) {
        int w = (int)strlen(matches[i]);
        if (w > maxw) maxw = w;
    }
    maxw += 2;
    struct winsize wsz;
    int term_cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz) == 0 && wsz.ws_col > 0) term_cols = wsz.ws_col;
    if (maxw > term_cols) maxw = term_cols;
    int cols = term_cols / maxw;
    if (cols > CHOICE_COLS) cols = CHOICE_COLS;
    if (cols < 1) cols = 1;
    int rows = (match_count + cols - 1) / cols;
    if (rows > CHOICE_ROWS) rows = CHOICE_ROWS;
    if (rows < 1) rows = 1;
    choice_cols = cols; choice_rows = rows;
    int end = off + rows * cols;
    if (end > match_count) end = match_count;
    fputs("\r\n\033[J", stdout);
    int lines = 0;
    for (int i = off; i < end; i++) {
        int col = (i - off) % cols;
        if (i == sel) fputs("\033[7m", stdout);
        int mlen = (int)strlen(matches[i]);
        int shown = mlen < maxw ? mlen : maxw;
        fwrite(matches[i], 1, (size_t)shown, stdout);
        if (i == sel) fputs("\033[0m", stdout);
        int pad = maxw - shown;
        if (col == cols - 1 || i == end - 1) { fputc('\n', stdout); lines++; }
        else for (int k = 0; k < pad; k++) fputc(' ', stdout);
    }
    list_lines = lines;
    fflush(stdout);
}
static void apply_completion(char *buf, int *len, int *pos, int ws, int we, const char *m) {
    int mlen = (int)strlen(m);
    int is_dir = (mlen > 0 && m[mlen - 1] == '/');
    int delta = mlen - (we - ws);
    if (*len + delta >= MAX_LINE) return;
    memmove(buf + ws + mlen, buf + we, (size_t)(*len - we));
    memcpy(buf + ws, m, (size_t)mlen);
    *len += delta;
    *pos = ws + mlen;
    if (!is_dir && *len + 1 < MAX_LINE) {
        memmove(buf + *pos + 1, buf + *pos, (size_t)(*len - *pos));
        buf[*pos] = ' '; (*len)++; (*pos)++;
    }
    buf[*len] = '\0';
}
static void choice_scroll(int sel, int *off) {
    int page = choice_rows * choice_cols;
    if (page < 1) page = 1;
    if (sel < 0) { *off = 0; return; }
    if (sel < *off) *off = (sel / page) * page;
    while (sel >= *off + page) *off += page;
    if (*off < 0) *off = 0;
}

/* Paste */
static char *paste_q[PASTE_MAX];
static int   paste_n = 0;
static int   in_paste = 0;
static int read_byte_timeout(unsigned char *b, int ms) {
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    struct timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) return 0;
    return read(STDIN_FILENO, b, 1) == 1;
}
static char *paste_pop(void) {
    if (paste_n == 0) return NULL;
    char *r = paste_q[0];
    for (int i = 1; i < paste_n; i++) paste_q[i - 1] = paste_q[i];
    paste_q[paste_n - 1] = NULL;
    paste_n--;
    return r;
}
static void paste_clear(void) {
    for (int i = 0; i < paste_n; i++) free(paste_q[i]);
    paste_n = 0;
    in_paste = 0;
}
static void paste_push(const char *s) {
    if (paste_n >= PASTE_MAX || !s) return;
    size_t l = strlen(s);
    if (l > MAX_LINE - 1) l = MAX_LINE - 1;
    paste_q[paste_n] = malloc(l + 1);
    if (!paste_q[paste_n]) return;
    memcpy(paste_q[paste_n], s, l);
    paste_q[paste_n][l] = '\0';
    paste_n++;
}

/* dynbuf / tokens / capture / lookup_dollar / tokenize */
typedef struct { char *data; size_t len; size_t cap; } dynbuf_t;
static void db_putc(dynbuf_t *db, char c) {
    if (db->len + 2 > db->cap) {
        size_t nc = db->cap ? db->cap * 2 : 64;
        char *nd = realloc(db->data, nc);
        if (!nd) return;
        db->data = nd; db->cap = nc;
    }
    db->data[db->len++] = c; db->data[db->len] = '\0';
}
static void db_puts(dynbuf_t *db, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) db_putc(db, s[i]);
}
static void db_free(dynbuf_t *db) { free(db->data); db->data = NULL; db->len = db->cap = 0; }

typedef struct { char **tokens; int *quoted; int count; int cap; } tokens_t;
static void tok_init(tokens_t *t) { t->tokens = NULL; t->quoted = NULL; t->count = t->cap = 0; }
static void tok_push(tokens_t *t, char *s, int q) {
    if (t->count >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        char **nt = realloc(t->tokens, (size_t)nc * sizeof(char *));
        if (!nt) { free(s); return; }
        int *nq = realloc(t->quoted, (size_t)nc * sizeof(int));
        if (!nq) { free(s); return; }
        t->tokens = nt; t->quoted = nq; t->cap = nc;
    }
    t->tokens[t->count] = s; t->quoted[t->count] = q; t->count++;
}
static void tok_free(tokens_t *t) {
    for (int i = 0; i < t->count; i++) free(t->tokens[i]);
    free(t->tokens); free(t->quoted);
    t->tokens = NULL; t->quoted = NULL; t->count = t->cap = 0;
}

static char *capture_command(const char *cmd) {
    int fds[2];
    if (pipe(fds) < 0) return strdup("");
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return strdup(""); }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL);
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        execl("/proc/self/exe", "gash", "-c", cmd, (char *)NULL);
        execlp("gash", "gash", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    dynbuf_t out = {0};
    char buf[4096]; ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) db_puts(&out, buf, (size_t)n);
    close(fds[0]);
    int st; waitpid(pid, &st, 0);
    while (out.len > 0 && (out.data[out.len - 1] == '\n' || out.data[out.len - 1] == '\r')) out.len--;
    if (!out.data) return strdup("");
    out.data[out.len] = '\0';
    return out.data;
}

static void lookup_dollar(const char *line, int *ip, int last_status, dynbuf_t *out) {
    int i = *ip;
    char vb[256]; int vl = 0;
    if (line[i] == '{') {
        i++;
        while (line[i] && line[i] != '}' && vl < (int)sizeof(vb) - 1) vb[vl++] = line[i++];
        if (line[i] == '}') i++;
    } else if (line[i] == '?' || line[i] == '#' || line[i] == '$' || line[i] == '!') {
        vb[vl++] = line[i++];
    } else {
        while (line[i] && (isalnum((unsigned char)line[i]) || line[i] == '_') && vl < (int)sizeof(vb) - 1)
            vb[vl++] = line[i++];
    }
    vb[vl] = '\0';
    if (vl > 0) {
        if (vb[0] == '?') {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", last_status);
            db_puts(out, tmp, strlen(tmp));
        } else if (vb[0] == '#') {
            int c = (script_argc > 0) ? script_argc - 1 : 0;
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", c);
            db_puts(out, tmp, strlen(tmp));
        } else if (vb[0] == '$') {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", (int)getpid());
            db_puts(out, tmp, strlen(tmp));
        } else if (isdigit((unsigned char)vb[0])) {
            int n = atoi(vb);
            if (n == 0) {
                const char *z = (script_argv && script_argv[0]) ? script_argv[0] : "gash";
                db_puts(out, z, strlen(z));
            } else if (script_argv && n < script_argc) {
                db_puts(out, script_argv[n], strlen(script_argv[n]));
            }
        } else {
            const char *v = getenv(vb);
            if (v) db_puts(out, v, strlen(v));
        }
    } else {
        db_putc(out, '$');
    }
    *ip = i;
}

static int tokenize(const char *line, tokens_t *out, int last_status) {
    dynbuf_t cur = {0};
    int in_cur = 0, has_quote = 0;
    int i = 0;
    while (line[i]) {
        char c = line[i];
        if (c == ' ' || c == '\t') {
            if (in_cur) { tok_push(out, strdup(cur.data ? cur.data : ""), has_quote); db_free(&cur); in_cur = 0; has_quote = 0; }
            i++; continue;
        }
        if (c == '|' || c == '<' || c == '>' || c == '&' || c == ';') {
            if (in_cur) { tok_push(out, strdup(cur.data ? cur.data : ""), has_quote); db_free(&cur); in_cur = 0; has_quote = 0; }
            char op[4] = {c, 0, 0, 0};
            if (c == '>' && line[i+1] == '>') { op[1] = '>'; i += 2; }
            else if (c == '|' && line[i+1] == '|') { op[1] = '|'; i += 2; }
            else if (c == '&' && line[i+1] == '&') { op[1] = '&'; i += 2; }
            else i++;
            tok_push(out, strdup(op), 1);
            continue;
        }
        if (c == '\'') {
            in_cur = 1; has_quote = 1; i++;
            while (line[i] && line[i] != '\'') { db_putc(&cur, line[i]); i++; }
            if (line[i] == '\'') i++; else { db_free(&cur); return -1; }
            continue;
        }
        if (c == '"') {
            in_cur = 1; has_quote = 1; i++;
            while (line[i] && line[i] != '"') {
                char d = line[i];
                if (d == '\\') {
                    char e = line[i+1];
                    if (e == '"' || e == '\\' || e == '$' || e == '`') { db_putc(&cur, e); i += 2; continue; }
                    db_putc(&cur, d); i++; continue;
                }
                if (d == '$' && line[i+1] == '(') {
                    int depth = 0, j = i + 1, in_s = 0, in_d = 0;
                    for (; line[j]; j++) {
                        char ch = line[j];
                        if (in_s) { if (ch == '\'') in_s = 0; continue; }
                        if (in_d) { if (ch == '\\' && line[j+1]) { j++; continue; } if (ch == '"') in_d = 0; continue; }
                        if (ch == '\'') { in_s = 1; continue; }
                        if (ch == '"') { in_d = 1; continue; }
                        if (ch == '(') depth++;
                        else if (ch == ')') { depth--; if (depth == 0) break; }
                    }
                    if (!line[j]) { db_free(&cur); return -1; }
                    char *inner = strndup(line + i + 2, (size_t)(j - i - 2));
                    char *sub = capture_command(inner);
                    free(inner);
                    if (sub) { db_puts(&cur, sub, strlen(sub)); free(sub); }
                    i = j + 1;
                    continue;
                }
                if (d == '$') { i++; lookup_dollar(line, &i, last_status, &cur); continue; }
                if (d == '`') {
                    int j = i + 1; dynbuf_t cbuf = {0};
                    while (line[j] && line[j] != '`') {
                        if (line[j] == '\\' && line[j+1]) { db_putc(&cbuf, line[j+1]); j += 2; }
                        else { db_putc(&cbuf, line[j]); j++; }
                    }
                    if (!line[j]) { db_free(&cur); db_free(&cbuf); return -1; }
                    char *sub = capture_command(cbuf.data ? cbuf.data : "");
                    db_free(&cbuf);
                    if (sub) { db_puts(&cur, sub, strlen(sub)); free(sub); }
                    i = j + 1;
                    continue;
                }
                db_putc(&cur, d); i++;
            }
            if (line[i] == '"') i++; else { db_free(&cur); return -1; }
            continue;
        }
        if (c == '\\') {
            char e = line[i+1];
            if (!e) { db_free(&cur); return -1; }
            db_putc(&cur, e); in_cur = 1; has_quote = 1; i += 2; continue;
        }
        if (c == '$' && line[i+1] == '(') {
            int depth = 0, j = i + 1, in_s = 0, in_d = 0;
            for (; line[j]; j++) {
                char ch = line[j];
                if (in_s) { if (ch == '\'') in_s = 0; continue; }
                if (in_d) { if (ch == '\\' && line[j+1]) { j++; continue; } if (ch == '"') in_d = 0; continue; }
                if (ch == '\'') { in_s = 1; continue; }
                if (ch == '"') { in_d = 1; continue; }
                if (ch == '(') depth++;
                else if (ch == ')') { depth--; if (depth == 0) break; }
            }
            if (!line[j]) { db_free(&cur); return -1; }
            char *inner = strndup(line + i + 2, (size_t)(j - i - 2));
            char *sub = capture_command(inner);
            free(inner);
            if (sub) { db_puts(&cur, sub, strlen(sub)); free(sub); }
            in_cur = 1; i = j + 1; continue;
        }
        if (c == '$') { in_cur = 1; i++; lookup_dollar(line, &i, last_status, &cur); continue; }
        if (c == '`') {
            int j = i + 1; dynbuf_t cbuf = {0};
            while (line[j] && line[j] != '`') {
                if (line[j] == '\\' && line[j+1]) { db_putc(&cbuf, line[j+1]); j += 2; }
                else { db_putc(&cbuf, line[j]); j++; }
            }
            if (!line[j]) { db_free(&cur); db_free(&cbuf); return -1; }
            char *sub = capture_command(cbuf.data ? cbuf.data : "");
            db_free(&cbuf);
            if (sub) { db_puts(&cur, sub, strlen(sub)); free(sub); }
            in_cur = 1; i = j + 1; continue;
        }
        db_putc(&cur, c); in_cur = 1; i++;
    }
    if (in_cur) tok_push(out, strdup(cur.data ? cur.data : ""), has_quote);
    db_free(&cur);
    return 0;
}

/* Handle leading assignments: VAR=value cmd ...
 * setenv() each leading NAME=VALUE; empty remain means the whole line is assignments. */
static int apply_prefix_assignments(const char *part, char *remain, size_t remain_cap) {
    const char *p = part;
    int applied = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *wstart = p;
        const char *wend = p;
        int in_sq = 0, in_dq = 0;
        while (*wend) {
            char c = *wend;
            if (in_sq) {
                if (c == '\'') in_sq = 0;
                wend++;
                continue;
            }
            if (in_dq) {
                if (c == '\\' && wend[1]) { wend += 2; continue; }
                if (c == '"') in_dq = 0;
                wend++;
                continue;
            }
            if (c == '\\' && wend[1]) { wend += 2; continue; }
            if (c == '\'') { in_sq = 1; wend++; continue; }
            if (c == '"')  { in_dq = 1; wend++; continue; }
            if (c == ' ' || c == '\t') break;
            wend++;
        }
        if (in_sq || in_dq) break;

        const char *eq = memchr(wstart, '=', (size_t)(wend - wstart));
        int is_assign = 0;
        if (eq && eq != wstart &&
            (isalpha((unsigned char)wstart[0]) || wstart[0] == '_')) {
            is_assign = 1;
            for (const char *q = wstart + 1; q < eq; q++) {
                if (!(isalnum((unsigned char)*q) || *q == '_')) { is_assign = 0; break; }
            }
        }
        if (!is_assign) break;

        char name[256];
        size_t nlen = (size_t)(eq - wstart);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, wstart, nlen);
        name[nlen] = '\0';

        dynbuf_t val = {0};
        const char *q = eq + 1;
        int vq_s = 0, vq_d = 0;
        while (q < wend) {
            char c = *q;
            if (vq_s) {
                if (c == '\'') vq_s = 0;
                else db_putc(&val, c);
                q++;
                continue;
            }
            if (vq_d) {
                if (c == '\\' && q + 1 < wend) { q++; db_putc(&val, *q++); continue; }
                if (c == '"') vq_d = 0;
                else db_putc(&val, c);
                q++;
                continue;
            }
            if (c == '\\' && q + 1 < wend) { q++; db_putc(&val, *q++); continue; }
            if (c == '\'') { vq_s = 1; q++; continue; }
            if (c == '"')  { vq_d = 1; q++; continue; }
            db_putc(&val, c);
            q++;
        }
        setenv(name, val.data ? val.data : "", 1);
        db_free(&val);
        applied = 1;
        p = wend;
    }

    if (!applied) return 0;
    if (remain && remain_cap)
        snprintf(remain, remain_cap, "%s", p);
    return 1;
}

static int parse_pipeline(char *line, cmd_t *cmds, int max_cmds, int last_status) {
    for (int i = 0; i < max_cmds; i++) {
        cmds[i].argc = 0;
        cmds[i].in_path = NULL;
        cmds[i].out_path = NULL;
        cmds[i].out_append = 0;
        cmds[i].argv[0] = NULL;
    }
    tokens_t toks; tok_init(&toks);
    if (tokenize(line, &toks, last_status) < 0) {
        fprintf(stderr, "gash: syntax error (unterminated quote or escape)\n");
        tok_free(&toks); return 0;
    }
    int nc = 0;
    for (int i = 0; i < toks.count; i++) {
        const char *tok = toks.tokens[i];
        if (strcmp(tok, "|") == 0) {
            cmds[nc].argv[cmds[nc].argc] = NULL;
            if (nc + 1 >= max_cmds) break;
            nc++;
        } else if (strcmp(tok, "<") == 0) {
            if (i + 1 >= toks.count) break;
            free(cmds[nc].in_path); cmds[nc].in_path = strdup(toks.tokens[++i]);
        } else if (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            int app = (tok[1] == '>');
            if (i + 1 >= toks.count) break;
            free(cmds[nc].out_path); cmds[nc].out_path = strdup(toks.tokens[++i]);
            cmds[nc].out_append = app;
        } else {
            if (cmds[nc].argc < MAX_ARGS - 1) {
                cmds[nc].argv[cmds[nc].argc] = strdup(tok);
                cmds[nc].argv_quoted[cmds[nc].argc] = toks.quoted[i];
                cmds[nc].argc++;
            }
        }
    }
    cmds[nc].argv[cmds[nc].argc] = NULL;
    tok_free(&toks);
    return nc + 1;
}
static void free_cmds(cmd_t *cmds, int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < cmds[i].argc; j++) free(cmds[i].argv[j]);
        free(cmds[i].in_path); free(cmds[i].out_path);
    }
}
static char *tilde_expand(const char *path) {
    if (!path || path[0] != '~') return path ? strdup(path) : NULL;
    const char *home = getenv("HOME");
    if (!home) home = "/";
    if (path[1] == '\0') return strdup(home);
    if (path[1] == '/') {
        size_t hl = strlen(home), pl = strlen(path);
        int home_slash = (hl > 0 && home[hl - 1] == '/');
        char *r = malloc(hl + pl + 1);
        if (!r) return strdup(path);
        memcpy(r, home, hl);
        if (home_slash) strcpy(r + hl, path + 2);
        else            strcpy(r + hl, path + 1);
        return r;
    }
    return strdup(path);
}
static void expand_globs(cmd_t *c) {
    char *newargv[MAX_ARGS];
    int   newargc = 0;
    for (int i = 0; i < c->argc; i++) {
        const char *arg = c->argv[i];
        int q = c->argv_quoted[i];
        char *exp = q ? strdup(arg) : tilde_expand(arg);
        if (q || i == 0 || !exp || !strpbrk(exp, "*?[")) {
            if (newargc < MAX_ARGS - 1) newargv[newargc++] = exp ? exp : strdup(arg);
            else free(exp);
            continue;
        }
        glob_t g; memset(&g, 0, sizeof(g));
        int r = glob(exp, GLOB_NOCHECK, NULL, &g);
        if (r == 0) {
            for (size_t j = 0; j < g.gl_pathc && newargc < MAX_ARGS - 1; j++)
                newargv[newargc++] = strdup(g.gl_pathv[j]);
            globfree(&g);
        } else if (newargc < MAX_ARGS - 1) {
            newargv[newargc++] = strdup(exp);
        }
        free(exp);
    }
    newargv[newargc] = NULL;
    for (int i = 0; i < c->argc; i++) free(c->argv[i]);
    c->argc = newargc;
    for (int i = 0; i < newargc; i++) c->argv[i] = newargv[i];
    for (int i = newargc; i < MAX_ARGS; i++) c->argv[i] = NULL;
}

/* Builtin name / pure logic / redirection wrapper */
static int is_builtin_name(const char *name) {
    static const char *names[] = {
        "exit","clear","true","false","cd","pwd","echo",".","..",
        "gash-clean","gash-history","gash-help","gash-version","jobs",
        "gash-process-new","gash-process-join","gash-process-exit",
        "gash-process-kill","gash-process-list", NULL
    };
    if (!name) return 0;
    for (int i = 0; names[i]; i++)
        if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

static int run_builtin(cmd_t *c, int *last_status) {
    if (c->argc == 0) return -1;
    const char *c0 = c->argv[0];
    if (!is_builtin_name(c0)) return -1;

    *last_status = 0;

    if (strcmp(c0, "exit") == 0 || strcmp(c0, "gash-process-exit") == 0) {
        const char *pane = getenv("GASH_PANE");
        if (pane && *pane) {
            /* Inside a pane: notify the parent shell to detach, then stop. The process survives and can be joined again.
             * tcflush drops pending master input so resuming does not run a leftover command (e.g. pressing exit twice). */
            fflush(NULL);
            tcflush(STDIN_FILENO, TCIFLUSH);
            /* Keep the pty slave in raw mode while stopped. After the pane resumes, the slave
             * is still raw, so input is not swallowed by cooked line buffering. */
            term_raw_mode();
            kill(getppid(), SIGUSR1);
            raise(SIGSTOP);
            *last_status = 0;
            return 0;
        }
        if (strcmp(c0, "exit") == 0) {
            if (c->argc > 1) *last_status = atoi(c->argv[1]);
            return 1; /* main shell actually exits */
        }
        fprintf(stderr, "gash-process-exit: not inside a pane\n");
        *last_status = 1;
        return 0;
    }
    if (strcmp(c0, "clear") == 0) {
        fputs("\033[2J\033[3J\033[H", stdout);
        fflush(stdout);
        return 0;
    }
    if (strcmp(c0, "true") == 0)  { *last_status = 0; return 0; }
    if (strcmp(c0, "false") == 0) { *last_status = 1; return 0; }
    if (strcmp(c0, "pwd") == 0) {
        char cwd[MAX_LINE];
        if (getcwd(cwd, sizeof(cwd))) printf("%s\n", cwd);
        else *last_status = 1;
        return 0;
    }
    if (strcmp(c0, "echo") == 0) {
        /* Built-in echo: -n suppresses the trailing newline. This image has no /bin/echo,
         * so scripts rely on this built-in entirely. */
        int i = 1, nl = 1;
        if (i < c->argc && strcmp(c->argv[i], "-n") == 0) { nl = 0; i++; }
        for (int first = 1; i < c->argc; i++, first = 0) {
            if (!first) fputc(' ', stdout);
            fputs(c->argv[i], stdout);
        }
        if (nl) fputc('\n', stdout);
        fflush(stdout);
        return 0;
    }
    if (strcmp(c0, "cd") == 0) {
        const char *dir = (c->argc > 1) ? c->argv[1] : getenv("HOME");
        if (!dir) dir = "/";
        char *exp = tilde_expand(dir);
        if (!exp) { *last_status = 1; return 0; }
        if (chdir(exp) < 0) {
            perror("cd");
            *last_status = 1;
        } else {
            char ncwd[MAX_LINE];
            if (getcwd(ncwd, sizeof(ncwd))) {
                snprintf(current_dir, sizeof(current_dir), "%s", ncwd);
                hist_switch_dir(current_dir);
            }
        }
        free(exp);
        return 0;
    }
    if (strcmp(c0, ".") == 0 || strcmp(c0, "..") == 0) {
        /* Convenience: a bare . / .. means cd . / cd .., bypassing exec.
         * Otherwise execvp on a directory returns EACCES -> "Permission denied". */
        if (c->argc > 1) {
            fprintf(stderr, "gash: %s: takes no arguments\n", c0);
            *last_status = 1;
            return 0;
        }
        if (chdir(c0) < 0) {
            perror("cd");
            *last_status = 1;
        } else {
            char ncwd[MAX_LINE];
            if (getcwd(ncwd, sizeof(ncwd))) {
                snprintf(current_dir, sizeof(current_dir), "%s", ncwd);
                hist_switch_dir(current_dir);
            }
        }
        return 0;
    }
    if (strcmp(c0, "gash-clean") == 0) {
        hist_free();
        char path[MAX_LINE + 64];
        snprintf(path, sizeof(path), "%s/%s", current_dir, HIST_FILE);
        FILE *f = fopen(path, "w");
        if (f) fclose(f);
        printf(CLR_NOTICE "History cleared: %s" CLR_RESET "\n", path);
        return 0;
    }
    if (strcmp(c0, "gash-history") == 0) {
        char path[MAX_LINE + 64];
        snprintf(path, sizeof(path), "%s/%s", current_dir, HIST_FILE);
        FILE *f = fopen(path, "r");
        if (!f) { perror("gash-history"); *last_status = 1; }
        else {
            char b[MAX_LINE]; int n = 1;
            while (fgets(b, sizeof(b), f)) printf(CLR_MATCH "%5d  " CLR_RESET "%s", n++, b);
            fclose(f);
        }
        return 0;
    }
    if (strcmp(c0, "gash-help") == 0)    { print_help();    return 0; }
    if (strcmp(c0, "gash-version") == 0) { print_version(); return 0; }
    if (strcmp(c0, "jobs") == 0) {
        if (njobs == 0) printf("no background jobs\n");
        else {
            printf("background jobs (%d):\n", njobs);
            for (int i = 0; i < njobs; i++) printf("  [%d] pid %d\n", i + 1, (int)job_pids[i]);
        }
        return 0;
    }
    if (strcmp(c0, "gash-process-new") == 0) {
        if (c->argc < 2) {
            fprintf(stderr, "usage: gash-process-new NAME\n");
            *last_status = 1;
            return 0;
        }
        *last_status = new_pane(c->argv[1]);
        return 0;
    }
    if (strcmp(c0, "gash-process-join") == 0) {
        if (c->argc < 2) {
            fprintf(stderr, "usage: gash-process-join NAME\n");
            *last_status = 1;
            return 0;
        }
        int idx = find_pane(c->argv[1]);
        if (idx < 0) {
            fprintf(stderr, "gash-process-join: no such pane '%s'\n", c->argv[1]);
            *last_status = 1;
            return 0;
        }
        *last_status = (attach_pane(idx) < 0) ? 1 : 0;
        return 0;
    }
    if (strcmp(c0, "gash-process-kill") == 0) {
        if (c->argc < 2) {
            fprintf(stderr, "usage: gash-process-kill NAME\n");
            *last_status = 1;
            return 0;
        }
        int idx = find_pane(c->argv[1]);
        if (idx < 0) {
            fprintf(stderr, "gash-process-kill: no such pane '%s'\n", c->argv[1]);
            *last_status = 1;
            return 0;
        }
        kill_pane(idx);
        return 0;
    }
    if (strcmp(c0, "gash-process-list") == 0) {
        reap_panes();
        int any = 0;
        printf("panes:\n");
        for (int i = 0; i < np; i++) {
            if (!panes[i].alive) continue;
            printf("  " CLR_PANE "%-20s" CLR_RESET " pid %d\n",
                   panes[i].name, (int)panes[i].pid);
            any = 1;
        }
        if (!any) printf("  (none)\n");
        return 0;
    }
    return -1;
}

static int try_builtin_redirs(cmd_t *c, int *last_status) {
    if (c->argc == 0 || !is_builtin_name(c->argv[0])) return -1;

    int saved_in = -1, saved_out = -1;

    if (c->in_path) {
        int fd = open(c->in_path, O_RDONLY);
        if (fd < 0) { perror(c->in_path); *last_status = 1; return 0; }
        saved_in = dup(STDIN_FILENO);
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->out_path) {
        int flags = O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC);
        int fd = open(c->out_path, flags, 0644);
        if (fd < 0) {
            perror(c->out_path);
            if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
            *last_status = 1;
            return 0;
        }
        saved_out = dup(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    int r = run_builtin(c, last_status);

    if (saved_out >= 0) {
        fflush(stdout);
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }
    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
    return r;
}

/* Pipeline execution */
static int run_pipeline(cmd_t *cmds, int ncmds, int background) {
    for (int i = 0; i < ncmds; i++) {
        if (cmds[i].argc == 0) {
            fprintf(stderr, "gash: syntax error near unexpected token\n");
            return 2;
        }
    }

    int prev_pipe_read = -1;
    pid_t pids[MAX_ARGS]; int npids = 0;

    for (int i = 0; i < ncmds; i++) {
        int pipefd[2] = {-1, -1};
        if (i < ncmds - 1) {
            if (pipe(pipefd) < 0) { perror("pipe"); break; }
        }
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            if (background) {
                if (i == 0) setpgid(0, 0);
                else        setpgid(0, pids[0]);
            }
            if (cmds[i].in_path) {
                int fd = open(cmds[i].in_path, O_RDONLY);
                if (fd < 0) { perror(cmds[i].in_path); _exit(1); }
                dup2(fd, STDIN_FILENO); close(fd);
            } else if (prev_pipe_read >= 0) {
                dup2(prev_pipe_read, STDIN_FILENO); close(prev_pipe_read);
            } else if (background) {
                int fd = open("/dev/null", O_RDONLY);
                if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
            }
            if (cmds[i].out_path) {
                int flags = O_WRONLY | O_CREAT | (cmds[i].out_append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].out_path, flags, 0644);
                if (fd < 0) { perror(cmds[i].out_path); _exit(1); }
                dup2(fd, STDOUT_FILENO); close(fd);
            } else if (pipefd[1] >= 0) {
                dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
            }
            if (pipefd[0] >= 0) close(pipefd[0]);

            int st = 0;
            if (run_builtin(&cmds[i], &st) >= 0) _exit(st);

            execvp(cmds[i].argv[0], cmds[i].argv);
            int exec_errno = errno;

            /* Paths with '/' (./a.sh, /tmp/x): when the exec bit or shebang is missing,
             * interpret in this child with gash instead of exec'ing a second gash.
             * Avoids recursion, ANSI leakage and line-number errors when the "gash"
             * on PATH is a wrapper script, or when /proc/self/exe is unavailable. */
            if (strchr(cmds[i].argv[0], '/')) {
                int sr = try_run_script_file(cmds[i].argv[0],
                                             cmds[i].argc, cmds[i].argv);
                if (sr >= 0) _exit(sr);
            } else if (exec_errno == ENOENT) {
                /* Convenience: the command is not on PATH but a regular file with the same name
                 * exists in the current directory (e.g. typing a.sh directly). */
                char cand[MAX_LINE];
                snprintf(cand, sizeof(cand), "./%s", cmds[i].argv[0]);
                int sr = try_run_script_file(cand, cmds[i].argc, cmds[i].argv);
                if (sr >= 0) _exit(sr);
            }

            perror(cmds[i].argv[0]);
            _exit(127);
        }

        if (pid > 0) {
            pids[npids++] = pid;
            if (background) {
                if (npids == 1) setpgid(pid, pid);
                else            setpgid(pid, pids[0]);
                job_add(pid);
            }
            if (prev_pipe_read >= 0) close(prev_pipe_read);
            if (pipefd[1] >= 0) close(pipefd[1]);
            prev_pipe_read = pipefd[0];
        } else {
            perror("fork");
            if (prev_pipe_read >= 0) close(prev_pipe_read);
            if (pipefd[0] >= 0) close(pipefd[0]);
            if (pipefd[1] >= 0) close(pipefd[1]);
            break;
        }
    }
    if (prev_pipe_read >= 0) close(prev_pipe_read);

    if (background) return 0;

    int rc = 0;
    for (int i = 0; i < npids; i++) {
        int st; waitpid(pids[i], &st, 0);
        if (i == npids - 1) {
            if (WIFEXITED(st)) rc = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) rc = 128 + WTERMSIG(st);
            else rc = 1;
        }
    }
    return rc;
}

/* exec_parsed */
static int exec_parsed(cmd_t *cmds, int ncmds, int *last_status, int background) {
    int empty = (ncmds == 0) ||
                (ncmds == 1 && cmds[0].argc == 0 && !cmds[0].in_path && !cmds[0].out_path);
    if (empty) { *last_status = 0; return 0; }

    for (int i = 0; i < ncmds; i++) expand_globs(&cmds[i]);

    if (!background && ncmds == 1) {
        int r = try_builtin_redirs(&cmds[0], last_status);
        if (r >= 0) return r;
    }

    *last_status = run_pipeline(cmds, ncmds, background);
    return 0;
}

/* Alias */
static void expand_alias(const char *line, char *out, size_t outsz) {
    int i = 0;
    char first[MAX_LINE];
    while (line[i] && !isspace((unsigned char)line[i]) && i < MAX_LINE - 1) { first[i] = line[i]; i++; }
    first[i] = '\0';
    for (int k = 0; aliases[k][0]; k++) {
        if (strcmp(first, aliases[k][0]) == 0) {
            snprintf(out, outsz, "%s%s", aliases[k][1], line + i);
            return;
        }
    }
    snprintf(out, outsz, "%s", line);
}

/* Gardenia render */
static char *gardenia_render(const char *text) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(1);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) { close(fd); return NULL; }
    
    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000; /* 20 ms timeout */
    ret = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (ret <= 0) { close(fd); return NULL; }
    
    fcntl(fd, F_SETFL, flags);
    tv.tv_sec = 0; tv.tv_usec = 100000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    size_t len = strlen(text);
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "POST /render HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", len);
    if (write(fd, header, (size_t)hlen) != hlen || (len > 0 && write(fd, text, len) != (ssize_t)len)) {
        close(fd); return NULL;
    }
    shutdown(fd, SHUT_WR);
    size_t cap = 8192, rlen = 0;
    char *resp = malloc(cap);
    if (!resp) { close(fd); return NULL; }
    for (;;) {
        if (rlen + 1 >= cap) {
            size_t nc = cap * 2;
            char *nr = realloc(resp, nc);
            if (!nr) break;
            resp = nr; cap = nc;
        }
        ssize_t r = read(fd, resp + rlen, cap - 1 - rlen);
        if (r <= 0) break;
        rlen += (size_t)r;
    }
    resp[rlen] = '\0';
    close(fd);
    char *sep = strstr(resp, "\r\n\r\n");
    if (!sep) { free(resp); return NULL; }
    
    long expected_len = -1;
    char *cl = strstr(resp, "Content-Length: ");
    if (cl) expected_len = atol(cl + 16);
    char *result = strdup(sep + 4);
    free(resp);
    if (expected_len >= 0 && result && strlen(result) > (size_t)expected_len)
        result[expected_len] = '\0';
    return result;
}
/* Bracketed paste */
static void handle_paste(void) {
    char *pasted = NULL;
    int plen = 0, cap = 0;
    size_t total = 0;

    for (;;) {
        unsigned char b;
        ssize_t read_ret = read(STDIN_FILENO, &b, 1);

        if (read_ret <= 0) {
            if (read_ret < 0 && errno == EINTR) continue;
            break;
        }

        if (++total > PASTE_READ_LIMIT) break;

        if (b == 27) {
            unsigned char t[8]; int tn = 0, tilde = 0;
            while (tn < 5) {
                unsigned char tb;
                if (!read_byte_timeout(&tb, 50)) break;
                t[tn++] = tb;
                if (tb == '~') { tilde = 1; break; }
            }
            if (tilde && tn == 5 && t[0] == '[' && t[1] == '2' && t[2] == '0' && t[3] == '1' && t[4] == '~') break;
            if (plen + tn + 2 >= cap) {
                int nc = cap ? cap * 2 : 4096;
                while (nc < plen + tn + 2) nc *= 2;
                char *np = realloc(pasted, (size_t)nc);
                if (!np) { free(pasted); return; }
                pasted = np; cap = nc;
            }
            pasted[plen++] = 27;
            for (int k = 0; k < tn; k++) pasted[plen++] = (char)t[k];
            continue;
        }
        if (plen + 2 >= cap) {
            int nc = cap ? cap * 2 : 4096;
            char *np = realloc(pasted, (size_t)nc);
            if (!np) { free(pasted); return; }
            pasted = np; cap = nc;
        }
        pasted[plen++] = (char)b;
    }

    if (!pasted) return;
    pasted[plen] = '\0';
    char *start = pasted;
    for (int i = 0; i < plen; i++) {
        if (pasted[i] == '\n' || pasted[i] == '\r') {
            char saved = pasted[i];
            pasted[i] = '\0';
            if (i > (int)(start - pasted)) paste_push(start);
            if (saved == '\r' && pasted[i+1] == '\n') i++;
            start = pasted + i + 1;
        }
    }
    if (plen > (int)(start - pasted)) paste_push(start);
    free(pasted);
    in_paste = 1;
}

/* Interactive line reader */
static char *read_line(const char *prompt) {
    if (paste_n > 0) {
        char *popped = paste_pop();
        if (popped) {
            fputs(prompt, stdout);
            char *rendered = gardenia_render(popped);
            if (rendered && *rendered) { fputs(rendered, stdout); fputs(CLR_RESET, stdout); }
            else fputs(popped, stdout);
            free(rendered);
            fputc('\n', stdout);
            fflush(stdout);
        }
        return popped;
    }
    if (in_paste) in_paste = 0;
    if (!isatty(STDIN_FILENO)) {
        fputs(prompt, stdout); fflush(stdout);
        char b[MAX_LINE];
        if (!fgets(b, sizeof(b), stdin)) return NULL;
        b[strcspn(b, "\n")] = '\0';
        return strdup(b);
    }
    fputs(prompt, stdout); fflush(stdout);
    term_raw_mode();
    char buf[MAX_LINE];
    int len = 0, pos = 0;
    int hidx = hist_count;
    char *draft = NULL;
    int eof = 0, cancel = 0;
    int tab_state = 0, tab_sel = 0, tab_off = 0;
    int tab_ws = 0, tab_we = 0;
    int list_visible = 0;
    for (;;) {
        if (list_visible) { erase_choice_list(); list_visible = 0; }
        {
            char *s = (tab_state == 0) ? compute_suggestion(buf, len, pos) : NULL;
            redraw_input(prompt, buf, len, pos, s);
            free(s);
        }
        if (tab_state >= 1 && match_count > 0) {
            draw_choice_list(tab_state == 2 ? tab_sel : -1, tab_off);
            list_visible = 1;
        }

        unsigned char c;
        ssize_t read_ret = read(STDIN_FILENO, &c, 1);

        if (read_ret < 0) {
            if (errno == EINTR) continue;
            eof = 1; break;
        } else if (read_ret == 0) {
            eof = 1; break;
        }

        if (c == '\n' || c == '\r') {
            if (tab_state >= 1 && match_count > 0) {
                int sel = tab_sel; if (sel < 0) sel = 0;
                apply_completion(buf, &len, &pos, tab_ws, tab_we, matches[sel]);
                tab_state = 0; tab_off = 0; continue;
            }
            break;
        }
        if (c == 4) {
            if (len == 0) { eof = 1; break; }
            if (pos < len) { memmove(&buf[pos], &buf[pos + 1], (size_t)(len - pos - 1)); len--; }
            continue;
        }
        if (c == 3) { cancel = 1; break; }
        if (c == 9) {
            if (tab_state == 0) {
                int ws = pos;
                while (ws > 0 && buf[ws - 1] != ' ' && buf[ws - 1] != '\t') ws--;
                int we = pos;
                char prefix[MAX_LINE]; int plen = we - ws;
                if (plen >= MAX_LINE) plen = MAX_LINE - 1;
                memcpy(prefix, buf + ws, (size_t)plen); prefix[plen] = '\0';
                int is_first = 1;
                for (int i = 0; i < ws; i++) if (buf[i] != ' ' && buf[i] != '\t') { is_first = 0; break; }
                free_matches();
                if (is_first) complete_commands(prefix); else complete_files(prefix);
                if (match_count == 0) tab_state = 0;
                else if (match_count == 1) { apply_completion(buf, &len, &pos, ws, we, matches[0]); tab_state = 0; }
                else {
                    qsort(matches, (size_t)match_count, sizeof(char *), cmp_str);
                    tab_ws = ws; tab_we = we; tab_sel = -1; tab_off = 0; tab_state = 1;
                }
            } else if (tab_state == 1) { tab_sel = 0; tab_off = 0; tab_state = 2; }
            else { tab_state = 2; }
            continue;
        }
        if (c == 127 || c == 8) {
            tab_state = 0;
            if (pos > 0) { memmove(&buf[pos - 1], &buf[pos], (size_t)(len - pos)); len--; pos--; }
            continue;
        }
        if (c == 12) { tab_state = 0; fputs("\033[2J\033[H", stdout); fflush(stdout); continue; }
        if (c == 27) {
            unsigned char s1, s2, fin;
            if (read(STDIN_FILENO, &s1, 1) != 1) break;
            if (s1 != '[') continue;
            if (read(STDIN_FILENO, &s2, 1) != 1) break;
            if (s2 == '2') {
                unsigned char s3, s4, s5;
                if (read(STDIN_FILENO, &s3, 1) != 1) break;
                if (s3 == '0') {
                    if (read(STDIN_FILENO, &s4, 1) != 1) break;
                    if (s4 == '0') {
                        if (read(STDIN_FILENO, &s5, 1) != 1) break;
                        if (s5 == '~') {
                            buf[len] = '\0';
                            handle_paste();

                            if (paste_n > 0) {
                                if (len > 0) {
                                    size_t cl = strlen(paste_q[0]);
                                    char *combined = malloc((size_t)len + cl + 1);
                                    if (combined) {
                                        memcpy(combined, buf, (size_t)len);
                                        memcpy(combined + len, paste_q[0], cl + 1);
                                        free(paste_q[0]);
                                        paste_q[0] = combined;
                                    }
                                }
                                if (list_visible) { erase_choice_list(); list_visible = 0; }
                                fputs("\r\033[K", stdout); fflush(stdout);
                                term_restore();
                                free(draft);
                                return strdup("");
                            }

                            in_paste = 0;
                            continue;
                        }
                    }
                }
                continue;
            }
            fin = s2;
            if (s2 >= '0' && s2 <= '9') {
                if (read(STDIN_FILENO, &fin, 1) != 1) break;
                if (fin != '~') continue;
            }
            if (fin == 'A') {
                if (tab_state == 2) { int ns = tab_sel - choice_cols; if (ns < 0) ns = 0; tab_sel = ns; choice_scroll(tab_sel, &tab_off); }
                else {
                    tab_state = 0;
                    if (hist_count == 0 || hidx == 0) continue;
                    if (hidx == hist_count) { free(draft); draft = strndup(buf, (size_t)len); }
                    hidx--;
                    int hl = (int)strlen(history[hidx]); if (hl > MAX_LINE - 1) hl = MAX_LINE - 1;
                    memcpy(buf, history[hidx], (size_t)hl); len = pos = hl;
                }
            } else if (fin == 'B') {
                if (tab_state == 2) { int ns = tab_sel + choice_cols; if (ns >= match_count) ns = match_count - 1; tab_sel = ns; choice_scroll(tab_sel, &tab_off); }
                else {
                    tab_state = 0;
                    if (hidx >= hist_count) continue;
                    hidx++;
                    if (hidx == hist_count) {
                        if (draft) { int dl = (int)strlen(draft); if (dl > MAX_LINE - 1) dl = MAX_LINE - 1; memcpy(buf, draft, (size_t)dl); len = pos = dl; }
                        else len = pos = 0;
                    } else {
                        int hl = (int)strlen(history[hidx]); if (hl > MAX_LINE - 1) hl = MAX_LINE - 1;
                        memcpy(buf, history[hidx], (size_t)hl); len = pos = hl;
                    }
                }
            } else if (fin == 'C') {
                if (tab_state == 2) { if (tab_sel < match_count - 1) tab_sel++; choice_scroll(tab_sel, &tab_off); }
                else {
                    if (pos == len) {
                        if (tab_state == 0) {
                            char *s = compute_suggestion(buf, len, pos);
                            if (s) {
                                int sl = (int)strlen(s);
                                if (len + sl < MAX_LINE) { memcpy(buf + len, s, (size_t)sl); len += sl; pos = len; buf[len] = '\0'; }
                                free(s);
                            }
                        }
                    } else { tab_state = 0; pos++; }
                }
            } else if (fin == 'D') {
                if (tab_state == 2) { if (tab_sel > 0) tab_sel--; choice_scroll(tab_sel, &tab_off); }
                else { tab_state = 0; if (pos > 0) pos--; }
            } else if (fin == 'H') { tab_state = 0; pos = 0; }
            else if (fin == 'F') { tab_state = 0; pos = len; }
            else if (s2 == '3' && fin == '~') {
                tab_state = 0;
                if (pos < len) { memmove(&buf[pos], &buf[pos + 1], (size_t)(len - pos - 1)); len--; }
            }
            continue;
        }
        if (c >= 32) {
            tab_state = 0;
            if (len < MAX_LINE - 1) {
                memmove(&buf[pos + 1], &buf[pos], (size_t)(len - pos));
                buf[pos] = (char)c; len++; pos++;
            }
        }
    }
    if (list_visible) erase_choice_list();
    buf[len] = '\0';

    if (!eof && !cancel) {
        fputs("\r\033[K", stdout);
        fputs(prompt, stdout);
        char *rendered = gardenia_render(buf);
        if (rendered && *rendered) {
            fputs(rendered, stdout);
            fputs(CLR_RESET, stdout);
            free(rendered);
        } else {
            fputs(buf, stdout);
        }
    }

    fputc('\n', stdout);
    fflush(stdout);
    term_restore();
    if (eof) { free(draft); return NULL; }
    if (cancel) { free(draft); return strdup(""); }
    buf[len] = '\0';
    free(draft);
    return strdup(buf);
}

/* Version / help */
static void print_version(void) {
    printf("%s", GARDENIA_ART);
    printf("\n");
    printf("  " CLR_USERHOST GASH_NAME CLR_RESET " version " GASH_VERSION "\n");
    printf("  build:   " GASH_BUILD "\n");
    printf("  author:  " GASH_AUTHOR "\n");
    printf("  license: " GASH_LICENSE "\n");
    printf("  " CLR_NOTICE GASH_DESC CLR_RESET "\n");
}
static void print_help(void) {
    printf("%s - a small GNU-ish shell\n\n", GASH_NAME);
    printf("Built-in commands:\n"
           "  cd [dir]        Change directory\n"
           "  pwd             Print working directory\n"
           "  echo [-n] ...   Print arguments\n"
           "  . / ..          cd . / cd ..\n"
           "  exit [code]     Exit (inside a pane: detach, keeps process)\n"
           "  clear           Clear screen\n"
           "  true / false    Return 0 / 1\n"
           "  jobs            List background jobs\n"
           "  gash-clean      Clear history\n"
           "  gash-history    Show history\n"
           "  gash-help       Show this help\n"
           "  gash-version    Show version info\n"
           "  gash-process-new NAME    Create + attach a pane\n"
           "  gash-process-join NAME   Attach to an existing pane\n"
           "  gash-process-exit        Detach from current pane (process kept)\n"
           "  gash-process-kill NAME   Kill a pane (only way to terminate)\n"
           "  gash-process-list        List panes\n"
           "  Ctrl+E then d            Detach; pane keeps running\n"
           "  Ctrl+E then x            Detach; pane keeps running\n\n");
    printf("Syntax:\n"
           "  cmd1 | cmd2               Pipe (builtins allowed)\n"
           "  cmd > file, cmd >> file   Output redirection\n"
           "  cmd < file                Input redirection\n"
           "  a && b, a || b, a ; b     Logic operators\n"
           "  cmd &                     Run in background\n"
           "  VAR=value                 Set environment variable\n"
           "  *, ?, [..]                Filename globbing\n"
           "  '...', \"...\", \\c          Quoting and escapes\n"
           "  $VAR, ${VAR}, $?          Environment / last exit status\n"
           "  $0, $1 .. $N, $#          Script positional parameters\n"
           "  $(cmd), `cmd`             Command substitution\n\n");
    printf("Usage:\n"
           "  gash                      Interactive shell\n"
           "  gash script.sh [args...]  Run a script file\n"
           "  gash -c 'cmd'             Execute command string\n"
           "  gash --help | --version\n");
}

/* logic split (with bg flags) */
typedef struct { char **parts; int *ops; int *bg; int n; } logic_t;

static int split_by_logic(const char *line, logic_t *out) {
    size_t len = strlen(line);
    char *cur = malloc(len + 1); size_t clen = 0;
    char **parts = NULL; int *ops = NULL; int *bg = NULL;
    int n = 0, cap = 0, pending = 0;
    int in_sq = 0, in_dq = 0;
    if (!cur) { out->parts = NULL; out->ops = NULL; out->bg = NULL; out->n = 0; return -1; }

    #define GROW() do { \
        if (n >= cap) { \
            cap = cap ? cap * 2 : 4; \
            parts = realloc(parts, (size_t)cap * sizeof(char *)); \
            ops   = realloc(ops,   (size_t)cap * sizeof(int)); \
            bg    = realloc(bg,    (size_t)cap * sizeof(int)); \
        } \
    } while (0)

    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (in_sq) { cur[clen++] = c; if (c == '\'') in_sq = 0; continue; }
        if (in_dq) {
            if (c == '\\' && i + 1 < len) { cur[clen++] = c; cur[clen++] = line[++i]; continue; }
            cur[clen++] = c; if (c == '"') in_dq = 0;
            continue;
        }
        if (c == '\\' && i + 1 < len) { cur[clen++] = c; cur[clen++] = line[++i]; continue; }
        if (c == '#' && (i == 0 || isspace((unsigned char)line[i-1]) || line[i-1] == ';' || line[i-1] == '&' || line[i-1] == '|'))
            break;
        if (c == '\'') { cur[clen++] = c; in_sq = 1; continue; }
        if (c == '"')  { cur[clen++] = c; in_dq = 1; continue; }
        if (c == '$' && i + 1 < len && line[i+1] == '(') {
            cur[clen++] = c;
            cur[clen++] = line[++i];
            int depth = 1;
            int inner_sq = 0, inner_dq = 0;
            while (i + 1 < len && depth > 0) {
                char ch = line[++i];
                cur[clen++] = ch;
                if (inner_sq) { if (ch == '\'') inner_sq = 0; continue; }
                if (inner_dq) {
                    if (ch == '\\' && i + 1 < len) { cur[clen++] = line[++i]; continue; }
                    if (ch == '"') inner_dq = 0;
                    continue;
                }
                if (ch == '\\' && i + 1 < len) { cur[clen++] = line[++i]; continue; }
                if (ch == '\'') { inner_sq = 1; continue; }
                if (ch == '"')  { inner_dq = 1; continue; }
                if (ch == '(') depth++;
                else if (ch == ')') depth--;
            }
            continue;
        }
        if (c == '`') {
            cur[clen++] = c;
            while (i + 1 < len && line[i+1] != '`') {
                i++;
                if (line[i] == '\\' && i + 1 < len) { cur[clen++] = line[i]; i++; cur[clen++] = line[i]; }
                else cur[clen++] = line[i];
            }
            if (i + 1 < len) { i++; cur[clen++] = line[i]; }
            continue;
        }
        if (c == '&' && i + 1 < len && line[i+1] == '&') {
            cur[clen] = '\0';
            GROW();
            parts[n] = strdup(cur); ops[n] = pending; bg[n] = 0; n++;
            pending = 1; clen = 0; i++;
            continue;
        }
        if (c == '|' && i + 1 < len && line[i+1] == '|') {
            cur[clen] = '\0';
            GROW();
            parts[n] = strdup(cur); ops[n] = pending; bg[n] = 0; n++;
            pending = 2; clen = 0; i++;
            continue;
        }
        if (c == '&') {
            cur[clen] = '\0';
            GROW();
            parts[n] = strdup(cur); ops[n] = pending; bg[n] = 1; n++;
            pending = 0; clen = 0; i++;
            continue;
        }
        if (c == ';') {
            cur[clen] = '\0';
            GROW();
            parts[n] = strdup(cur); ops[n] = pending; bg[n] = 0; n++;
            pending = 0; clen = 0; i++;
            continue;
        }
        cur[clen++] = c;
    }
    cur[clen] = '\0';
    GROW();
    parts[n] = strdup(cur); ops[n] = pending; bg[n] = 0; n++;
    free(cur);
    out->parts = parts; out->ops = ops; out->bg = bg; out->n = n;
    #undef GROW
    return 0;
}

static int assignment_split(const char *s, char **name_out, const char **val_out) {
    const char *eq = strchr(s, '=');
    if (!eq || eq == s) return 0;
    int nlen = (int)(eq - s);
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (int i = 1; i < nlen; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    char *name = malloc((size_t)nlen + 1);
    if (!name) return 0;
    memcpy(name, s, (size_t)nlen); name[nlen] = '\0';
    *name_out = name;
    *val_out  = eq + 1;
    return 1;
}

static int line_is_pure_assignments(const char *s) {
    const char *p = s;
    int saw = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *wstart = p;
        int in_sq = 0, in_dq = 0;
        while (*p) {
            char c = *p;
            if (in_sq) { if (c == '\'') in_sq = 0; p++; continue; }
            if (in_dq) {
                if (c == '\\' && p[1]) { p += 2; continue; }
                if (c == '"') in_dq = 0;
                p++;
                continue;
            }
            if (c == '\\' && p[1]) { p += 2; continue; }
            if (c == '\'') { in_sq = 1; p++; continue; }
            if (c == '"')  { in_dq = 1; p++; continue; }
            if (c == ' ' || c == '\t') break;
            p++;
        }
        if (in_sq || in_dq) return 0;
        const char *eq = memchr(wstart, '=', (size_t)(p - wstart));
        if (!eq || eq == wstart) return 0;
        if (!(isalpha((unsigned char)wstart[0]) || wstart[0] == '_')) return 0;
        for (const char *q = wstart + 1; q < eq; q++)
            if (!(isalnum((unsigned char)*q) || *q == '_')) return 0;
        saw = 1;
    }
    return saw;
}

static int try_assign_var(const char *part, int last_status) {
    if (!line_is_pure_assignments(part)) return 0;
    tokens_t t;
    tok_init(&t);
    if (tokenize(part, &t, last_status) < 0) { tok_free(&t); return 0; }
    if (t.count == 0) { tok_free(&t); return 0; }
    for (int i = 0; i < t.count; i++) {
        char *name = NULL; const char *val = NULL;
        if (!assignment_split(t.tokens[i], &name, &val)) { free(name); tok_free(&t); return 0; }
        free(name);
    }
    for (int i = 0; i < t.count; i++) {
        char *name = NULL; const char *val = NULL;
        if (assignment_split(t.tokens[i], &name, &val)) { setenv(name, val, 1); free(name); }
    }
    tok_free(&t);
    return 1;
}

static int execute_line(const char *line_in, int *last_status) {
    logic_t lg;
    if (split_by_logic(line_in, &lg) < 0) return 0;
    int do_exit = 0;
    for (int pi = 0; pi < lg.n && !do_exit; pi++) {
        int run = 1;
        if (pi > 0) {
            if (lg.ops[pi] == 1 && *last_status != 0) run = 0;
            if (lg.ops[pi] == 2 && *last_status == 0) run = 0;
        }
        if (!run) continue;

        if (try_assign_var(lg.parts[pi], *last_status)) {
            *last_status = 0;
            continue;
        }

        char expanded[MAX_LINE];
        expand_alias(lg.parts[pi], expanded, sizeof(expanded));

        char cmdline[MAX_LINE];
        snprintf(cmdline, sizeof(cmdline), "%s", expanded);
        {
            char remain[MAX_LINE];
            if (apply_prefix_assignments(cmdline, remain, sizeof(remain))) {
                if (remain[0] == '\0') { *last_status = 0; continue; }
                snprintf(cmdline, sizeof(cmdline), "%s", remain);
            }
        }

        cmd_t cmds[MAX_ARGS];
        int ncmds = parse_pipeline(cmdline, cmds, MAX_ARGS, *last_status);
        int rc = exec_parsed(cmds, ncmds, last_status, lg.bg[pi]);
        free_cmds(cmds, ncmds);
        if (rc) do_exit = 1;
    }
    for (int i = 0; i < lg.n; i++) free(lg.parts[i]);
    free(lg.parts); free(lg.ops); free(lg.bg);
    return do_exit;
}

/* Script runner / script-file fallback */
static int run_script(const char *path, int argn, char **args) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 127; }

    /* Save the outer script's positional parameters so nested scripts work */
    int saved_argc = script_argc;
    char **saved_argv = script_argv;

    script_argc = argn + 1;
    script_argv = malloc((size_t)(script_argc + 1) * sizeof(char *));
    if (script_argv) {
        script_argv[0] = (char *)path;
        for (int i = 0; i < argn; i++) script_argv[i + 1] = args[i];
        script_argv[script_argc] = NULL;
    }

    int last_status = 0;
    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), f)) {
        /* Strip both \r and \n so CRLF scripts do not pollute the command name with "echo\r" */
        buf[strcspn(buf, "\r\n")] = '\0';
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        if (execute_line(p, &last_status)) break;
    }
    fclose(f);

    free(script_argv);
    script_argv = saved_argv;
    script_argc = saved_argc;
    return last_status;
}

/* If path is a readable regular file, interpret it in-process with gash;
 * files with the exec bit and a shebang go through exec (return -1) to keep standard semantics. */
static int try_run_script_file(const char *path, int argc, char **argv) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    if (access(path, R_OK) != 0) return -1;

    if (access(path, X_OK) == 0) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char b[2] = {0};
            ssize_t r = read(fd, b, 2);
            close(fd);
            if (r == 2 && b[0] == '#' && b[1] == '!') return -1;
        }
    }
    return run_script(path, argc - 1, argv + 1);
}

/* main */
int main(int argc, char **argv) {
    atexit(term_restore);
    atexit(free_matches);
    if (!getenv("PATH")) setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);

    signal(SIGINT, SIG_IGN);

    /* SIGUSR1 is always installed: pane detach notifies the main shell, and staying registered
     * prevents the default action from killing us when SIGUSR1 arrives outside attach. */
    {
        struct sigaction sa_usr1;
        memset(&sa_usr1, 0, sizeof(sa_usr1));
        sa_usr1.sa_handler = sigusr1_handler;
        sa_usr1.sa_flags = 0;
        sigemptyset(&sa_usr1.sa_mask);
        sigaction(SIGUSR1, &sa_usr1, NULL);
    }

    if (!getcwd(current_dir, sizeof(current_dir))) strcpy(current_dir, "?");

    if (argc > 1) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) { print_version(); return 0; }
        if (strcmp(argv[1], "--help")    == 0 || strcmp(argv[1], "-h") == 0) { print_help();    return 0; }
        if (strcmp(argv[1], "-c") == 0) {
            if (argc < 3) {
                fprintf(stderr, "usage: gash -c 'command'\n");
                return 2;
            }
            int last_status = 0;
            execute_line(argv[2], &last_status);
            return last_status;
        }
        return run_script(argv[1], argc - 2, argv + 2);
    }

    hist_switch_dir(current_dir);

    int last_status = 0;
    for (;;) {
        reap_jobs();
        reap_panes();
        if (!getcwd(current_dir, sizeof(current_dir))) strcpy(current_dir, "?");
        char prompt[MAX_LINE + 400];
        build_prompt(prompt, sizeof(prompt), current_dir);
        char *line = read_line(prompt);
        if (!line) break;
        if (line[0] == '\0') { free(line); continue; }

        char orig[MAX_LINE]; snprintf(orig, sizeof(orig), "%s", line);
        {
            char first[MAX_LINE]; int i = 0;
            while (orig[i] && !isspace((unsigned char)orig[i]) && i < MAX_LINE - 1) { first[i] = orig[i]; i++; }
            first[i] = '\0';
            if (first[0] != '\0' &&
                strcmp(first, "gash-clean") != 0 &&
                strcmp(first, "gash-history") != 0) {
                hist_add(orig); hist_append_current(orig);
            }
        }

        int do_exit = execute_line(line, &last_status);
        free(line);

        if (in_paste && (last_status == 127 || last_status == 2 || last_status >= 128))
            paste_clear();
        if (do_exit) break;
    }
    for (int i = 0; i < np; i++) if (panes[i].alive) kill_pane(i);
    paste_clear();
    hist_free();
    printf("\n");
    return last_status;
}

