#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "librender.h"
#include <sys/ioctl.h>

#ifndef MOUNT_FLAGS_H
#define MOUNT_FLAGS_H
#endif

/* Column widths; keep long fields from spilling into the next column */
#define W_DEV   30
#define W_MNT   30
#define W_TYPE  15
#define W_OPTS  45  /* Cap the Options column so wrapping does not misalign */

/* Print padded to width; truncate long values and mark them with '~' */
static void print_padded(const char *s, int width) {
    size_t len = strlen(s);
    if (len <= (size_t)width) {
        printf("%-*s", width, s);
    } else {
        printf("%.*s~", width - 1, s);
    }
}

/* List all mount points (mount with no arguments) */

static int list_mounts(void) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        fprintf(stderr, "mount: cannot open /proc/mounts: %s\n", strerror(errno));
        return 1;
    }

    /* Get the current terminal width */
    struct winsize ws;
    int term_cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        term_cols = ws.ws_col;
    }

    char line[4096];
    char dev[256], mnt[256], type[64], opts[1024];

    /* Use the table layout when the terminal is wide enough (>= 110 cols) */
    int use_table = (term_cols >= 110);

    if (use_table) {
        /* Header */
        printf("\033[1m");
        print_padded("Device", W_DEV);
        printf(" ");
        print_padded("Mounted on", W_MNT);
        printf(" ");
        print_padded("Type", W_TYPE);
        printf(" Options\n\033[0m");

        /* Separator */
        printf("\033[90m");
        for (int i = 0; i < W_DEV + W_MNT + W_TYPE + W_OPTS + 3; i++) {
            putchar('-');
        }
        printf("\033[0m\n");
    }

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%255s %255s %63s %1023s", dev, mnt, type, opts) == 4) {
            if (use_table) {
                /* Table mode: colors, alignment, truncation */
                printf("\033[38;5;204m");
                print_padded(dev, W_DEV);
                printf("\033[0m ");

                printf("\033[38;5;75m");
                print_padded(mnt, W_MNT);
                printf("\033[0m ");

                printf("\033[38;5;141m");
                print_padded(type, W_TYPE);
                printf("\033[0m ");

                printf("\033[38;5;114m");
                print_padded(opts, W_OPTS);
                printf("\033[0m\n");
            } else {
                /* Narrow mode: like native mount, no truncation, wraps, shows everything */
                printf("\033[38;5;204m%s\033[0m on ", dev);
                printf("\033[38;5;75m%s\033[0m type ", mnt);
                printf("\033[38;5;141m%s\033[0m ", type);
                printf("\033[38;5;114m(%s)\033[0m\n", opts);
            }
        }
    }
    fclose(f);
    return 0;
}
/* Safely append a data argument; avoids strncat length bugs that drop characters or overflow */
static void append_data(char *data_out, size_t data_sz, const char *token) {
    size_t len = strlen(data_out);
    if (len > 0) {
        if (len + 1 >= data_sz) return;
        data_out[len++] = ',';
        data_out[len] = '\0';
    }
    size_t tlen = strlen(token);
    size_t avail = data_sz - len - 1;
    if (tlen > avail) tlen = avail;
    memcpy(data_out + len, token, tlen);
    data_out[len + tlen] = '\0';
}

/* Parse -o options into the flags mount() expects */
static int parse_mount_flags(const char *opts, char *data_out, size_t data_sz) {
    int flags = 0;
    data_out[0] = '\0';
    if (!opts) return 0;

    char *copy = strdup(opts);
    if (!copy) return 0;
    char *token = strtok(copy, ",");
    while (token) {
        if (strcmp(token, "ro") == 0)         flags |= MS_RDONLY;
        else if (strcmp(token, "nosuid") == 0)  flags |= MS_NOSUID;
        else if (strcmp(token, "nodev") == 0)   flags |= MS_NODEV;
        else if (strcmp(token, "noexec") == 0)  flags |= MS_NOEXEC;
        else if (strcmp(token, "sync") == 0)    flags |= MS_SYNCHRONOUS;
        else if (strcmp(token, "remount") == 0) flags |= MS_REMOUNT;
        else if (strcmp(token, "bind") == 0)    flags |= MS_BIND;
        else if (strcmp(token, "rbind") == 0)   flags |= MS_BIND | MS_REC;
        else if (strncmp(token, "mode=", 5) == 0 ||
            strncmp(token, "uid=", 4) == 0 ||
            strncmp(token, "gid=", 4) == 0 ||
            strncmp(token, "size=", 5) == 0) {
            /* These are data arguments passed to the filesystem driver */
            append_data(data_out, data_sz, token);
            }
            token = strtok(NULL, ",");
    }
    free(copy);
    return flags;
}

static int do_mount(int argc, char **argv) {
    if (argc == 1) return list_mounts();

    const char *source = NULL;
    const char *target = NULL;
    const char *fstype = NULL;
    const char *opts = NULL;
    int end_opts = 0;

    for (int i = 1; i < argc; i++) {
        if (!end_opts && strcmp(argv[i], "--") == 0) {
            end_opts = 1;
            continue;
        }
        if (!end_opts && strcmp(argv[i], "-t") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "mount: option -t requires an argument\n");
                return 1;
            }
            fstype = argv[i];
        } else if (!end_opts && strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "mount: option -o requires an argument\n");
                return 1;
            }
            opts = argv[i];
        } else if (!end_opts && argv[i][0] == '-') {
            fprintf(stderr, "mount: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            if (!source) source = argv[i];
            else if (!target) target = argv[i];
            else {
                fprintf(stderr, "mount: too many arguments\n");
                return 1;
            }
        }
    }

    if (!source || !target) {
        fprintf(stderr, "usage: mount [-t fstype] [-o options] device dir\n");
        return 1;
    }

    char data[512];
    int flags = parse_mount_flags(opts, data, sizeof(data));

    if (mount(source, target, fstype, flags, data[0] ? data : NULL) < 0) {
        fprintf(stderr, "mount: mounting %s on %s failed: %s\n",
                source, target, strerror(errno));
        return 1;
    }

    return 0;
}

static int do_umount(int argc, char **argv) {
    int flags = 0;
    const char *target = NULL;
    int end_opts = 0;

    for (int i = 1; i < argc; i++) {
        if (!end_opts && strcmp(argv[i], "--") == 0) {
            end_opts = 1;
            continue;
        }
        if (!end_opts && strcmp(argv[i], "-f") == 0) {
            flags |= MNT_FORCE;
        } else if (!end_opts && argv[i][0] == '-') {
            fprintf(stderr, "umount: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            target = argv[i];
        }
    }

    if (!target) {
        fprintf(stderr, "usage: umount [-f] target\n");
        return 1;
    }

    if (umount2(target, flags) < 0) {
        fprintf(stderr, "umount: can't unmount %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    render_begin();

    /* BusyBox-style applet dispatch */
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    if (strcmp(prog, "umount") == 0) {
        return do_umount(argc, argv);
    }

    /* Default to mount */
    return do_mount(argc, argv);
}
