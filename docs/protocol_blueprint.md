# Application Protocol Blueprint: Rock-Paper-Scissors-Lizard-Spock

**Author:** Nicholas Schindler
**Course:** CS 457 - Computer Networks
**Sprint:** 1 - Application Protocol & FSM Design
**Server Host:** `server.schindler.edu` (port to be assigned in a later sprint)

---

## 1. Overview

This document specifies the application-layer protocol between the RPSLS game server and its two clients. It defines the transport, the framing rule that delimits messages on the TCP byte stream, the exact schema of every message, and how connections are terminated, both gracefully and abruptly.

**Game summary:** Each round, both players secretly choose one of ROCK, PAPER, SCISSORS, LIZARD or SPOCK. The server reveals both choices once both are in and awards the round. Tied rounds are recorded as draws and do not count toward victory. The first player to win 3 rounds wins the match.

**Turn model:** Each round is one turn in which *both* players act. The server collects one move from each player per turn. A player may not move twice in the same turn, and no one may move outside a turn.

---

## 2. Transport & Serialization

| Property | Value |
|---|---|
| Transport | TCP (reliable, ordered byte stream) |
| Serialization | JSON |
| Character encoding | UTF-8 |
| Framing | Newline-delimited JSON: one compact JSON object per line, terminated by `\n` (`0x0A`) |
| Topology | 1 server, 2 clients; each client holds one persistent TCP connection to the server |

---

## 3. Framing Rule

### 3.1 The Problem

TCP delivers a continuous stream of bytes with no message boundaries. A single `recv()` call may return:

- **Part of a message (fragmentation):** one JSON object split across two or more `recv()` calls.
- **Several messages at once (coalescing):** two or more JSON objects back-to-back in one `recv()` call.
- **Both:** one complete message followed by the beginning of the next.

The receiver therefore can never assume that one `recv()` equals one message.

### 3.2 The Rule

1. **Serialization:** every message is serialized as a single line of compact JSON with no pretty-printing and no literal newlines.
2. **Encoding:** the JSON text is encoded as UTF-8.
3. **Termination:** exactly one newline byte `\n` (`0x0A`) is appended after each message.
4. **Uniqueness of the delimiter:** the byte `0x0A` appears on the wire **only** as the message terminator. This is guaranteed because JSON requires newline characters inside string values to be escaped as the two-character sequence `\` `n` (`0x5C 0x6E`). A user-supplied alias containing a newline therefore cannot break framing.

**Sender (reference Python):**

```python
line = json.dumps(message_dict, separators=(",", ":")) + "\n"
sock.sendall(line.encode("utf-8"))
```

`sendall()` is required (rather than `send()`) because `send()` may transmit only part of the buffer.

### 3.3 Receiver Extraction Logic

Each connection has its own receive buffer (a `bytes`/`bytearray`), which persists between `recv()` calls.

1. **Read:** call `recv()` and append the returned bytes to the connection's buffer.
2. **Check for EOF:** if `recv()` returned `b""`, the peer has closed the connection (see Section 6). Stop.
3. **Extract complete messages:** while the buffer contains `\n`:
   1. Split the buffer at the **first** `\n`.
   2. The bytes before it are one complete message. Decode as UTF-8 and parse as JSON, then validate and dispatch it (Section 5).
   3. The bytes after it become the new buffer.
4. **Keep the remainder:** any bytes left with no `\n` are an incomplete message. Leave them in the buffer and return to step 1.

```python
buffer = b""

while True:
    chunk = sock.recv(4096)
    if not chunk:                 # 0-byte read = EOF, peer closed
        handle_disconnect()
        break
    buffer += chunk
    while b"\n" in buffer:
        line, buffer = buffer.split(b"\n", 1)
        if line:                  # ignore empty lines
            handle_line(line)     # decode, parse, validate, dispatch
```

### 3.4 Wire Stream Examples

Notation: `⏎` marks the newline byte `0x0A` on the wire. Line breaks in the examples below are for readability only; on the wire the bytes are contiguous.

**Continuous stream: server to Client 1 during game start and the first round**

```
{"msg_type":"LOBBY_WAIT","timestamp":1790000001}⏎{"msg_type":"GAME_START","player_role":"P1","opponent_alias":"Bob","timestamp":1790000010}⏎{"msg_type":"STATE_UPDATE","status":"WAITING_FOR_OPPONENT","your_selection":"SPOCK","opponent_selection":null,"round_winner":null,"score":{"wins":0,"losses":0,"draws":0},"timestamp":1790000015}⏎{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"SPOCK","opponent_selection":"ROCK","round_winner":"YOU","score":{"wins":1,"losses":0,"draws":0},"timestamp":1790000018}⏎
```

**Example A: coalescing (two messages in one `recv()`)**

The server sends `GAME_START` and then immediately a `STATE_UPDATE`. Both arrive in a single chunk:

```
recv() #1 returns:
{"msg_type":"GAME_START","player_role":"P2","opponent_alias":"Alice","timestamp":1790000010}⏎{"msg_type":"STATE_UPDATE","status":"WAITING_FOR_OPPONENT", ... ,"timestamp":1790000015}⏎
```

| Step | Buffer before | Action | Buffer after |
|---|---|---|---|
| 1 | *(empty)* | Append chunk; buffer holds 2 complete lines | `GAME_START…⏎STATE_UPDATE…⏎` |
| 2 | `GAME_START…⏎STATE_UPDATE…⏎` | Split at first `⏎`; dispatch `GAME_START` | `STATE_UPDATE…⏎` |
| 3 | `STATE_UPDATE…⏎` | Split at first `⏎`; dispatch `STATE_UPDATE` | *(empty)* |
| 4 | *(empty)* | No `⏎` left; call `recv()` again | *(empty)* |

**Example B: fragmentation (one message across two `recv()` calls)**

A client's `MOVE` is split in transit:

```
recv() #1 returns:  {"msg_type":"MOVE","selec
recv() #2 returns:  tion":"LIZARD","timestamp":1790000015}⏎
```

| Step | Buffer before | Action | Buffer after |
|---|---|---|---|
| 1 | *(empty)* | Append chunk #1; no `⏎` found, so nothing to dispatch | `{"msg_type":"MOVE","selec` |
| 2 | `{"msg_type":"MOVE","selec` | Append chunk #2; `⏎` found | `{"msg_type":"MOVE","selection":"LIZARD","timestamp":1790000015}⏎` |
| 3 | *(as above)* | Split at `⏎`; dispatch the complete `MOVE` | *(empty)* |

**Example C: both at once (one complete message plus the start of the next)**

```
recv() #1 returns:  {"msg_type":"MOVE","selection":"ROCK","timestamp":1790000015}⏎{"msg_type":"DISCO
recv() #2 returns:  NNECT","timestamp":1790000030}⏎
```

After `recv()` #1, the `MOVE` is dispatched and `{"msg_type":"DISCO` is held in the buffer. After `recv()` #2, the buffer completes the line and the `DISCONNECT` is dispatched.

---

## 4. Message Specifications

### 4.1 Common Rules

- **Required fields:** every message is a JSON object with at least `msg_type` (string) and `timestamp` (integer).
- **`timestamp`:** Unix epoch time in seconds at which the sender created the message. It is used for logging and trace correlation only; the server does not make game decisions based on it.
- **No client identifiers:** client-to-server messages carry no `player_id`. The server identifies the sender by the TCP connection (socket) the message arrived on. This prevents a client from impersonating the other player by forging an ID field.
- **Personalized server messages:** the server sends each client its own copy of `GAME_START`, `STATE_UPDATE` and `GAME_OVER`, written from that client's perspective (`your_selection` / `opponent_selection`, `"YOU"` / `"OPPONENT"`, wins/losses relative to the recipient). The two clients receive different content for the same game event.
- **Field values:** all enumerated string values are uppercase and case-sensitive.
- **Field order:** field order within a JSON object is not significant.

### 4.2 Message Summary

| # | Message | Direction | Purpose |
|---|---|---|---|
| 1 | `CONNECT` | Client → Server | Join the game room with a player alias |
| 2 | `LOBBY_WAIT` | Server → Client | Tell the first player the server is waiting for a second player |
| 3 | `GAME_START` | Server → each Client | Start the match; assign role P1/P2 and give the opponent's alias |
| 4 | `MOVE` | Client → Server | Submit this round's selection |
| 5 | `STATE_UPDATE` | Server → each Client | Acknowledge a move, or reveal the round result and updated score |
| 6 | `ERROR` | Server → Client | Report an invalid, out-of-turn or malformed message |
| 7 | `DISCONNECT` | Client → Server | Announce an intentional quit |
| 8 | `GAME_OVER` | Server → each Client | Announce the match result (win or forfeit) and final score |

### 4.3 Shared Types

**`Selection`**: one of `"ROCK"`, `"PAPER"`, `"SCISSORS"`, `"LIZARD"`, `"SPOCK"`.

**Win table** (each selection beats exactly two others):

| Selection | Beats |
|---|---|
| ROCK | SCISSORS, LIZARD |
| PAPER | ROCK, SPOCK |
| SCISSORS | PAPER, LIZARD |
| LIZARD | PAPER, SPOCK |
| SPOCK | ROCK, SCISSORS |

**`Score`**: an object of non-negative integers, relative to the recipient:

| Field | Type | Description |
|---|---|---|
| `wins` | integer | Rounds the recipient has won |
| `losses` | integer | Rounds the recipient has lost |
| `draws` | integer | Rounds that ended in a tie |

```json
{"wins": 2, "losses": 1, "draws": 1}
```

---

### 4.4 `CONNECT`

**Direction:** Client → Server
**Purpose:** The first message a client sends after opening its TCP connection. It requests a seat in the game room under the given alias.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"CONNECT"` |
| `alias` | string | yes | Display name chosen by the player; must be non-empty |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
{"msg_type":"CONNECT","alias":"Alice","timestamp":1790000000}
```

**Server response:** `LOBBY_WAIT` if this is the first player; `GAME_START` to both clients if this is the second.

---

### 4.5 `LOBBY_WAIT`

**Direction:** Server → Client
**Purpose:** Tells the first connected player that their `CONNECT` was accepted and the server is waiting for an opponent.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"LOBBY_WAIT"` |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
{"msg_type":"LOBBY_WAIT","timestamp":1790000001}
```

---

### 4.6 `GAME_START`

**Direction:** Server → each Client (personalized)
**Purpose:** Sent to both clients once the second player connects. It assigns each player a role and introduces the opponent. Round 1 begins immediately, so clients should prompt for a selection on receipt.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"GAME_START"` |
| `player_role` | string | yes | `"P1"` (first to connect) or `"P2"` (second to connect) |
| `opponent_alias` | string | yes | The other player's alias |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
// to Alice (connected first)
{"msg_type":"GAME_START","player_role":"P1","opponent_alias":"Bob","timestamp":1790000010}
// to Bob (connected second)
{"msg_type":"GAME_START","player_role":"P2","opponent_alias":"Alice","timestamp":1790000010}
```

---

### 4.7 `MOVE`

**Direction:** Client → Server
**Purpose:** Submits the player's selection for the current round. Each player sends exactly one `MOVE` per round.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"MOVE"` |
| `selection` | string | yes | A `Selection` value |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
{"msg_type":"MOVE","selection":"SPOCK","timestamp":1790000015}
```

**Server response:** `STATE_UPDATE` (accepted) or `ERROR` (rejected; see Section 5).

---

### 4.8 `STATE_UPDATE`

**Direction:** Server → Client(s) (personalized)
**Purpose:** Has two forms, distinguished by `status`:

- **`WAITING_FOR_OPPONENT`:** sent only to the player whose move was just accepted, while the opponent has not yet moved. It confirms receipt and echoes the player's own selection. It never reveals any information about the opponent's move. `score` reflects results through the previous round.
- **`ROUND_RESULT`:** sent to both players once both moves are in. It reveals both selections, the round winner and the updated score. If neither player has reached 3 wins, a new round begins immediately and clients should prompt for the next selection.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"STATE_UPDATE"` |
| `status` | string | yes | `"WAITING_FOR_OPPONENT"` or `"ROUND_RESULT"` |
| `your_selection` | string | yes | The recipient's `Selection` this round |
| `opponent_selection` | string or null | yes | The opponent's `Selection`; `null` when `status` is `WAITING_FOR_OPPONENT` |
| `round_winner` | string or null | yes | `"YOU"`, `"OPPONENT"` or `"DRAW"`; `null` when `status` is `WAITING_FOR_OPPONENT` |
| `score` | `Score` | yes | Recipient-relative score |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
// Alice picked first; Bob has not moved yet (sent to Alice only)
{"msg_type":"STATE_UPDATE","status":"WAITING_FOR_OPPONENT","your_selection":"SPOCK","opponent_selection":null,"round_winner":null,"score":{"wins":1,"losses":0,"draws":2},"timestamp":1790000015}

// Bob picked ROCK; round resolved (sent to Alice)
{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"SPOCK","opponent_selection":"ROCK","round_winner":"YOU","score":{"wins":2,"losses":0,"draws":2},"timestamp":1790000018}

// Same round (sent to Bob)
{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"ROCK","opponent_selection":"SPOCK","round_winner":"OPPONENT","score":{"wins":0,"losses":2,"draws":2},"timestamp":1790000018}

// A tied round (both picked PAPER, sent to Alice)
{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"PAPER","opponent_selection":"PAPER","round_winner":"DRAW","score":{"wins":2,"losses":0,"draws":3},"timestamp":1790000025}
```

When the round that ends the match is resolved, the server sends the `ROUND_RESULT` for that round and then `GAME_OVER`.

---

### 4.9 `ERROR`

**Direction:** Server → Client (only the offending client)
**Purpose:** Reports that the client's message was rejected. The server's game state is **unchanged**, the connection stays open, and the client may send a corrected message.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"ERROR"` |
| `error_code` | string | yes | One of the codes below |
| `error_message` | string | yes | Human-readable explanation for display |
| `timestamp` | integer | yes | Unix epoch seconds |

| `error_code` | Meaning | Example cause |
|---|---|---|
| `MALFORMED` | The message failed parsing or schema validation | Invalid JSON; invalid UTF-8; missing `msg_type` or a required field; wrong field type; unknown or server-only `msg_type` |
| `INVALID_MOVE` | The `MOVE` is well-formed but its value is not legal | `"selection":"BANANA"` |
| `DUPLICATE_MOVE` | The player already submitted a move this round (out-of-turn) | Second `MOVE` before the opponent has moved |
| `NOT_IN_GAME` | The message is not valid in the current game state (out-of-turn) | `MOVE` sent while in the lobby or after `GAME_OVER` |

```json
{"msg_type":"ERROR","error_code":"MALFORMED","error_message":"Missing required field: selection","timestamp":1790000014}
{"msg_type":"ERROR","error_code":"INVALID_MOVE","error_message":"Selection must be one of ROCK, PAPER, SCISSORS, LIZARD, SPOCK","timestamp":1790000014}
{"msg_type":"ERROR","error_code":"DUPLICATE_MOVE","error_message":"You already selected a move this round","timestamp":1790000016}
{"msg_type":"ERROR","error_code":"NOT_IN_GAME","error_message":"The game has not started yet","timestamp":1790000005}
```

---

### 4.10 `DISCONNECT`

**Direction:** Client → Server
**Purpose:** Announces that the player is quitting intentionally. After sending it, the client closes its socket. See Section 6 for how the server reacts in each state.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"DISCONNECT"` |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
{"msg_type":"DISCONNECT","timestamp":1790000030}
```

---

### 4.11 `GAME_OVER`

**Direction:** Server → each remaining Client (personalized)
**Purpose:** Announces the end of the match, either because a player reached 3 round wins or because a player left mid-game. After sending it, the server closes the connections and resets for a new game (Section 7).

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"GAME_OVER"` |
| `winner` | string | yes | `"YOU"` or `"OPPONENT"` |
| `reason` | string | yes | `"WIN"` (a player reached 3 round wins) or `"FORFEIT"` (the opponent disconnected mid-game) |
| `score` | `Score` | yes | Recipient-relative final score |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
// Match won normally (sent to the winner)
{"msg_type":"GAME_OVER","winner":"YOU","reason":"WIN","score":{"wins":3,"losses":1,"draws":2},"timestamp":1790000040}

// Same match (sent to the loser)
{"msg_type":"GAME_OVER","winner":"OPPONENT","reason":"WIN","score":{"wins":1,"losses":3,"draws":2},"timestamp":1790000040}

// Opponent disconnected mid-game (sent to the remaining player only)
{"msg_type":"GAME_OVER","winner":"YOU","reason":"FORFEIT","score":{"wins":1,"losses":2,"draws":0},"timestamp":1790000033}
```

A draw outcome is not possible at the match level: tied rounds are discarded, so the match always ends with one player at 3 wins or with a forfeit.

---

## 5. Message Validation & Dispatch

The server processes each complete line extracted by the framing layer in this order. The first check that fails produces an `ERROR` to the sender; processing of that line stops, the game state is unchanged, and the server returns to reading.

| Order | Check | Failure response |
|---|---|---|
| 1 | Line decodes as UTF-8 | `ERROR` `MALFORMED` |
| 2 | Line parses as a JSON object | `ERROR` `MALFORMED` |
| 3 | `msg_type` is present and is a client-to-server type (`CONNECT`, `MOVE`, `DISCONNECT`) | `ERROR` `MALFORMED` |
| 4 | All required fields for that type are present with the correct types | `ERROR` `MALFORMED` |
| 5 | The message is allowed in the current game state | `ERROR` `NOT_IN_GAME` |
| 6 | (`MOVE` only) The sender has not already moved this round | `ERROR` `DUPLICATE_MOVE` |
| 7 | (`MOVE` only) `selection` is a valid `Selection` | `ERROR` `INVALID_MOVE` |

Parsing errors (such as `json.JSONDecodeError`, `UnicodeDecodeError` or `KeyError`) must be caught and converted to `ERROR` responses. A bad message from one client must never crash the server or affect the other client.

---

## 6. Connection Termination & Socket Lifecycle

A connection can end in three ways. The server maps all three to the same internal event, **`CLIENT_DISCONNECTED`**, which drives the state machine (see `fsm_specification.md`).

### 6.1 Graceful: Application-Layer `DISCONNECT`

1. The client sends `{"msg_type":"DISCONNECT","timestamp":...}⏎`.
2. The client calls `sock.close()`, which starts the TCP FIN handshake.
3. The server receives the `DISCONNECT`, raises `CLIENT_DISCONNECTED` for that player, and closes its end of the socket.

This is the preferred path: the server learns *why* the player left before the transport closes.

### 6.2 Graceful: TCP FIN without `DISCONNECT` (0-Byte EOF)

If a client process exits normally or closes its socket without sending `DISCONNECT` (for example, the user presses Ctrl+C and the OS closes the socket), the operating system still sends a TCP FIN.

When the peer has closed cleanly, **`recv()` does not raise an exception; it returns 0 bytes (`b""`).** This is the POSIX end-of-file indicator.

**Every receive loop must check for this.** A `recv()` on a closed socket returns `b""` immediately on every call, forever. A loop that does not test for it will spin at 100% CPU without ever blocking.

```python
chunk = sock.recv(4096)
if not chunk:                       # b"" -> peer sent FIN
    log.info("Peer closed connection (EOF)")
    sock.close()
    raise_event("CLIENT_DISCONNECTED", player)
```

Any bytes still in the receive buffer without a terminating `\n` at EOF are an incomplete message and are discarded.

### 6.3 Abrupt: TCP RST, Crashes & Network Drops

If a client is killed (`kill -9`), loses power, or its network path fails (for example, a link is cut in CML), no FIN handshake occurs. The server learns of the failure in one of these ways:

| Signal | When it occurs |
|---|---|
| `ConnectionResetError` | `recv()` or `send()` after the peer's host responds with TCP RST (e.g., process killed while its host stays up) |
| `BrokenPipeError` | `send()`/`sendall()` to a socket whose remote end has already closed |
| `ConnectionAbortedError` | The local OS aborts the connection (e.g., retransmission failure) |
| `TimeoutError` / `socket.timeout` | The idle timeout (Section 6.4) expires |

All socket reads **and writes**, including broadcasts of `STATE_UPDATE` and `GAME_OVER`, are wrapped so these exceptions are caught and converted to `CLIENT_DISCONNECTED` rather than crashing the server:

```python
try:
    chunk = sock.recv(4096)
    if not chunk:
        raise_event("CLIENT_DISCONNECTED", player)
except (ConnectionResetError, BrokenPipeError,
        ConnectionAbortedError, TimeoutError) as e:
    log.warning(f"Connection lost abruptly: {e}")
    raise_event("CLIENT_DISCONNECTED", player)
```

### 6.4 Idle Timeout

A severed link with no FIN or RST can leave a socket waiting forever. To detect this, the server applies an **idle timeout of 120 seconds** during a game:

- **When it applies:** while the server is waiting for a `MOVE` from a player, if no message arrives from that player within 120 seconds, the server treats it as `CLIENT_DISCONNECTED`.
- **When it does not apply:** the timeout does not apply in the lobby, and it does not apply to a player who has already moved and is legitimately waiting for the opponent.
- **Silent drop of a player who has already moved:** this is detected when the server next tries to write to that socket (`BrokenPipeError`/`ConnectionResetError`) or when the round ends.

### 6.5 Server Response by State

| Server state when `CLIENT_DISCONNECTED` occurs | Server action |
|---|---|
| Lobby (one player connected and waiting) | Close the socket, discard the player, and return to waiting for players. No message is sent. |
| Mid-game (game started, no winner yet) | Close the leaving player's socket. Send the remaining player `GAME_OVER` with `winner: "YOU"` and `reason: "FORFEIT"`, then close that socket and run cleanup. |
| After `GAME_OVER` has been sent | Ignore; the connection is already being closed during cleanup. |

The forfeit `GAME_OVER` is sent with the same exception protection as any other write, so if the remaining player has also dropped, the server proceeds directly to cleanup.

### 6.6 Client Handling of Server Loss

If the client's `recv()` returns `b""`, or it catches `ConnectionResetError`, `BrokenPipeError` or `ConnectionAbortedError` on the server connection, the client prints a notice that the connection to the server was lost and exits. The client does not attempt to reconnect automatically.

---

## 7. Connection Lifecycle Summary

```
Client                                  Server
  |---- TCP 3-way handshake ------------->|
  |---- CONNECT ------------------------->|
  |<--- LOBBY_WAIT -----------------------|   (first player only)
  |<--- GAME_START -----------------------|   (once second player connects)
  |---- MOVE ---------------------------->|
  |<--- STATE_UPDATE (WAITING_FOR_OPP.) --|   (if opponent hasn't moved)
  |<--- STATE_UPDATE (ROUND_RESULT) ------|   (once both moved)
  |        ... repeat rounds ...          |
  |<--- GAME_OVER ------------------------|
  |<--- TCP FIN (server closes) ----------|
```

**Post-game reset:** after `GAME_OVER`, the server closes both connections, resets all game state (scores, roles, pending moves) and returns to waiting for players. To play again, players reconnect and send a new `CONNECT`. No separate rematch message is needed.
