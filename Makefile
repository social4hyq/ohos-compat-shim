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
#   make clean

OHOS_NDK_HOME ?= $(shell ls -d $(HOME)/.harmonybrew/Cellar/ohos-sdk/*/native 2>/dev/null | sort -V | tail -1)
CLANG := $(OHOS_NDK_HOME)/llvm/bin/clang
SYSROOT := $(OHOS_NDK_HOME)/sysroot
CC = $(CLANG) --target=aarch64-linux-ohos --sysroot=$(SYSROOT)
CFLAGS = -O2 -g -Wall -Wextra
LDFLAGS = -ldl

LIB := libohos_compat.so
SMOKE := test/smoke
FUNCTIONAL := test/functional
BENCH := test/bench
RVF := test/real_vs_fallback

.PHONY: all sign smoke functional bench real-vs-fallback clean

all: $(LIB) $(SMOKE) $(FUNCTIONAL) $(BENCH) $(RVF)

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

sign: all
	binary-sign-tool sign -selfSign 1 -inFile $(LIB) -outFile $(LIB) && chmod +x $(LIB)
	binary-sign-tool sign -selfSign 1 -inFile $(SMOKE) -outFile $(SMOKE) && chmod +x $(SMOKE)
	binary-sign-tool sign -selfSign 1 -inFile $(FUNCTIONAL) -outFile $(FUNCTIONAL) && chmod +x $(FUNCTIONAL)
	binary-sign-tool sign -selfSign 1 -inFile $(BENCH) -outFile $(BENCH) && chmod +x $(BENCH)
	binary-sign-tool sign -selfSign 1 -inFile $(RVF) -outFile $(RVF) && chmod +x $(RVF)

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

clean:
	rm -f $(LIB) $(SMOKE) $(FUNCTIONAL) $(BENCH) $(RVF)
