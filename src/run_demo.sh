#!/bin/sh
# run_demo.sh - Scripted localhost demo: one server, two clients, full match.
#
# Builds the binaries, starts the server on 127.0.0.1, connects two clients
# whose keyboard input is fed from timed scripts, and writes each process's
# console output to logs/. Usage:  sh run_demo.sh [port]
#
# Match played (first to 3 wins):
#   R1 SPOCK vs ROCK      -> Shelly      R2 PAPER vs PAPER    -> draw
#   R3 ROCK vs PAPER      -> Lenny       R4 LIZARD vs SPOCK   -> Shelly
#   R5 SCISSORS vs LIZARD -> Shelly wins the match 3-1
set -e
PORT=${1:-8080}
cd "$(dirname "$0")"
make -s
mkdir -p logs

./server -p "$PORT" > logs/server.log 2>&1 &
SERVER_PID=$!
sleep 1

# Player 1 (Shelly). "banana" shows local input validation.
{
    sleep 3;   echo 5            # SPOCK
    sleep 2;   echo banana; echo 2   # invalid, then PAPER
    sleep 2;   echo rock
    sleep 2;   echo L            # LIZARD
    sleep 2;   echo scissors
    sleep 3
} | ./client -i localhost -p "$PORT" -n Shelly > logs/client_shelly.log 2>&1 &
P1=$!
sleep 1

# Player 2 (Lenny), answers ~1 s after Shelly each round.
{
    sleep 3;   echo 1            # ROCK
    sleep 2;   echo paper
    sleep 2;   echo 2            # PAPER
    sleep 2;   echo spock
    sleep 2;   echo 4            # LIZARD
    sleep 3
} | ./client -i localhost -p "$PORT" -n Lenny > logs/client_lenny.log 2>&1 &
P2=$!

wait $P1 $P2 || true
sleep 1
kill $SERVER_PID 2>/dev/null || true
echo "Done. Logs written to logs/server.log, logs/client_shelly.log, logs/client_lenny.log"
