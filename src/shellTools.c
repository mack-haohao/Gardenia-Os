#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "librender.h"

/* ---------- touch ---------- */
static int cmd_touch(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "touch: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) { perror(argv[i]); rc = 1; continue; }
        close(fd);
        utimensat(AT_FDCWD, argv[i], NULL, 0);
    }
    return rc;
}

/* ---------- mkdir [-p] ---------- */
static int cmd_mkdir(int argc, char **argv) {
    int pflag = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-p") == 0) { pflag = 1; i++; }
    if (i >= argc) { fprintf(stderr, "mkdir: missing operand\n"); return 1; }
    int rc = 0;
    for (; i < argc; i++) {
        if (pflag) {
            char tmp[4096]; snprintf(tmp, sizeof(tmp), "%s", argv[i]);
            for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
            }
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) { perror(argv[i]); rc = 1; }
        } else {
            if (mkdir(argv[i], 0755) < 0) { perror(argv[i]); rc = 1; }
        }
    }
    return rc;
}

/* ---------- pwd ---------- */
static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { perror("pwd"); return 1; }
    puts(cwd);
    return 0;
}

/* ---------- sleep ---------- */
static int cmd_sleep(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "sleep: missing operand\n"); return 1; }
    double sec = atof(argv[1]);
    struct timespec ts;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    return 0;
}

/* ---------- wc [-l|-c|-w] ---------- */
static int cmd_wc(int argc, char **argv) {
    int lflag = 0, cflag = 0, wflag = 0, i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strchr(argv[i], 'l')) lflag = 1;
        if (strchr(argv[i], 'c')) cflag = 1;
        if (strchr(argv[i], 'w')) wflag = 1;
        i++;
    }
    if (!lflag && !cflag && !wflag) lflag = cflag = wflag = 1;
    long lines = 0, bytes = 0, words = 0;
    int infd = (i < argc) ? open(argv[i], O_RDONLY) : STDIN_FILENO;
    if (infd < 0) { perror(argv[i]); return 1; }
    char buf[4096]; ssize_t n;
    int in_word = 0;
    while ((n = read(infd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (ssize_t k = 0; k < n; k++) {
            if (buf[k] == '\n') lines++;
            if (buf[k] == ' ' || buf[k] == '\t' || buf[k] == '\n') in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
    }
    if (infd != STDIN_FILENO) close(infd);
    if (lflag) printf("%ld\n", lines);
    if (wflag) printf("%ld\n", words);
    if (cflag) printf("%ld\n", bytes);
    return 0;
}

/* ---------- head [-n N|-c N|-N] ---------- */
static int cmd_head(int argc, char **argv) {
    int n = 10, cflag = 0, i = 1;
    if (i < argc && argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) {
        n = atoi(argv[i] + 1); i++;
    } else if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        n = atoi(argv[i+1]); i += 2;
    } else if (i + 1 < argc && strcmp(argv[i], "-c") == 0) {
        cflag = 1; n = atoi(argv[i+1]); i += 2;
    }
    int infd = (i < argc) ? open(argv[i], O_RDONLY) : STDIN_FILENO;
    if (infd < 0) { perror(argv[i]); return 1; }
    char buf[4096]; ssize_t r;
    if (cflag) {
        int left = n;
        while (left > 0 && (r = read(infd, buf, sizeof(buf))) > 0) {
            int take = r < left ? (int)r : left;
            fwrite(buf, 1, (size_t)take, stdout);
            left -= take;
        }
    } else {
        int lines = 0;
        while (lines < n && (r = read(infd, buf, sizeof(buf))) > 0) {
            for (ssize_t k = 0; k < r && lines < n; k++) {
                putchar(buf[k]);
                if (buf[k] == '\n') lines++;
            }
        }
    }
    if (infd != STDIN_FILENO) close(infd);
    return 0;
}

/* ---------- tail [-n N|-c N|-N] ---------- */
static int cmd_tail(int argc, char **argv) {
    int n = 10, cflag = 0, i = 1;
    if (i < argc && argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) {
        n = atoi(argv[i] + 1); i++;
    } else if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        n = atoi(argv[i+1]); i += 2;
    } else if (i + 1 < argc && strcmp(argv[i], "-c") == 0) {
        cflag = 1; n = atoi(argv[i+1]); i += 2;
    }
    int infd = (i < argc) ? open(argv[i], O_RDONLY) : STDIN_FILENO;
    if (infd < 0) { perror(argv[i]); return 1; }
    char *data = NULL; size_t len = 0, cap = 0;
    char buf[4096]; ssize_t r;
    while ((r = read(infd, buf, sizeof(buf))) > 0) {
        if (len + (size_t)r > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            while (ncap < len + (size_t)r) ncap *= 2;
            char *nd = realloc(data, ncap);
            if (!nd) {
                fprintf(stderr, "tail: out of memory\n");
                free(data);
                if (infd != STDIN_FILENO) close(infd);
                return 1;
            }
            data = nd;
            cap = ncap;
        }
        memcpy(data + len, buf, (size_t)r);
        len += (size_t)r;
    }
    if (infd != STDIN_FILENO) close(infd);
    if (!data) return 0;
    if (cflag) {
        size_t start = (len > (size_t)n) ? len - (size_t)n : 0;
        fwrite(data + start, 1, len - start, stdout);
    } else {
        int lines = 0;
        size_t start = 0;
        for (size_t k = len; k > 0; k--) {
            if (data[k-1] == '\n') {
                if (++lines > n) { start = k; break; }
            }
        }
        fwrite(data + start, 1, len - start, stdout);
    }
    free(data);
    return 0;
}

/* ---------- printf ---------- */
static int cmd_printf(int argc, char **argv) {
    if (argc < 2) return 0;
    const char *fmt = argv[1];
    int ai = 2;
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') putchar('\n');
            else if (*p == 't') putchar('\t');
            else if (*p == '\\') putchar('\\');
            else putchar(*p);
        } else if (*p == '%' && p[1]) {
            p++;
            if (*p == 's') printf("%s", ai < argc ? argv[ai++] : "");
            else if (*p == 'd') printf("%d", ai < argc ? atoi(argv[ai++]) : 0);
            else if (*p == 'c') { if (ai < argc) putchar(argv[ai++][0]); }
            else if (*p == '%') putchar('%');
        } else putchar(*p);
    }
    return 0;
}

/* ---------- env ---------- */
extern char **environ;
static int cmd_env(int argc, char **argv) {
    (void)argc; (void)argv;
    for (char **e = environ; *e; e++) puts(*e);
    return 0;
}

int main(int argc, char **argv) {
    render_begin();
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    if (!strcmp(prog, "touch"))   return cmd_touch(argc, argv);
    if (!strcmp(prog, "mkdir"))   return cmd_mkdir(argc, argv);
    if (!strcmp(prog, "pwd"))     return cmd_pwd(argc, argv);
    if (!strcmp(prog, "sleep"))   return cmd_sleep(argc, argv);
    if (!strcmp(prog, "wc"))      return cmd_wc(argc, argv);
    if (!strcmp(prog, "head"))    return cmd_head(argc, argv);
    if (!strcmp(prog, "tail"))    return cmd_tail(argc, argv);
    if (!strcmp(prog, "printf"))  return cmd_printf(argc, argv);
    if (!strcmp(prog, "env"))     return cmd_env(argc, argv);
    fprintf(stderr, "coreutils: unknown applet '%s'\n", prog);
    return 127;
}
