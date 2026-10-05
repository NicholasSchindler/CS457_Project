## Transport & Serialization

| Property | Value |
|---|---|
| Transport | TCP |
| Serialization | JSON (Option A) |
| Character encoding | UTF-8 |
| Framing | Newline-delimited JSON, with one JSON object per line |
| Termination | '\n' at end of message. |

---

## Framing Rules

### Wire Stream Examples

Note: `⏎` is a newline character on the wire stream.

**Example stream from server to P1 after P1 initially joins**

```
{"msg_type":"LOBBY_WAIT","timestamp":1790000001}⏎{"msg_type":"GAME_START","player_role":"P1","opponent_alias":"Lenny","timestamp":1790000010}⏎{"msg_type":"STATE_UPDATE","status":"WAITING_FOR_OPPONENT","your_selection":"SPOCK","opponent_selection":null,"round_winner":null,"score":{"wins":0,"losses":0,"draws":0},"timestamp":1790000015}⏎{"msg_type":"STATE_UPDATE","status":"ROUND_RESULT","your_selection":"SPOCK","opponent_selection":"ROCK","round_winner":"YOU","score":{"wins":1,"losses":0,"draws":0},"timestamp":1790000018}⏎
```

## Message Specifications

### Common Rules

- **No client identifiers:** TCP socket is used by the server to identify which client it is receiving from.
- **Personalized server messages:** The server generates certain messages for each client (i.e., you are P1 and opponent is P2 vs your are P2 and opponent is P1)

### `CONNECT`

**Direction:** Client → Server
**Purpose:** Initial message from client to server requesting to join game.

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
**Purpose:** Tells first player their connection was accepted and that they are waiting for another player.

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
**Purpose:** Assigns player role, tells opponent name, and starts game.

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
**Purpose:** Tells server what the player selected.

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
**Purpose:** 
- **`WAITING_FOR_OPPONENT`:** sent to the first player a 'MOVE' message is received from.
- **`ROUND_RESULT`:** sent to both players once both have moved, provides state at end of round.

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

---

### `ERROR`

**Direction:** Server → Client (only the client that sent a bad message)
**Purpose:** Rejects client message. Does not change game state, so client can send a corrected message.

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
**Purpose:** Player deliberately sends a message to quit.

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
**Purpose:** Communicates to client the final game state.

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
