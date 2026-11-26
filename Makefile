include config.mk

SRCROOT := src
OBJROOT := obj
BINDIR  := build

GIT_HASH := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)

CFLAGS := -std=c11 -I$(SRCROOT) -Wall -Wextra -Wno-missing-field-initializers -Wno-missing-braces -Wimplicit-fallthrough -g
CFLAGS += -MMD -MP
CFLAGS += -DSTORTHC_VERSION_MAJOR=$(VERSION_MAJOR) -DSTORTHC_VERSION_MINOR=$(VERSION_MINOR) -DSTORTHC_VERSION_PATCH=$(VERSION_PATCH) -DSTORTHC_GIT_HASH='"$(GIT_HASH)"' -DST_STACK_SIZE=$(STACK_SIZE) -DST_DYNAMIC_LINKER_PATH='"$(DYNAMIC_LINKER_PATH)"' -DST_LIBC_LIBRARY_PATH='"$(LIBC_LIBRARY_PATH)"' -DST_MODULES_DIR='"$(MODULES_DIR)"'
CFLAGS += -DST_BIND_GENERATOR=$(BIND_GENERATOR)
LDLIBS := -lm

TARGET := $(BINDIR)/storthc

SRC := $(shell find $(SRCROOT) -name '*.c' ! -name '*win32*')
OBJ := $(patsubst $(SRCROOT)/%.c,$(OBJROOT)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

TESTROOT := tests
TESTBIN := $(TESTROOT)/build
TESTSOURCES := $(shell find $(TESTROOT) -type d -name 'build' -prune -o -name '*.st' -print)
TESTSET := $(TESTROOT)/set.sh
TESTSRC := $(TESTROOT)/st_tester.c
TESTOBJ := $(patsubst $(TESTROOT)/%.c,$(OBJROOT)/%.o,$(TESTSRC))
TESTUTIL := $(addprefix $(OBJROOT)/utils/,st_arena.o st_ht.o st_process.o st_string.o platform/st_process_linux.o)
TESTTARGET := $(BINDIR)/st_tester
TESTCFLAGS := -std=c11 -Wall -Wextra -Wno-missing-field-initializers -I$(SRCROOT) -pthread

V ?= 0
ifeq ($(V),0)
QUIET := @
GREEN  := \033[1;32m
RED    := \033[1;31m
RESET  := \033[0m
else
QUIET :=
endif

ifeq ($(BIND_GENERATOR),1)
LLVM_CONFIG ?= llvm-config
CFLAGS   += -I$(shell $(LLVM_CONFIG) --includedir)
LDLIBS   += -L$(shell $(LLVM_CONFIG) --libdir) -lclang \
            -Wl,-rpath,$(shell $(LLVM_CONFIG) --libdir)
endif

.PHONY: all test clean install uninstall

all: $(TARGET)

test: $(TESTTARGET)
	$< -dir ./tests

$(TESTTARGET): $(TESTOBJ) $(TESTUTIL) $(TESTBIN) | $(BINDIR)
	$(QUIET)printf "$(GREEN)LD$(RESET)  %s\n" "$@"
	$(QUIET)$(CC) $(TESTCFLAGS) -o $@ $(TESTOBJ) $(TESTUTIL) $(LDLIBS) \
		&& printf "$(GREEN)build ok$(RESET)  %s\n" "$@" \
		|| { printf "$(RED)build failed$(RESET)  %s\n" "$@"; exit 1; }

$(TESTBIN): $(TESTSOURCES)
	$(QUIET)$(TESTSET) record

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

install: uninstall $(TARGET)
	install -d $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/storthc
	install -d $(MODULES_DIR)
	cp -r modules/. $(MODULES_DIR)/

uninstall:
	$(RM) -r $(MODULES_DIR) $(INSTALL_DIR)/storthc


$(BINDIR):
	@mkdir -p $(BINDIR)

$(OBJROOT):
	@mkdir -p $(OBJROOT)

clean:
	rm -rf $(OBJROOT) $(BINDIR) $(TESTBIN)
