.PHONY: clean run

CC      := gcc
INCLUDE := -Isrc
CFLAGS  := -g -std=gnu99 -Wall -Wextra -Wpedantic -O2 -Os
LDFLAGS := $(shell pkg-config --libs openssl)

OBJDIR := src
OBJS   := $(addprefix $(OBJDIR)/,cache.o netutil.o main.o)
executable := bin/ombud


CACHE_DIR := cache-ombud


all: $(executable)

$(executable): $(OBJS)
	mkdir -p bin
	$(CC) $(INCLUDE) $(CFLAGS) -o $(executable) $(OBJS) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(INCLUDE) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(executable)
	rm -f $(OBJS)
	rm -rf $(CACHE_DIR)
	rm -f valgrind.log

run: $(executable)
	$(executable)

valgrind: $(executable)
	valgrind -v --leak-check=full --show-leak-kinds=all --trace-children=yes \
	         --undef-value-errors=no --log-file=valgrind.log \
	         $(executable) &
	sleep 2
	# a few simple test cases
	echo localhost:22 | nc localhost 8090 &
	echo badservice | nc localhost 8090 &
	echo : | nc localhost 8090 &
	echo | nc localhost 8090 &
	sleep 2
	-pkill -f valgrind &
	@echo
	@echo "\tSee valgrind.log for results"
	@echo
