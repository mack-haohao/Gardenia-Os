/* textTools.c - BusyBox-style text tool collection
 * Build:  cc -O2 -o texttools textTools.c
 * Setup:  ln -s texttools grep; ln -s texttools sed; ln -s texttools sort */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <regex.h>
#include <errno.h>

/* Shared helpers */

typedef struct { char **v; int n; int cap; } strvec;

static void sv_push(strvec *sv, char *s) {
    if (sv->n >= sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 64;
        sv->v = realloc(sv->v, sizeof(char *) * sv->cap);
    }
    sv->v[sv->n++] = s;
}
static void sv_free(strvec *sv) {
    for (int i = 0; i < sv->n; i++) free(sv->v[i]);
    free(sv->v);
    sv->v = NULL; sv->n = 0; sv->cap = 0;
}

static void read_lines_from(FILE *f, strvec *sv) {
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, f)) >= 0) {
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        sv_push(sv, strdup(line));
    }
    free(line);
}

static FILE *open_input(const char *path) {
    if (!path || !strcmp(path, "-")) return stdin;
    FILE *f = fopen(path, "rb");
    if (!f) perror(path);
    return f;
}

static int write_lines_to(const char *path, strvec *lines) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    for (int i = 0; i < lines->n; i++) {
        fputs(lines->v[i], f);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

/* Simple LCS line diff */
#define COL_DEL  "\033[41m"   /* Red bg: deletion */
#define COL_ADD  "\033[42m"   /* Green bg: addition */
#define COL_RST  "\033[0m"

static void print_diff(strvec *a, strvec *b) {
    int na = a->n, nb = b->n;

    /* Huge inputs degrade to positional line diff to avoid O(n*m) memory */
    if ((long long)na * nb > 4000000LL) {
        int m = na > nb ? na : nb;
        for (int i = 0; i < m; i++) {
            const char *la = i < na ? a->v[i] : NULL;
            const char *lb = i < nb ? b->v[i] : NULL;
            if (la && lb && !strcmp(la, lb)) continue;
            if (la) printf(COL_DEL "- %s" COL_RST "\n", la);
            if (lb) printf(COL_ADD "+ %s" COL_RST "\n", lb);
        }
        return;
    }

    int w = nb + 1;
    int *dp = calloc((size_t)(na + 1) * w, sizeof(int));
    if (!dp) return;
    for (int i = na - 1; i >= 0; i--)
        for (int j = nb - 1; j >= 0; j--) {
            if (!strcmp(a->v[i], b->v[j]))
                dp[i*w + j] = dp[(i+1)*w + j+1] + 1;
            else {
                int x = dp[(i+1)*w + j], y = dp[i*w + j+1];
                dp[i*w + j] = x > y ? x : y;
            }
        }
    int i = 0, j = 0;
    while (i < na && j < nb) {
        if (!strcmp(a->v[i], b->v[j])) { i++; j++; }
        else if (dp[(i+1)*w + j] >= dp[i*w + j+1]) {
            printf(COL_DEL "- %s" COL_RST "\n", a->v[i]); i++;
        } else {
            printf(COL_ADD "+ %s" COL_RST "\n", b->v[j]); j++;
        }
    }
    while (i < na) printf(COL_DEL "- %s" COL_RST "\n", a->v[i++]);
    while (j < nb) printf(COL_ADD "+ %s" COL_RST "\n", b->v[j++]);
    free(dp);
}

/* grep */

typedef struct {
    int invert, icase;
    int show_lineno;      /* -n or --full/-f */
    int full;             /* --full remembers so we can hint */
    int show;             /* --show/-s: highlight matches */
    int count_only;       /* -c */
    int files_with_matches; /* -l */
    int quiet;            /* -q */
    int force_filename;   /* -H */
    int no_filename;      /* -h */
} grep_opts;

/* Green-background highlight of matches (the "after" view for --show) */
static void grep_highlight(const char *line, regex_t *re) {
    size_t off = 0, len = strlen(line);
    while (off <= len) {
        regmatch_t m;
        int ef = off ? REG_NOTBOL : 0;
        if (regexec(re, line + off, 1, &m, ef) != 0) break;

        fwrite(line + off, 1, m.rm_so, stdout);

        if (m.rm_eo == m.rm_so) {                 /* Zero-length match guard */
            if (off + m.rm_so < len) putchar(line[off + m.rm_so]);
            off += m.rm_so + 1;
            if (off > len) break;
            continue;
        }
        printf("\033[30;42m");
        fwrite(line + off + m.rm_so, 1, m.rm_eo - m.rm_so, stdout);
        printf(COL_RST);
        off += m.rm_eo;
    }
    if (off < len) fwrite(line + off, 1, len - off, stdout);
    putchar('\n');
}

static int grep_file(const char *name, FILE *f, regex_t *re,
                     grep_opts *o, int show_prefix) {
    char *line = NULL; size_t cap = 0; ssize_t n;
    long lineno = 0, count = 0;

    while ((n = getline(&line, &cap, f)) >= 0) {
        lineno++;
        size_t len = (size_t)n;
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        int matched = (regexec(re, line, 0, NULL, 0) == 0);
        if (o->invert) matched = !matched;
        if (!matched) continue;

        count++;
        if (o->quiet)           { free(line); return 1; }
        if (o->count_only)      continue;
        if (o->files_with_matches) { if (name) puts(name); free(line); return 1; }

        if (show_prefix && name) printf("%s:", name);
        if (o->show_lineno)      printf("%ld: ", lineno);   /* --full format */
        if (o->show)             grep_highlight(line, re);
        else                   { fputs(line, stdout); putchar('\n'); }
    }

    if (o->count_only) {
        if (show_prefix && name) printf("%s:", name);
        printf("%ld\n", count);
    }
    free(line);
    return count > 0;
}

static int cmd_grep(int argc, char **argv) {
    grep_opts o; memset(&o, 0, sizeof o);
    strvec patterns = {0}, files = {0};
    int i = 1;

    while (i < argc) {
        char *a = argv[i];
        if (!strcmp(a, "--full"))  { o.full = 1; o.show_lineno = 1; i++; }
        else if (!strcmp(a, "--show")) { o.show = 1; i++; }
        else if (!strcmp(a, "--"))  { i++; break; }
        else if (a[0] == '-' && a[1]) {
            for (char *c = a + 1; *c; c++) {
                switch (*c) {
                case 'i': o.icase = 1; break;
                case 'v': o.invert = 1; break;
                case 'n': o.show_lineno = 1; break;
                case 'c': o.count_only = 1; break;
                case 'l': o.files_with_matches = 1; break;
                case 'q': o.quiet = 1; break;
                case 'H': o.force_filename = 1; break;
                case 'h': o.no_filename = 1; break;
                case 'f': o.full = 1; o.show_lineno = 1; break;   /* --full */
                case 's': o.show = 1; break;                       /* --show */
                case 'e': goto take_e;
                default: break;
                }
            }
            i++;
            continue;
take_e:
            if (i + 1 >= argc) { fprintf(stderr, "grep: -e needs arg\n"); return 2; }
            sv_push(&patterns, strdup(argv[i + 1]));
            i += 2;
        } else break;
    }
    if (patterns.n == 0) {
        if (i >= argc) { fprintf(stderr, "grep: missing pattern\n"); return 2; }
        sv_push(&patterns, strdup(argv[i++]));
    }
    for (; i < argc; i++) sv_push(&files, strdup(argv[i]));

    /* Assemble the regex: multiple patterns as (...) | (...) */
    char *pat;
    if (patterns.n == 1) pat = strdup(patterns.v[0]);
    else {
        size_t tot = 0;
        for (int k = 0; k < patterns.n; k++) tot += strlen(patterns.v[k]) + 4;
        pat = malloc(tot + 1); pat[0] = 0;
        for (int k = 0; k < patterns.n; k++) {
            if (k) strcat(pat, "|");
            strcat(pat, "("); strcat(pat, patterns.v[k]); strcat(pat, ")");
        }
    }

    regex_t re;
    int cflags = REG_EXTENDED | (o.icase ? REG_ICASE : 0);
    if (regcomp(&re, pat, cflags) != 0) {
        fprintf(stderr, "grep: bad regex: %s\n", pat);
        free(pat); return 2;
    }
    free(pat);

    int show_prefix = o.force_filename || (files.n > 1 && !o.no_filename);
    int any = 0;

    if (files.n == 0) {
        any |= grep_file(NULL, stdin, &re, &o, 0);
    } else {
        for (int k = 0; k < files.n; k++) {
            FILE *f = open_input(files.v[k]);
            if (!f) continue;
            any |= grep_file(files.v[k], f, &re, &o, show_prefix);
            if (f != stdin) fclose(f);
        }
    }

    regfree(&re);
    sv_free(&patterns);
    sv_free(&files);
    return any ? 0 : 1;
}

/* sed */

typedef struct sed_cmd {
    int a1_type;   /* 0=none 1=line 2=regex 3=$ */
    long a1_num;   regex_t a1_re;
    int a2_type;
    long a2_num;   regex_t a2_re;
    int negate;
    int cmd;

    regex_t s_re;
    char *s_repl;
    int s_global, s_number, s_print;

    char *text;    /* text for a / i / c */
    int range_active;

    struct sed_cmd *next;
} sed_cmd;

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read up to delim; \<delim> becomes delim, other escapes are kept verbatim */
static char *read_until_delim(const char **pp, char delim) {
    const char *p = *pp;
    char *buf = malloc(strlen(p) + 1);
    size_t n = 0;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1] == delim)      { buf[n++] = delim; p += 2; }
        else if (*p == '\\' && p[1])          { buf[n++] = *p++; buf[n++] = *p++; }
        else                                  { buf[n++] = *p++; }
    }
    if (*p == delim) p++;
    buf[n] = 0;
    *pp = p;
    return buf;
}

static int parse_addr(const char **pp, int *type, long *num, regex_t *re) {
    const char *p = skip_ws(*pp);
    if (!*p || *p == ';' || *p == '\n') return 0;

    if (*p == '$') { *type = 3; p++; }
    else if (isdigit((unsigned char)*p)) {
        *type = 1;
        char *end;
        *num = strtol(p, &end, 10);
        p = end;
    } else if (*p == '/') {
        p++;
        char *buf = malloc(strlen(p) + 1);
        size_t n = 0;
        while (*p && *p != '/') {
            if (*p == '\\' && p[1] == '/') { buf[n++] = '/'; p += 2; }
            else if (*p == '\\' && p[1])   { buf[n++] = *p++; buf[n++] = *p++; }
            else                           { buf[n++] = *p++; }
        }
        if (*p == '/') p++;
        buf[n] = 0;
        /* Use ERE so + ? | () are available */
        regcomp(re, buf, REG_EXTENDED);
        free(buf);
        *type = 2;
    } else return 0;

    *pp = p;
    return 1;
}

static sed_cmd *parse_sed(const char *script) {
    sed_cmd *head = NULL, *tail = NULL;
    const char *p = script;

    while (*p) {
        while (*p == ';' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;

        sed_cmd *c = calloc(1, sizeof(*c));
        if (!parse_addr(&p, &c->a1_type, &c->a1_num, &c->a1_re)) c->a1_type = 0;
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            if (!parse_addr(&p, &c->a2_type, &c->a2_num, &c->a2_re)) c->a2_type = 0;
            p = skip_ws(p);
        }
        if (*p == '!') { c->negate = 1; p++; }
        p = skip_ws(p);
        if (!*p) { free(c); break; }

        char ch = *p++;
        c->cmd = ch;

        switch (ch) {
        case 's': {
            char delim = *p ? *p++ : '/';
            char *pat  = read_until_delim(&p, delim);
            char *repl = read_until_delim(&p, delim);
            int flags = REG_EXTENDED;
            while (*p && *p != ';' && *p != '\n') {
                if (*p == 'g')            c->s_global = 1;
                else if (*p == 'p')       c->s_print = 1;
                else if (*p == 'i' || *p == 'I') flags |= REG_ICASE;
                else if (isdigit((unsigned char)*p)) {
                    char *end;
                    c->s_number = (int)strtol(p, &end, 10);
                    p = end;
                    continue;
                }
                p++;
            }
            regcomp(&c->s_re, pat, flags);
            c->s_repl = repl;
            free(pat);
            break;
        }
        case 'p': case 'd': case 'q': case '=': case 'n':
            break;
        case 'a': case 'i': case 'c': {
            p = skip_ws(p);
            if (*p == '\\') p++;
            char *buf = malloc(strlen(p) + 1);
            size_t n = 0;
            while (*p && *p != '\n' && *p != ';') {
                if (*p == '\\' && p[1] == ';') { buf[n++] = ';'; p += 2; }
                else                            buf[n++] = *p++;
            }
            buf[n] = 0;
            c->text = buf;
            break;
        }
        default:
            fprintf(stderr, "sed: unknown command '%c'\n", ch);
            free(c);
            return head;
        }

        if (tail) tail->next = c; else head = c;
        tail = c;

        while (*p && *p != ';' && *p != '\n') p++;
    }
    return head;
}

static int addr_match_one(int type, long num, regex_t *re,
                          int lineno, int last, const char *line) {
    switch (type) {
    case 1: return lineno == num;
    case 2: return regexec(re, line, 0, NULL, 0) == 0;
    case 3: return lineno == last;
    }
    return 1;
}

static int addr_match(sed_cmd *c, int lineno, int last, const char *line) {
    int result;
    if (c->a1_type == 0) result = 1;
    else if (c->a2_type != 0) {
        if (c->range_active) {
            result = 1;
            if (addr_match_one(c->a2_type, c->a2_num, &c->a2_re, lineno, last, line))
                c->range_active = 0;
        } else if (addr_match_one(c->a1_type, c->a1_num, &c->a1_re, lineno, last, line)) {
            result = 1;
            if (!addr_match_one(c->a2_type, c->a2_num, &c->a2_re, lineno, last, line))
                c->range_active = 1;
        } else result = 0;
    } else {
        result = addr_match_one(c->a1_type, c->a1_num, &c->a1_re, lineno, last, line);
    }
    if (c->negate) result = !result;
    return result;
}

/* Dynamic string buffer */
typedef struct { char *buf; size_t pos, cap; } sbuf;
static void sb_init(sbuf *b) { b->buf = NULL; b->pos = b->cap = 0; }
static void sb_reserve(sbuf *b, size_t need) {
    if (b->pos + need + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->pos + need + 1) nc *= 2;
        b->buf = realloc(b->buf, nc);
        b->cap = nc;
    }
}
static void sb_add(sbuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->buf + b->pos, s, n);
    b->pos += n;
    b->buf[b->pos] = 0;
}
static void sb_addc(sbuf *b, char c) { sb_add(b, &c, 1); }
static char *sb_done(sbuf *b) {
    if (!b->buf) { b->buf = strdup(""); b->cap = 1; }
    return b->buf;
}

static char *do_subst(sed_cmd *c, const char *line) {
    sbuf out; sb_init(&out);
    size_t off = 0, len = strlen(line);
    int count = 0, stop = 0;

    while (off <= len && !stop) {
        regmatch_t pm[10];
        int ef = off ? REG_NOTBOL : 0;
        if (regexec(&c->s_re, line + off, 10, pm, ef) != 0) break;

        size_t so = off + pm[0].rm_so;
        size_t eo = off + pm[0].rm_eo;
        sb_add(&out, line + off, so - off);
        count++;

        int do_repl;
        if (c->s_number > 0) do_repl = (count == c->s_number);
        else if (c->s_global) do_repl = 1;
        else                  do_repl = (count == 1);

        if (do_repl) {
            for (const char *r = c->s_repl; *r; r++) {
                if (*r == '&') {
                    sb_add(&out, line + so, eo - so);
                } else if (*r == '\\' && r[1]) {
                    r++;
                    if (*r >= '1' && *r <= '9') {
                        int g = *r - '0';
                        if (pm[g].rm_so >= 0)
                            sb_add(&out, line + off + pm[g].rm_so,
                                   pm[g].rm_eo - pm[g].rm_so);
                    } else if (*r == 'n') sb_addc(&out, '\n');
                    else if (*r == 't')   sb_addc(&out, '\t');
                    else                  sb_addc(&out, *r);
                } else sb_addc(&out, *r);
            }
        } else {
            sb_add(&out, line + so, eo - so);
        }

        if (eo == so) {                       /* Zero-length match guard */
            if (so < len) sb_addc(&out, line[so]);
            off = so + 1;
        } else off = eo;

        if (!c->s_global && c->s_number == 0) stop = 1;
        if (c->s_number > 0 && count >= c->s_number) stop = 1;
    }

    if (off < len) sb_add(&out, line + off, len - off);
    return sb_done(&out);
}

static void sed_run(sed_cmd *cmds, strvec *in, strvec *out, int opt_n) {
    int last = in->n;
    for (int i = 0; i < in->n; i++) {
        int lineno = i + 1;
        char *cur = strdup(in->v[i]);
        int deleted = 0, quit = 0;
        strvec append_buf = {0};

        for (sed_cmd *c = cmds; c && !deleted && !quit; c = c->next) {
            if (!addr_match(c, lineno, last, cur)) continue;
            switch (c->cmd) {
            case 's': {
                char *nw = do_subst(c, cur);
                free(cur); cur = nw;
                if (c->s_print) sv_push(out, strdup(cur));
                break;
            }
            case 'p': sv_push(out, strdup(cur)); break;
            case 'd': deleted = 1; break;
            case 'q': quit = 1; break;
            case '=': {
                char b[32]; snprintf(b, sizeof b, "%d", lineno);
                sv_push(out, strdup(b));
                break;
            }
            case 'a': sv_push(&append_buf, strdup(c->text)); break;
            case 'i': sv_push(out, strdup(c->text)); break;
            case 'c': sv_push(out, strdup(c->text)); deleted = 1; break;
            case 'n': /* Simplified: ignore, treat as printing the current line */ break;
            }
        }

        if (!deleted && !opt_n) sv_push(out, strdup(cur));
        for (int k = 0; k < append_buf.n; k++) sv_push(out, append_buf.v[k]);
        free(append_buf.v);   /* Ownership of elements moved to out */
        free(cur);
        if (quit) break;
    }
}

static int cmd_sed(int argc, char **argv) {
    int opt_n = 0, opt_i = 0, opt_show = 0;
    strvec scripts = {0}, files = {0};
    int i = 1, got_e = 0;

    while (i < argc) {
        char *a = argv[i];
        if (!strcmp(a, "-n"))                         opt_n = 1;
        else if (!strcmp(a, "-s") || !strcmp(a, "--show")) opt_show = 1;
        else if (!strcmp(a, "-i"))                    opt_i = 1;
        else if (!strncmp(a, "-i", 2))                opt_i = 1;   /* Ignore the suffix */
        else if (!strcmp(a, "-e")) {
            if (i + 1 >= argc) { fprintf(stderr, "sed: -e needs arg\n"); return 1; }
            sv_push(&scripts, strdup(argv[i + 1]));
            got_e = 1; i += 2; continue;
        }
        else if (!strcmp(a, "--")) { i++; break; }
        else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "sed: unknown option %s\n", a); return 1;
        }
        else break;
        i++;
    }

    if (!got_e) {
        if (i >= argc) { fprintf(stderr, "sed: missing script\n"); return 1; }
        sv_push(&scripts, strdup(argv[i++]));
    }
    for (; i < argc; i++) sv_push(&files, strdup(argv[i]));

    /* Merge all -e scripts */
    size_t tot = 0;
    for (int k = 0; k < scripts.n; k++) tot += strlen(scripts.v[k]) + 2;
    char *script = malloc(tot + 1); script[0] = 0;
    for (int k = 0; k < scripts.n; k++) {
        if (k) strcat(script, "\n");
        strcat(script, scripts.v[k]);
    }

    sed_cmd *cmds = parse_sed(script);
    free(script);
    sv_free(&scripts);

    int rc = 0;
    if (files.n == 0) {
        strvec in = {0}, out = {0};
        read_lines_from(stdin, &in);
        sed_run(cmds, &in, &out, opt_n);
        if (opt_show) print_diff(&in, &out);
        else if (!opt_i)
            for (int k = 0; k < out.n; k++) { fputs(out.v[k], stdout); putchar('\n'); }
        sv_free(&in); sv_free(&out);
    } else {
        for (int k = 0; k < files.n; k++) {
            strvec in = {0}, out = {0};
            FILE *f = open_input(files.v[k]);
            if (!f) { rc = 1; continue; }
            read_lines_from(f, &in);
            if (f != stdin) fclose(f);
            sed_run(cmds, &in, &out, opt_n);
            if (opt_show) print_diff(&in, &out);
            if (opt_i) {
                if (write_lines_to(files.v[k], &out) < 0) rc = 1;
            } else if (!opt_show) {
                for (int j = 0; j < out.n; j++) { fputs(out.v[j], stdout); putchar('\n'); }
            }
            sv_free(&in); sv_free(&out);
        }
    }

    sv_free(&files);
    return rc;
}

/* sort */

static int sort_numeric, sort_reverse, sort_unique, sort_fold;
static int sort_field = 0;
static char sort_sep = 0;

static char *field_dup(const char *s, int k) {
    if (sort_sep) {
        int cur = 1;
        const char *start = s;
        while (cur < k) {
            const char *p = strchr(start, sort_sep);
            if (!p) return strdup("");
            start = p + 1; cur++;
        }
        const char *end = strchr(start, sort_sep);
        size_t n = end ? (size_t)(end - start) : strlen(start);
        char *r = malloc(n + 1);
        memcpy(r, start, n); r[n] = 0;
        return r;
    } else {
        const char *p = s;
        int cur = 1;
        while (cur < k) {
            while (*p && !isspace((unsigned char)*p)) p++;
            while (*p &&  isspace((unsigned char)*p)) p++;
            cur++;
        }
        const char *end = p;
        while (*end && !isspace((unsigned char)*end)) end++;
        size_t n = (size_t)(end - p);
        char *r = malloc(n + 1);
        memcpy(r, p, n); r[n] = 0;
        return r;
    }
}

static int cmp_lines(const void *pa, const void *pb) {
    const char *a = *(const char **)pa;
    const char *b = *(const char **)pb;
    char *fa = NULL, *fb = NULL;

    if (sort_field > 0) {
        fa = field_dup(a, sort_field);
        fb = field_dup(b, sort_field);
        a = fa; b = fb;
    }

    int r;
    if (sort_numeric) {
        double da = strtod(a, NULL), db = strtod(b, NULL);
        r = (da < db) ? -1 : (da > db) ? 1 : 0;
    } else {
        r = sort_fold ? strcasecmp(a, b) : strcmp(a, b);
    }

    free(fa); free(fb);
    return sort_reverse ? -r : r;
}

static int cmd_sort(int argc, char **argv) {
    int opt_show = 0;
    const char *outfile = NULL;
    strvec files = {0};

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if      (!strcmp(a, "-n"))                      sort_numeric = 1;
        else if (!strcmp(a, "-r"))                      sort_reverse = 1;
        else if (!strcmp(a, "-u"))                      sort_unique  = 1;
        else if (!strcmp(a, "-f"))                      sort_fold    = 1;
        else if (!strcmp(a, "-s") || !strcmp(a, "--show")) opt_show = 1;
        else if (!strcmp(a, "-k")) { if (++i < argc) sort_field = atoi(argv[i]); }
        else if (!strcmp(a, "-t")) { if (++i < argc) sort_sep   = argv[i][0]; }
        else if (!strcmp(a, "-o")) { if (++i < argc) outfile = argv[i]; }
        else if (!strcmp(a, "--")) { for (i++; i < argc; i++) sv_push(&files, strdup(argv[i])); break; }
        else if (a[0] == '-' && a[1]) { /* Ignore unknown options */ }
        else sv_push(&files, strdup(a));
    }

    strvec lines = {0};
    if (files.n == 0) read_lines_from(stdin, &lines);
    else for (int k = 0; k < files.n; k++) {
        FILE *f = open_input(files.v[k]);
        if (!f) continue;
        read_lines_from(f, &lines);
        if (f != stdin) fclose(f);
    }

    strvec orig = {0};
    if (opt_show)
        for (int k = 0; k < lines.n; k++) sv_push(&orig, strdup(lines.v[k]));

    qsort(lines.v, lines.n, sizeof(char *), cmp_lines);

    if (sort_unique) {
        int w = 0;
        for (int k = 0; k < lines.n; k++) {
            if (w == 0 || cmp_lines(&lines.v[k], &lines.v[w-1]) != 0) {
                if (w != k) { free(lines.v[w]); lines.v[w] = lines.v[k]; lines.v[k] = NULL; }
                w++;
            } else { free(lines.v[k]); lines.v[k] = NULL; }
        }
        lines.n = w;
    }

    if (opt_show) print_diff(&orig, &lines);

    if (outfile) write_lines_to(outfile, &lines);
    else if (!opt_show)
        for (int k = 0; k < lines.n; k++) { fputs(lines.v[k], stdout); putchar('\n'); }

    sv_free(&orig);
    sv_free(&lines);
    sv_free(&files);
    return 0;
}

/* Applet dispatch */

int main(int argc, char **argv) {
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    if (!strcmp(prog, "grep")) return cmd_grep(argc, argv);
    if (!strcmp(prog, "sed"))  return cmd_sed(argc, argv);
    if (!strcmp(prog, "sort")) return cmd_sort(argc, argv);

    fprintf(stderr,
        "texttools: unknown applet '%s'\n"
        "applets: grep, sed, sort\n", prog);
    return 127;
}
