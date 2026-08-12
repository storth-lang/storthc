include config.mk

SRCROOT := src
OBJROOT := obj
BINDIR := bin

CFLAGS := -std=c11 -I$(SRCROOT) -Wall -Wextra -Wno-missing-field-initializers -Wno-missing-braces -Wimplicit-fallthrough -g
LDLIBS := -lm
TARGET := $(BINDIR)/storthc

SRC := $(shell find $(SRCROOT) -name '*.c')
OBJ := $(patsubst $(SRCROOT)/%.c,$(OBJROOT)/%.o,$(SRC))

TESTROOT := tests
TESTSRC := $(TESTROOT)/st_tester.c
TESTOBJ := $(patsubst $(TESTROOT)/%.c,$(OBJROOT)/%.o,$(TESTSRC))
TESTTARGET := $(BINDIR)/st_tester
TESTCFLAGS := -std=c99 -Wall -Wextra

.PHONY: all test clean

all: $(TARGET)

test: $(TESTTARGET)
	$< -dir ./tests

$(TESTTARGET): $(TESTOBJ) | $(BINDIR)
	$(CC) $(TESTCFLAGS) -o $@ $(TESTOBJ) $(LDLIBS)

$(OBJROOT)/%.o: $(TESTROOT)/%.c | $(OBJROOT)
		$(CC) $(TESTCFLAGS) -I$(TESTROOT) -c $< -o $@

$(TARGET): $(OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJROOT)/%.o: $(SRCROOT)/%.c | $(OBJROOT)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	install -d $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/storthc
	install -d $(MODULES_DIR)
	cp -r modules/. $(MODULES_DIR)/
	@echo 'export STORTHC_MODULE_PATH="$(MODULES_DIR)"' > ~/.bashrc

$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJROOT):
	mkdir -p $(OBJROOT)

clean:
	rm -rf $(OBJROOT) $(BINDIR)
