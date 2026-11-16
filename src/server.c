/*
 * server.c - RPSLS game server.
 *
 * CS 457 Final Project - Rock-Paper-Scissors-Lizard-Spock
 * Author: Nicholas Schindler
 *
 * Usage:
 *   ./server -h
 *   ./server -p 8080
 *
 * Concurrency architecture (Sprint 2, Option A: multi-threading):
 *
 *   main thread    socket() / bind(0.0.0.0) / listen(), then loops on
 *                  accept() and spawns one detached client thread per
 *                  connection.
 *
 *   client thread  Owns one TCP socket. Reads newline-framed JSON, runs the
 *                  protocol validation pipeline, and records the player's
 *                  join request or move in the shared game record. It never
 *                  advances the game itself; it just signals the game thread.
 *
 *   game thread    Dedicated thread that runs the Sprint 1 FSM. It sleeps on
 *                  a condition variable and wakes whenever a client thread
 *                  reports a join, a move, or a disconnect.
 *
 *   All shared state lives in one struct (g_game) protected by one mutex.
 *   The game thread holds that mutex for every transition, so client
 *   threads only ever observe the stable states (WAITING_FOR_PLAYERS,
 *   P1_CONNECTED, WAITING_FOR_MOVES). Each player's move slot starts at
 *   SEL_NONE; a client thread may only fill an empty slot (this is the
 *   turn-enforcement rule), and the game thread resets both slots at the
 *   start of every new round.
 *
 * FSM (docs/fsm_specification.md):
 *
 *   INIT -> WAITING_FOR_PLAYERS -> P1_CONNECTED -> GAME_START
 *        -> WAITING_FOR_MOVES <-> ERROR
 *        -> SCORING -> NEW_ROUND -> WAITING_FOR_MOVES ...
 *        -> GAME_OVER -> CLEANUP -> WAITING_FOR_PLAYERS
 */
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Types                                                                  */
/* ====================================================================== */

typedef enum {
    ST_INIT,
    ST_WAITING_FOR_PLAYERS,
    ST_P1_CONNECTED,
    ST_GAME_START,
    ST_WAITING_FOR_MOVES,
    ST_ERROR,
    ST_SCORING,
    ST_NEW_ROUND,
    ST_GAME_OVER,
    ST_CLEANUP
} game_state_t;

static const char *const STATE_NAMES[] = {
    "INIT", "WAITING_FOR_PLAYERS", "P1_CONNECTED", "GAME_START",
    "WAITING_FOR_MOVES", "ERROR", "SCORING", "NEW_ROUND", "GAME_OVER",
    "CLEANUP"
};

/* One accepted TCP connection; owned and freed by its client thread. */
typedef struct {
    int  fd;
    char peer[64];   /* "ip:port", for log messages */
} conn_t;

/* One player seat (index 0 = P1, index 1 = P2). */
typedef struct {
    conn_t     *conn;     /* NULL when the seat is empty or player left */
    int         left;     /* 1 once the seated player disconnected      */
    char        alias[MAX_ALIAS_LEN + 1];
    selection_t move;     /* shared move variable; SEL_NONE = not yet   */
    selection_t last;     /* move copied out by the game thread         */
    int         wins;
} seat_t;

/* All state shared between threads. Guarded by lock. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  changed;   /* signalled on join / move / disconnect */
    game_state_t    state;
    seat_t          seat[2];
    int             round;     /* 1-based round counter                 */
    int             draws;
    int             winner;    /* seat index of match winner, or -1     */
    int             forfeit;   /* 1 if the match ended by disconnect    */
} game_t;

static game_t g_game = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .state = ST_INIT,
};

/* ====================================================================== */
/* Logging                                                                */
/* ====================================================================== */

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/* Timestamped, thread-safe log line to stdout. */
static void log_msg(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void log_msg(const char *tag, const char *fmt, ...)
{
    char       stamp[16];
    time_t     now = time(NULL);
    struct tm  tm_now;
    va_list    ap;

    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_now);

    pthread_mutex_lock(&g_log_lock);
    printf("[%s] %-6s ", stamp, tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&g_log_lock);
}

/* ====================================================================== */
/* Messaging helpers (caller holds g_game.lock unless noted)              */
/* ====================================================================== */

/* Send one framed JSON message and log it. */
#define SEND(conn, ...)                                                   \
    do {                                                                  \
        char send_buf_[MAX_MSG_LEN];                                      \
        snprintf(send_buf_, sizeof(send_buf_), __VA_ARGS__);              \
        if (send_json((conn)->fd, "%s", send_buf_) == 0) {                \
            log_msg("SEND", "%s <- %s", (conn)->peer, send_buf_);         \
        } else {                                                          \
            log_msg("SEND", "%s <- FAILED (%s)", (conn)->peer,            \
                    strerror(errno));                                     \
        }                                                                 \
    } while (0)

static void send_error(conn_t *c, const char *code, const char *text)
{
    SEND(c, "{\"msg_type\":\"ERROR\",\"error_code\":\"%s\","
            "\"error_message\":\"%s\",\"timestamp\":%ld}",
         code, text, (long)time(NULL));
}

/* Recipient-relative score object for seat i. */
static void score_json(int i, char *out, size_t n)
{
    snprintf(out, n, "{\"wins\":%d,\"losses\":%d,\"draws\":%d}",
             g_game.seat[i].wins, g_game.seat[1 - i].wins, g_game.draws);
}

static void set_state(game_state_t next)
{
    log_msg("FSM", "%s -> %s", STATE_NAMES[g_game.state], STATE_NAMES[next]);
    g_game.state = next;
}

/* Find the seat index occupied by connection c, or -1. */
static int seat_of(const conn_t *c)
{
    for (int i = 0; i < 2; i++) {
        if (g_game.seat[i].conn == c) {
            return i;
        }
    }
    return -1;
}

static void reset_seat(seat_t *s)
{
    memset(s, 0, sizeof(*s));
    s->move = SEL_NONE;
    s->last = SEL_NONE;
}

static void reset_game(void)
{
    reset_seat(&g_game.seat[0]);
    reset_seat(&g_game.seat[1]);
    g_game.round = 0;
    g_game.draws = 0;
    g_game.winner = -1;
    g_game.forfeit = 0;
}

/* ====================================================================== */
/* Game thread: FSM engine                                                */
/* ====================================================================== */

/* Broadcast the personalized ROUND_RESULT for the round just scored. */
static void broadcast_round_result(int round_winner)
{
    for (int i = 0; i < 2; i++) {
        seat_t     *me = &g_game.seat[i];
        seat_t     *op = &g_game.seat[1 - i];
        const char *who;
        char        score[96];

        if (me->conn == NULL) {
            continue;
        }
        if (round_winner < 0) {
            who = "DRAW";
        } else {
            who = (round_winner == i) ? "YOU" : "OPPONENT";
        }
        score_json(i, score, sizeof(score));
        SEND(me->conn,
             "{\"msg_type\":\"STATE_UPDATE\",\"status\":\"ROUND_RESULT\","
             "\"your_selection\":\"%s\",\"opponent_selection\":\"%s\","
             "\"round_winner\":\"%s\",\"score\":%s,\"timestamp\":%ld}",
             SELECTION_NAMES[me->last], SELECTION_NAMES[op->last], who,
             score, (long)time(NULL));
    }
}

/*
 * Run one FSM step. Returns 1 if the state changed (so the caller should
 * loop again immediately) or 0 if the FSM must wait for a client event.
 * Called with g_game.lock held.
 */
static int fsm_step(void)
{
    seat_t *p1 = &g_game.seat[0];
    seat_t *p2 = &g_game.seat[1];

    switch (g_game.state) {
    case ST_INIT:
        reset_game();
        set_state(ST_WAITING_FOR_PLAYERS);
        return 1;

    case ST_WAITING_FOR_PLAYERS:
        /* A client thread seats the first CONNECT in seat 0. */
        if (p1->left) {
            reset_seat(p1);   /* joined and left before we got here */
            return 1;
        }
        if (p1->conn == NULL && p2->conn != NULL) {
            *p1 = *p2;        /* only seat 1 is filled: move it to P1 */
            reset_seat(p2);
        }
        if (p1->conn == NULL) {
            return 0;
        }
        log_msg("GAME", "P1 '%s' joined from %s", p1->alias, p1->conn->peer);
        SEND(p1->conn, "{\"msg_type\":\"LOBBY_WAIT\",\"timestamp\":%ld}",
             (long)time(NULL));
        set_state(ST_P1_CONNECTED);
        return 1;

    case ST_P1_CONNECTED:
        if (p1->left) {
            /* P1 left the lobby. Promote a waiting P2, else start over. */
            log_msg("GAME", "P1 '%s' left the lobby", p1->alias);
            if (p2->conn != NULL) {
                *p1 = *p2;
                reset_seat(p2);
                log_msg("GAME", "'%s' promoted to P1", p1->alias);
                SEND(p1->conn, "{\"msg_type\":\"LOBBY_WAIT\","
                               "\"timestamp\":%ld}", (long)time(NULL));
                return 1;
            }
            reset_seat(p1);
            set_state(ST_WAITING_FOR_PLAYERS);
            return 1;
        }
        if (p2->conn == NULL) {
            return 0;
        }
        log_msg("GAME", "P2 '%s' joined from %s", p2->alias, p2->conn->peer);
        set_state(ST_GAME_START);
        return 1;

    case ST_GAME_START:
        for (int i = 0; i < 2; i++) {
            char alias[MAX_ALIAS_LEN * 6 + 1];   /* worst-case escaping */

            json_escape(g_game.seat[1 - i].alias, alias, sizeof(alias));
            SEND(g_game.seat[i].conn,
                 "{\"msg_type\":\"GAME_START\",\"player_role\":\"P%d\","
                 "\"opponent_alias\":\"%s\",\"timestamp\":%ld}",
                 i + 1, alias, (long)time(NULL));
        }
        g_game.round = 1;
        log_msg("GAME", "Match started: '%s' (P1) vs '%s' (P2)",
                p1->alias, p2->alias);
        log_msg("GAME", "Round %d", g_game.round);
        set_state(ST_WAITING_FOR_MOVES);
        return 1;

    case ST_WAITING_FOR_MOVES:
        if (p1->left || p2->left) {
            /* Either player disconnected mid-game: the other one wins. */
            g_game.winner = p1->left ? 1 : 0;
            g_game.forfeit = 1;
            log_msg("GAME", "'%s' disconnected mid-game (forfeit)",
                    g_game.seat[1 - g_game.winner].alias);
            set_state(ST_GAME_OVER);
            return 1;
        }
        if (p1->move == SEL_NONE || p2->move == SEL_NONE) {
            return 0;
        }
        set_state(ST_SCORING);
        return 1;

    case ST_SCORING: {
        int cmp;
        int round_winner;

        /* Copy the shared move variables, then score the round. */
        p1->last = p1->move;
        p2->last = p2->move;
        cmp = selection_compare(p1->last, p2->last);
        round_winner = (cmp > 0) ? 0 : (cmp < 0) ? 1 : -1;
        if (round_winner < 0) {
            g_game.draws++;
        } else {
            g_game.seat[round_winner].wins++;
        }
        log_msg("GAME", "Round %d: %s (P1) vs %s (P2) -> %s", g_game.round,
                SELECTION_NAMES[p1->last], SELECTION_NAMES[p2->last],
                round_winner < 0 ? "DRAW" : g_game.seat[round_winner].alias);
        log_msg("GAME", "Score: P1 %d - P2 %d (draws %d)",
                p1->wins, p2->wins, g_game.draws);

        /* Results go out before any GAME_OVER so both players see them. */
        broadcast_round_result(round_winner);

        if (p1->wins >= WINS_TO_WIN || p2->wins >= WINS_TO_WIN) {
            g_game.winner = (p1->wins >= WINS_TO_WIN) ? 0 : 1;
            set_state(ST_GAME_OVER);
        } else {
            set_state(ST_NEW_ROUND);
        }
        return 1;
    }

    case ST_NEW_ROUND:
        /* Reset both shared move variables so new MOVEs are accepted. */
        p1->move = SEL_NONE;
        p2->move = SEL_NONE;
        g_game.round++;
        log_msg("GAME", "Round %d", g_game.round);
        set_state(ST_WAITING_FOR_MOVES);
        return 1;

    case ST_GAME_OVER:
        for (int i = 0; i < 2; i++) {
            char score[96];

            if (g_game.seat[i].conn == NULL) {
                continue;   /* the player who left gets nothing */
            }
            score_json(i, score, sizeof(score));
            SEND(g_game.seat[i].conn,
                 "{\"msg_type\":\"GAME_OVER\",\"winner\":\"%s\","
                 "\"reason\":\"%s\",\"score\":%s,\"timestamp\":%ld}",
                 g_game.winner == i ? "YOU" : "OPPONENT",
                 g_game.forfeit ? "FORFEIT" : "WIN", score, (long)time(NULL));
        }
        log_msg("GAME", "Match over: '%s' wins (%s)",
                g_game.seat[g_game.winner].alias,
                g_game.forfeit ? "FORFEIT" : "WIN");
        set_state(ST_CLEANUP);
        return 1;

    case ST_CLEANUP:
        /*
         * Shut down the remaining sockets. Each client thread then sees
         * EOF, finds it no longer has a seat, closes its fd and exits.
         * The fds are closed only by their owning threads, so there is
         * no risk of the game thread touching a reused descriptor.
         */
        for (int i = 0; i < 2; i++) {
            if (g_game.seat[i].conn != NULL) {
                shutdown(g_game.seat[i].conn->fd, SHUT_RDWR);
            }
        }
        reset_game();
        set_state(ST_WAITING_FOR_PLAYERS);
        return 1;

    case ST_ERROR:
    default:
        /* ERROR is handled inline by client threads; never parked here. */
        set_state(ST_WAITING_FOR_MOVES);
        return 1;
    }
}

static void *game_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_game.lock);
    for (;;) {
        while (fsm_step()) {
            /* keep stepping until the FSM needs a client event */
        }
        pthread_cond_wait(&g_game.changed, &g_game.lock);
    }
    return NULL;   /* not reached */
}

/* ====================================================================== */
/* Client threads: receive, validate, record                              */
/* ====================================================================== */

/*
 * Handle one CONNECT. Returns 0 to keep the connection, -1 to close it.
 * Called with g_game.lock held.
 */
static int handle_connect(conn_t *c, const char *alias)
{
    int slot;

    if (seat_of(c) >= 0) {
        send_error(c, "NOT_IN_GAME", "You have already joined the game");
        return 0;
    }
    if (g_game.state != ST_WAITING_FOR_PLAYERS &&
        g_game.state != ST_P1_CONNECTED) {
        send_error(c, "NOT_IN_GAME",
                   "A game is already in progress; try again later");
        return -1;
    }
    slot = (g_game.seat[0].conn == NULL && !g_game.seat[0].left) ? 0 :
           (g_game.seat[1].conn == NULL) ? 1 : -1;
    if (slot < 0) {
        send_error(c, "NOT_IN_GAME", "The game is full; try again later");
        return -1;
    }
    reset_seat(&g_game.seat[slot]);
    g_game.seat[slot].conn = c;
    snprintf(g_game.seat[slot].alias, sizeof(g_game.seat[slot].alias),
             "%s", alias);
    pthread_cond_signal(&g_game.changed);
    return 0;
}

/* Handle one MOVE. Called with g_game.lock held. */
static void handle_move(conn_t *c, const char *selection_text)
{
    int         me = seat_of(c);
    selection_t sel;
    char        score[96];

    /* Validation step 5: allowed in the current state? */
    if (me < 0 || g_game.state != ST_WAITING_FOR_MOVES) {
        send_error(c, "NOT_IN_GAME", "The game has not started yet");
        return;
    }
    /* Step 6: turn enforcement - only an empty move slot may be filled. */
    if (g_game.seat[me].move != SEL_NONE) {
        log_msg("FSM", "WAITING_FOR_MOVES -> ERROR (duplicate move)");
        send_error(c, "DUPLICATE_MOVE",
                   "You already selected a move this round");
        log_msg("FSM", "ERROR -> WAITING_FOR_MOVES");
        return;
    }
    /* Step 7: legal selection? */
    sel = selection_from_name(selection_text);
    if (sel == SEL_NONE) {
        log_msg("FSM", "WAITING_FOR_MOVES -> ERROR (invalid move)");
        send_error(c, "INVALID_MOVE", "Selection must be one of ROCK, "
                   "PAPER, SCISSORS, LIZARD, SPOCK");
        log_msg("FSM", "ERROR -> WAITING_FOR_MOVES");
        return;
    }

    g_game.seat[me].move = sel;
    log_msg("GAME", "P%d '%s' locked in a move", me + 1,
            g_game.seat[me].alias);

    /* First player to move this round is told to wait for the opponent. */
    if (g_game.seat[1 - me].move == SEL_NONE) {
        score_json(me, score, sizeof(score));
        SEND(c, "{\"msg_type\":\"STATE_UPDATE\","
                "\"status\":\"WAITING_FOR_OPPONENT\","
                "\"your_selection\":\"%s\",\"opponent_selection\":null,"
                "\"round_winner\":null,\"score\":%s,\"timestamp\":%ld}",
             SELECTION_NAMES[sel], score, (long)time(NULL));
    }
    pthread_cond_signal(&g_game.changed);
}

/*
 * Run the validation pipeline (protocol_blueprint.md, "Message Validation
 * & Dispatch") on one received line. Returns 0 to keep reading, -1 when
 * the connection should be closed.
 */
static int process_line(conn_t *c, const char *line)
{
    char msg_type[32];
    char text[MAX_MSG_LEN];
    long ts;
    int  rc = 0;

    log_msg("RECV", "%s -> %s", c->peer, line);

    /* Steps 1-4: syntax and schema checks (no shared state needed). */
    if (!utf8_valid(line)) {
        pthread_mutex_lock(&g_game.lock);
        send_error(c, "MALFORMED", "Message is not valid UTF-8");
        pthread_mutex_unlock(&g_game.lock);
        return 0;
    }
    if (!json_is_object(line)) {
        pthread_mutex_lock(&g_game.lock);
        send_error(c, "MALFORMED", "Message is not a valid JSON object");
        pthread_mutex_unlock(&g_game.lock);
        return 0;
    }
    if (!json_get_string(line, "msg_type", msg_type, sizeof(msg_type)) ||
        (strcmp(msg_type, "CONNECT") != 0 && strcmp(msg_type, "MOVE") != 0 &&
         strcmp(msg_type, "DISCONNECT") != 0)) {
        pthread_mutex_lock(&g_game.lock);
        send_error(c, "MALFORMED", "Missing or unknown msg_type");
        pthread_mutex_unlock(&g_game.lock);
        return 0;
    }
    if (!json_get_long(line, "timestamp", &ts)) {
        pthread_mutex_lock(&g_game.lock);
        send_error(c, "MALFORMED", "Missing required field: timestamp");
        pthread_mutex_unlock(&g_game.lock);
        return 0;
    }

    if (strcmp(msg_type, "DISCONNECT") == 0) {
        log_msg("NET", "%s sent DISCONNECT", c->peer);
        return -1;
    }

    if (strcmp(msg_type, "CONNECT") == 0) {
        size_t len;

        if (!json_get_string(line, "alias", text, sizeof(text)) ||
            (len = strlen(text)) == 0 || len > MAX_ALIAS_LEN) {
            pthread_mutex_lock(&g_game.lock);
            send_error(c, "MALFORMED",
                       "alias must be a string of 1-32 characters");
            pthread_mutex_unlock(&g_game.lock);
            return 0;
        }
        pthread_mutex_lock(&g_game.lock);
        rc = handle_connect(c, text);
        pthread_mutex_unlock(&g_game.lock);
        return rc;
    }

    /* MOVE */
    if (!json_get_string(line, "selection", text, sizeof(text))) {
        pthread_mutex_lock(&g_game.lock);
        send_error(c, "MALFORMED", "Missing required field: selection");
        pthread_mutex_unlock(&g_game.lock);
        return 0;
    }
    pthread_mutex_lock(&g_game.lock);
    handle_move(c, text);
    pthread_mutex_unlock(&g_game.lock);
    return 0;
}

/*
 * On an idle timeout, decide whether to drop the connection. Only a
 * player who owes a move (or a connection that never joined) is dropped;
 * a player waiting on the lobby or on the opponent may sit idle.
 */
static int timeout_should_drop(conn_t *c)
{
    int me;
    int drop;

    pthread_mutex_lock(&g_game.lock);
    me = seat_of(c);
    if (me < 0) {
        drop = 1;
    } else {
        drop = g_game.state == ST_WAITING_FOR_MOVES &&
               g_game.seat[me].move == SEL_NONE;
    }
    pthread_mutex_unlock(&g_game.lock);
    return drop;
}

static void *client_thread(void *arg)
{
    conn_t       *c = arg;
    line_reader_t lr;
    char          line[MAX_MSG_LEN + 1];
    int           running = 1;

    line_reader_init(&lr, c->fd);

    while (running) {
        int rc = line_reader_fill(&lr);

        if (rc == LR_EOF) {
            log_msg("NET", "%s closed the connection (EOF)", c->peer);
            break;
        }
        if (rc == LR_ERROR) {
            log_msg("NET", "%s connection error: %s", c->peer,
                    strerror(errno));
            break;
        }
        if (rc == LR_TIMEOUT) {
            if (timeout_should_drop(c)) {
                log_msg("NET", "%s idle for %ds, dropping", c->peer,
                        IDLE_TIMEOUT_S);
                break;
            }
            continue;
        }

        /* One recv() may hold zero, one, or several complete messages. */
        for (;;) {
            int lrc = line_reader_next(&lr, line, sizeof(line));

            if (lrc == LR_NONE) {
                break;
            }
            if (lrc == LR_TOOLONG) {
                pthread_mutex_lock(&g_game.lock);
                send_error(c, "MALFORMED", "Message exceeds maximum length");
                pthread_mutex_unlock(&g_game.lock);
                continue;
            }
            if (line[0] == '\0') {
                continue;   /* ignore blank keep-alive lines */
            }
            if (process_line(c, line) < 0) {
                running = 0;
                break;
            }
        }
    }

    /* Raise CLIENT_DISCONNECTED: vacate our seat and wake the FSM. */
    pthread_mutex_lock(&g_game.lock);
    {
        int me = seat_of(c);

        if (me >= 0) {
            g_game.seat[me].conn = NULL;
            g_game.seat[me].left = 1;
            pthread_cond_signal(&g_game.changed);
        }
    }
    pthread_mutex_unlock(&g_game.lock);

    close(c->fd);
    log_msg("NET", "%s socket closed", c->peer);
    free(c);
    return NULL;
}

/* ====================================================================== */
/* main: CLI parsing, listening socket, accept loop                       */
/* ====================================================================== */

static void usage(const char *prog, FILE *out)
{
    fprintf(out,
            "Usage: %s -p <port>\n"
            "\n"
            "Rock-Paper-Scissors-Lizard-Spock game server.\n"
            "\n"
            "Options:\n"
            "  -p <port>   TCP port to listen on (required, %d-%d).\n"
            "              The server binds to 0.0.0.0 (all interfaces).\n"
            "  -h          Show this help message and exit.\n"
            "\n"
            "Example:\n"
            "  %s -p 8080\n",
            prog, PORT_MIN, PORT_MAX, prog);
}

/* Create, bind (0.0.0.0:port) and listen. Exits on failure. */
static int open_listener(int port)
{
    struct sockaddr_in addr;
    int                fd;
    int                yes = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[ERROR] socket");
        exit(1);
    }
    /* Allow quick restarts while old connections sit in TIME_WAIT. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 0.0.0.0 */
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ERROR] Unable to bind 0.0.0.0:%d: %s\n", port,
                strerror(errno));
        exit(1);
    }
    if (listen(fd, 8) < 0) {
        perror("[ERROR] listen");
        exit(1);
    }
    return fd;
}

int main(int argc, char *argv[])
{
    int       port = -1;
    int       opt;
    int       listen_fd;
    pthread_t tid;

    opterr = 0;   /* we print our own messages */
    while ((opt = getopt(argc, argv, ":hp:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0], stdout);
            return 0;
        case 'p':
            port = parse_port(optarg);
            if (port < 0) {
                fprintf(stderr, "[ERROR] Invalid port '%s': must be an "
                        "integer from %d to %d.\n\n", optarg, PORT_MIN,
                        PORT_MAX);
                usage(argv[0], stderr);
                return 1;
            }
            break;
        case ':':
            fprintf(stderr, "[ERROR] Option -%c requires a value.\n\n",
                    optopt);
            usage(argv[0], stderr);
            return 1;
        default:
            fprintf(stderr, "[ERROR] Unknown option -%c.\n\n", optopt);
            usage(argv[0], stderr);
            return 1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "[ERROR] Unexpected argument '%s'.\n\n",
                argv[optind]);
        usage(argv[0], stderr);
        return 1;
    }
    if (port < 0) {
        fprintf(stderr, "[ERROR] Missing required option -p <port>.\n\n");
        usage(argv[0], stderr);
        return 1;
    }

    /* Belt and braces: never die from SIGPIPE on a dead client socket. */
    signal(SIGPIPE, SIG_IGN);

    listen_fd = open_listener(port);
    log_msg("NET", "Listening on 0.0.0.0:%d (TCP)", port);

    if (pthread_create(&tid, NULL, game_thread, NULL) != 0) {
        fprintf(stderr, "[ERROR] Unable to start game thread\n");
        return 1;
    }
    pthread_detach(tid);

    /* Accept loop: one detached worker thread per connection. */
    for (;;) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        struct timeval     tv = { .tv_sec = IDLE_TIMEOUT_S, .tv_usec = 0 };
        conn_t            *c;
        int                fd;

        fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (errno != EINTR) {
                log_msg("NET", "accept failed: %s", strerror(errno));
            }
            continue;
        }

        /* Idle timeout for recv(), and a bound on blocking sends. */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        c = calloc(1, sizeof(*c));
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->fd = fd;
        snprintf(c->peer, sizeof(c->peer), "%s:%u",
                 inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        log_msg("NET", "Accepted connection from %s", c->peer);

        if (pthread_create(&tid, NULL, client_thread, c) != 0) {
            log_msg("NET", "Unable to start thread for %s", c->peer);
            close(fd);
            free(c);
            continue;
        }
        pthread_detach(tid);
    }
}
