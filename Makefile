# ohos-compat-shim/Makefile
#
# Local (on-device) build for development/smoke-testing. The npm-published
# artifact is built by .github/workflows/release.yml on a cloud x86 runner
# cross-compiling with the same OHOS NDK clang invocation — this Makefile is
# for iterating directly on a HarmonyOS device or any host with the OHOS NDK
# installed via harmonybrew.
#
# Targets:
#   make               -> build libohos_compat.so + test/smoke
#   make sign          -> build then self-sign libohos_compat.so + smoke
#   make smoke         -> build, sign, and run the smoke test WITHOUT the shim
#                         preloaded (baseline) then WITH it preloaded
#   make functional    -> build, sign, and run test/functional.c the same way
#   make bench         -> build, sign, and run test/bench.c the same way
#   make real-vs-fallback -> build, sign, and run test/real_vs_fallback.c
#                         (--dump, then the performance comparison) — never
#                         needs LD_PRELOAD, it calls both implementations
#                         directly in a single unshimmed process (still
#                         needs signing to execute on a real HarmonyOS
#                         device, same as every other binary here)
#   make check         -> build, sign, and run ohos-compat-check (`ohos-shim
#                         check`'s payload) with a forced clean LD_PRELOAD
#                         baseline -- see src/ohos_compat_check.c
#   make clean

OHOS_NDK_HOME ?= $(shell ls -d $(HOME)/.harmonybrew/Cellar/ohos-sdk/*/native 2>/dev/null | sort -V | tail -1)
CLANG := $(OHOS_NDK_HOME)/llvm/bin/clang
SYSROOT := $(OHOS_NDK_HOME)/sysroot
CC = $(CLANG) --target=aarch64-linux-ohos --sysroot=$(SYSROOT)
CFLAGS = -O2 -g -Wall -Wextra
LDFLAGS = -ldl

LIB := libohos_compat.so
CHECKDEP := libohos_compat_checkdep.so
SMOKE := test/smoke
FUNCTIONAL := test/functional
BENCH := test/bench
RVF := test/real_vs_fallback
# Flat next to $(LIB)/$(CHECKDEP), not under src/ -- resolve_sibling_lib()'s
# dev-layout fallback expects the check binary and both .so's as siblings.
CHECK := ohos-compat-check

.PHONY: all sign smoke functional bench real-vs-fallback check clean

all: $(LIB) $(SMOKE) $(FUNCTIONAL) $(BENCH) $(RVF) $(CHECK) $(CHECKDEP)

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

$(CHECK): src/ohos_compat_check.c
	$(CC) $(CFLAGS) -rdynamic -pthread $< -o $@ $(LDFLAGS)

# Sign via a temp file + atomic rename, never in place (-inFile == -outFile):
# in-place signing sporadically fails on-device with FILE_NOT_FOUND right
# after "write code sign data success" (observed on test/smoke,
# test/real_vs_fallback, test/functional — then the half-written file makes
# every later in-place attempt fail too). Temp + mv has never failed.
sign: all
	@for f in $(LIB) $(SMOKE) $(FUNCTIONAL) $(BENCH) $(RVF) $(CHECK) $(CHECKDEP); do \
		echo "sign $$f"; \
		binary-sign-tool sign -selfSign 1 -inFile $$f -outFile $$f.signed && \
		chmod +x $$f.signed && mv -f $$f.signed $$f || exit 1; \
	done

real-vs-fallback: sign
	@echo "=== functional: real vs fallback ==="
	@./$(RVF) --dump
	@echo ""
	@echo "=== performance: real vs fallback ==="
	@./$(RVF)

# IMPORTANT: "baseline" runs use `env -u LD_PRELOAD` rather than assuming the
# ambient shell has none set. Bitten by this once already during development
# — an unrelated command (indirectly) left LD_PRELOAD exported pointing at a
# *different* preload library (the older, narrower close-range-shim) in a
# long-lived shell, which silently made every "baseline" run in that shell
# actually shimmed by something else, invalidating comparisons until caught.
# `env -u` strips it for this one invocation regardless of shell state.
smoke: sign
	@echo "=== baseline (no shim) ==="
	@env -u LD_PRELOAD ./$(SMOKE) || true
	@echo ""
	@echo "=== with LD_PRELOAD=$(LIB) ==="
	@env LD_PRELOAD=$(CURDIR)/$(LIB) ./$(SMOKE)

functional: sign
	@echo "=== baseline (no shim) ==="
	@env -u LD_PRELOAD ./$(FUNCTIONAL) || true
	@echo ""
	@echo "=== with LD_PRELOAD=$(LIB) ==="
	@env LD_PRELOAD=$(CURDIR)/$(LIB) ./$(FUNCTIONAL)

bench: sign
	@echo "=== baseline (no shim) ==="
	@env -u LD_PRELOAD ./$(BENCH) || true
	@echo ""
	@echo "=== with LD_PRELOAD=$(LIB) ==="
	@env LD_PRELOAD=$(CURDIR)/$(LIB) ./$(BENCH)

# check does its own `env -u LD_PRELOAD` equivalent internally (unsetenv at
# startup, see src/ohos_compat_check.c) so this doesn't wrap it here -- but
# do it anyway for defense in depth against a shell that has something else
# entirely exported.
check: sign
	@env -u LD_PRELOAD ./$(CHECK)

clean:
	rm -f $(LIB) $(CHECKDEP) $(SMOKE) $(FUNCTIONAL) $(BENCH) $(RVF) $(CHECK)
