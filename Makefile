CC ?= cc

SRCROOT := src
OBJROOT := obj
BINDIR := bin

CFLAGS := -std=c11 -I$(SRCROOT) -Wall -Wextra -Wpedantic -fsanitize=address -Wswitch-enum -Warray-bounds=2 -g
LDLIBS := -lm
TARGET := $(BINDIR)/storthc

SRC := $(shell find $(SRCROOT) -name '*.c')
OBJ := $(patsubst $(SRCROOT)/%.c,$(OBJROOT)/%.o,$(SRC))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJROOT)/%.o: $(SRCROOT)/%.c | $(OBJROOT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJROOT):
	mkdir -p $(OBJROOT)

clean:
	rm -rf $(OBJROOT) $(BINDIR)
