#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>   // terminal width
#include <openssl/ssl.h>
#include <openssl/err.h>

static int write_all_sock(int fd, SSL *ssl, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n;
        if (ssl) n = SSL_write(ssl, buf + off, (int)(len - off));
        else     n = write(fd, buf + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

// Send rendered data to localhost:1
static void render_to_gapi(const char *data, size_t len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fwrite(data, 1, len, stdout); return; }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(1);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) >= 0) {
        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "POST /render HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", len);
        send(fd, header, hlen, 0);
        send(fd, data, len, 0);

        char res[4096];
        ssize_t n;
        int is_body = 0;
        while ((n = recv(fd, res, sizeof(res), 0)) > 0) {
            if (!is_body) {
                res[n] = '\0';
                char *body = strstr(res, "\r\n\r\n");
                if (body) {
                    is_body = 1; body += 4;
                    int blen = n - (body - res);
                    if (blen > 0) fwrite(body, 1, blen, stdout);
                }
            } else {
                fwrite(res, 1, n, stdout);
            }
        }
        close(fd);
    } else {
        close(fd);
        fwrite(data, 1, len, stdout);
    }
}

// Format HTML: smart indent, wrap to terminal width, strip scripts
static void format_html(const char *input, size_t len) {
    struct winsize w;
    int term_cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        term_cols = w.ws_col;
    }

    size_t cap = len * 4 + 2048;
    char *out = malloc(cap);
    if (!out) { render_to_gapi(input, len); return; }
    size_t out_len = 0;

#define PUT(ch) do { \
    if (out_len + 1 >= cap) { \
        size_t ncap = cap * 2; \
        char *nb = realloc(out, ncap); \
        if (!nb) { free(out); render_to_gapi(input, len); return; } \
        out = nb; cap = ncap; \
    } \
    out[out_len++] = (char)(ch); \
} while (0)

    int indent = 0;
    int in_tag = 0;
    int skip_content = 0;
    int line_len = 0;

    for (size_t i = 0; i < len; i++) {
        char c = input[i];

        if (!in_tag && !skip_content) {
            if (i + 7 <= len && strncasecmp(input + i, "<script", 7) == 0) skip_content = 1;
            else if (i + 6 <= len && strncasecmp(input + i, "<style", 6) == 0) skip_content = 2;
        }
        if (skip_content == 1) {
            if (i + 9 <= len && strncasecmp(input + i, "</script>", 9) == 0) { skip_content = 0; i += 8; }
            continue;
        }
        if (skip_content == 2) {
            if (i + 8 <= len && strncasecmp(input + i, "</style>", 8) == 0) { skip_content = 0; i += 7; }
            continue;
        }

        if (c == '<') {
            if (out_len > 0 && out[out_len - 1] != '\n') {
                PUT('\n');
                line_len = 0;
            }

            int is_closing = (i + 1 < len && input[i + 1] == '/');
            if (is_closing && indent > 0) indent--;

            int spaces = indent * 2;
            if (spaces > 8) spaces = 8;
            for (int k = 0; k < spaces; k++) PUT(' ');
            line_len = spaces;

            in_tag = 1;
            PUT(c);
            line_len++;

            int self_closing = 0;
            if ((i + 3 <= len && strncasecmp(input + i, "<br", 3) == 0) ||
                (i + 4 <= len && strncasecmp(input + i, "<img", 4) == 0) ||
                (i + 5 <= len && strncasecmp(input + i, "<meta", 5) == 0) ||
                (i + 5 <= len && strncasecmp(input + i, "<link", 5) == 0) ||
                (i + 3 <= len && strncasecmp(input + i, "<hr", 3) == 0) ||
                (i + 6 <= len && strncasecmp(input + i, "<input", 6) == 0) ||
                (i + 9 <= len && strncasecmp(input + i, "<!DOCTYPE", 9) == 0)) {
                self_closing = 1;
            }
            if (!is_closing && !self_closing) indent++;

        } else if (c == '>') {
            PUT(c);
            PUT('\n');
            line_len = 0;
            in_tag = 0;
        } else if (in_tag) {
            if (line_len >= term_cols - 4 && isspace((unsigned char)c)) {
                PUT('\n');
                int next_indent = (indent * 2 > 8 ? 8 : indent * 2) + 2;
                for (int k = 0; k < next_indent; k++) PUT(' ');
                line_len = next_indent;
            } else {
                PUT(c);
                line_len++;
            }
        } else {
            if (isspace((unsigned char)c)) {
                if (out_len > 0 && out[out_len - 1] != '\n' && out[out_len - 1] != ' ') {
                    PUT(' ');
                    line_len++;
                }
            } else {
                if (line_len == 0) {
                    int spaces = indent * 2;
                    if (spaces > 8) spaces = 8;
                    for (int k = 0; k < spaces; k++) PUT(' ');
                    line_len = spaces;
                } else if (line_len >= term_cols - 5) {
                    PUT('\n');
                    int spaces = indent * 2;
                    if (spaces > 8) spaces = 8;
                    for (int k = 0; k < spaces; k++) PUT(' ');
                    line_len = spaces;
                }
                PUT(c);
                line_len++;
            }
        }
    }

#undef PUT

    render_to_gapi(out, out_len);
    free(out);
}

// Parse the URL
static void parse_url(const char *url, char *host, char *path, char *port, int *use_ssl) {
    snprintf(port, 16, "80");
    *use_ssl = 0;

    if (strncmp(url, "https://", 8) == 0) {
        *use_ssl = 1; snprintf(port, 16, "443"); url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    } else {
        *use_ssl = 1; snprintf(port, 16, "443");
    }

    const char *slash = strchr(url, '/');
    const char *host_end = slash ? slash : url + strlen(url);
    const char *host_start = url;
    const char *port_start = NULL;

    if (*url == '[') {
        const char *rb = memchr(url, ']', (size_t)(host_end - url));
        if (rb) {
            host_start = url + 1;
            const char *after = rb + 1;
            if (after < host_end && *after == ':') port_start = after + 1;
            host_end = rb;
        }
    } else {
        const char *colon = memchr(url, ':', (size_t)(host_end - url));
        if (colon && memchr(colon + 1, ':', (size_t)(host_end - colon - 1)) == NULL) {
            port_start = colon + 1;
            host_end = colon;
        }
    }

    size_t hlen = (size_t)(host_end - host_start);
    if (hlen >= 256) hlen = 255;
    memcpy(host, host_start, hlen);
    host[hlen] = '\0';

    if (port_start) {
        const char *p_end = slash ? slash : url + strlen(url);
        size_t plen = (size_t)(p_end - port_start);
        if (plen >= 16) plen = 15;
        memcpy(port, port_start, plen);
        port[plen] = '\0';
    }

    if (slash) snprintf(path, 1024, "%s", slash);
    else       snprintf(path, 1024, "/");
}

// Issue the request
static int do_request(const char *url, int redirect_count) {
    if (redirect_count > 5) {
        fprintf(stderr, "curl: Too many redirects\n");
        return 1;
    }

    char host[256] = {0}, path[1024] = {0}, port[16] = {0};
    int use_ssl = 0;
    parse_url(url, host, path, port, &use_ssl);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        fprintf(stderr, "curl: Could not resolve host: %s\n", host);
        return 1;
    }

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "curl: Failed to connect to %s\n", host);
        freeaddrinfo(res); close(sockfd); return 1;
    }
    freeaddrinfo(res);

    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;

    if (use_ssl) {
        SSL_library_init();
        SSL_load_error_strings();
        const SSL_METHOD *meth = TLS_client_method();
        ctx = SSL_CTX_new(meth);
        if (!ctx) {
            ERR_print_errors_fp(stderr);
            close(sockfd);
            return 1;
        }
        ssl = SSL_new(ctx);
        if (!ssl) {
            ERR_print_errors_fp(stderr);
            SSL_CTX_free(ctx);
            close(sockfd);
            return 1;
        }
        SSL_set_tlsext_host_name(ssl, host);
        SSL_set_fd(ssl, sockfd);
        
        if (SSL_connect(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            SSL_free(ssl); SSL_CTX_free(ctx); close(sockfd); return 1;
        }
    }

    char req[2048];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Gardenia-Curl/1.0\r\nConnection: close\r\n\r\n", path, host);
    
    if (write_all_sock(sockfd, use_ssl ? ssl : NULL, req, strlen(req)) < 0) {
        if (use_ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(sockfd);
        return 1;
    }

    char buf[4096];
    char headers[8192] = {0};
    size_t header_len = 0;
    char *body = malloc(2 * 1024 * 1024); /* 2MB */
    size_t body_len = 0;
    if (!body) {
        if (use_ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(sockfd);
        return 1;
    }

    int n;
    int header_done = 0;

    while (1) {
        if (use_ssl) n = SSL_read(ssl, buf, sizeof(buf) - 1);
        else n = read(sockfd, buf, sizeof(buf) - 1);

        if (n <= 0) break;
        buf[n] = '\0';

        if (!header_done) {
            size_t room = sizeof(headers) - 1 - header_len;
            size_t add = (size_t)n < room ? (size_t)n : room;
            if (add > 0) {
                memcpy(headers + header_len, buf, add);
                header_len += add;
                headers[header_len] = '\0';
            }
            char *split = strstr(headers, "\r\n\r\n");
            if (split) {
                header_done = 1;
                size_t h_len = (size_t)(split - headers);
                size_t blen = header_len - (h_len + 4);
                if (blen > 0 && body_len + blen < 2 * 1024 * 1024) {
                    memcpy(body + body_len, headers + h_len + 4, blen);
                    body_len += blen;
                }
                if (add < (size_t)n) {
                    size_t extra = (size_t)n - add;
                    if (body_len + extra < 2 * 1024 * 1024) {
                        memcpy(body + body_len, buf + add, extra);
                        body_len += extra;
                    }
                }
                headers[h_len] = '\0';
                header_len = h_len;
            }
        } else {
            if (body_len + (size_t)n < 2 * 1024 * 1024) {
                memcpy(body + body_len, buf, (size_t)n);
                body_len += (size_t)n;
            }
        }
    }

    if (use_ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
    close(sockfd);

    int ret = 0;

    // Handle redirects
    if (strstr(headers, "301 Moved") || strstr(headers, "302 Found") || strstr(headers, "308 Permanent Redirect")) {
        char *loc = strstr(headers, "Location: ");
        if (loc) {
            loc += 10;
            char *end = strstr(loc, "\r\n");
            if (end) *end = '\0';
            
            char next_url[1024];
            if (strncmp(loc, "http", 4) == 0) {
                strcpy(next_url, loc);
            } else if (strncmp(loc, "//", 2) == 0) {
                snprintf(next_url, sizeof(next_url), "%s:%s", use_ssl ? "https" : "http", loc);
            } else if (loc[0] == '/') {
                snprintf(next_url, sizeof(next_url), "%s://%s%s", use_ssl ? "https" : "http", host, loc);
            } else {
                snprintf(next_url, sizeof(next_url), "%s://%s/%s", use_ssl ? "https" : "http", host, loc);
            }
            ret = do_request(next_url, redirect_count + 1);
            free(body);
            return ret;
        }
    }

    // Handle chunked encoding and format
    int is_chunked = (strstr(headers, "Transfer-Encoding: chunked") != NULL || strstr(headers, "Transfer-Encoding: Chunked") != NULL);

    if (header_done && body_len > 0) {
        body[body_len] = '\0';
        if (is_chunked) {
            char *decoded = malloc(body_len + 1);
            size_t decoded_len = 0;
            if (decoded) {
                size_t i = 0;
                while (i < body_len) {
                    char *size_end = memchr(body + i, '\r', body_len - i);
                    if (!size_end) break;
                    long chunk_size = strtol(body + i, NULL, 16);
                    if (chunk_size <= 0) break;
                    i = (size_t)(size_end - body) + 2;
                    if (i + (size_t)chunk_size > body_len) break;
                    memcpy(decoded + decoded_len, body + i, (size_t)chunk_size);
                    decoded_len += (size_t)chunk_size;
                    i += (size_t)chunk_size;
                    if (i + 1 < body_len && body[i] == '\r' && body[i+1] == '\n') i += 2;
                }
                decoded[decoded_len] = '\0';
                format_html(decoded, decoded_len);
                free(decoded);
            } else {
                format_html(body, body_len);
            }
        } else {
            format_html(body, body_len);
        }
    }

    free(body);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: curl <url>\n");
        return 1;
    }
    return do_request(argv[1], 0);
}
