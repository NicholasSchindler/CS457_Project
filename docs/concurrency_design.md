### Server Concurrency Model Selection

Option A: Multi-Threading will be used.

The server will have a dedicated thread that runs the game.

`socket` and `bind`, followed by `listen`, will be used by the server when waiting for clients.

`accept` and `pthread` will then be used to accept clients when they connect (assuming that the game is not full).

The only shared section will be a single variable for each client, shared between the respective client thread and the server's main loop. The client thread will use `mutex` when updating the variable, and the server will use `mutex` when updating it.

As an additional design safeguard, the server is going to be doing the majority of the game state management. Clients will only provide their selection for the current round and initiating disconnections.

---

### Turn Enforcement Logic

In the previous section a variable storing a turn's move was discussed. Prior to updating the variable, the client thread will inspect the variable. If it is in the default state it will update with the turn's move. If it is in any other state, the client thread will not update the variable.

The server will use `mutex` on both clients' variables when updating game state between turns, and reset both to default so that the next round's input will be accepted.

---
