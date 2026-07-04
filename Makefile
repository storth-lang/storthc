CC ?= cc

CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -fsanitize=address -g

TARGET := bin/storthc

SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c,obj/%.o,$(SRC))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $^

obj/%.o: src/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj:
	mkdir -p obj

bin:
	mkdir -p bin

clean:
	rm -rf obj bin
