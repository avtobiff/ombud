.PHONY: clean

CC      := gcc
CFLAGS  := -g -std=gnu99 -Wall -Wextra -Wpedantic -O2 -Os
LDFLAGS :=

OBJDIR := src
OBJS   := $(addprefix $(OBJDIR)/,main.o)
executable := bin/ombud


all: $(executable)

$(executable): $(OBJS)
	$(CC) $(CFLAGS) -o $(executable) $(OBJS) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(executable)
	rm -f $(OBJS)
