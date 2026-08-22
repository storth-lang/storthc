#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <limits.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <dirent.h>

#define PORT 8080
#define MAX_EVENTS 64
#define REQUEST_MAX 16384
#define PATH_MAX_SAFE 4096
#define FILE_MAX_SIZE (32UL * 1024UL * 1024UL)

static char ROOT[PATH_MAX];

static int make_socket_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

static int url_decode(char *dst, size_t dst_size, const char *src) {
    size_t j = 0;

    while (*src) {
        unsigned char c;

        if (*src == '%') {
            if (!src[1] || !src[2])
                return -1;

            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);

            if (hi < 0 || lo < 0)
                return -1;

            c = (unsigned char)((hi << 4) | lo);
            src += 3;
        } else {
            c = (unsigned char)*src++;
        }

        if (j + 1 >= dst_size)
            return -1;

        dst[j++] = (char)c;
    }

    dst[j] = '\0';
    return 0;
}

static int path_is_safe(const char *path) {
    if (!path || path[0] != '/')
        return 0;

    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;

        if (!*p)
            break;

        const char *start = p;

        while (*p && *p != '/')
            p++;

        size_t len = (size_t)(p - start);

        if ((len == 1 && start[0] == '.') || (len == 2 && start[0] == '.' && start[1] == '.') ||
            memchr(start, '\\', len) != NULL)
            return 0;
    }

    return 1;
}

static void html_escape(FILE *out, const char *s) {
    while (*s) {
        switch (*s) {
            case '&':
                fputs("&amp;", out);
                break;
            case '<':
                fputs("&lt;", out);
                break;
            case '>':
                fputs("&gt;", out);
                break;
            case '"':
                fputs("&quot;", out);
                break;
            case '\'':
                fputs("&#39;", out);
                break;
            default:
                fputc((unsigned char)*s, out);
                break;
        }

        s++;
    }
}

static const char *storth_keywords[] = { "fn",        "pub",    "struct",  "enum",  "enum_flag",
                                         "tag_union", "extern", "using",   "if",    "else",
                                         "while",     "for",    "return",  "break", "continue",
                                         "defer",     "case",   "default", "goto",  "label",
                                         "then",      "static", "const",   NULL };

static const char *storth_types[] = { "i8",   "i16",  "i32",    "i64", "u8",   "u16",
                                      "u32",  "u64",  "f32",    "f64", "f128", "bool",
                                      "char", "void", "string", "any", NULL };

static int is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static int match_word_list(const char *word, size_t len, const char **list) {
    for (int i = 0; list[i]; i++) {
        if (strlen(list[i]) == len && strncmp(word, list[i], len) == 0)
            return 1;
    }
    return 0;
}

static void emit_escaped_span(FILE *out, const char *cls, const char *text, size_t len) {
    if (cls)
        fprintf(out, "<span class=\"%s\">", cls);

    for (size_t i = 0; i < len; i++) {
        switch (text[i]) {
            case '&':
                fputs("&amp;", out);
                break;
            case '<':
                fputs("&lt;", out);
                break;
            case '>':
                fputs("&gt;", out);
                break;
            default:
                fputc((unsigned char)text[i], out);
                break;
        }
    }

    if (cls)
        fputs("</span>", out);
}

static void highlight_storth_line(FILE *out, const char *line) {
    size_t len = strlen(line);
    size_t i = 0;

    while (i < len) {
        if (line[i] == '/' && i + 1 < len && line[i + 1] == '/') {
            emit_escaped_span(out, "tok-com", line + i, len - i);
            return;
        }

        if (line[i] == '"') {
            size_t start = i;
            i++;

            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len)
                    i++;
                i++;
            }

            if (i < len)
                i++;

            emit_escaped_span(out, "tok-str", line + start, i - start);
            continue;
        }

        if (line[i] == '\'') {
            size_t start = i;
            i++;

            while (i < len && line[i] != '\'') {
                if (line[i] == '\\' && i + 1 < len)
                    i++;
                i++;
            }

            if (i < len)
                i++;

            emit_escaped_span(out, "tok-str", line + start, i - start);
            continue;
        }

        if (line[i] == '#' && i + 1 < len && is_ident_start(line[i + 1])) {
            size_t start = i;
            i++;

            while (i < len && is_ident_char(line[i]))
                i++;

            emit_escaped_span(out, "tok-comptime", line + start, i - start);
            continue;
        }

        if (isdigit((unsigned char)line[i])) {
            size_t start = i;

            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '.' || line[i] == '_'))
                i++;

            emit_escaped_span(out, "tok-num", line + start, i - start);
            continue;
        }

        if (is_ident_start(line[i])) {
            size_t start = i;

            while (i < len && is_ident_char(line[i]))
                i++;

            size_t wlen = i - start;

            if (match_word_list(line + start, wlen, storth_keywords))
                emit_escaped_span(out, "tok-kw", line + start, wlen);
            else if (match_word_list(line + start, wlen, storth_types))
                emit_escaped_span(out, "tok-type", line + start, wlen);
            else
                emit_escaped_span(out, NULL, line + start, wlen);

            continue;
        }

        emit_escaped_span(out, NULL, line + i, 1);
        i++;
    }
}

static int send_all(int fd, const void *buffer, size_t length) {
    const char *p = buffer;

    while (length > 0) {
        ssize_t sent = send(fd, p, length, MSG_NOSIGNAL);

        if (sent > 0) {
            p += sent;
            length -= (size_t)sent;
            continue;
        }

        if (sent < 0 && errno == EINTR)
            continue;

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {

            struct pollfd pfd;
            memset(&pfd, 0, sizeof(pfd));

            pfd.fd = fd;
            pfd.events = POLLOUT;

            int result;

            do {
                result = poll(&pfd, 1, 5000);
            } while (result < 0 && errno == EINTR);

            if (result <= 0)
                return -1;

            continue;
        }

        return -1;
    }

    return 0;
}

static void parse_inline_elements(const char *src, FILE *out) {
    size_t len = strlen(src);
    size_t i = 0;
    int bold = 0;
    int code = 0;

    while (i < len) {
        if (i + 1 < len && src[i] == '*' && src[i + 1] == '*') {

            fputs(bold ? "</strong>" : "<strong>", out);
            bold = !bold;
            i += 2;
            continue;
        }

        if (src[i] == '`') {
            fputs(code ? "</code>" : "<code>", out);
            code = !code;
            i++;
            continue;
        }

        if (src[i] == '[') {
            const char *close_bracket = strchr(src + i + 1, ']');

            if (close_bracket && close_bracket[1] == '(') {

                const char *close_paren = strchr(close_bracket + 2, ')');

                if (close_paren) {
                    size_t text_len = (size_t)(close_bracket - (src + i + 1));

                    size_t url_len = (size_t)(close_paren - (close_bracket + 2));

                    if (text_len <= 4096 && url_len <= 4096) {
                        char *text = malloc(text_len + 1);
                        char *url = malloc(url_len + 1);

                        if (text && url) {
                            memcpy(text, src + i + 1, text_len);

                            text[text_len] = '\0';

                            memcpy(url, close_bracket + 2, url_len);

                            url[url_len] = '\0';

                            char lower[4097];
                            size_t n = url_len < sizeof(lower) - 1 ? url_len : sizeof(lower) - 1;

                            for (size_t k = 0; k < n; k++) {
                                lower[k] = (char)tolower((unsigned char)url[k]);
                            }

                            lower[n] = '\0';

                            int dangerous = strncmp(lower, "javascript:", 11) == 0 ||
                                            strncmp(lower, "data:", 5) == 0 ||
                                            strncmp(lower, "vbscript:", 10) == 0;

                            if (!dangerous) {
                                fputs("<a href=\"", out);
                                html_escape(out, url);
                                fputs("\">", out);
                                html_escape(out, text);
                                fputs("</a>", out);

                                free(text);
                                free(url);

                                i = (size_t)(close_paren - src) + 1;

                                continue;
                            }
                        }

                        free(text);
                        free(url);
                    }
                }
            }
        }

        char one[2] = { src[i], '\0' };
        html_escape(out, one);
        i++;
    }

    if (bold)
        fputs("</strong>", out);

    if (code)
        fputs("</code>", out);
}

static int is_table_separator(const char *line) {
    const char *p = line;
    int found_pipe = 0;
    int found_dash = 0;

    while (*p) {
        if (*p == '|') {
            found_pipe = 1;
        } else if (*p == '-') {
            found_dash = 1;
        } else if (*p != ':' && *p != ' ' && *p != '\t') {
            return 0;
        }

        p++;
    }

    return found_pipe && found_dash;
}

static void render_table_row(const char *line, FILE *out, const char *tag) {
    char *copy = strdup(line);

    if (!copy)
        return;

    char *start = copy;

    while (*start == ' ' || *start == '\t')
        start++;

    if (*start == '|')
        start++;

    size_t len = strlen(start);

    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '|')) {
        start[--len] = '\0';
    }

    fputs("<tr>", out);

    char *saveptr = NULL;
    char *cell = strtok_r(start, "|", &saveptr);

    while (cell) {
        while (*cell == ' ' || *cell == '\t')
            cell++;

        char *end = cell + strlen(cell);

        while (end > cell && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }

        fprintf(out, "<%s>", tag);
        parse_inline_elements(cell, out);
        fprintf(out, "</%s>", tag);

        cell = strtok_r(NULL, "|", &saveptr);
    }

    fputs("</tr>\n", out);

    free(copy);
}

static void parse_markdown(const char *md, FILE *out) {
    char line[8192];
    int in_code = 0;
    int in_list = 0;
    int in_table = 0;

    const char *p = md;

    while (*p) {
        size_t len = 0;

        while (*p && *p != '\n') {
            if (len + 1 < sizeof(line))
                line[len++] = *p;

            p++;
        }

        if (*p == '\n')
            p++;

        line[len] = '\0';

        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        if (strncmp(line, "```", 3) == 0) {
            if (in_table) {
                fputs("</tbody></table>\n", out);
                in_table = 0;
            }

            if (in_list) {
                fputs("</ul>\n", out);
                in_list = 0;
            }

            if (in_code) {
                fputs("</code></pre>\n", out);
                in_code = 0;
            } else {
                fputs("<pre><code>", out);
                in_code = 1;
            }

            continue;
        }

        if (in_code) {
            highlight_storth_line(out, line);
            fputc('\n', out);
            continue;
        }

        if (len == 0) {
            if (in_table) {
                fputs("</tbody></table>\n", out);
                in_table = 0;
            }

            if (in_list) {
                fputs("</ul>\n", out);
                in_list = 0;
            }

            continue;
        }

        if (strcmp(line, "---") == 0 || strcmp(line, "***") == 0) {

            if (in_table) {
                fputs("</tbody></table>\n", out);
                in_table = 0;
            }

            if (in_list) {
                fputs("</ul>\n", out);
                in_list = 0;
            }

            fputs("<hr>\n", out);
            continue;
        }

        if (!in_table && strchr(line, '|') && p) {

            const char *next = p;
            char next_line[8192];
            size_t next_len = 0;

            while (*next && *next != '\n') {
                if (next_len + 1 < sizeof(next_line))
                    next_line[next_len++] = *next;

                next++;
            }

            next_line[next_len] = '\0';

            if (next_len > 0 && next_line[next_len - 1] == '\r')
                next_line[--next_len] = '\0';

            if (is_table_separator(next_line)) {
                if (in_list) {
                    fputs("</ul>\n", out);
                    in_list = 0;
                }

                fputs("<table><thead>", out);
                render_table_row(line, out, "th");
                fputs("</thead><tbody>\n", out);

                const char *skip = p;

                while (*skip && *skip != '\n')
                    skip++;

                if (*skip == '\n')
                    skip++;

                p = skip;
                in_table = 1;
                continue;
            }
        }

        if (in_table && strchr(line, '|')) {
            render_table_row(line, out, "td");
            continue;
        }

        if (in_table) {
            fputs("</tbody></table>\n", out);
            in_table = 0;
        }

        if (strncmp(line, "### ", 4) == 0) {
            if (in_list) {
                fputs("</ul>\n", out);
                in_list = 0;
            }

            fputs("<h3>", out);
            parse_inline_elements(line + 4, out);
            fputs("</h3>\n", out);

        } else if (strncmp(line, "## ", 3) == 0) {
            if (in_list) {
                fputs("</ul>\n", out);
                in_list = 0;
            }

            fputs("<h2>", out);
            parse_inline_elements(line + 3, out);
            fputs("</h2>\n", out);

        } else if (strncmp(line, "# ", 2) == 0) {
            if (in_list) {
                fputs("</ul>\n", out);
                in_list = 0;
            }

            fputs("<h1>", out);
            parse_inline_elements(line + 2, out);
            fputs("</h1>\n", out);

        } else if (strncmp(line, "* ", 2) == 0 || strncmp(line, "- ", 2) == 0) {

            if (!in_list) {
                fputs("<ul>\n", out);
                in_list = 1;
            }

            fputs("<li>", out);
            parse_inline_elements(line + 2, out);
            fputs("</li>\n", out);

        } else {
            if (in_list) {
                fputs("</ul>\n", out);
                in_list = 0;
            }

            fputs("<p>", out);
            parse_inline_elements(line, out);
            fputs("</p>\n", out);
        }
    }

    if (in_code)
        fputs("</code></pre>\n", out);

    if (in_table)
        fputs("</tbody></table>\n", out);

    if (in_list)
        fputs("</ul>\n", out);
}

static void render_page_start(FILE *out, const char *title) {
    fputs("<!doctype html>"
          "<html>"
          "<head>"
          "<meta charset=\"utf-8\">"
          "<meta name=\"viewport\" "
          "content=\"width=device-width,initial-scale=1\">"
          "<title>",
          out);

    html_escape(out, title);

    fputs("</title>"
          "<style>"
          "body{"
          "font-family:-apple-system,BlinkMacSystemFont,"
          "\"Segoe UI\",Roboto,Helvetica,Arial,sans-serif;"
          "max-width:900px;"
          "margin:40px auto;"
          "padding:0 24px;"
          "line-height:1.6;"
          "color:#24292f;"
          "background:#fff;"
          "}"
          "h1,h2,h3{"
          "line-height:1.25;"
          "margin-top:1.5em;"
          "}"
          "h1{"
          "font-size:2em;"
          "border-bottom:1px solid #d0d7de;"
          "padding-bottom:.3em;"
          "}"
          "h2{"
          "font-size:1.5em;"
          "border-bottom:1px solid #d0d7de;"
          "padding-bottom:.3em;"
          "}"
          "a{"
          "color:#0969da;"
          "text-decoration:none;"
          "}"
          "a:hover{"
          "text-decoration:underline;"
          "}"
          "code{"
          "font-family:ui-monospace,SFMono-Regular,Menlo,"
          "Monaco,Consolas,\"Liberation Mono\",\"Courier New\",monospace;"
          "background:#f6f8fa;"
          "padding:.2em .4em;"
          "border-radius:6px;"
          "font-size:85%;"
          "}"
          "pre{"
          "background:#062329;"
          "color:#d1b897;"
          "padding:16px;"
          "overflow:auto;"
          "border-radius:6px;"
          "line-height:1.45;"
          "}"
          "pre code{"
          "background:none;"
          "padding:0;"
          "font-size:100%;"
          "color:inherit;"
          "}"
          ".tok-kw{color:#ffffff;}"
          ".tok-type{color:#8cde94;}"
          ".tok-comptime{color:#8cde94;}"
          ".tok-str{color:#2ec09c;}"
          ".tok-com{color:#44b340;}"
          ".tok-num{color:#7ad0c6;}"
          "blockquote{"
          "margin:1em 0;"
          "padding:0 1em;"
          "color:#57606a;"
          "border-left:4px solid #d0d7de;"
          "}"
          "table{"
          "border-collapse:collapse;"
          "width:100%;"
          "margin:1em 0;"
          "}"
          "th,td{"
          "border:1px solid #d0d7de;"
          "padding:6px 13px;"
          "text-align:left;"
          "}"
          "th{"
          "font-weight:600;"
          "}"
          "tr:nth-child(even){"
          "background:#f6f8fa;"
          "}"
          "hr{"
          "height:1px;"
          "border:0;"
          "background:#d0d7de;"
          "margin:24px 0;"
          "}"
          "img{"
          "max-width:100%;"
          "}"
          "body.dark{"
          "background:#062329;"
          "color:#d1b897;"
          "}"
          "body.dark h1,body.dark h2{"
          "border-bottom-color:#126367;"
          "}"
          "body.dark a{"
          "color:#7ad0c6;"
          "}"
          "body.dark code{"
          "background:#0b3335;"
          "color:#d1b897;"
          "}"
          "body.dark pre{"
          "background:#0b3335;"
          "}"
          "body.dark blockquote{"
          "color:#a1efe4;"
          "border-left-color:#126367;"
          "}"
          "body.dark th,body.dark td{"
          "border-color:#126367;"
          "}"
          "body.dark tr:nth-child(even){"
          "background:#0b3335;"
          "}"
          "body.dark hr{"
          "background:#126367;"
          "}"
          "#theme-toggle{"
          "position:fixed;"
          "top:12px;"
          "right:12px;"
          "background:none;"
          "border:1px solid currentColor;"
          "border-radius:6px;"
          "padding:4px 10px;"
          "cursor:pointer;"
          "font-size:.85em;"
          "color:inherit;"
          "}"
          "</style>"
          "</head>"
          "<body>"
          "<script>if(localStorage.getItem('theme')==='dark')"
          "document.body.classList.add('dark');</script>",
          out);
}

static void render_page_end(FILE *out) {
    fputs("<button id=\"theme-toggle\" onclick=\""
          "document.body.classList.toggle('dark');"
          "localStorage.setItem('theme',"
          "document.body.classList.contains('dark')?'dark':'light');"
          "\">&#127769;</button>"
          "</body></html>\n",
          out);
}

static void render_breadcrumbs(const char *web_path, FILE *out) {
    fputs("<p><a href=\"/\">Home</a>", out);

    char copy[PATH_MAX_SAFE];

    strncpy(copy, web_path, sizeof(copy) - 1);

    copy[sizeof(copy) - 1] = '\0';

    char accumulated[PATH_MAX_SAFE] = "";

    char *saveptr = NULL;
    char *token = strtok_r(copy, "/", &saveptr);

    while (token) {
        if (*token) {
            size_t used = strlen(accumulated);

            if (used + strlen(token) + 2 < sizeof(accumulated)) {

                strcat(accumulated, "/");
                strcat(accumulated, token);

                fputs(" / <a href=\"", out);
                html_escape(out, accumulated);
                fputs("/\">", out);
                html_escape(out, token);
                fputs("</a>", out);
            }
        }

        token = strtok_r(NULL, "/", &saveptr);
    }

    fputs("</p>", out);
}

static int compare_entries(const void *a, const void *b) {
    const char *name_a = *(const char *const *)a;

    const char *name_b = *(const char *const *)b;

    return strcmp(name_a, name_b);
}

static void render_directory_listing(const char *dir_path, const char *web_path, FILE *out) {
    DIR *dir = opendir(dir_path);

    if (!dir) {
        fputs("<h1>Error opening directory</h1>", out);

        return;
    }

    render_breadcrumbs(web_path, out);

    fputs("<ul>", out);

    if (strcmp(web_path, "/") != 0)
        fputs("<li><a href=\"..\">..</a></li>\n", out);

    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || name[0] == '.')
            continue;

        char full_path[PATH_MAX_SAFE];

        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);

        if (n < 0 || (size_t)n >= sizeof(full_path))
            continue;

        struct stat st;

        if (stat(full_path, &st) != 0)
            continue;

        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
            continue;

        if (S_ISREG(st.st_mode)) {
            size_t len = strlen(name);

            if (len < 3 || strcmp(name + len - 3, ".md") != 0)
                continue;
        }

        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 32 : capacity * 2;

            char **new_names = realloc(names, new_capacity * sizeof(*names));

            if (!new_names)
                break;

            names = new_names;
            capacity = new_capacity;
        }

        names[count] = strdup(name);

        if (!names[count])
            break;

        count++;
    }

    closedir(dir);

    qsort(names, count, sizeof(*names), compare_entries);

    for (size_t i = 0; i < count; i++) {
        char full_path[PATH_MAX_SAFE];

        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, names[i]);

        if (n >= 0 && (size_t)n < sizeof(full_path)) {

            struct stat st;

            if (stat(full_path, &st) == 0) {
                fputs("<li><a href=\"", out);

                html_escape(out, names[i]);

                if (S_ISDIR(st.st_mode))
                    fputs("/\">", out);
                else
                    fputs("\">", out);

                html_escape(out, names[i]);

                if (S_ISDIR(st.st_mode))
                    fputs("/", out);

                fputs("</a></li>\n", out);
            }
        }

        free(names[i]);
    }

    free(names);

    fputs("</ul>", out);
}

static char *read_file(const char *path, size_t *out_size) {
    *out_size = 0;

    FILE *f = fopen(path, "rb");

    if (!f)
        return NULL;

    struct stat st;

    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (unsigned long long)st.st_size > FILE_MAX_SIZE) {

        fclose(f);
        return NULL;
    }

    size_t size = (size_t)st.st_size;

    char *data = malloc(size + 1);

    if (!data) {
        fclose(f);
        return NULL;
    }

    size_t total = 0;

    while (total < size) {
        size_t n = fread(data + total, 1, size - total, f);

        if (n > 0) {
            total += n;
            continue;
        }

        if (ferror(f)) {
            free(data);
            fclose(f);
            return NULL;
        }

        if (feof(f))
            break;
    }

    fclose(f);

    data[total] = '\0';
    *out_size = total;

    return data;
}

static int try_serve_index_html(int client_fd, const char *method, const char *dir_path) {
    char index_path[PATH_MAX_SAFE];

    int n = snprintf(index_path, sizeof(index_path), "%s/index.html", dir_path);

    if (n < 0 || (size_t)n >= sizeof(index_path))
        return 0;

    struct stat st;

    if (stat(index_path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;

    size_t size = 0;
    char *data = read_file(index_path, &size);

    if (!data)
        return 0;

    char header[1024];

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "\r\n",
                 size);

    if (n < 0 || (size_t)n >= sizeof(header)) {
        free(data);
        return 1;
    }

    send_all(client_fd, header, (size_t)n);

    if (strcmp(method, "HEAD") != 0 && size > 0)
        send_all(client_fd, data, size);

    free(data);
    return 1;
}

static int send_simple_response(int fd, int status, const char *reason, const char *content_type,
                                const char *body) {
    size_t body_len = body ? strlen(body) : 0;

    char header[1024];

    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "\r\n",
                     status, reason, content_type, body_len);

    if (n < 0 || (size_t)n >= sizeof(header))
        return -1;

    if (send_all(fd, header, (size_t)n) != 0)
        return -1;

    if (body_len > 0)
        return send_all(fd, body, body_len);

    return 0;
}

static int read_request(int fd, char *buffer, size_t buffer_size) {
    size_t used = 0;

    while (used + 1 < buffer_size) {
        ssize_t n = recv(fd, buffer + used, buffer_size - used - 1, 0);

        if (n > 0) {
            used += (size_t)n;
            buffer[used] = '\0';

            if (strstr(buffer, "\r\n\r\n"))
                return 0;

            continue;
        }

        if (n == 0)
            return -1;

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK) {

            struct pollfd pfd;

            memset(&pfd, 0, sizeof(pfd));

            pfd.fd = fd;
            pfd.events = POLLIN;

            int result;

            do {
                result = poll(&pfd, 1, 5000);
            } while (result < 0 && errno == EINTR);

            if (result <= 0)
                return -1;

            continue;
        }

        return -1;
    }

    return -1;
}

static int process_request(int client_fd) {
    char request[REQUEST_MAX];

    if (read_request(client_fd, request, sizeof(request)) != 0)
        return -1;

    char method[16];
    char raw_target[PATH_MAX_SAFE];

    memset(method, 0, sizeof(method));
    memset(raw_target, 0, sizeof(raw_target));

    if (sscanf(request, "%15s %4095s", method, raw_target) != 2) {

        send_simple_response(client_fd, 400, "Bad Request", "text/html; charset=utf-8",
                             "<h1>400 Bad Request</h1>");

        return -1;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {

        send_simple_response(client_fd, 405, "Method Not Allowed", "text/html; charset=utf-8",
                             "<h1>405 Method Not Allowed</h1>");

        return -1;
    }

    char target[PATH_MAX_SAFE];

    strncpy(target, raw_target, sizeof(target) - 1);

    target[sizeof(target) - 1] = '\0';

    char *query = strchr(target, '?');

    if (query)
        *query = '\0';

    char *fragment = strchr(target, '#');

    if (fragment)
        *fragment = '\0';

    if (target[0] == '\0')
        strcpy(target, "/");

    char decoded_path[PATH_MAX_SAFE];

    if (url_decode(decoded_path, sizeof(decoded_path), target) != 0) {

        send_simple_response(client_fd, 400, "Bad Request", "text/html; charset=utf-8",
                             "<h1>400 Bad Request</h1>");

        return -1;
    }

    if (!path_is_safe(decoded_path)) {
        send_simple_response(client_fd, 403, "Forbidden", "text/html; charset=utf-8",
                             "<h1>403 Forbidden</h1>");

        return -1;
    }

    size_t path_len = strlen(decoded_path);

    int requested_directory_slash =
        strcmp(decoded_path, "/") == 0 || (path_len > 0 && decoded_path[path_len - 1] == '/');

    char candidate[PATH_MAX_SAFE];

    int n = snprintf(candidate, sizeof(candidate), "%s%s", ROOT, decoded_path);

    if (n < 0 || (size_t)n >= sizeof(candidate)) {

        send_simple_response(client_fd, 414, "URI Too Long", "text/html; charset=utf-8",
                             "<h1>414 URI Too Long</h1>");

        return -1;
    }

    char root_real[PATH_MAX];
    char path_real[PATH_MAX];

    if (!realpath(ROOT, root_real) || !realpath(candidate, path_real)) {

        send_simple_response(client_fd, 404, "Not Found", "text/html; charset=utf-8",
                             "<h1>404 Not Found</h1>");

        return -1;
    }

    size_t root_len = strlen(root_real);

    if (strncmp(path_real, root_real, root_len) != 0 ||
        (path_real[root_len] != '\0' && path_real[root_len] != '/')) {

        send_simple_response(client_fd, 403, "Forbidden", "text/html; charset=utf-8",
                             "<h1>403 Forbidden</h1>");

        return -1;
    }

    struct stat st;

    if (stat(path_real, &st) != 0) {
        send_simple_response(client_fd, 404, "Not Found", "text/html; charset=utf-8",
                             "<h1>404 Not Found</h1>");

        return -1;
    }

    if (S_ISDIR(st.st_mode) && !requested_directory_slash) {

        char location[PATH_MAX_SAFE + 32];

        n = snprintf(location, sizeof(location), "%s/", decoded_path);

        if (n < 0 || (size_t)n >= sizeof(location))
            return -1;

        char header[PATH_MAX_SAFE + 256];

        n = snprintf(header, sizeof(header),
                     "HTTP/1.1 301 Moved Permanently\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     location);

        if (n < 0 || (size_t)n >= sizeof(header))
            return -1;

        send_all(client_fd, header, (size_t)n);

        return -1;
    }

    if (S_ISDIR(st.st_mode) && try_serve_index_html(client_fd, method, path_real))
        return 0;

    char *response_buf = NULL;
    size_t response_size = 0;

    FILE *mem = open_memstream(&response_buf, &response_size);

    if (!mem) {
        send_simple_response(client_fd, 500, "Internal Server Error", "text/html; charset=utf-8",
                             "<h1>500 Internal Server Error</h1>");

        return -1;
    }

    render_page_start(mem, S_ISDIR(st.st_mode) ? "Markdown Server" : decoded_path);

    if (S_ISDIR(st.st_mode)) {
        render_directory_listing(path_real, decoded_path, mem);
    } else if (S_ISREG(st.st_mode)) {
        size_t name_len = strlen(path_real);

        if (name_len < 3 || strcmp(path_real + name_len - 3, ".md") != 0) {

            fclose(mem);
            free(response_buf);

            send_simple_response(client_fd, 404, "Not Found", "text/html; charset=utf-8",
                                 "<h1>404 Not Found</h1>");

            return -1;
        }

        fputs("<p><a href=\"./\">Back to Directory</a></p>", mem);

        size_t md_size = 0;

        char *md = read_file(path_real, &md_size);

        if (!md) {
            fclose(mem);
            free(response_buf);

            send_simple_response(client_fd, 500, "Internal Server Error",
                                 "text/html; charset=utf-8",
                                 "<h1>Unable to read markdown file</h1>");

            return -1;
        }

        parse_markdown(md, mem);

        free(md);
    } else {
        fclose(mem);
        free(response_buf);

        send_simple_response(client_fd, 404, "Not Found", "text/html; charset=utf-8",
                             "<h1>404 Not Found</h1>");

        return -1;
    }

    render_page_end(mem);

    if (fclose(mem) != 0) {
        free(response_buf);
        return -1;
    }

    if (!response_buf) {
        response_buf = malloc(1);

        if (!response_buf)
            return -1;

        response_buf[0] = '\0';
        response_size = 0;
    }

    char header[1024];

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "\r\n",
                 response_size);

    if (n < 0 || (size_t)n >= sizeof(header)) {

        free(response_buf);
        return -1;
    }

    if (send_all(client_fd, header, (size_t)n) != 0) {

        free(response_buf);
        return -1;
    }

    if (strcmp(method, "HEAD") != 0 && response_size > 0) {

        if (send_all(client_fd, response_buf, response_size) != 0) {

            free(response_buf);
            return -1;
        }
    }

    free(response_buf);

    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    if (!realpath(".", ROOT)) {
        perror("realpath");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {

        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (make_socket_non_blocking(server_fd) < 0) {
        perror("fcntl");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {

        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    int epoll_fd = epoll_create1(0);

    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event server_event;

    memset(&server_event, 0, sizeof(server_event));

    server_event.events = EPOLLIN | EPOLLET;
    server_event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &server_event) < 0) {

        perror("epoll_ctl");
        close(epoll_fd);
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Markdown server running at "
           "http://127.0.0.1:%d/\n",
           PORT);

    printf("Serving directory: %s\n", ROOT);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (count < 0) {
            if (errno == EINTR)
                continue;

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < count; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                for (;;) {
                    int client_fd = accept(server_fd, NULL, NULL);

                    if (client_fd < 0) {
                        if (errno == EINTR)
                            continue;

                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;

                        perror("accept");
                        break;
                    }

                    if (make_socket_non_blocking(client_fd) < 0) {
                        close(client_fd);
                        continue;
                    }

                    struct epoll_event client_event;

                    memset(&client_event, 0, sizeof(client_event));

                    client_event.events = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;

                    client_event.data.fd = client_fd;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) < 0) {

                        close(client_fd);
                        continue;
                    }
                }

                continue;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {

                close(fd);
                continue;
            }

            process_request(fd);

            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);

            close(fd);
        }
    }

    close(epoll_fd);
    close(server_fd);

    return EXIT_SUCCESS;
}
