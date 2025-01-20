CC = gcc
CFLAGS = -Wall -pedantic -pthread

all: server client

server: server.c
	$(CC) $(CFLAGS) server.c -o bin/server

client: client.c
	$(CC) $(CFLAGS) client.c -o bin/client -lm

clean:
	rm -f bin/server bin/client
