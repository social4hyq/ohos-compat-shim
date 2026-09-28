# Local OHOS build and on-device checks. Override OHOS_NDK_HOME to use a
# specific SDK; by default, use OHOS_SDK_NATIVE or the latest Harmonybrew SDK.

OHOS_NDK_HOME ?= $(OHOS_SDK_NATIVE)
ifeq ($(strip $(OHOS_NDK_HOME)),)
OHOS_NDK_HOME := $(shell ls -d $(HOME)/.harmonybrew/Cellar/ohos-sdk/*/native 2>/dev/null | sort -V | tail -1)
endif

OHOS_TARGET ?= aarch64-linux-ohos
OHOS_SYSROOT ?= $(OHOS_NDK_HOME)/sysroot
ifeq ($(origin OHOS_CC),undefined)
ifneq ($(wildcard $(OHOS_NDK_HOME)/llvm/bin/cc),)
OHOS_CC := $(OHOS_NDK_HOME)/llvm/bin/cc
else
OHOS_CC := $(OHOS_NDK_HOME)/llvm/bin/clang
endif
endif
CC = $(OHOS_CC) --target=$(OHOS_TARGET) --sysroot=$(OHOS_SYSROOT)
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?= -ldl

LIB := libohos_compat.so
CHECKDEP := libohos_compat_checkdep.so
SMOKE := test/smoke
FUNCTIONAL := test/functional
BENCH := test/bench
RVF := test/real_vs_fallback
GHOST := test/epoll_ghost
CHECK := ohos-compat-check
ARTIFACTS := $(LIB) $(SMOKE) $(FUNCTIONAL) $(BENCH) $(RVF) $(CHECK) $(CHECKDEP) $(GHOST)

.DEFAULT_GOAL := $(LIB)
.PHONY: all sign smoke functional bench real-vs-fallback check ghost clean

# `make` builds the runtime library; `make all` also builds every test and
# diagnostic executable. Compilation never signs or runs the outputs.
all: $(ARTIFACTS)

$(LIB): src/ohos_compat_shim.c
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@ $(LDFLAGS)

$(SMOKE): test/smoke.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(FUNCTIONAL): test/functional.c
	$(CC) $(CFLAGS) -pthread $< -o $@ $(LDFLAGS)

$(BENCH): test/bench.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(RVF): test/real_vs_fallback.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(CHECKDEP): src/checkdep.c
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@ $(LDFLAGS)

# The check program resolves checkdep beside itself or in the Homebrew lib dir.
$(CHECK): src/ohos_compat_check.c
	$(CC) $(CFLAGS) -rdynamic -pthread $< -o $@ $(LDFLAGS)

$(GHOST): test/epoll_ghost.c src/ohos_compat_shim.c
	$(CC) $(CFLAGS) -pthread $< -o $@ $(LDFLAGS)

# Sign to a temporary file and atomically replace the unsigned output. Signing
# in place can fail on-device and leave a truncated artifact.
define sign_files
	@set -e; for file in $(1); do \
		tmp="$${file}.signed"; \
		trap 'rm -f "$$tmp"' EXIT HUP INT TERM; \
		echo "sign $$file"; \
		binary-sign-tool sign -selfSign 1 -inFile "$$file" -outFile "$$tmp"; \
		chmod +x "$$tmp"; \
		mv -f "$$tmp" "$$file"; \
	done; trap - EXIT HUP INT TERM
endef

sign: all
	$(call sign_files,$(ARTIFACTS))

# Baseline runs must not inherit an unrelated LD_PRELOAD from the shell.
smoke: $(LIB) $(SMOKE)
	$(call sign_files,$^)
	@echo "=== baseline (no shim) ==="
	@env -u LD_PRELOAD ./$(SMOKE) || true
	@echo ""
	@echo "=== with LD_PRELOAD=$(LIB) ==="
	@env LD_PRELOAD=$(CURDIR)/$(LIB) ./$(SMOKE)

functional: $(LIB) $(FUNCTIONAL)
	$(call sign_files,$^)
	@echo "=== baseline (no shim) ==="
	@env -u LD_PRELOAD ./$(FUNCTIONAL) || true
	@echo ""
	@echo "=== with LD_PRELOAD=$(LIB) ==="
	@env LD_PRELOAD=$(CURDIR)/$(LIB) ./$(FUNCTIONAL)

bench: $(LIB) $(BENCH)
	$(call sign_files,$^)
	@echo "=== baseline (no shim) ==="
	@env -u LD_PRELOAD ./$(BENCH) || true
	@echo ""
	@echo "=== with LD_PRELOAD=$(LIB) ==="
	@env LD_PRELOAD=$(CURDIR)/$(LIB) ./$(BENCH)

real-vs-fallback: $(RVF)
	$(call sign_files,$^)
	@echo "=== functional: real vs fallback ==="
	@./$(RVF) --dump
	@echo ""
	@echo "=== performance: real vs fallback ==="
	@./$(RVF)

# The checker clears LD_PRELOAD itself; keep the environment clean as well.
check: $(CHECK) $(CHECKDEP)
	$(call sign_files,$^)
	@env -u LD_PRELOAD ./$(CHECK)

# This deterministic state-machine test mocks epoll_pwait; it does not need
# the shared library or LD_PRELOAD.
ghost: $(GHOST)
	$(call sign_files,$^)
	@env -u LD_PRELOAD ./$(GHOST)

clean:
	rm -f $(ARTIFACTS)
