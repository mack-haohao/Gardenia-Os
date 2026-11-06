#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <ctype.h>
#include "librender.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define GARDENIA_RM_VERSION "1.0.0"

#define CLR_RESET   "\x1b[0m"
#define CLR_BOLD    "\x1b[1m"
#define CLR_DIM     "\x1b[2m"
#define CLR_PURPLE  "\x1b[38;2;203;166;247m"
#define CLR_LILAC   "\x1b[38;2;180;190;254m"
#define CLR_RED     "\x1b[38;2;243;139;168m"

static int g_color_out = 0;
static int g_color_err = 0;

static const char *CO(const char *s) { return g_color_out ? s : ""; }
static const char *CE(const char *s) { return g_color_err ? s : ""; }

static void init_theme(void)
{
    const char *no = getenv("NO_COLOR");
    int no_color = (no != NULL && *no != '\0');
    g_color_out = !no_color && isatty(STDOUT_FILENO);
    g_color_err = !no_color && isatty(STDERR_FILENO);
}

static void errmsg(const char *fmt, ...)
{
    fprintf(stderr, "%srm:%s ", CE(CLR_RED), CE(CLR_RESET));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* options */
static int opt_force     = 0;
static int opt_recursive = 0;
static int opt_list      = 0;

/* helpers */

/*
 * Recursively count every entry below `path` (files, links, nested
 * directories).  Symbolic links are never followed — lstat() is used on
 * each child, mirroring the removal semantics.
 */
static unsigned long count_items(const char *path)
{
    DIR *dh = opendir(path);
    if (!dh)
        return 0;

    unsigned long n = 0;
    struct dirent *ent;

    while ((ent = readdir(dh)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        n++;

        char child[PATH_MAX];
        int len = snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
        if (len < 0 || (size_t)len >= sizeof child)
            continue;

        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            n += count_items(child);
    }
    closedir(dh);
    return n;
}

/* removal engine (unchanged semantics) */

static int remove_entry(const char *path);

static int remove_dir_contents(const char *path)
{
    DIR *dh = opendir(path);
    if (!dh) {
        errmsg("cannot open directory '%s': %s", path, strerror(errno));
        return 1;
    }

    int status = 0;
    struct dirent *ent;

    while ((ent = readdir(dh)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        int n = snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
        if (n < 0 || n >= (int)sizeof child) {
            errmsg("path too long: %s/%s", path, ent->d_name);
            status = 1;
            continue;
        }
        status |= remove_entry(child);
    }
    closedir(dh);
    return status;
}

static int remove_entry(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT && opt_force)
            return 0;
        errmsg("cannot remove '%s': %s", path, strerror(errno));
        return 1;
    }

    /* Symlinks to directories are NOT directories here: lstat() reported
     * the link itself, so unlink() will remove just the link. */
    int is_dir = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);

    if (is_dir) {
        if (!opt_recursive) {
            errmsg("cannot remove '%s': Is a directory", path);
            return 1;
        }
        int status = remove_dir_contents(path);
        if (rmdir(path) < 0) {
            errmsg("cannot remove '%s': %s", path, strerror(errno));
            status = 1;
        }
        return status;
    }

    if (unlink(path) < 0) {
        if (errno == ENOENT && opt_force)
            return 0;
        errmsg("cannot remove '%s': %s", path, strerror(errno));
        return 1;
    }
    return 0;
}

/* listing (--list) */

/* Returns 1 if the entry was shown, 0 if skipped (missing + -f). */
static int list_target(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT && opt_force)
            return 0;
        printf("  %s%s%s %s(not found)%s\n",
               CO(CLR_RED), path, CO(CLR_RESET),
               CO(CLR_DIM), CO(CLR_RESET));
        return 1;
    }

    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        size_t len = strlen(path);
        int has_slash = (len > 0 && path[len - 1] == '/');
        unsigned long n = count_items(path);

        printf("  %s%s%s%s ",
               CO(CLR_RED), path, has_slash ? "" : "/", CO(CLR_RESET));

        if (n == 0)
            printf("%s(empty)%s\n", CO(CLR_DIM), CO(CLR_RESET));
        else
            printf("%s(%lu item%s)%s\n",
                   CO(CLR_DIM), n, n == 1 ? "" : "s", CO(CLR_RESET));
    } else if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        ssize_t k = readlink(path, tgt, sizeof tgt - 1);
        if (k >= 0)
            tgt[k] = '\0';

        printf("  %s%s%s %s-> %s%s\n",
               CO(CLR_RED), path, CO(CLR_RESET),
               CO(CLR_DIM), k >= 0 ? tgt : "?", CO(CLR_RESET));
    } else {
        printf("  %s%s%s\n", CO(CLR_RED), path, CO(CLR_RESET));
    }
    return 1;
}

/* confirmation box */

static void box_rule(int inner, int top)
{
    (void)top;
    printf("  %s+", CO(CLR_PURPLE));
    for (int i = 0; i < inner; i++)
        printf("-");
    printf("+%s\n", CO(CLR_RESET));
}

static void box_text(const char *text, int inner)
{
    int avail = inner - 2;
    int w = (int)strlen(text);
    if (w > avail)
        w = avail;

    printf("  %s|%s ", CO(CLR_PURPLE), CO(CLR_RESET)); /* │ */
    fwrite(text, 1, (size_t)w, stdout);
    for (int i = w; i < avail; i++)
        putchar(' ');
    printf(" %s|%s\n", CO(CLR_PURPLE), CO(CLR_RESET));
}

static int confirm_removal(int count)
{
    if (!isatty(STDIN_FILENO)) {
        errmsg("cannot ask for confirmation: stdin is not a terminal");
        return -1;
    }

    const int inner = 46;
    char line[128];

    printf("\n");
    box_rule(inner, 1);
    box_text("", inner);
    snprintf(line, sizeof line, "Remove %d item%s?",
             count, count == 1 ? "" : "s");
    box_text(line, inner);
    box_text("", inner);
    box_text("[y] Yes           [n] No", inner);
    box_text("", inner);
    box_rule(inner, 0);

    printf("  %sChoose >%s ", CO(CLR_PURPLE), CO(CLR_RESET));
    fflush(stdout);

    char buf[64];
    if (!fgets(buf, sizeof buf, stdin))
        return -1;

    for (char *p = buf; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    return buf[0] == 'y';
}

/* usage */

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: rm [OPTION]... FILE...\n"
        "Remove (unlink) the FILE(s).\n"
        "\n"
        "  -f, --force          ignore nonexistent files, never prompt\n"
        "  -r, -R, --recursive  remove directories and their contents recursively\n"
        "  -l, --list           preview what would be removed, then ask to confirm\n"
        "  -h, --help           display this help and exit\n"
        "  -v, --version        output version information and exit\n"
        "\n"
        "By default, rm does not remove directories.  Use --recursive (-r)\n"
        "to remove each listed directory, along with all of its contents.\n");
}

/* main */

int main(int argc, char **argv)
{
    init_theme();
    render_begin();

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0')
            break;

        if (!strcmp(a, "--")) {
            i++;
            break;
        }

        if (a[1] == '-') {                       /* long option */
            if      (!strcmp(a, "--force"))     opt_force     = 1;
            else if (!strcmp(a, "--recursive")) opt_recursive = 1;
            else if (!strcmp(a, "--list"))      opt_list      = 1;
            else if (!strcmp(a, "--help"))    { usage(stdout); return 0; }
            else if (!strcmp(a, "--version")) {
                printf("rm %s\n", GARDENIA_RM_VERSION);
                return 0;
            } else {
                errmsg("unknown option '%s'", a);
                errmsg("Try 'rm --help' for more information.");
                return 2;
            }
        } else {                                 /* short option cluster */
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'f': opt_force     = 1; break;
                case 'r':
                case 'R': opt_recursive = 1; break;
                case 'l': opt_list      = 1; break;
                case 'h': usage(stdout); return 0;
                case 'v':
                    printf("rm %s\n", GARDENIA_RM_VERSION);
                    return 0;
                default:
                    errmsg("unknown option '-%c'", *p);
                    errmsg("Try 'rm --help' for more information.");
                    return 2;
                }
            }
        }
    }

    if (i >= argc) {
        if (!opt_force) {
            errmsg("missing operand");
            errmsg("Try 'rm --help' for more information.");
            return 2;
        }
        return 0;
    }

    if (opt_list) {
        printf("%sGardenia%s %srm%s \u2014 the following will be removed:\n\n",
               CO(CLR_BOLD), CO(CLR_RESET), CO(CLR_LILAC), CO(CLR_RESET));

        int shown = 0;
        for (int j = i; j < argc; j++)
            shown += list_target(argv[j]);

        if (shown == 0) {
            printf("  %s(nothing to remove)%s\n",
                   CO(CLR_DIM), CO(CLR_RESET));
            return 0;
        }

        printf("\n");
        int r = confirm_removal(shown);
        if (r < 0)
            return 1;
        if (r == 0) {
            printf("\n  %sAborted, nothing was removed.%s\n",
                   CO(CLR_DIM), CO(CLR_RESET));
            return 0;
        }
        printf("\n");
    }

    int status = 0;
    for (; i < argc; i++)
        status |= remove_entry(argv[i]);

    return status;
}
