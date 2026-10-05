## Transport & Serialization

| Property | Value |
|---|---|
| Transport | TCP |
| Serialization | JSON (Option A) |
| Character encoding | UTF-8 |
| Framing | Newline-delimited JSON, with one compact JSON object per line |
| Termination | '\n' at end of message. This is safe due to how JSON requires newline to be escaped. |

---

## Framing Rules

### Wire Stream Examples

Note: `⏎` is a newline character on the wire stream.

**Continuous stream: server to Client 1 during game start and the first round**

```
{"msg_type":"LOBBY_WAIT","timestamp":1790000001}⏎{"msg_type":"GAME_START","player_role":"P1","opponent_alias":"Lenny","timestamp":1790000010}⏎{"msg_type":"STATE_UPDATE","status":"WAITING_FOR_OPPONENT","your_selection":"SPOCK","opponent_selection":null,"round_winner":null,"score":{"wins":0,"losses":0,"draws":0},"timestamp":1790000015}⏎{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"SPOCK","opponent_selection":"ROCK","round_winner":"YOU","score":{"wins":1,"losses":0,"draws":0},"timestamp":1790000018}⏎
```

## Message Specifications

### Common Rules

- **Required fields:** every message is a JSON object with at least `msg_type` (string) and `timestamp` (integer).
- **`timestamp`:** Unix epoch time in seconds at which the sender created the message.
- **No client identifiers:** TCP socket is used by the server to identify which client it is receiving from.
- **Personalized server messages:** the server sends each client its own copy of `GAME_START`, `STATE_UPDATE` and `GAME_OVER`, written from that client's perspective (`your_selection` / `opponent_selection`, `"YOU"` / `"OPPONENT"`, wins/losses relative to the recipient). The two clients receive different content for the same game event.

### `CONNECT`

**Direction:** Client → Server
**Purpose:** The first message a client sends after opening its TCP connection. It requests a seat in the game room under the given alias.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"CONNECT"` |
| `alias` | string | yes | Display name chosen by the player; must be non-empty |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
{"msg_type":"CONNECT","alias":"Shelly","timestamp":1790000000}
```

**Server response:** `LOBBY_WAIT` if this is the first player; `GAME_START` to both clients if this is the second.

---

### `LOBBY_WAIT`

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

### `GAME_START`

**Direction:** Server → each Client (personalized)
**Purpose:** Sent to both clients once the second player connects. It assigns each player a role and introduces the opponent. Round 1 begins immediately, so clients should prompt for a selection on receipt.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"GAME_START"` |
| `player_role` | string | yes | `"P1"` (first to connect) or `"P2"` (second to connect) |
| `opponent_alias` | string | yes | The other player's alias |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
// to Shelly (connected first)
{"msg_type":"GAME_START","player_role":"P1","opponent_alias":"Lenny","timestamp":1790000010}
// to Lenny (connected second)
{"msg_type":"GAME_START","player_role":"P2","opponent_alias":"Shelly","timestamp":1790000010}
```

---

### `MOVE`

**Direction:** Client → Server
**Purpose:** Submits the player's selection for the current round. Each player sends exactly one `MOVE` per round.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"MOVE"` |
| `selection` | string | yes | '"ROCK"', '"PAPER"', '"SCISSORS"', '"LIZARD"', or '"SPOCK"' |
| `timestamp` | integer | yes | Unix epoch seconds |

```json
{"msg_type":"MOVE","selection":"SPOCK","timestamp":1790000015}
```

**Server response:** `STATE_UPDATE` (accepted) or `ERROR` (rejected).

---

### `STATE_UPDATE`

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
// Shelly picked first; Lenny has not moved yet (sent to Shelly only)
{"msg_type":"STATE_UPDATE","status":"WAITING_FOR_OPPONENT","your_selection":"SPOCK","opponent_selection":null,"round_winner":null,"score":{"wins":1,"losses":0,"draws":2},"timestamp":1790000015}

// Lenny picked ROCK; round resolved (sent to Shelly)
{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"SPOCK","opponent_selection":"ROCK","round_winner":"YOU","score":{"wins":2,"losses":0,"draws":2},"timestamp":1790000018}

// Same round (sent to Lenny)
{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"ROCK","opponent_selection":"SPOCK","round_winner":"OPPONENT","score":{"wins":0,"losses":2,"draws":2},"timestamp":1790000018}

// A tied round (both picked PAPER, sent to Shelly)
{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"PAPER","opponent_selection":"PAPER","round_winner":"DRAW","score":{"wins":2,"losses":0,"draws":3},"timestamp":1790000025}
```

When the round that ends the match is resolved, the server sends the `ROUND_RESULT` for that round and then `GAME_OVER`.

---

### `ERROR`

**Direction:** Server → Client (only the offending client)
**Purpose:** Reports that the client's message was rejected. The server's game state is unchanged, the connection stays open, and the client may send a corrected message.

| Field | Type | Required | Description |
|---|---|---|---|
| `msg_type` | string | yes | `"ERROR"` |
| `error_code` | string | yes | One of the codes below |
| `error_message` | string | yes | Human-readable explanation for display |
| `timestamp` | integer | yes | Unix epoch seconds |

| `error_code` | Meaning | Example cause |
|---|---|---|
| `MALFORMED` | The message failed parsing or schema validation | Invalid JSON; invalid UTF-8; missing `msg_type` or a required field; wrong field type; unknown or server-only `msg_type` |
| `INVALID_MOVE` | The `MOVE` is well-formed but its value is not legal | `"selection":"STAR WARS"` |
| `DUPLICATE_MOVE` | The player already submitted a move this round (out-of-turn) | Second `MOVE` before the opponent has moved |
| `NOT_IN_GAME` | The message is not valid in the current game state (out-of-turn) | `MOVE` sent while in the lobby or after `GAME_OVER` |

```json
{"msg_type":"ERROR","error_code":"MALFORMED","error_message":"Missing required field: selection","timestamp":1790000014}
{"msg_type":"ERROR","error_code":"INVALID_MOVE","error_message":"Selection must be one of ROCK, PAPER, SCISSORS, LIZARD, SPOCK","timestamp":1790000014}
{"msg_type":"ERROR","error_code":"DUPLICATE_MOVE","error_message":"You already selected a move this round","timestamp":1790000016}
{"msg_type":"ERROR","error_code":"NOT_IN_GAME","error_message":"The game has not started yet","timestamp":1790000005}
```

---

### `DISCONNECT`

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

### `GAME_OVER`

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

A draw outcome is not possible because the win condition is based purely on number of wins.

---

## Message Validation & Dispatch

The server processes each complete line extracted by the framing layer in this order. The first check that fails produces an `ERROR` to the sender; processing of that line stops, the game state is unchanged, and the server returns to reading.

| Order | Check | Failure response |
|---|---|---|
| 1 | Line decodes as UTF-8 | `ERROR` `MALFORMED` |
| 2 | Line parses as a JSON object | `ERROR` `MALFORMED` |
| 3 | `msg_type` is present and is a client-to-server type (`CONNECT`, `MOVE`, etc.) | `ERROR` `MALFORMED` |
| 4 | All required fields for that type are present with the correct types | `ERROR` `MALFORMED` |
| 5 | The message is allowed in the current game state | `ERROR` `NOT_IN_GAME` |
| 6 | (`MOVE` only) The sender has not already moved this round | `ERROR` `DUPLICATE_MOVE` |
| 7 | (`MOVE` only) `selection` is a valid `Selection` | `ERROR` `INVALID_MOVE` |

Parsing errors (such as `json.JSONDecodeError`, `UnicodeDecodeError` or `KeyError`) must be caught and converted to `ERROR` responses, which are handled by the server and never seen by the other client.

---

## Connection Termination & Socket Lifecycle

### Graceful Disconnect

1. The client sends `{"msg_type":"DISCONNECT","timestamp":...}⏎`.
2. The client calls `sock.close()`, which starts the TCP FIN handshake.
3. The server receives the `DISCONNECT`, raises `CLIENT_DISCONNECTED` for that player, and closes its end of the socket.

This is the preferred path: the server learns *why* the player left before the transport closes.

### Graceful Disconnect (Rage Quit)

If a client process exits normally or closes its socket without sending `DISCONNECT` (for example, the user presses Ctrl+C and closes the socket), the operating system still sends a TCP FIN.

### Abrupt: TCP RST, Crashes & Network Drops

If a client unexpectedly disconnects, the server learns of the failure in one of these ways:

| Signal | When it occurs |
|---|---|
| `ConnectionResetError` | `recv()` or `send()` after the peer's host responds with TCP RST (e.g., process killed while its host stays up) |
| `BrokenPipeError` | `send()`/`sendall()` to a socket whose remote end has already closed |
| `ConnectionAbortedError` | The local OS aborts the connection (e.g., retransmission failure) |
| `TimeoutError` / `socket.timeout` | Idle timeout (60 seconds expires)

All socket reads and writes are wrapped so these exceptions are caught and converted to `CLIENT_DISCONNECTED` rather than crashing the server:

### Server Response by State

| Server state when `CLIENT_DISCONNECTED` occurs | Server action |
|---|---|
| Lobby (one player connected and waiting) | Close the socket, discard the player, and return to waiting for players. No message is sent. |
| Mid-game (game started, no winner yet) | Close the leaving player's socket. Send the remaining player `GAME_OVER` with `winner: "YOU"` and `reason: "FORFEIT"`. |
| After `GAME_OVER` has been sent | Ignore; the connection is already being closed during cleanup. |

### Client Handling of Server Loss

If the client's `recv()` returns `b""`, or it catches `ConnectionResetError`, `BrokenPipeError` or `ConnectionAbortedError` on the server connection, the client prints a notice that the connection to the server was lost and exits. The client does not attempt to reconnect automatically.

---

**Post-game reset:** after `GAME_OVER`, the server closes both connections, resets all game state (scores, roles, pending moves) and returns to waiting for players. To play again, players reconnect and send a new `CONNECT`. No separate rematch message is needed.
