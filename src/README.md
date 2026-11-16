# CS 457 Sprint 3 — Rock-Paper-Scissors-Lizard-Spock (C implementation)

A two-player networked RPSLS game: a multithreaded TCP server and an interactive
CLI client, both written in C (POSIX), targeting the Alpine Linux nodes in CML.
First player to **3 round wins** takes the match; draws are thrown out.

## Files

| File | Purpose |
|---|---|
| `server.c` | Game server: CLI parsing, `0.0.0.0` listener, accept loop, per-client threads, FSM game thread |
| `client.c` | Game client: CLI parsing, DNS resolution, TCP connect, `poll()` event loop, CLI board/prompts |
| `common.c` / `common.h` | Shared protocol code: newline stream framing, JSON parse/serialize, game rules, port validation |
| `Makefile` | Builds `server` and `client` |
| `run_demo.sh` | Scripted localhost demo (server + 2 clients, full match), writes `logs/` |
| `logs/gameplay_log.txt` | Intermediate gameplay execution log (server + both clients, 5 rounds) |

## Build (Alpine Linux)

```sh
apk add build-base
make
```

The build uses `gcc -std=gnu11 -Wall -Wextra -pthread` and compiles with no warnings against musl libc.

## Run

```sh
./server -h
./server -p 8080

./client -h
./client -i server.schindler.edu -p 8080            # prompts for a player name
./client -i server.schindler.edu -p 8080 -n Shelly  # or pass it with -n
```

Run two clients to play. On your turn, enter `1`-`5`, a name (`rock`, `SPOCK`, ...), or a letter
(`r p s l k`). Enter `q` or press Ctrl+C to quit. Quitting sends `DISCONNECT`, and the
opponent wins by forfeit.

### CLI validation

* `-p` must be an integer from 1024 to 65535. Otherwise the program prints an error and the usage, and exits with status 1.
* A missing required option, an option with no value, an unknown option or a stray argument also prints an error and the usage, and exits with status 1.
* `-h` prints the usage and exits with status 0.

### DNS resolution and graceful failure

The client resolves `-i` with `getaddrinfo()` (the C equivalent of `socket.gethostbyname()`; the
hostname is never hardcoded). If resolution fails (`EAI_NONAME`, `EAI_AGAIN` when the DNS server is
unreachable, etc.) the client prints:

```
[ERROR] Unable to resolve DNS hostname 'server.schindler.edu' (Name does not resolve).
Please check your Cisco router DNS configuration ('ip dns server' / 'ip host') or verify domain spelling.
```

and exits with status 1. No crash, no core dump. A refused or unreachable TCP connection is reported
the same way.

## Protocol (Sprint 1)

* TCP stream sockets: `socket(AF_INET, SOCK_STREAM, 0)`.
* **Framing:** newline-delimited JSON, one UTF-8 JSON object per line.
  `line_reader_fill()` does one `recv()` and `line_reader_next()` pops complete lines. Because one
  `recv()` may return part of a message or several messages, the code never assumes one `recv()`
  equals one message. Lines over 1024 bytes are rejected as `MALFORMED`, and bytes left without a
  newline at EOF are discarded.
* **Messages:** `CONNECT`, `LOBBY_WAIT`, `GAME_START`, `MOVE`, `STATE_UPDATE`
  (`WAITING_FOR_OPPONENT` / `ROUND_RESULT`), `ERROR`, `DISCONNECT`, `GAME_OVER`, exactly as in
  `docs/protocol_blueprint.md`. Server messages are personalized (`YOU`/`OPPONENT`, score relative
  to the recipient).
* **Validation order** on the server: UTF-8, then JSON object, then a client-to-server `msg_type`,
  then the required fields and their types (all four fail as `MALFORMED`). Next, whether the
  message is allowed in the current state (`NOT_IN_GAME`). For a `MOVE` only, it then checks
  whether the player already moved (`DUPLICATE_MOVE`) and whether the selection is valid
  (`INVALID_MOVE`). A rejected message never changes game state.

## Concurrency (Sprint 2: multithreading)

* **Main thread:** `bind(0.0.0.0:port)`, `listen()`, then `accept()` in a loop, starting one detached
  pthread per connection.
* **Client threads:** each reads and validates its own socket and records a join or a move in the
  shared game record. A thread may only fill an *empty* move slot, which is how turns are enforced.
* **Game thread:** a dedicated thread that runs the FSM. It sleeps on a condition variable and wakes
  on join, move and disconnect events.
* **Synchronization:** a single mutex guards all shared state. The game thread holds it for every
  transition, so client threads only ever see stable states. Only a connection's own thread closes
  its socket. Cleanup uses `shutdown()` to wake that thread, so a reused descriptor is never touched.
* **Disconnects:** EOF (FIN), `DISCONNECT`, RST/errors and the 60 s idle timeout all raise one internal
  `CLIENT_DISCONNECTED` event. The idle timeout only applies to a player who owes a move. In the
  lobby, the server returns to waiting. Mid-game, the remaining player gets `GAME_OVER` / `FORFEIT`.
* A third client that connects while a match is in progress gets `ERROR NOT_IN_GAME` and is closed.
  After `CLEANUP` the server returns to `WAITING_FOR_PLAYERS` for the next match.

### FSM (logged by the server as `[FSM] A -> B`)

```
INIT -> WAITING_FOR_PLAYERS -> P1_CONNECTED -> GAME_START -> WAITING_FOR_MOVES
WAITING_FOR_MOVES -> ERROR -> WAITING_FOR_MOVES            (bad message)
WAITING_FOR_MOVES -> SCORING -> NEW_ROUND -> WAITING_FOR_MOVES
SCORING -> GAME_OVER -> CLEANUP -> WAITING_FOR_PLAYERS     (3 wins)
WAITING_FOR_MOVES -> GAME_OVER                             (disconnect = forfeit)
P1_CONNECTED -> WAITING_FOR_PLAYERS                        (P1 leaves lobby)
```

## Demo / execution log

```sh
sh run_demo.sh 8080
```

This starts the server and two scripted clients on localhost and plays a full five-round match
(including a draw and a rejected invalid input). Output goes to `logs/`. A captured run is in
`logs/gameplay_log.txt`.
