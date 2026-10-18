# Move, Score & State Broadcast Sequence

One round of play on the multi-threaded server. Each client has its own detached worker thread (`pthread_create`) blocked in `recv()` on that client's socket. All shared game state is guarded by a single `pthread_mutex_t game_lock`.

```mermaid
sequenceDiagram
    autonumber
    actor C1 as Client 1 (P1)
    participant T1 as Worker Thread 1
    participant GS as Shared Game State
    participant T2 as Worker Thread 2
    actor C2 as Client 2 (P2)

    Note over T1,T2: Both worker threads blocked in recv() on their own socket.<br/>Game state is WAITING_FOR_MOVES.

    C1->>T1: MOVE (selection: SPOCK)
    Note over T1: Frame on newline, parse JSON, check schema<br/>(validation steps 1-4). Uses only this<br/>thread's receive buffer, so no lock is needed.
    alt Malformed message
        T1-->>C1: ERROR (MALFORMED)
    else Well-formed MOVE
        rect rgb(255, 243, 205)
            T1->>GS: pthread_mutex_lock(game_lock)
            Note over T1,GS: Critical section: validation steps 5-7
            alt Game is not in WAITING_FOR_MOVES
                T1-->>C1: ERROR (NOT_IN_GAME)
            else P1 already moved this round
                T1-->>C1: ERROR (DUPLICATE_MOVE)
            else Selection is not a valid item
                T1-->>C1: ERROR (INVALID_MOVE)
            else Valid move
                T1->>GS: selection[P1] = SPOCK
                Note over GS: selection[P2] still empty,<br/>so the round cannot be scored yet
                T1-->>C1: STATE_UPDATE (WAITING_FOR_OPPONENT)
            end
            T1->>GS: pthread_mutex_unlock(game_lock)
        end
    end

    C2->>T2: MOVE (selection: ROCK)
    Note over T2: Frame, parse, check schema (no lock)
    rect rgb(255, 243, 205)
        T2->>GS: pthread_mutex_lock(game_lock)
        Note over T2,GS: Steps 5-7 pass. Store selection[P2] = ROCK
        Note over GS: Both selections present, so SCORING:<br/>SPOCK beats ROCK, wins[P1] += 1<br/>Clear selection[P1] and selection[P2]
        alt Neither player has 3 wins
            Note over GS: NEW_ROUND. State stays WAITING_FOR_MOVES
            par Broadcast to both clients while game_lock is held
                T2-->>C1: STATE_UPDATE (ROUND_RESULT, round_winner: YOU, score 1-0-0)
            and
                T2-->>C2: STATE_UPDATE (ROUND_RESULT, round_winner: OPPONENT, score 0-1-0)
            end
        else A player reached 3 wins
            Note over GS: State = GAME_OVER
            par Broadcast to both clients while game_lock is held
                T2-->>C1: GAME_OVER (winner: YOU, reason: WIN, final score)
            and
                T2-->>C2: GAME_OVER (winner: OPPONENT, reason: WIN, final score)
            end
        end
        T2->>GS: pthread_mutex_unlock(game_lock)
    end
```

Scores are shown as wins-losses-draws from the recipient's point of view.

## Narrative

1. **Move request.** Player 1 sends `MOVE`. Worker Thread 1 frames and parses it using only its own receive buffer, so validation steps 1-4 run without the lock. A malformed message gets `ERROR MALFORMED` and the game state is unchanged.
2. **Server authority validation.** Thread 1 takes `game_lock` before reading any shared state. Under the lock it checks the game phase (`NOT_IN_GAME`), whether P1 already moved this round (`DUPLICATE_MOVE`), and whether the selection is legal (`INVALID_MOVE`). Any failure sends `ERROR` to P1 only and leaves the state untouched.
3. **Out-of-turn protection.** RPSLS has simultaneous moves rather than alternating turns, so a player's "turn" is the single move they get each round. A second `MOVE` before the round resolves is rejected with `DUPLICATE_MOVE`, and a `MOVE` outside `WAITING_FOR_MOVES` is rejected with `NOT_IN_GAME`.
4. **State mutation.** A valid move is stored in `selection[P1]`. P2 has not moved, so P1 gets `STATE_UPDATE WAITING_FOR_OPPONENT` and Thread 1 releases the lock.
5. **Round completion.** When P2's move arrives, Thread 2 takes the lock, validates, and stores `selection[P2]`. The check that both selections are present happens in the same critical section as the store. That prevents both threads from each seeing the other as "not yet moved", which would leave the round unscored, or scoring it twice.
6. **Scoring.** Still holding the lock, Thread 2 decides the round, updates `wins[]` / `draws`, and clears both selections for the next round.
7. **Synchronous broadcast.** Thread 2 sends a personalized `STATE_UPDATE ROUND_RESULT`, or `GAME_OVER` if someone reached 3 wins, to **both** client sockets before releasing the lock. Because no other thread can change the state until both sends finish, both clients receive the same round result and score, and neither can see a newer state first.
