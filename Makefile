.PHONY: clean run

CC      := gcc
INCLUDE := -Isrc
CFLAGS  := -g -std=gnu99 -Wall -Wextra -Wpedantic -O2 -Os
LDFLAGS := $(shell pkg-config --libs openssl)

OBJDIR := src
OBJS   := $(addprefix $(OBJDIR)/,cache.o main.o)
executable := bin/ombud


CACHE_DIR := cache-ombud


all: $(executable)

$(executable): $(OBJS)
	$(CC) $(INCLUDE) $(CFLAGS) -o $(executable) $(OBJS) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(INCLUDE) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(executable)
	rm -f $(OBJS)
	rm -rf $(CACHE_DIR)

run: all
	bin/ombud
