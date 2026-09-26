# vfw-harness build.
#
#   make              build build/vfw-harness (i386)
#   make test         unit tests, ABI cross-check and CLI smoke test
#   make fixtures     build the test DLLs (needs mingw-w64)
#   make check-abi    compare include/win32 with the mingw-w64 headers
#   make asan         unit tests under AddressSanitizer and UBSan
#   make clean
#
# Without the mingw-w64 cross compiler, the ABI check and the tests that need
# fixture DLLs are skipped; set REQUIRE_MINGW=1 (as CI does) to make that an
# error instead.

CC           ?= gcc
MINGW_PREFIX ?= i686-w64-mingw32-
MINGW_CC     ?= $(MINGW_PREFIX)gcc
WINDRES      ?= $(MINGW_PREFIX)windres
DLLTOOL      ?= $(MINGW_PREFIX)dlltool
MINGW_OBJCOPY ?= $(MINGW_PREFIX)objcopy
BUILD        ?= build

HAVE_MINGW := $(shell command -v $(MINGW_CC) >/dev/null 2>&1 && echo 1)

CPPFLAGS += -Iinclude -Isrc -D_GNU_SOURCE
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

.PHONY: all test unit fixtures check-abi smoke asan clean

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

# Test fixture DLLs. They are built without a C runtime so that, until the
# Win32 runtime exists, they import nothing but each other.
FIXTURE_DIR    := $(BUILD)/fixtures
FIXTURE_SRC    := tests/fixtures
FIXTURE_CFLAGS := -O2 -Wall -nostdlib -ffreestanding -fno-stack-protector \
	-mno-stack-arg-probe -fno-asynchronous-unwind-tables -shared \
	-Wl,--image-base,0x10000000 -Wl,-e,_DllMain@12
FIXTURES := $(addprefix $(FIXTURE_DIR)/,pe_basic.dll pe_reloc.dll \
	pe_noreloc.dll pe_tls.dll pe_dep.dll pe_fail.dll pe_missing.dll \
	pe_runtime.dll)

fixtures: $(FIXTURES)

$(FIXTURE_DIR)/%.res.o: $(FIXTURE_SRC)/%.rc
	@mkdir -p $(dir $@)
	$(WINDRES) $< -O coff -o $@

$(FIXTURE_DIR)/lib%.a: $(FIXTURE_SRC)/%.def
	@mkdir -p $(dir $@)
	$(DLLTOOL) -k -d $< -l $@

$(FIXTURE_DIR)/pe_basic.dll: $(FIXTURE_SRC)/pe_basic.c \
		$(FIXTURE_SRC)/pe_basic.def $(FIXTURE_DIR)/pe_basic.res.o \
		$(FIXTURE_SRC)/fixture.h
	$(MINGW_CC) $(FIXTURE_CFLAGS) -o $@ $(filter-out %.h,$^)

$(FIXTURE_DIR)/pe_dep.dll: $(FIXTURE_SRC)/pe_dep.c $(FIXTURE_SRC)/pe_dep.def \
		$(FIXTURE_DIR)/libpe_basic.a $(FIXTURE_SRC)/fixture.h
	$(MINGW_CC) $(FIXTURE_CFLAGS) -o $@ $(filter-out %.h,$^)

$(FIXTURE_DIR)/pe_missing.dll: $(FIXTURE_SRC)/pe_missing.c \
		$(FIXTURE_DIR)/libzz_nosuch.a $(FIXTURE_DIR)/libpe_basic.a \
		$(FIXTURE_SRC)/fixture.h
	$(MINGW_CC) $(FIXTURE_CFLAGS) -o $@ $(filter-out %.h,$^)

# Links against kernel32 (the mingw import library) and a made-up function.
$(FIXTURE_DIR)/pe_runtime.dll: $(FIXTURE_SRC)/pe_runtime.c \
		$(FIXTURE_DIR)/libvfw_missing.a $(FIXTURE_SRC)/fixture.h
	$(MINGW_CC) $(FIXTURE_CFLAGS) -o $@ $< $(FIXTURE_DIR)/libvfw_missing.a \
		-lkernel32

# The same image as pe_reloc.dll with its relocations stripped.
$(FIXTURE_DIR)/pe_noreloc.dll: $(FIXTURE_DIR)/pe_reloc.dll
	$(MINGW_OBJCOPY) --remove-section .reloc $< $@

$(FIXTURE_DIR)/%.dll: $(FIXTURE_SRC)/%.c $(FIXTURE_SRC)/fixture.h
	@mkdir -p $(dir $@)
	$(MINGW_CC) $(FIXTURE_CFLAGS) -o $@ $<

$(ABI_GEN): tests/abi/gen_abi_checks.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests/abi $(CFLAGS) -MMD -MP $(LDFLAGS) -o $@ $<

$(ABI_INC): $(ABI_GEN)
	$(ABI_GEN) > $@

test: unit check-abi smoke

# A test exits 77 when it needs fixtures that were not built.
unit: $(UNIT_BINS) $(if $(HAVE_MINGW),fixtures)
	@status=0; for t in $(abspath $(UNIT_BINS)); do \
		VFW_FIXTURES=$(abspath $(FIXTURE_DIR)) $$t; rc=$$?; \
		if [ $$rc -eq 77 ]; then \
			echo "$$(basename $$t): SKIP (no fixture DLLs; install mingw-w64)"; \
			[ -z "$(REQUIRE_MINGW)" ] || status=1; \
		elif [ $$rc -ne 0 ]; then \
			status=1; \
		fi; \
	done; exit $$status

check-abi: $(ABI_INC)
	@if [ -n "$(HAVE_MINGW)" ]; then \
		$(MINGW_CC) -std=c11 -fsyntax-only -I$(BUILD)/tests \
			tests/abi/check_abi.c && \
		echo "check-abi: $$(wc -l < $(ABI_INC)) assertions match mingw-w64"; \
	elif [ -n "$(REQUIRE_MINGW)" ]; then \
		echo "check-abi: $(MINGW_CC) not found" >&2; exit 1; \
	else \
		echo "check-abi: skipped, $(MINGW_CC) not found"; \
	fi

smoke: $(BIN) $(if $(HAVE_MINGW),fixtures)
	@$(BIN) --version
	@$(BIN) --help > /dev/null
	@! $(BIN) no-such-command 2> /dev/null
	@! $(BIN) info Makefile 2> /dev/null
	@if [ -n "$(HAVE_MINGW)" ]; then \
		$(BIN) info $(FIXTURE_DIR)/pe_dep.dll > /dev/null; \
	fi
	@echo "smoke: CLI ok"

SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined

asan:
	$(MAKE) BUILD=$(BUILD)-asan \
		CFLAGS="-O1 -g -fno-omit-frame-pointer $(SANITIZE) -m32 -std=c11 -Wall -Wextra -Wno-unused-parameter" \
		LDFLAGS="-m32 $(SANITIZE)" unit

clean:
	rm -rf $(BUILD) $(BUILD)-asan

-include $(OBJS:.o=.d) $(UNIT_BINS:=.d) $(ABI_GEN).d
