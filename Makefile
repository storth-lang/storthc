include config.mk

SRCROOT := src
OBJROOT := obj
BINDIR := bin

GIT_HASH := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)

CFLAGS := -std=c11 -I$(SRCROOT) -Wall -Wextra -Wno-missing-field-initializers -Wno-missing-braces -Wimplicit-fallthrough -g
CFLAGS += -MMD -MP
CFLAGS += -DSTORTHC_VERSION_MAJOR=$(VERSION_MAJOR) -DSTORTHC_VERSION_MINOR=$(VERSION_MINOR) -DSTORTHC_VERSION_PATCH=$(VERSION_PATCH) -DSTORTHC_GIT_HASH='"$(GIT_HASH)"'
LDLIBS := -lm

TARGET := $(BINDIR)/storthc

SRC := $(shell find $(SRCROOT) -name '*.c')
OBJ := $(patsubst $(SRCROOT)/%.c,$(OBJROOT)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

TESTROOT := tests
TESTSRC := $(TESTROOT)/st_tester.c
TESTOBJ := $(patsubst $(TESTROOT)/%.c,$(OBJROOT)/%.o,$(TESTSRC))
TESTTARGET := $(BINDIR)/st_tester
TESTCFLAGS := -std=c99 -Wall -Wextra

V ?= 0
ifeq ($(V),0)
QUIET := @
GREEN  := \033[1;32m
RED    := \033[1;31m
RESET  := \033[0m
else
QUIET :=
endif

.PHONY: all test clean

all: $(TARGET)

test: $(TESTTARGET)
	$< -dir ./tests

$(TESTTARGET): $(TESTOBJ) | $(BINDIR)
	$(QUIET)printf "$(GREEN)LD$(RESET)  %s\n" "$@"
	$(QUIET)$(CC) $(TESTCFLAGS) -o $@ $(TESTOBJ) $(LDLIBS) \
		&& printf "$(GREEN)build ok$(RESET)  %s\n" "$@" \
		|| { printf "$(RED)build failed$(RESET)  %s\n" "$@"; exit 1; }

$(OBJROOT)/%.o: $(TESTROOT)/%.c | $(OBJROOT)
	$(QUIET)printf "$(GREEN)CC$(RESET)  %s\n" "$@"
	$(QUIET)$(CC) $(TESTCFLAGS) -I$(TESTROOT) -c $< -o $@ \
		|| { printf "$(RED)failed$(RESET)  %s\n" "$@"; exit 1; }

$(TARGET): $(OBJ) | $(BINDIR)
	$(QUIET)printf "$(GREEN)LD$(RESET)  %s\n" "$@"
	$(QUIET)$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS) \
		&& printf "$(GREEN)build ok$(RESET)  %s\n" "$@" \
		|| { printf "$(RED)build failed$(RESET)  %s\n" "$@"; exit 1; }

$(OBJROOT)/%.o: $(SRCROOT)/%.c | $(OBJROOT)
	@mkdir -p $(dir $@)
	$(QUIET)printf "$(GREEN)CC$(RESET)  %s\n" "$@"
	$(QUIET)$(CC) $(CFLAGS) -c $< -o $@ \
		|| { printf "$(RED)failed$(RESET)  %s\n" "$@"; exit 1; }

-include $(DEP)

install: $(TARGET)
	install -d $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/storthc
	install -d $(MODULES_DIR)
	cp -r modules/. $(MODULES_DIR)/
	@echo 'export STORTHC_MODULE_PATH="$(MODULES_DIR)"' > ~/.bashrc

$(BINDIR):
	@mkdir -p $(BINDIR)

$(OBJROOT):
	@mkdir -p $(OBJROOT)

clean:
	rm -rf $(OBJROOT) $(BINDIR)
