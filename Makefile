# vfw-harness build.
#
#   make              build build/vfw-harness (i386)
#   make test         unit tests, ABI cross-check and CLI smoke test
#   make check-abi    compare include/win32 with the mingw-w64 headers
#   make clean
#
# The ABI check is skipped when the mingw-w64 cross compiler is missing;
# set REQUIRE_MINGW=1 (as CI does) to make that an error instead.

CC       ?= gcc
MINGW_CC ?= i686-w64-mingw32-gcc
BUILD    ?= build

CPPFLAGS += -Iinclude -D_GNU_SOURCE
CFLAGS   ?= -O2 -g
CFLAGS   += -m32 -std=c11 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  += -m32

SRCS     := $(shell find src -name '*.c' | sort)
OBJS     := $(SRCS:%.c=$(BUILD)/%.o)
LIB_OBJS := $(filter-out $(BUILD)/src/main.o,$(OBJS))
BIN      := $(BUILD)/vfw-harness

UNIT_SRCS := $(wildcard tests/unit/test_*.c)
UNIT_BINS := $(UNIT_SRCS:tests/unit/%.c=$(BUILD)/tests/%)

ABI_GEN := $(BUILD)/tests/gen_abi_checks
ABI_INC := $(BUILD)/tests/abi_checks.inc

.PHONY: all test unit check-abi smoke clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

# Unit tests link against everything except main().
$(BUILD)/tests/%: tests/unit/%.c $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests/unit $(CFLAGS) -MMD -MP $(LDFLAGS) \
		-o $@ $< $(LIB_OBJS) $(LDLIBS)

$(ABI_GEN): tests/abi/gen_abi_checks.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests/abi $(CFLAGS) -MMD -MP $(LDFLAGS) -o $@ $<

$(ABI_INC): $(ABI_GEN)
	$(ABI_GEN) > $@

test: unit check-abi smoke

unit: $(UNIT_BINS)
	@for t in $(UNIT_BINS); do ./$$t || exit 1; done

check-abi: $(ABI_INC)
	@if command -v $(MINGW_CC) >/dev/null 2>&1; then \
		$(MINGW_CC) -std=c11 -fsyntax-only -I$(BUILD)/tests \
			tests/abi/check_abi.c && \
		echo "check-abi: $$(wc -l < $(ABI_INC)) assertions match mingw-w64"; \
	elif [ -n "$(REQUIRE_MINGW)" ]; then \
		echo "check-abi: $(MINGW_CC) not found" >&2; exit 1; \
	else \
		echo "check-abi: skipped, $(MINGW_CC) not found"; \
	fi

smoke: $(BIN)
	@$(BIN) --version
	@$(BIN) --help > /dev/null
	@! $(BIN) no-such-command 2> /dev/null
	@echo "smoke: CLI ok"

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(UNIT_BINS:=.d) $(ABI_GEN).d
