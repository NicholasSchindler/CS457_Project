# Makefile - RPSLS client and server (CS 457 Final Project)
#
# On Alpine Linux, install the toolchain first:
#     apk add build-base
#
# Then:
#     make            build ./server and ./client
#     make clean      remove build output

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
LDFLAGS ?=
LDLIBS  := -pthread

all: server client

server: server.o common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

client: client.o common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

clean:
	rm -f server client *.o

.PHONY: all clean
