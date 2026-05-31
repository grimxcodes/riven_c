CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude -g
LDFLAGS = -lm -lpthread

SRC = src/lexer.c src/parser.c src/value.c src/stdlib.c src/interpreter.c src/main.c
OBJ = $(SRC:.c=.o)
BIN = rvn

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c include/riven.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
