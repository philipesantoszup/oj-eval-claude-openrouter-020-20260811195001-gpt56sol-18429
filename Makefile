CC := gcc
CFLAGS := -std=gnu11 -O2 -Wall -Wextra

.PHONY: all clean
all: code

code: main.c buddy.c buddy.h utils.h
	$(CC) $(CFLAGS) -o $@ main.c buddy.c

clean:
	rm -f code
