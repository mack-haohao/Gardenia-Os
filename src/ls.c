#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include "librender.h"

#define LS_VERSION "1.0.0"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ANSI color codes */
#define C_RESET  "\033[0m"
#define C_DIR    "\033[38;5;141m"
#define C_LINK   "\033[38;5;177m"
#define C_EXEC   "\033[38;5;183m"
#define C_FIFO   "\033[38;5;183m"
#define C_SOCK   "\033[38;5;189m"
#define C_DEV    "\033[38;5;189m"
#define C_HIDDEN "\033[38;5;245m"

static int use_color = 0;

typedef struct {
    int invisible;
    int permission;
    int size;
    int has_depth;   /* was --depth given?                     */
    int max_depth;   /* -1 = unlimited, 0 = don't expand       */
} options_t;

/* color */

static const char *color_for(mode_t mode, const char *name)
{
    if (!use_color) return "";
    if (S_ISDIR(mode))  return C_DIR;
    if (S_ISLNK(mode))  return C_LINK;
    if (S_ISFIFO(mode)) return C_FIFO;
    if (S_ISSOCK(mode)) return C_SOCK;
    if (S_ISBLK(mode) || S_ISCHR(mode)) return C_DEV;
    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return C_EXEC;
    if (name && name[0] == '.') return C_HIDDEN;
    return "";
}

/* formatting */

static void format_permissions(mode_t mode, char out[11])
{
    out[0] = S_ISDIR(mode)  ? 'd' :
             S_ISLNK(mode)  ? 'l' :
             S_ISCHR(mode)  ? 'c' :
             S_ISBLK(mode)  ? 'b' :
             S_ISFIFO(mode) ? 'p' :
             S_ISSOCK(mode) ? 's' : '-';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_ISUID) ? ((mode & S_IXUSR) ? 's' : 'S')
                              : ((mode & S_IXUSR) ? 'x' : '-');
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_ISGID) ? ((mode & S_IXGRP) ? 's' : 'S')
                              : ((mode & S_IXGRP) ? 'x' : '-');
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_ISVTX) ? ((mode & S_IXOTH) ? 't' : 'T')
                              : ((mode & S_IXOTH) ? 'x' : '-');
    out[10] = '\0';
}

static void format_size(off_t size, char *buf, size_t bufsz)
{
    static const char *units[] = {"B", "K", "M", "G", "T", "P"};
    double s = (double)size;
    int u = 0;
    while (s >= 1024.0 && u < 5) { s /= 1024.0; u++; }
    if (u == 0)
        snprintf(buf, bufsz, "%lldB", (long long)size);
    else
        snprintf(buf, bufsz, "%.1f%s", s, units[u]);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static void print_meta(const struct stat *st, const options_t *opts)
{
    if (opts->permission) {
        char perms[11];
        format_permissions(st->st_mode, perms);
        printf("%s ", perms);
    }
    if (opts->size) {
        char buf[32];
        format_size(st->st_size, buf, sizeof(buf));
        printf("%8s ", buf);
    }
}

static void print_unknown_meta(const options_t *opts)
{
    if (opts->permission) printf("?????????? ");
    if (opts->size)       printf("%8s ", "?");
}

/* directory listing */

/*
 * is_tree = 0 : flat listing, every entry printed on its own line
 * is_tree = 1 : tree listing with ├── / └── prefixes and recursion
 *
 * remaining_depth only applies in tree mode:
 *   -1 = unlimited, 0 = don't recurse, N > 0 = N more levels
 */
static int list_dir_contents(const char *dir, const char *prefix,
                             int is_tree, int remaining_depth,
                             const options_t *opts)
{
    DIR *dh = opendir(dir);
    if (!dh) {
        fprintf(stderr, "ls: %s: %s\n", dir, strerror(errno));
        return 1;
    }

    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dh)) != NULL) {
        const char *nm = ent->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (!opts->invisible && nm[0] == '.') continue;

        if (count >= cap) {
            cap = cap ? cap * 2 : 64;
            char **tmp = realloc(names, (size_t)cap * sizeof(char *));
            if (!tmp) {
                for (int k = 0; k < count; k++) free(names[k]);
                free(names);
                closedir(dh);
                return 1;
            }
            names = tmp;
        }
        names[count] = strdup(nm);
        if (!names[count]) {
            for (int k = 0; k < count; k++) free(names[k]);
            free(names);
            closedir(dh);
            return 1;
        }
        count++;
    }
    closedir(dh);

    if (count > 1)
        qsort(names, (size_t)count, sizeof(char *), cmp_str);

    int status = 0;
    for (int i = 0; i < count; i++) {
        int last = (i == count - 1);

        char fullpath[PATH_MAX];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, names[i]);
        if (n < 0 || n >= (int)sizeof(fullpath)) {
            fprintf(stderr, "ls: path too long: %s/%s\n", dir, names[i]);
            free(names[i]);
            status = 1;
            continue;
        }

        struct stat st;
        int ok = (lstat(fullpath, &st) == 0);
        int is_dir = ok && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);

        if (is_tree)
	    printf("%s%s", prefix, last ? "`-- " : "|-- ");
        if (ok) {
            print_meta(&st, opts);
            const char *col = color_for(st.st_mode, names[i]);
            printf("%s%s%s", col, names[i], is_dir ? "/" : "");
            if (use_color) printf(C_RESET);
        } else {
            print_unknown_meta(opts);
            printf("%s", names[i]);
        }
        printf("\n");

        if (is_tree && is_dir && (remaining_depth == -1 || remaining_depth > 0)) {
            char newprefix[PATH_MAX];
		snprintf(newprefix, sizeof(newprefix), "%s%s",
	        	 prefix, last ? "    " : "|   ");
            status |= list_dir_contents(fullpath, newprefix, 1,
                                        remaining_depth == -1 ? -1 : remaining_depth - 1,
                                        opts);
        }
        free(names[i]);
    }
    free(names);
    return status;
}

/* top-level path */

static int list_path(const char *path, const options_t *opts)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    int is_dir = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);

    if (!is_dir) {
        /* a single file: always one line, flat */
        print_meta(&st, opts);
        const char *col = color_for(st.st_mode, path);
        printf("%s%s", col, path);
        if (use_color) printf(C_RESET);
        printf("\n");
        return 0;
    }

    if (!opts->has_depth) {
        /* default: flat listing of the directory contents */
        return list_dir_contents(path, "", 0, 0, opts);
    }

    /* tree mode: print the directory itself, then expand */
    print_meta(&st, opts);
    const char *col = color_for(st.st_mode, path);
    printf("%s%s", col, path);
    size_t len = strlen(path);
    if (len == 0 || path[len - 1] != '/') printf("/");
    if (use_color) printf(C_RESET);
    printf("\n");

    if (opts->max_depth == 0)
        return 0;

    return list_dir_contents(path, "", 1, opts->max_depth, opts);
}

/* CLI */

static void print_help(const char *prog)
{
    printf("Gals - Gardenia ls\n");
    printf("Usage: %s [options] [paths...]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  --version              Show version information\n");
    printf("  --invisible, -i        Show hidden files\n");
    printf("  --permission, -p       Show file permissions\n");
    printf("  --full, -f             Show full details (permission + size)\n");
    printf("  --size, -s             Show file size\n");
    printf("  --depth N, -d N        Tree view; N levels or * for unlimited\n");
    printf("  --help, -h             Show this help message\n");
}

int main(int argc, char **argv)
{
    use_color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
    render_begin();
    options_t opts;
    memset(&opts, 0, sizeof(opts));

    char **paths = calloc((size_t)argc, sizeof(char *));
    if (!paths) return 1;
    int npaths = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--version") == 0) {
            printf("Gals (refactored ls) %s\n", LS_VERSION);
            free(paths);
            return 0;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_help(argv[0]);
            free(paths);
            return 0;
        }

        if (strcmp(a, "--invisible") == 0 || strcmp(a, "-i") == 0 ||
            strcmp(a, "-a") == 0 || strcmp(a, "-A") == 0) {
            opts.invisible = 1;
        } else if (strcmp(a, "--permission") == 0 || strcmp(a, "-p") == 0) {
            opts.permission = 1;
        } else if (strcmp(a, "--size") == 0 || strcmp(a, "-s") == 0) {
            opts.size = 1;
        } else if (strcmp(a, "--full") == 0 || strcmp(a, "-f") == 0 ||
                   strcmp(a, "-l") == 0) {
            opts.permission = 1;
            opts.size = 1;
        } else if (strcmp(a, "-al") == 0 || strcmp(a, "-la") == 0 ||
                   strcmp(a, "-alF") == 0 || strcmp(a, "-laF") == 0) {
            opts.invisible = 1;
            opts.permission = 1;
            opts.size = 1;
        } else if (strcmp(a, "-F") == 0 || strcmp(a, "-CF") == 0) {
            /* accepted for compatibility, no effect */
        } else if (strcmp(a, "--depth") == 0 || strcmp(a, "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ls: option '%s' requires an argument\n", a);
                free(paths);
                return 2;
            }
            i++;
            opts.has_depth = 1;
            if (strcmp(argv[i], "*") == 0) {
                opts.max_depth = -1;
            } else {
                char *end = NULL;
                long v = strtol(argv[i], &end, 10);
                if (!end || *end != '\0' || v < 0) {
                    fprintf(stderr, "ls: invalid depth '%s'\n", argv[i]);
                    free(paths);
                    return 2;
                }
                opts.max_depth = (int)v;
            }
        } else if (strncmp(a, "--depth=", 8) == 0) {
            const char *v = a + 8;
            opts.has_depth = 1;
            if (strcmp(v, "*") == 0) {
                opts.max_depth = -1;
            } else {
                char *end = NULL;
                long d = strtol(v, &end, 10);
                if (!end || *end != '\0' || d < 0) {
                    fprintf(stderr, "ls: invalid depth '%s'\n", v);
                    free(paths);
                    return 2;
                }
                opts.max_depth = (int)d;
            }
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "ls: unknown option '%s'\n", a);
            free(paths);
            return 2;
        } else {
            paths[npaths++] = argv[i];
        }
    }

    if (npaths == 0) {
        static char dot[] = ".";
        paths[npaths++] = dot;
    }

    int status = 0;
    for (int i = 0; i < npaths; i++) {
        if (i > 0) printf("\n");
        status |= list_path(paths[i], &opts);
    }

    free(paths);
    return status;
}
