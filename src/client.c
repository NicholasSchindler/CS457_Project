/*
 * client.c - RPSLS game client.
 *
 * CS 457 Final Project - Rock-Paper-Scissors-Lizard-Spock
 * Author: Nicholas Schindler
 *
 * Usage:
 *   ./client -h
 *   ./client -i server.schindler.edu -p 8080 [-n alias]
 *
 * The client resolves the server hostname through DNS (getaddrinfo, the C
 * equivalent of socket.gethostbyname), opens a TCP stream socket, and
 * exchanges newline-delimited JSON messages with the server.
 *
 * It multiplexes the keyboard and the server socket with poll(), so a
 * message from the server (opponent forfeit, server shutdown) is shown
 * immediately even while the player is still deciding on a move.
 *
 * All game rules and scoring live on the server; the client only sends the
 * player's alias, selections, and a DISCONNECT when quitting.
 */
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== */
/* Client-side view of the game                                           */
/* ====================================================================== */

typedef enum {
    CL_LOBBY,      /* connected, waiting for GAME_START          */
    CL_CHOOSING,   /* our turn: waiting for keyboard input       */
    CL_WAITING,    /* move sent, waiting for the round result    */
    CL_OVER        /* GAME_OVER received                         */
} client_state_t;

typedef struct {
    int            fd;
    client_state_t state;
    char           alias[MAX_ALIAS_LEN + 1];
    char           opponent[MAX_ALIAS_LEN + 1];
    char           role[4];      /* "P1" or "P2" */
    int            round;
    long           wins;
    long           losses;
    long           draws;
} client_t;

/* Set by the SIGINT handler so Ctrl+C sends a polite DISCONNECT. */
static volatile sig_atomic_t g_interrupted = 0;

/* 1 when stdin is a pipe/file; typed input is then echoed into the log. */
static int g_echo_input = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

/* ====================================================================== */
/* Keyboard line buffer                                                   */
/*                                                                        */
/* stdin is read with raw read() rather than stdio: fgets() could pull    */
/* several lines into a hidden buffer, after which poll() would no longer */
/* report stdin as readable and those lines would be stranded.            */
/* ====================================================================== */

static char   g_in_buf[512];
static size_t g_in_len = 0;

/* One read() from stdin. Returns >0 bytes read, 0 on EOF, -1 on error. */
static int stdin_fill(void)
{
    ssize_t n;

    if (g_in_len == sizeof(g_in_buf)) {
        g_in_len = 0;   /* absurdly long line: drop it */
    }
    do {
        n = read(STDIN_FILENO, g_in_buf + g_in_len,
                 sizeof(g_in_buf) - g_in_len);
    } while (n < 0 && errno == EINTR && !g_interrupted);
    if (n > 0) {
        g_in_len += (size_t)n;
    }
    return (int)n;
}

/* Pop one complete line (no newline) from the buffer. 1 = got one. */
static int stdin_next_line(char *out, size_t n)
{
    char  *nl = memchr(g_in_buf, '\n', g_in_len);
    size_t len;

    if (nl == NULL) {
        return 0;
    }
    len = (size_t)(nl - g_in_buf);
    snprintf(out, n, "%.*s", (int)len, g_in_buf);
    out[strcspn(out, "\r")] = '\0';
    memmove(g_in_buf, nl + 1, g_in_len - len - 1);
    g_in_len -= len + 1;
    return 1;
}

/* ====================================================================== */
/* CLI rendering                                                          */
/* ====================================================================== */

#define RULE "+------------------------------------------------------+"

static void print_scoreboard(const client_t *cl)
{
    char you[64];

    snprintf(you, sizeof(you), "%s (%s)", cl->alias, cl->role);
    printf("%s\n", RULE);
    printf("| %-24.24s vs %-24.24s |\n", you, cl->opponent);
    printf("| Wins: %-3ld  Losses: %-3ld  Draws: %-3ld   First to %d      |\n",
           cl->wins, cl->losses, cl->draws, WINS_TO_WIN);
    printf("%s\n", RULE);
}

static void print_prompt(const client_t *cl)
{
    printf("\n========================= ROUND %d "
           "=========================\n", cl->round);
    print_scoreboard(cl);
    printf(">>> YOUR TURN <<<\n");
    printf("  1) ROCK   2) PAPER   3) SCISSORS   4) LIZARD   5) SPOCK"
           "   q) quit\n");
    printf("Your choice: ");
    fflush(stdout);
}

/* Show what beat what, e.g. "SPOCK vaporizes ROCK". */
static const char *verb_for(selection_t a, selection_t b)
{
    static const char *const VERBS[SEL_COUNT][SEL_COUNT] = {
        /* ROCK */     { NULL, NULL, "crushes", "crushes", NULL },
        /* PAPER */    { "covers", NULL, NULL, NULL, "disproves" },
        /* SCISSORS */ { NULL, "cuts", NULL, "decapitates", NULL },
        /* LIZARD */   { NULL, "eats", NULL, NULL, "poisons" },
        /* SPOCK */    { "vaporizes", NULL, "smashes", NULL, NULL },
    };

    if (a == SEL_NONE || b == SEL_NONE) {
        return NULL;
    }
    return VERBS[a][b];
}

/* ====================================================================== */
/* Sending                                                                */
/* ====================================================================== */

static void send_disconnect(client_t *cl)
{
    send_json(cl->fd, "{\"msg_type\":\"DISCONNECT\",\"timestamp\":%ld}",
              (long)time(NULL));
}

/* Parse a keyboard choice: 1-5, a name, or a unique first letter. */
static selection_t parse_choice(const char *in)
{
    char upper[16];
    size_t i;

    for (i = 0; in[i] != '\0' && i + 1 < sizeof(upper); i++) {
        upper[i] = (char)toupper((unsigned char)in[i]);
    }
    upper[i] = '\0';

    if (strlen(upper) == 1) {
        switch (upper[0]) {
        case '1': case 'R': return SEL_ROCK;
        case '2': case 'P': return SEL_PAPER;
        case '3': case 'S': return SEL_SCISSORS;
        case '4': case 'L': return SEL_LIZARD;
        case '5': case 'K': return SEL_SPOCK;   /* K for spocK */
        default:  return SEL_NONE;
        }
    }
    return selection_from_name(upper);
}

/* ====================================================================== */
/* Handling server messages                                               */
/* ====================================================================== */

/* Read the recipient-relative "score" object into the client struct. */
static void read_score(client_t *cl, const char *msg)
{
    char score[256];

    if (json_get_object(msg, "score", score, sizeof(score))) {
        json_get_long(score, "wins", &cl->wins);
        json_get_long(score, "losses", &cl->losses);
        json_get_long(score, "draws", &cl->draws);
    }
}

static void on_round_result(client_t *cl, const char *msg)
{
    char        mine[16] = "?";
    char        theirs[16] = "?";
    char        winner[16] = "?";
    selection_t a;
    selection_t b;
    const char *verb;

    json_get_string(msg, "your_selection", mine, sizeof(mine));
    json_get_string(msg, "opponent_selection", theirs, sizeof(theirs));
    json_get_string(msg, "round_winner", winner, sizeof(winner));
    read_score(cl, msg);

    a = selection_from_name(mine);
    b = selection_from_name(theirs);

    printf("\n--- Round %d result ---\n", cl->round);
    printf("  You played:      %s\n", mine);
    printf("  Opponent played: %s  (%s)\n", theirs, cl->opponent);
    if (strcmp(winner, "YOU") == 0) {
        verb = verb_for(a, b);
        printf("  %s %s %s.  ** You win the round! **\n", mine,
               verb ? verb : "beats", theirs);
    } else if (strcmp(winner, "OPPONENT") == 0) {
        verb = verb_for(b, a);
        printf("  %s %s %s.  You lose the round.\n", theirs,
               verb ? verb : "beats", mine);
    } else {
        printf("  Both picked %s.  Draw - the round is thrown out.\n", mine);
    }
    printf("  Score: Wins %ld | Losses %ld | Draws %ld\n",
           cl->wins, cl->losses, cl->draws);

    if (cl->wins >= WINS_TO_WIN || cl->losses >= WINS_TO_WIN) {
        /* Match point reached; GAME_OVER is on its way. */
        cl->state = CL_WAITING;
        return;
    }
    cl->round++;
    cl->state = CL_CHOOSING;
    print_prompt(cl);
}

/*
 * Dispatch one message from the server. Returns 0 to keep going, 1 when
 * the game is over (normal exit), or -1 on a fatal protocol problem.
 */
static int handle_server_message(client_t *cl, const char *msg)
{
    char type[32];

    if (!utf8_valid(msg) || !json_is_object(msg) ||
        !json_get_string(msg, "msg_type", type, sizeof(type))) {
        printf("\n[WARN] Ignoring malformed message from server: %s\n", msg);
        return 0;
    }

    if (strcmp(type, "LOBBY_WAIT") == 0) {
        printf("Joined the lobby as '%s'. Waiting for Player 2...\n",
               cl->alias);

    } else if (strcmp(type, "GAME_START") == 0) {
        json_get_string(msg, "player_role", cl->role, sizeof(cl->role));
        json_get_string(msg, "opponent_alias", cl->opponent,
                        sizeof(cl->opponent));
        cl->round = 1;
        cl->wins = cl->losses = cl->draws = 0;
        printf("\n******************************************************\n");
        printf("  GAME START!  You are %s.  Opponent: %s\n", cl->role,
               cl->opponent);
        printf("  First to %d round wins takes the match.\n", WINS_TO_WIN);
        printf("******************************************************\n");
        cl->state = CL_CHOOSING;
        print_prompt(cl);

    } else if (strcmp(type, "STATE_UPDATE") == 0) {
        char status[32] = "";
        char mine[16] = "?";

        json_get_string(msg, "status", status, sizeof(status));
        if (strcmp(status, "WAITING_FOR_OPPONENT") == 0) {
            json_get_string(msg, "your_selection", mine, sizeof(mine));
            read_score(cl, msg);
            printf("You locked in %s. Waiting for %s...\n", mine,
                   cl->opponent);
            cl->state = CL_WAITING;
        } else if (strcmp(status, "ROUND_RESULT") == 0) {
            on_round_result(cl, msg);
        }

    } else if (strcmp(type, "ERROR") == 0) {
        char code[32] = "?";
        char text[256] = "";

        json_get_string(msg, "error_code", code, sizeof(code));
        json_get_string(msg, "error_message", text, sizeof(text));
        printf("\n[SERVER ERROR] %s: %s\n", code, text);

        if (strcmp(code, "INVALID_MOVE") == 0 ||
            strcmp(code, "MALFORMED") == 0) {
            if (cl->state == CL_WAITING) {
                cl->state = CL_CHOOSING;   /* move rejected: try again */
                print_prompt(cl);
            }
        } else if (strcmp(code, "DUPLICATE_MOVE") == 0) {
            printf("Still waiting for %s...\n", cl->opponent);
        }
        /* NOT_IN_GAME in the lobby means "server full"; EOF follows. */

    } else if (strcmp(type, "GAME_OVER") == 0) {
        char winner[16] = "?";
        char reason[16] = "?";

        json_get_string(msg, "winner", winner, sizeof(winner));
        json_get_string(msg, "reason", reason, sizeof(reason));
        read_score(cl, msg);

        printf("\n######################################################\n");
        printf("                      GAME OVER\n");
        if (strcmp(reason, "FORFEIT") == 0) {
            printf("  %s disconnected. You win by forfeit!\n", cl->opponent);
        } else if (strcmp(winner, "YOU") == 0) {
            printf("  Congratulations, %s - you won the match!\n", cl->alias);
        } else {
            printf("  %s won the match. Better luck next time.\n",
                   cl->opponent);
        }
        printf("  Final score: Wins %ld | Losses %ld | Draws %ld\n",
               cl->wins, cl->losses, cl->draws);
        printf("######################################################\n");
        cl->state = CL_OVER;
        return 1;

    } else {
        printf("\n[WARN] Unknown message type from server: %s\n", type);
    }
    fflush(stdout);
    return 0;
}

/* ====================================================================== */
/* Handling keyboard input                                                */
/* ====================================================================== */

/* Returns 1 if the user asked to quit, else 0. */
static int handle_user_line(client_t *cl, char *line)
{
    selection_t sel;

    line[strcspn(line, "\r\n")] = '\0';
    if (g_echo_input) {
        printf("%s\n", line);   /* make scripted input visible in logs */
    }
    if (strcmp(line, "q") == 0 || strcmp(line, "Q") == 0 ||
        strcmp(line, "quit") == 0) {
        return 1;
    }

    if (cl->state != CL_CHOOSING) {
        if (cl->state == CL_LOBBY) {
            printf("Still waiting for Player 2... (q to quit)\n");
        } else {
            printf("Waiting for %s... (q to quit)\n", cl->opponent);
        }
        return 0;
    }

    sel = parse_choice(line);
    if (sel == SEL_NONE) {
        printf("'%s' is not a valid choice. Enter 1-5 or a name.\n", line);
        printf("Your choice: ");
        fflush(stdout);
        return 0;
    }
    if (send_json(cl->fd, "{\"msg_type\":\"MOVE\",\"selection\":\"%s\","
                  "\"timestamp\":%ld}", SELECTION_NAMES[sel],
                  (long)time(NULL)) < 0) {
        return 0;   /* the receive path will report the lost connection */
    }
    cl->state = CL_WAITING;
    return 0;
}

/* ====================================================================== */
/* Setup: CLI, DNS, connect                                               */
/* ====================================================================== */

static void usage(const char *prog, FILE *out)
{
    fprintf(out,
            "Usage: %s -i <server_hostname_or_ip> -p <port> [-n <alias>]\n"
            "\n"
            "Rock-Paper-Scissors-Lizard-Spock game client.\n"
            "\n"
            "Options:\n"
            "  -i <host>   Server DNS hostname or IPv4 address (required),\n"
            "              e.g. server.schindler.edu\n"
            "  -p <port>   Server TCP port (required, %d-%d).\n"
            "  -n <alias>  Player display name (optional; prompted for if\n"
            "              omitted, max %d characters).\n"
            "  -h          Show this help message and exit.\n"
            "\n"
            "Example:\n"
            "  %s -i server.schindler.edu -p 8080\n",
            prog, PORT_MIN, PORT_MAX, MAX_ALIAS_LEN, prog);
}

/*
 * Resolve hostname to an IPv4 address via DNS. On failure print a
 * friendly message and exit(1) - never an unhandled crash.
 */
static struct sockaddr_in resolve_server(const char *host, int port)
{
    struct addrinfo    hints;
    struct addrinfo   *res = NULL;
    struct sockaddr_in addr;
    int                rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;         /* IPv4, like gethostbyname() */
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        /* The C equivalent of catching socket.gaierror. */
        fprintf(stderr, "[ERROR] Unable to resolve DNS hostname '%s' (%s).\n",
                host, rc != 0 ? gai_strerror(rc) : "no address returned");
        fprintf(stderr, "Please check your Cisco router DNS configuration "
                "('ip dns server' / 'ip host') or verify domain "
                "spelling.\n");
        if (res != NULL) {
            freeaddrinfo(res);
        }
        exit(1);
    }

    memcpy(&addr, res->ai_addr, sizeof(addr));
    addr.sin_port = htons((unsigned short)port);
    freeaddrinfo(res);
    return addr;
}

/* Ask for an alias on the terminal until a valid one is entered. */
static void prompt_alias(char *out, size_t n)
{
    char line[256];

    for (;;) {
        printf("Enter your player name (1-%d characters): ", MAX_ALIAS_LEN);
        fflush(stdout);
        while (!stdin_next_line(line, sizeof(line))) {
            if (stdin_fill() <= 0) {
                fprintf(stderr, "\n[ERROR] No player name entered.\n");
                exit(1);
            }
        }
        if (g_echo_input) {
            printf("%s\n", line);
        }
        if (line[0] != '\0' && strlen(line) <= MAX_ALIAS_LEN) {
            snprintf(out, n, "%s", line);
            return;
        }
        printf("Name must be between 1 and %d characters.\n", MAX_ALIAS_LEN);
    }
}

/* ====================================================================== */
/* Main event loop                                                        */
/* ====================================================================== */

/* Returns the process exit status. */
static int run(client_t *cl)
{
    line_reader_t lr;
    char          line[MAX_MSG_LEN + 1];
    struct pollfd fds[2];
    int           stdin_open = 1;

    line_reader_init(&lr, cl->fd);

    for (;;) {
        int nfds = 1;
        int rc;

        if (g_interrupted) {
            printf("\nQuitting - sending DISCONNECT to the server.\n");
            send_disconnect(cl);
            return 0;
        }

        fds[0].fd = cl->fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        if (stdin_open) {
            fds[1].fd = STDIN_FILENO;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }

        rc = poll(fds, (nfds_t)nfds, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;   /* loop top handles g_interrupted */
            }
            perror("[ERROR] poll");
            return 1;
        }

        /* ---- Server socket ---- */
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            rc = line_reader_fill(&lr);
            if (rc == LR_EOF || rc == LR_ERROR) {
                if (cl->state == CL_OVER) {
                    return 0;
                }
                printf("\n[ERROR] Connection to the server was lost%s%s.\n",
                       rc == LR_ERROR ? ": " : "",
                       rc == LR_ERROR ? strerror(errno) : "");
                return 1;
            }
            for (;;) {
                int lrc = line_reader_next(&lr, line, sizeof(line));

                if (lrc == LR_NONE) {
                    break;
                }
                if (lrc == LR_TOOLONG || line[0] == '\0') {
                    continue;
                }
                if (handle_server_message(cl, line) != 0) {
                    return 0;   /* GAME_OVER */
                }
            }
        }

        /* ---- Keyboard ---- */
        if (nfds == 2 && (fds[1].revents & (POLLIN | POLLHUP))) {
            char input[256];

            if (stdin_fill() <= 0) {
                if (g_interrupted) {
                    continue;
                }
                /* EOF on stdin (Ctrl+D or end of a piped script). */
                printf("\nInput closed - leaving the game.\n");
                send_disconnect(cl);
                return 0;
            }
            while (stdin_next_line(input, sizeof(input))) {
                if (handle_user_line(cl, input)) {
                    printf("Quitting - sending DISCONNECT to the server.\n");
                    send_disconnect(cl);
                    return 0;
                }
            }
        }
        fflush(stdout);
    }
}

int main(int argc, char *argv[])
{
    const char        *host = NULL;
    int                port = -1;
    int                opt;
    client_t           cl;
    struct sockaddr_in addr;
    struct sigaction   sa;
    char               ip[INET_ADDRSTRLEN];
    char               alias_esc[MAX_ALIAS_LEN * 6 + 1];
    int                status;

    memset(&cl, 0, sizeof(cl));
    cl.state = CL_LOBBY;
    g_echo_input = !isatty(STDIN_FILENO);

    /* ---- CLI parsing ---- */
    opterr = 0;
    while ((opt = getopt(argc, argv, ":hi:p:n:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0], stdout);
            return 0;
        case 'i':
            host = optarg;
            break;
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
        case 'n':
            if (optarg[0] == '\0' || strlen(optarg) > MAX_ALIAS_LEN) {
                fprintf(stderr, "[ERROR] Alias must be 1-%d characters.\n",
                        MAX_ALIAS_LEN);
                return 1;
            }
            snprintf(cl.alias, sizeof(cl.alias), "%s", optarg);
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
    if (host == NULL || port < 0) {
        fprintf(stderr, "[ERROR] Missing required option%s.\n\n",
                host == NULL && port < 0 ? "s -i <host> and -p <port>" :
                host == NULL ? " -i <host>" : " -p <port>");
        usage(argv[0], stderr);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    /* ---- DNS resolution (exits cleanly with status 1 on failure) ---- */
    addr = resolve_server(host, port);
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    printf("Resolved %s -> %s\n", host, ip);

    if (cl.alias[0] == '\0') {
        prompt_alias(cl.alias, sizeof(cl.alias));
    }

    /* ---- TCP connect ---- */
    cl.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cl.fd < 0) {
        perror("[ERROR] socket");
        return 1;
    }
    if (connect(cl.fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ERROR] Unable to connect to %s (%s) port %d: %s\n",
                host, ip, port, strerror(errno));
        fprintf(stderr, "Check that the server is running and reachable.\n");
        close(cl.fd);
        return 1;
    }
    printf("Connected to %s:%d over TCP.\n", ip, port);

    /* Ctrl+C -> send DISCONNECT instead of dying silently. No SA_RESTART,
     * so poll() is interrupted and the loop notices immediately. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* ---- Join request ---- */
    json_escape(cl.alias, alias_esc, sizeof(alias_esc));
    if (send_json(cl.fd, "{\"msg_type\":\"CONNECT\",\"alias\":\"%s\","
                  "\"timestamp\":%ld}", alias_esc, (long)time(NULL)) < 0) {
        fprintf(stderr, "[ERROR] Unable to send CONNECT: %s\n",
                strerror(errno));
        close(cl.fd);
        return 1;
    }

    status = run(&cl);
    close(cl.fd);
    return status;
}
