CC ?= cc

CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -fsanitize=address -Wswitch-enum -g
LDFLAGS:= -lm

TARGET := bin/storthc

SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c,obj/%.o,$(SRC))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj/%.o: src/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@ $(LDFLAGS)

obj:
	mkdir -p obj

bin:
	mkdir -p bin

clean:
	rm -rf obj bin
