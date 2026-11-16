/*
 * common.c - Shared protocol helpers for the RPSLS client and server.
 *
 * CS 457 Final Project - Rock-Paper-Scissors-Lizard-Spock
 * Author: Nicholas Schindler
 *
 * See common.h for the module overview.
 */
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* ====================================================================== */
/* Game rules                                                             */
/* ====================================================================== */

const char *const SELECTION_NAMES[SEL_COUNT] = {
    "ROCK", "PAPER", "SCISSORS", "LIZARD", "SPOCK"
};

/*
 * BEATS[a][b] is 1 when selection a defeats selection b.
 * Each selection beats exactly two others.
 */
static const int BEATS[SEL_COUNT][SEL_COUNT] = {
    /*              ROCK PAPER SCISS LIZRD SPOCK */
    /* ROCK     */ { 0,   0,    1,    1,    0 },
    /* PAPER    */ { 1,   0,    0,    0,    1 },
    /* SCISSORS */ { 0,   1,    0,    1,    0 },
    /* LIZARD   */ { 0,   1,    0,    0,    1 },
    /* SPOCK    */ { 1,   0,    1,    0,    0 },
};

selection_t selection_from_name(const char *name)
{
    for (int i = 0; i < SEL_COUNT; i++) {
        if (strcmp(name, SELECTION_NAMES[i]) == 0) {
            return (selection_t)i;
        }
    }
    return SEL_NONE;
}

int selection_compare(selection_t a, selection_t b)
{
    if (BEATS[a][b]) {
        return 1;
    }
    if (BEATS[b][a]) {
        return -1;
    }
    return 0;
}

int parse_port(const char *text)
{
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < PORT_MIN || value > PORT_MAX) {
        return -1;
    }
    return (int)value;
}

/* ====================================================================== */
/* Stream framing                                                         */
/* ====================================================================== */

void line_reader_init(line_reader_t *lr, int fd)
{
    lr->fd = fd;
    lr->len = 0;
    lr->discarding = 0;
}

int line_reader_fill(line_reader_t *lr)
{
    ssize_t n;

    /*
     * If the buffer is full and holds no newline, line_reader_next() has
     * already flagged it as overlong and emptied it, so there is always
     * room here.
     */
    for (;;) {
        n = recv(lr->fd, lr->buf + lr->len, sizeof(lr->buf) - lr->len, 0);
        if (n > 0) {
            lr->len += (size_t)n;
            return LR_OK;
        }
        if (n == 0) {
            /*
             * TCP FIN. Any buffered bytes without a terminating '\n' are
             * an incomplete message and are discarded (protocol spec).
             */
            lr->len = 0;
            return LR_EOF;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return LR_TIMEOUT;
        }
        return LR_ERROR;
    }
}

int line_reader_next(line_reader_t *lr, char *out, size_t out_size)
{
    char  *nl;
    size_t line_len;

    for (;;) {
        nl = memchr(lr->buf, '\n', lr->len);

        if (nl == NULL) {
            /* No full line yet. Guard against a line that never ends. */
            if (lr->len >= MAX_MSG_LEN) {
                int was_discarding = lr->discarding;

                lr->len = 0;
                lr->discarding = 1;
                if (!was_discarding) {
                    return LR_TOOLONG;
                }
            }
            return LR_NONE;
        }

        line_len = (size_t)(nl - lr->buf);

        if (lr->discarding) {
            /* This newline ends the overlong line we were skipping. */
            lr->discarding = 0;
            memmove(lr->buf, nl + 1, lr->len - line_len - 1);
            lr->len -= line_len + 1;
            continue;
        }

        if (line_len > MAX_MSG_LEN || line_len + 1 > out_size) {
            memmove(lr->buf, nl + 1, lr->len - line_len - 1);
            lr->len -= line_len + 1;
            return LR_TOOLONG;
        }

        memcpy(out, lr->buf, line_len);
        out[line_len] = '\0';
        if (line_len > 0 && out[line_len - 1] == '\r') {
            out[line_len - 1] = '\0';   /* tolerate CRLF senders */
        }

        /* Shift the remaining bytes (start of the next message) down. */
        memmove(lr->buf, nl + 1, lr->len - line_len - 1);
        lr->len -= line_len + 1;
        return LR_LINE;
    }
}

/* ====================================================================== */
/* UTF-8 validation                                                       */
/* ====================================================================== */

int utf8_valid(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p) {
        int extra;
        unsigned int cp;

        if (*p < 0x80) {
            p++;
            continue;
        } else if ((*p & 0xE0) == 0xC0) {
            extra = 1;
            cp = *p & 0x1F;
        } else if ((*p & 0xF0) == 0xE0) {
            extra = 2;
            cp = *p & 0x0F;
        } else if ((*p & 0xF8) == 0xF0) {
            extra = 3;
            cp = *p & 0x07;
        } else {
            return 0;
        }
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (*p & 0x3F);
        }
        /* Reject overlong encodings, surrogates and out-of-range values. */
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }
    }
    return 1;
}

/* ====================================================================== */
/* JSON parsing                                                           */
/*                                                                        */
/* A small recursive-descent parser. It fully validates JSON syntax, and  */
/* can locate the value for a top-level key. Values are never copied into */
/* a tree; lookups re-scan the (short) message text, which keeps the code */
/* simple and allocation-free.                                            */
/* ====================================================================== */

#define JSON_MAX_DEPTH 16

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Append one code point as UTF-8 to out (bounded). */
static void put_utf8(unsigned int cp, char *out, size_t n, size_t *pos)
{
    char   tmp[4];
    size_t len;

    if (cp < 0x80) {
        tmp[0] = (char)cp;
        len = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    for (size_t i = 0; i < len; i++) {
        if (out != NULL && *pos + 1 < n) {
            out[*pos] = tmp[i];
        }
        (*pos)++;
    }
}

/* Read 4 hex digits at p. Returns the value or -1. */
static long read_hex4(const char *p)
{
    long v = 0;

    for (int i = 0; i < 4; i++) {
        int h = hex_value(p[i]);

        if (h < 0) {
            return -1;
        }
        v = (v << 4) | h;
    }
    return v;
}

/*
 * Parse a JSON string starting at the opening quote. If out is non-NULL
 * the unescaped contents are written there (truncated to n-1 bytes and
 * NUL-terminated). Returns a pointer just past the closing quote, or NULL
 * on a syntax error. *full_len receives the untruncated length.
 */
static const char *parse_string(const char *p, char *out, size_t n,
                                size_t *full_len)
{
    size_t pos = 0;

    if (*p != '"') {
        return NULL;
    }
    p++;
    while (*p != '"') {
        unsigned char c = (unsigned char)*p;

        if (c == '\0' || c < 0x20) {
            return NULL;   /* unterminated, or raw control character */
        }
        if (c == '\\') {
            p++;
            switch (*p) {
            case '"':  put_utf8('"', out, n, &pos);  break;
            case '\\': put_utf8('\\', out, n, &pos); break;
            case '/':  put_utf8('/', out, n, &pos);  break;
            case 'b':  put_utf8('\b', out, n, &pos); break;
            case 'f':  put_utf8('\f', out, n, &pos); break;
            case 'n':  put_utf8('\n', out, n, &pos); break;
            case 'r':  put_utf8('\r', out, n, &pos); break;
            case 't':  put_utf8('\t', out, n, &pos); break;
            case 'u': {
                long cp = read_hex4(p + 1);

                if (cp < 0) {
                    return NULL;
                }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* High surrogate: must be followed by a low one. */
                    long lo;

                    if (p[1] != '\\' || p[2] != 'u') {
                        return NULL;
                    }
                    lo = read_hex4(p + 3);
                    if (lo < 0xDC00 || lo > 0xDFFF) {
                        return NULL;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return NULL;   /* lone low surrogate */
                }
                put_utf8((unsigned int)cp, out, n, &pos);
                break;
            }
            default:
                return NULL;
            }
            p++;
        } else {
            if (out != NULL && pos + 1 < n) {
                out[pos] = (char)c;
            }
            pos++;
            p++;
        }
    }
    if (out != NULL && n > 0) {
        out[pos < n ? pos : n - 1] = '\0';
    }
    if (full_len != NULL) {
        *full_len = pos;
    }
    return p + 1;
}

/* Parse a JSON number. *is_int is set when it has no fraction/exponent. */
static const char *parse_number(const char *p, int *is_int)
{
    *is_int = 1;
    if (*p == '-') {
        p++;
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    } else {
        return NULL;
    }
    if (*p == '.') {
        *is_int = 0;
        p++;
        if (!(*p >= '0' && *p <= '9')) {
            return NULL;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        *is_int = 0;
        p++;
        if (*p == '+' || *p == '-') {
            p++;
        }
        if (!(*p >= '0' && *p <= '9')) {
            return NULL;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    return p;
}

static const char *parse_value(const char *p, json_type_t *type, int depth);

/* Parse an object or array body starting at '{' or '['. */
static const char *parse_container(const char *p, int depth)
{
    char close = (*p == '{') ? '}' : ']';
    int  is_obj = (*p == '{');
    json_type_t ignored;

    if (depth > JSON_MAX_DEPTH) {
        return NULL;
    }
    p = skip_ws(p + 1);
    if (*p == close) {
        return p + 1;
    }
    for (;;) {
        if (is_obj) {
            p = parse_string(p, NULL, 0, NULL);
            if (p == NULL) {
                return NULL;
            }
            p = skip_ws(p);
            if (*p != ':') {
                return NULL;
            }
            p = skip_ws(p + 1);
        }
        p = parse_value(p, &ignored, depth + 1);
        if (p == NULL) {
            return NULL;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == close) {
            return p + 1;
        }
        return NULL;
    }
}

/* Parse any JSON value; reports its type. Returns end pointer or NULL. */
static const char *parse_value(const char *p, json_type_t *type, int depth)
{
    int is_int;

    p = skip_ws(p);
    switch (*p) {
    case '"':
        *type = JT_STRING;
        return parse_string(p, NULL, 0, NULL);
    case '{':
        *type = JT_OBJECT;
        return parse_container(p, depth);
    case '[':
        *type = JT_ARRAY;
        return parse_container(p, depth);
    case 't':
        *type = JT_BOOL;
        return strncmp(p, "true", 4) == 0 ? p + 4 : NULL;
    case 'f':
        *type = JT_BOOL;
        return strncmp(p, "false", 5) == 0 ? p + 5 : NULL;
    case 'n':
        *type = JT_NULL;
        return strncmp(p, "null", 4) == 0 ? p + 4 : NULL;
    default:
        p = parse_number(p, &is_int);
        *type = is_int ? JT_INT : JT_NUMBER;
        return p;
    }
}

int json_is_object(const char *text)
{
    json_type_t type;
    const char *end;

    end = parse_value(text, &type, 0);
    if (end == NULL || type != JT_OBJECT) {
        return 0;
    }
    return *skip_ws(end) == '\0';
}

/*
 * Locate a top-level key in a (previously validated) JSON object.
 * On success returns a pointer to the start of the value, sets *type and
 * *end (one past the value). Returns NULL if the key is absent.
 */
static const char *json_find(const char *obj, const char *key,
                             json_type_t *type, const char **end)
{
    char        name[MAX_MSG_LEN];
    const char *p = skip_ws(obj);
    const char *val;

    if (*p != '{') {
        return NULL;
    }
    p = skip_ws(p + 1);
    while (*p == '"') {
        p = parse_string(p, name, sizeof(name), NULL);
        if (p == NULL) {
            return NULL;
        }
        p = skip_ws(p);
        if (*p != ':') {
            return NULL;
        }
        val = skip_ws(p + 1);
        p = parse_value(val, type, 1);
        if (p == NULL) {
            return NULL;
        }
        if (strcmp(name, key) == 0) {
            *end = p;
            return val;
        }
        p = skip_ws(p);
        if (*p != ',') {
            break;
        }
        p = skip_ws(p + 1);
    }
    return NULL;
}

json_type_t json_type_of(const char *obj, const char *key)
{
    json_type_t type;
    const char *end;

    return json_find(obj, key, &type, &end) ? type : JT_MISSING;
}

int json_get_string(const char *obj, const char *key, char *out, size_t n)
{
    json_type_t type;
    const char *end;
    const char *val = json_find(obj, key, &type, &end);

    if (val == NULL || type != JT_STRING) {
        return 0;
    }
    return parse_string(val, out, n, NULL) != NULL;
}

int json_get_long(const char *obj, const char *key, long *out)
{
    json_type_t type;
    const char *end;
    const char *val = json_find(obj, key, &type, &end);

    if (val == NULL || type != JT_INT) {
        return 0;
    }
    errno = 0;
    *out = strtol(val, NULL, 10);
    return errno == 0;
}

int json_get_object(const char *obj, const char *key, char *out, size_t n)
{
    json_type_t type;
    const char *end;
    const char *val = json_find(obj, key, &type, &end);
    size_t      len;

    if (val == NULL || type != JT_OBJECT) {
        return 0;
    }
    len = (size_t)(end - val);
    if (len + 1 > n) {
        return 0;
    }
    memcpy(out, val, len);
    out[len] = '\0';
    return 1;
}

void json_escape(const char *in, char *out, size_t n)
{
    size_t pos = 0;

    if (n == 0) {
        return;
    }
    for (; *in != '\0' && pos + 1 < n; in++) {
        unsigned char c = (unsigned char)*in;
        char          esc[8];
        const char   *piece = esc;

        if (c == '"') {
            piece = "\\\"";
        } else if (c == '\\') {
            piece = "\\\\";
        } else if (c == '\n') {
            piece = "\\n";
        } else if (c == '\r') {
            piece = "\\r";
        } else if (c == '\t') {
            piece = "\\t";
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
        } else {
            esc[0] = (char)c;
            esc[1] = '\0';
        }
        size_t plen = strlen(piece);

        if (pos + plen >= n) {
            break;
        }
        memcpy(out + pos, piece, plen);
        pos += plen;
    }
    out[pos] = '\0';
}

/* ====================================================================== */
/* Sending                                                                */
/* ====================================================================== */

int send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        /*
         * MSG_NOSIGNAL: a send to a peer that already closed returns
         * EPIPE instead of killing the process with SIGPIPE.
         */
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

int send_json(int fd, const char *fmt, ...)
{
    char    line[MAX_MSG_LEN + 2];
    va_list ap;
    int     len;

    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= sizeof(line) - 1) {
        return -1;
    }
    line[len++] = '\n';   /* framing delimiter */
    return send_all(fd, line, (size_t)len);
}
