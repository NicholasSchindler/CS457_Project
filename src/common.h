/*
 * common.h - Shared protocol helpers for the RPSLS client and server.
 *
 * CS 457 Final Project - Rock-Paper-Scissors-Lizard-Spock
 * Author: Nicholas Schindler
 *
 * Wire format (Sprint 1 protocol blueprint):
 *   - Transport:     TCP (AF_INET, SOCK_STREAM)
 *   - Serialization: JSON, UTF-8
 *   - Framing:       newline-delimited, exactly one JSON object per line
 *
 * This module provides:
 *   1. A line reader that turns an arbitrary TCP byte stream into complete
 *      '\n'-terminated messages (handles partial and coalesced recv() data).
 *   2. A small validating JSON parser for flat message objects.
 *   3. Helpers to serialize and send framed JSON messages.
 *   4. Game constants shared by client and server (selections, limits).
 *
 * Written against plain POSIX so it builds on Alpine Linux (musl libc).
 */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <sys/types.h>

/* ---------------------------------------------------------------------- */
/* Limits and protocol constants                                          */
/* ---------------------------------------------------------------------- */

#define MAX_MSG_LEN     1024   /* longest accepted line, excluding '\n'   */
#define MAX_ALIAS_LEN   32     /* longest accepted player alias (bytes)   */
#define WINS_TO_WIN     3      /* round wins needed to win the match      */
#define IDLE_TIMEOUT_S  60     /* idle receive timeout (protocol spec)    */
#define PORT_MIN        1024
#define PORT_MAX        65535

/* The five legal selections. SEL_NONE means "no move yet this round". */
typedef enum {
    SEL_NONE = -1,
    SEL_ROCK = 0,
    SEL_PAPER,
    SEL_SCISSORS,
    SEL_LIZARD,
    SEL_SPOCK,
    SEL_COUNT
} selection_t;

/* Wire names, indexed by selection_t. */
extern const char *const SELECTION_NAMES[SEL_COUNT];

/* Map a wire name ("ROCK") to a selection; returns SEL_NONE if invalid. */
selection_t selection_from_name(const char *name);

/*
 * Decide a round. Returns 1 if a beats b, -1 if b beats a, 0 for a draw.
 * Rules: Rock > Scissors, Lizard | Paper > Rock, Spock |
 *        Scissors > Paper, Lizard | Lizard > Paper, Spock |
 *        Spock > Rock, Scissors
 */
int selection_compare(selection_t a, selection_t b);

/* Parse and range-check a port string (1024-65535). Returns -1 if bad. */
int parse_port(const char *text);

/* ---------------------------------------------------------------------- */
/* Stream framing: newline-delimited line reader                          */
/* ---------------------------------------------------------------------- */

/* Result codes from line_reader_fill(). */
#define LR_OK        1    /* bytes were appended to the buffer           */
#define LR_EOF       0    /* peer closed the connection (TCP FIN)        */
#define LR_ERROR    -1    /* socket error (RST, abort, ...)              */
#define LR_TIMEOUT  -2    /* SO_RCVTIMEO expired with no data            */

/* Result codes from line_reader_next(). */
#define LR_LINE      1    /* a complete line was copied out              */
#define LR_NONE      0    /* no complete line buffered yet               */
#define LR_TOOLONG  -3    /* a line exceeded MAX_MSG_LEN and was dropped */

typedef struct {
    int    fd;
    char   buf[MAX_MSG_LEN * 2];
    size_t len;          /* bytes currently buffered                     */
    int    discarding;   /* 1 while skipping the rest of an overlong line */
} line_reader_t;

void line_reader_init(line_reader_t *lr, int fd);

/* Perform ONE recv() and append whatever arrives to the buffer. */
int line_reader_fill(line_reader_t *lr);

/*
 * Extract the next complete line (without '\n', and without a trailing
 * '\r' if present) into out. Never calls recv(); call it in a loop after
 * every line_reader_fill() because one recv() may hold several messages.
 */
int line_reader_next(line_reader_t *lr, char *out, size_t out_size);

/* ---------------------------------------------------------------------- */
/* Minimal JSON support                                                   */
/* ---------------------------------------------------------------------- */

typedef enum {
    JT_MISSING = 0,  /* key not present                               */
    JT_STRING,
    JT_INT,          /* number without fraction or exponent           */
    JT_NUMBER,       /* any other number                              */
    JT_NULL,
    JT_BOOL,
    JT_OBJECT,
    JT_ARRAY
} json_type_t;

/* Return 1 if text is valid UTF-8, else 0. */
int utf8_valid(const char *text);

/* Return 1 if text is exactly one well-formed JSON object, else 0. */
int json_is_object(const char *text);

/* Type of the value stored under a top-level key (JT_MISSING if absent). */
json_type_t json_type_of(const char *obj, const char *key);

/* Copy a top-level string value (unescaped) into out. 1 = ok, 0 = fail. */
int json_get_string(const char *obj, const char *key, char *out, size_t n);

/* Read a top-level integer value. 1 = ok, 0 = missing or not an int. */
int json_get_long(const char *obj, const char *key, long *out);

/*
 * Copy the raw text of a nested object value (e.g. "score") into out so
 * it can be queried with the functions above. 1 = ok, 0 = fail.
 */
int json_get_object(const char *obj, const char *key, char *out, size_t n);

/* Escape a C string for inclusion inside JSON double quotes. */
void json_escape(const char *in, char *out, size_t n);

/* ---------------------------------------------------------------------- */
/* Sending framed messages                                                */
/* ---------------------------------------------------------------------- */

/* send() the whole buffer, retrying on partial writes. 0 = ok, -1 = fail. */
int send_all(int fd, const char *data, size_t len);

/*
 * printf-style: format one JSON object, append '\n' and send it.
 * Returns 0 on success, -1 on failure.
 */
int send_json(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* COMMON_H */
