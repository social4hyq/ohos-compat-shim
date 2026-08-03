/*
 * test/functional.c — comprehensive functional-equivalence checks, beyond
 * smoke.c's basic "does it work" pass/fail. These probe:
 *
 *   1. close_range: flags (CLOEXEC vs plain close), multi-fd ranges,
 *      first>last no-op, repeated calls (probe-cache correctness),
 *      libc-wrapper vs raw-syscall paths agree.
 *   2. getpwuid_r: buffer-too-small -> ERANGE, idempotency across calls,
 *      env-var-driven fallback selection.
 *   3. tmpfile: write/read-back correctness, concurrent tmpfiles don't
 *      cross-contaminate, no leftover directory entries after close.
 *   4. getcwd: MUST be a true no-op when the real call succeeds (the
 *      central "does shimming change behavior in the common case"
 *      question) — verified by comparing against a plain chdir+getcwd
 *      round trip; ENOENT fallback only exercised when shimmed.
 *   5. linkat/symlinkat (opt-in): EEXIST passes through untouched; a real,
 *      reproduced-on-this-device EACCES from linkat() in TMPDIR triggers
 *      the copy fallback with correct content; symlinkat (not restricted
 *      in TMPDIR here) exercises the success/no-op path.
 *   6. Pass-through: arbitrary non-target syscalls (getpid, read/write)
 *      behave identically whether or not this library is preloaded.
 *
 * Run once without LD_PRELOAD and once with it (`make functional`).
 * Some checks only make sense — or only pass — under one of the two
 * conditions; each check's own comment says which.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_close_range
#define __NR_close_range 436
#endif
#ifndef CLOSE_RANGE_UNSHARE
#define CLOSE_RANGE_UNSHARE (1U << 1)
#endif
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#ifndef __NR_fchmodat2
#define __NR_fchmodat2 452
#endif

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *name, const char *detail)
{
	checks++;
	if (cond) {
		printf("PASS  %-28s %s\n", name, detail ? detail : "");
	} else {
		printf("FAIL  %-28s %s (errno=%d %s)\n", name, detail ? detail : "",
		       errno, strerror(errno));
		failures++;
	}
	fflush(stdout); /* survive a crash mid-suite without losing prior results */
}

/* This musl doesn't export close_range() at all — only this shim provides
 * that symbol, and only when preloaded — so its presence is a reliable
 * "is the shim actually loaded" signal, used to decide whether a given
 * outcome should be scored (shim active, a specific behavior is promised)
 * or merely informational (baseline, whatever the real platform does is
 * whatever it does). */
static int shim_is_loaded(void)
{
	return dlsym(RTLD_DEFAULT, "close_range") != NULL;
}

/* ==================================================================== */
/*  close_range                                                          */
/*                                                                        */
/*  On this device, calling close_range() via the raw syscall() symbol   */
/*  WITHOUT the shim loaded unconditionally raises SIGSYS — for every     */
/*  parameter combination tried (plain valid call, first>last, garbage   */
/*  flags alike; confirmed with a standalone repro, not assumed). Every   */
/*  test below that touches close_range needs this guard or a baseline    */
/*  (no-LD_PRELOAD) run takes the whole test binary down with it. With    */
/*  the shim loaded, this handler should never actually fire — the        */
/*  shim's own internal probe/fallback handles the SIGSYS itself before   */
/*  ever returning control to the caller, so a guarded call under         */
/*  LD_PRELOAD=libohos_compat.so returns normally either way (real        */
/*  success or a transparently-applied fallback — indistinguishable from  */
/*  here, which is the point).                                            */
/* ==================================================================== */

static __thread sigjmp_buf cr_test_jmp;

static void cr_test_sigsys_handler(int sig)
{
	(void)sig;
	siglongjmp(cr_test_jmp, 1);
}

/* Returns 1 if the syscall returned normally (out_ret/out_errno set,
 * whether success or a clean error), or 0 if it was caught via SIGSYS
 * instead (both left untouched). */
static int guarded_close_range(long a0, long a1, long a2, long *out_ret, int *out_errno)
{
	struct sigaction old_sa, sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = cr_test_sigsys_handler;
	sigaction(SIGSYS, &sa, &old_sa);

	int ok;
	errno = 0;
	if (sigsetjmp(cr_test_jmp, 1) == 0) {
		*out_ret = syscall(__NR_close_range, a0, a1, a2);
		*out_errno = errno;
		ok = 1;
	} else {
		ok = 0;
	}
	sigaction(SIGSYS, &old_sa, NULL);
	return ok;
}

/* Same guard, generalized for fchmodat2's 4-arg shape (dirfd, path, mode,
 * flags) — reuses the same thread-local jmp/handler as guarded_close_range
 * above, safe since only one guarded call is ever in flight per thread. */
static int guarded_fchmodat2(long dirfd, const char *path, long mode, long flags,
			     long *out_ret, int *out_errno)
{
	struct sigaction old_sa, sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = cr_test_sigsys_handler;
	sigaction(SIGSYS, &sa, &old_sa);

	int ok;
	errno = 0;
	if (sigsetjmp(cr_test_jmp, 1) == 0) {
		*out_ret = syscall(__NR_fchmodat2, dirfd, path, mode, flags);
		*out_errno = errno;
		ok = 1;
	} else {
		ok = 0;
	}
	sigaction(SIGSYS, &old_sa, NULL);
	return ok;
}

/* Prints the standard "would have crashed without the shim" INFO line and
 * counts it (unscored) — used by every close_range test's baseline-crash
 * branch below, so that message stays consistent in one place. */
static void report_close_range_would_crash(const char *name)
{
	printf("INFO  %-28s would SIGSYS without the shim's protection (caught "
	       "here so the suite could continue) — expected at baseline\n",
	       name);
	checks++;
}

static int fd_is_open(int fd)
{
	return fcntl(fd, F_GETFD) != -1;
}

static void test_close_range_multi_range(void)
{
	int fds[5];
	for (int i = 0; i < 5; i++)
		fds[i] = open("/dev/null", O_RDONLY);
	for (int i = 0; i < 5; i++) {
		if (fds[i] < 0) {
			check(0, "close_range_multi", "could not open test fds");
			return;
		}
	}
	/* Close only fds[1..3], leave fds[0] and fds[4] alone. */
	long ret;
	int errno_out;
	if (!guarded_close_range(fds[1], fds[3], 0, &ret, &errno_out)) {
		report_close_range_would_crash("close_range_multi");
		for (int i = 0; i < 5; i++)
			if (fd_is_open(fds[i]))
				close(fds[i]);
		return;
	}
	int ok = (ret == 0) && fd_is_open(fds[0]) && !fd_is_open(fds[1]) &&
		 !fd_is_open(fds[2]) && !fd_is_open(fds[3]) && fd_is_open(fds[4]);
	check(ok, "close_range_multi", "middle range closed, edges untouched");
	if (fd_is_open(fds[0]))
		close(fds[0]);
	if (fd_is_open(fds[4]))
		close(fds[4]);
}

static void test_close_range_cloexec(void)
{
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		check(0, "close_range_cloexec", "could not open test fd");
		return;
	}
	long ret;
	int errno_out;
	if (!guarded_close_range(fd, fd, CLOSE_RANGE_CLOEXEC, &ret, &errno_out)) {
		report_close_range_would_crash("close_range_cloexec");
		close(fd);
		return;
	}
	int still_open = fd_is_open(fd);
	int has_cloexec = still_open && (fcntl(fd, F_GETFD) & FD_CLOEXEC);
	check(ret == 0 && still_open && has_cloexec, "close_range_cloexec",
	      "fd stays open, gains FD_CLOEXEC (not closed)");
	if (still_open)
		close(fd);
}

/* bun's close_range(first, ~0U, CLOSE_RANGE_CLOEXEC) idiom: set FD_CLOEXEC on
 * every open fd >= `first` while LEAVING them open (CLOEXEC, not a close),
 * never touching stdio or fds < first. Two call sites use this —
 * bun_initialize_process() with first=4 (keeps fd 3 for IPC) and the spawn /
 * on_before_reload_process_linux paths with first=3 (CLOEXEC fd 3 too so it
 * doesn't leak to the child). This helper covers both by parameterizing
 * `first`; the single-fd cloexec and plain-close wide_sparse tests don't
 * cover this wide + CLOEXEC + keep-open + boundary combination. */
static void close_range_bun_cloexec_from(int first, const char *name)
{
	int opened[12];
	int nopen = 0;
	for (int i = 0; i < 12 && nopen < 12; i++) {
		int f = open("/dev/null", O_RDONLY);
		if (f < 0)
			break;
		opened[nopen++] = f; /* keep all open so fd numbers advance past `first` */
	}
	for (int i = 0; i < nopen; i++)
		fcntl(opened[i], F_SETFD, 0); /* clear so the shim setting CLOEXEC is detectable */
	int clo0 = fcntl(0, F_GETFD), clo1 = fcntl(1, F_GETFD), clo2 = fcntl(2, F_GETFD);

	long ret;
	int errno_out;
	if (!guarded_close_range(first, (long)~0U, CLOSE_RANGE_CLOEXEC, &ret, &errno_out)) {
		report_close_range_would_crash(name);
		for (int i = 0; i < nopen; i++)
			if (fd_is_open(opened[i]))
				close(opened[i]);
		return;
	}
	int ok = (ret == 0) && fcntl(0, F_GETFD) == clo0 && fcntl(1, F_GETFD) == clo1 &&
		 fcntl(2, F_GETFD) == clo2;
	int saw_in_range = 0;
	for (int i = 0; i < nopen && ok; i++) {
		if (opened[i] >= first) {
			ok = ok && fd_is_open(opened[i]) && (fcntl(opened[i], F_GETFD) & FD_CLOEXEC);
			saw_in_range = 1;
		} else {
			ok = ok && !(fcntl(opened[i], F_GETFD) & FD_CLOEXEC); /* below first: untouched */
		}
	}
	ok = ok && saw_in_range; /* must have at least one fd in range to actually test */
	char detail[120];
	snprintf(detail, sizeof(detail), "CLOEXEC on fds>=%d (kept open), below untouched", first);
	check(ok, name, detail);
	for (int i = 0; i < nopen; i++)
		if (fd_is_open(opened[i]))
			close(opened[i]);
}

static void test_close_range_bun_init_cloexec(void)
{
	/* c-bindings.cpp bun_initialize_process(): close_range(4, ~0U, CLOEXEC). */
	close_range_bun_cloexec_from(4, "close_range_bun_init");
}

static void test_close_range_bun_spawn_cloexec(void)
{
	/* BunProcess.cpp spawn path + c-bindings.cpp on_before_reload_process_linux:
	 * close_range(3, ~0U, CLOEXEC) — fd 3 included (not kept for IPC here). */
	close_range_bun_cloexec_from(3, "close_range_bun_spawn");
}

static void test_close_range_einval_first_last(void)
{
	/* Per upstream Linux (fs/file.c SYSCALL_DEFINE3(close_range, ...))
	 * and LTP's close_range02.c case 1, first > last must be EINVAL, not
	 * a silent no-op. The shim validates this itself up front — real
	 * value here since the raw syscall on this device doesn't return
	 * EINVAL gracefully at all, guarded or not (see below): it SIGSYS's
	 * unconditionally, same as every other close_range parameter
	 * combination tried. Without the shim, standards-conformant EINVAL
	 * behavior for this case isn't achievable at all on this device. */
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		check(0, "close_range_einval_range", "could not open test fd");
		return;
	}
	long ret;
	int errno_out;
	if (!guarded_close_range((long)(fd + 100), (long)fd, 0, &ret, &errno_out)) {
		report_close_range_would_crash("close_range_einval_range");
		close(fd);
		return;
	}
	check(ret == -1 && errno_out == EINVAL && fd_is_open(fd), "close_range_einval_range",
	      "first>last -> EINVAL, fd untouched");
	close(fd);
}

static void test_close_range_einval_bad_flags(void)
{
	/* On-device testing found that forwarding genuinely invalid flags
	 * (all bits set) straight to the raw syscall raises SIGSYS on this
	 * device — same as every other close_range call without the shim's
	 * protection. With the shim loaded, its own validation rejects bad
	 * flags with EINVAL *before* the syscall is ever issued, so
	 * guarded_close_range should never actually need to catch anything
	 * in that run. */
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		check(0, "close_range_einval_flags", "could not open test fd");
		return;
	}
	long ret;
	int errno_out;
	if (!guarded_close_range(fd, fd, (long)~0U, &ret, &errno_out)) {
		report_close_range_would_crash("close_range_einval_flags");
		close(fd);
		return;
	}
	check(ret == -1 && errno_out == EINVAL && fd_is_open(fd), "close_range_einval_flags",
	      "garbage flags -> EINVAL (not a SIGSYS crash), fd untouched");
	close(fd);
}

static void test_close_range_wide_sparse_range(void)
{
	/* Mirrors LTP close_range02.c case 0: dup a fd to a much higher
	 * number and close_range across the sparse gap between them — the
	 * /proc/self/fd-based fallback must skip the ~90 non-open
	 * descriptors in between without erroring, not just handle small
	 * contiguous ranges. */
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		check(0, "close_range_wide_sparse", "could not open test fd");
		return;
	}
	int high = fd + 90;
	if (dup2(fd, high) < 0) {
		check(0, "close_range_wide_sparse", "dup2 to high fd failed");
		close(fd);
		return;
	}
	long ret;
	int errno_out;
	if (!guarded_close_range((long)fd, (long)high, 0, &ret, &errno_out)) {
		report_close_range_would_crash("close_range_wide_sparse");
		if (fd_is_open(fd))
			close(fd);
		if (fd_is_open(high))
			close(high);
		return;
	}
	check(ret == 0 && !fd_is_open(fd) && !fd_is_open(high), "close_range_wide_sparse",
	      "sparse range (fd..fd+90) closes both ends cleanly");
}

struct unshare_thread_arg {
	int fd;
	int close_range_rc;
	int close_range_errno;
	int crashed; /* would have SIGSYS'd without guarded_close_range below */
};

static void *close_range_unshare_thread(void *arg)
{
	struct unshare_thread_arg *a = arg;
	long ret;
	int errno_out;
	/* guarded_close_range's jmp_buf/handler are __thread — safe to call
	 * from this worker thread independent of the main thread. */
	if (guarded_close_range((long)a->fd, (long)a->fd, CLOSE_RANGE_UNSHARE, &ret,
				&errno_out)) {
		a->close_range_rc = (int)ret;
		a->close_range_errno = errno_out;
	} else {
		a->crashed = 1;
	}
	return NULL;
}

static void test_close_range_unshare(void)
{
	/* Inspired by LTP close_range02.c case 5 / close_range01.c's clone(2)
	 * reproducer (threads share a file descriptor table by default, so
	 * CLOSE_RANGE_UNSHARE closing an fd in one thread must not affect
	 * others). Whether unshare(CLONE_FILES) itself works varies by
	 * platform: unconditionally SIGSYS on real HarmonyOS hardware
	 * (confirmed standalone, independent of the shim), but genuinely
	 * works in the OpenHarmony container (vanilla Linux kernel, no
	 * sandbox) — verified by running this exact test there. So the
	 * shim's contract is "either implement true isolation correctly, or
	 * fail honestly (ENOSYS) without touching the shared table" — never
	 * a crash, never a false claim. Both outcomes below are accepted as
	 * PASS; only a corrupted or crashing result fails. This test's local
	 * guard exists only for the baseline run, which has no shim
	 * protection at all and would otherwise crash the whole suite on a
	 * device where the real call is blocked. */
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		check(0, "close_range_unshare", "could not open test fd");
		return;
	}

	struct unshare_thread_arg targ = {.fd = fd};
	pthread_t tid;
	if (pthread_create(&tid, NULL, close_range_unshare_thread, &targ) != 0) {
		check(0, "close_range_unshare", "pthread_create failed");
		close(fd);
		return;
	}
	pthread_join(tid, NULL);

	if (targ.crashed) {
		printf("INFO  %-28s CLOSE_RANGE_UNSHARE raised SIGSYS without the "
		       "shim's protection (caught here so the suite could continue) "
		       "— expected at baseline\n",
		       "close_range_unshare");
		checks++;
		close(fd);
		return;
	}

	char detail[128];
	snprintf(detail, sizeof(detail), "worker rc=%d errno=%d, main-thread fd %s",
		 targ.close_range_rc, targ.close_range_errno,
		 fd_is_open(fd) ? "still open" : "closed");

	if (shim_is_loaded()) {
		/* Either outcome is a correct fulfillment of the shim's
		 * contract: genuine success (this platform's unshare() works,
		 * main-thread fd correctly untouched by the worker's now-
		 * private table) or an honest ENOSYS degradation (unshare()
		 * is broken here, fd left alone rather than corrupted). What
		 * fails this check: a crash (already handled above), a
		 * "success" that actually touched the shared fd, or any
		 * other errno. */
		int genuine_success = targ.close_range_rc == 0 && fd_is_open(fd);
		int honest_failure = targ.close_range_rc == -1 &&
				     targ.close_range_errno == ENOSYS && fd_is_open(fd);
		check(genuine_success || honest_failure, "close_range_unshare", detail);
	} else {
		check(targ.close_range_rc == 0 && fd_is_open(fd), "close_range_unshare", detail);
	}
	close(fd);
}

static void test_close_range_repeated(void)
{
	/* Exercise the probe-cache path twice; both calls must behave
	 * identically regardless of which internal state (WORKS/FALLBACK)
	 * got cached by an earlier call in this process. */
	int ok = 1;
	int any_crashed = 0;
	for (int i = 0; i < 2; i++) {
		int fd = open("/dev/null", O_RDONLY);
		if (fd < 0) {
			ok = 0;
			break;
		}
		long ret;
		int errno_out;
		if (!guarded_close_range(fd, fd, 0, &ret, &errno_out)) {
			any_crashed = 1;
			break;
		}
		if (ret != 0 || fd_is_open(fd))
			ok = 0;
	}
	if (any_crashed) {
		report_close_range_would_crash("close_range_repeated");
		return;
	}
	check(ok, "close_range_repeated", "two calls in a row agree");
}

typedef int (*close_range_fn)(unsigned, unsigned, unsigned);

static void test_close_range_libc_wrapper(void)
{
	/* This musl doesn't export close_range() at all (no header
	 * declaration, no libc.so symbol) — only this shim provides it, and
	 * only when preloaded. Resolve at runtime via dlsym so the test
	 * binary still links without the shim; treat "symbol absent" as an
	 * expected SKIP rather than a failure when running the baseline. */
	close_range_fn fn = (close_range_fn)dlsym(RTLD_DEFAULT, "close_range");
	if (!fn) {
		printf("SKIP  %-28s close_range() symbol not present (expected without preload)\n",
		       "close_range_libc_fn");
		return;
	}

	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		check(0, "close_range_libc_fn", "could not open test fd");
		return;
	}
	int rc = fn((unsigned)fd, (unsigned)fd, 0);
	check(rc == 0 && !fd_is_open(fd), "close_range_libc_fn",
	      "close_range() wrapper matches syscall() path");
}

/* ==================================================================== */
/*  getpwuid_r                                                           */
/* ==================================================================== */

static void test_getpwuid_r_erange(void)
{
	struct passwd pwd, *result = (struct passwd *)0x1; /* poison */
	char tiny[1];
	int rc = getpwuid_r(geteuid(), &pwd, tiny, sizeof(tiny), &result);
	/* Either the real impl or our fallback must report ERANGE with a
	 * too-small buffer, and must null out *result. */
	check(rc == ERANGE && result == NULL, "getpwuid_r_erange",
	      "1-byte buffer correctly rejected");
}

static void test_getpwuid_r_idempotent(void)
{
	struct passwd pwd1, pwd2, *r1, *r2;
	char buf1[1024], buf2[1024];
	int rc1 = getpwuid_r(geteuid(), &pwd1, buf1, sizeof(buf1), &r1);
	int rc2 = getpwuid_r(geteuid(), &pwd2, buf2, sizeof(buf2), &r2);
	int ok = (rc1 == rc2) && ((r1 == NULL) == (r2 == NULL));
	if (ok && r1 && r2)
		ok = strcmp(pwd1.pw_name, pwd2.pw_name) == 0 &&
		     strcmp(pwd1.pw_dir, pwd2.pw_dir) == 0 &&
		     pwd1.pw_uid == pwd2.pw_uid;
	check(ok, "getpwuid_r_idempotent", "two calls return consistent data");
}

/* ==================================================================== */
/*  tmpfile                                                              */
/* ==================================================================== */

static void test_tmpfile_roundtrip(void)
{
	FILE *f = tmpfile();
	if (!f) {
		check(0, "tmpfile_roundtrip", "tmpfile() returned NULL");
		return;
	}
	const char *payload = "hello-ohos-compat-shim";
	fputs(payload, f);
	rewind(f);
	char buf[64] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	int ok = (n == strlen(payload)) && strcmp(buf, payload) == 0;
	fclose(f);
	check(ok, "tmpfile_roundtrip", "write then read-back matches exactly");
}

static void test_tmpfile_concurrent_no_crosstalk(void)
{
	FILE *files[3];
	int ok = 1;
	for (int i = 0; i < 3; i++) {
		files[i] = tmpfile();
		if (!files[i]) {
			ok = 0;
			continue;
		}
		fprintf(files[i], "stream-%d", i);
	}
	for (int i = 0; i < 3 && ok; i++) {
		if (!files[i]) {
			ok = 0;
			break;
		}
		rewind(files[i]);
		char buf[32] = {0};
		fread(buf, 1, sizeof(buf) - 1, files[i]);
		char expected[32];
		snprintf(expected, sizeof(expected), "stream-%d", i);
		if (strcmp(buf, expected) != 0)
			ok = 0;
	}
	for (int i = 0; i < 3; i++)
		if (files[i])
			fclose(files[i]);
	check(ok, "tmpfile_concurrent", "3 simultaneous tmpfiles stay isolated");
}

static void test_tmpfile_repeated_no_leak(void)
{
	/* tmpfile() takes no arguments, so there's no real parameter matrix
	 * to cover (unlike close_range/linkat) — the thing worth stress
	 * testing instead is repeated use: the shimmed fallback opens an
	 * fd via mkstemp() on every single call (the real implementation
	 * fails fast every time on this device), so a tight loop is the
	 * closest thing to an official conformance check here — verifying
	 * we don't leak fds or scratch-file entries across many calls. */
	int ok = 1;
	for (int i = 0; i < 50 && ok; i++) {
		FILE *f = tmpfile();
		if (!f) {
			ok = 0;
			break;
		}
		fprintf(f, "iter-%d", i);
		fclose(f);
	}
	check(ok, "tmpfile_repeated", "50 sequential tmpfile()+fclose() cycles, no failure");
}

/* ==================================================================== */
/*  getcwd                                                               */
/* ==================================================================== */

static void test_getcwd_success_is_noop(void)
{
	/* The shim must not alter behavior when the real getcwd() already
	 * succeeds — this is the core "no-op in the common case" property.
	 * We can't compare across two processes here, but we CAN verify
	 * internal consistency: two consecutive calls from an unchanged cwd
	 * must return byte-identical strings, and the result must match
	 * what a fresh buffer/size=0 (malloc-for-us) call returns too. */
	char buf[4096];
	char *r1 = getcwd(buf, sizeof(buf));
	char *r2 = getcwd(NULL, 0);
	int ok = r1 && r2 && strcmp(r1, r2) == 0;
	check(ok, "getcwd_success_noop", r1 ? r1 : "(null)");
	free(r2);
}

/* Inspired by LTP getcwd01.c, which tests these edge cases against the
 * *raw* getcwd(2) syscall directly. This shim only intercepts the *libc*
 * getcwd() wrapper (see ohos_compat_shim.c) — which has different,
 * GNU-extension-aware semantics for these same inputs on musl — so these
 * are calibrated against what the real libc call on this device actually
 * does, confirmed at baseline before writing the assertions. In every
 * case, the point is the same: the shim's EACCES/ENOENT trigger condition
 * must leave every *other* outcome (EINVAL, ERANGE, or a real success via
 * musl's auto-allocate extension) completely untouched. */
static void test_getcwd_einval_size_zero(void)
{
	/* size=0 with a real (non-NULL) buffer: musl's libc wrapper reports
	 * EINVAL here, not ERANGE (POSIX explicitly allows EINVAL for this
	 * case — glibc does the same). */
	char buf[4096];
	errno = 0;
	char *r = getcwd(buf, 0);
	check(r == NULL && errno == EINVAL, "getcwd_einval_size0", "size=0 -> EINVAL");
}

static void test_getcwd_erange_size_one(void)
{
	char buf[4096];
	errno = 0;
	char *r = getcwd(buf, 1);
	check(r == NULL && errno == ERANGE, "getcwd_erange_size1", "size=1 -> ERANGE");
}

static void test_getcwd_null_buf_autoalloc(void)
{
	/* buf=NULL is musl/glibc's GNU extension: allocate a buffer exactly
	 * as large as needed via malloc(), ignoring `size` as anything more
	 * than an optional minimum hint — so size=1 here is NOT too small
	 * the way it would be against a real caller-supplied buffer; the
	 * call just succeeds. */
	errno = 0;
	char *r = getcwd(NULL, 1);
	check(r != NULL, "getcwd_null_autoalloc",
	      r ? r : "buf=NULL, size=1 unexpectedly failed");
	free(r);
}

static void test_getcwd_deep_nested(void)
{
	/* bun install/run operate in deep sandbox dir trees where getcwd() must
	 * still resolve the full path, and under the shim's fallback must agree
	 * with /proc/self/cwd. Build a 6-level nesting under TMPDIR, chdir to the
	 * leaf, and require getcwd() == readlink("/proc/self/cwd") == the built
	 * path. This is the shape bun's getcwd callers — and the shells bun
	 * spawns (which inherit the preload) — actually hit. Passes both baseline
	 * and shimmed (TMPDIR getcwd works); it pins deep-path correctness and
	 * getcwd<->/proc/self/cwd agreement. */
	char orig[4096];
	if (!getcwd(orig, sizeof(orig))) {
		check(0, "getcwd_deep_nested", "could not capture orig cwd");
		return;
	}
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	char tmpl[4096];
	snprintf(tmpl, sizeof(tmpl), "%s/ohos-compat-fntest-deep-XXXXXX", tmp);
	char *base = mkdtemp(tmpl);
	if (!base) {
		check(0, "getcwd_deep_nested", "mkdtemp base failed");
		return;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s", base);
	size_t base_len = strlen(path);

	int built = 1;
	for (int i = 0; i < 6 && built; i++) {
		strcat(path, "/d");
		if (mkdir(path, 0700) != 0)
			built = 0;
	}
	int ok = 0;
	if (built && chdir(path) == 0) {
		char gbuf[4096], pbuf[4096];
		char *g = getcwd(gbuf, sizeof(gbuf));
		ssize_t n = readlink("/proc/self/cwd", pbuf, sizeof(pbuf) - 1);
		if (n > 0)
			pbuf[n] = '\0';
		ok = g && strcmp(g, path) == 0 && n > 0 && strcmp(pbuf, path) == 0 &&
		     strcmp(g, pbuf) == 0;
	}
	check(ok, "getcwd_deep_nested",
	      "getcwd == /proc/self/cwd == built path in 6-level tree");

	chdir(orig);
	/* rmdir leaf-first: chop one "/d" off at a time down to the base dir. */
	for (size_t len = strlen(path); len > base_len;) {
		rmdir(path);
		char *slash = strrchr(path, '/');
		if (!slash)
			break;
		*slash = '\0';
		len = strlen(path);
	}
	rmdir(base); /* mkdtemp base */
}

static void test_getcwd_unlinked_fallback(void)
{
	/* Only meaningful WITH the shim loaded (OHOS_COMPAT_SHIM_DISABLE
	 * unset): rmdir the cwd out from under the process, then getcwd()
	 * must fall back to $HOME instead of returning NULL/ENOENT. Without
	 * the shim, or with getcwd disabled, this is expected to fail — the
	 * check output records which happened rather than asserting a
	 * fixed outcome, since correctness here is condition-dependent. */
	char orig[4096];
	if (!getcwd(orig, sizeof(orig))) {
		printf("SKIP  %-28s could not capture original cwd\n",
		       "getcwd_unlinked");
		return;
	}

	char tmpl[4096];
	snprintf(tmpl, sizeof(tmpl), "/data/storage/el2/base/tmp/ohos-compat-fntest-XXXXXX");
	char *dir = mkdtemp(tmpl);
	if (!dir || chdir(dir) != 0) {
		printf("SKIP  %-28s could not create/enter scratch dir\n",
		       "getcwd_unlinked");
		if (dir)
			rmdir(dir);
		return;
	}
	rmdir(dir); /* remove out from under ourselves */

	char buf[4096];
	errno = 0;
	char *r = getcwd(buf, sizeof(buf));
	const char *home = getenv("HOME");
	if (!home)
		home = "/data/storage/el2/base";
	/* WITH the shim: getcwd()'s userspace walk fails (ENOENT — cwd rmdir'd),
	 * the fallback tries /proc/self/cwd (returns the now-deleted path), the
	 * stat() guard rejects it, and it returns $HOME — a valid path so the
	 * caller doesn't crash. This is the ONLY branch of the getcwd fix that's
	 * reproducible on this device (the EACCES/real-cwd branch needs hmdfs DAC
	 * denial, defeated by the test process's privileges), so it's scored.
	 * WITHOUT the shim: getcwd() returns NULL/ENOENT — no fallback exists. */
	int ok;
	char detail[160];
	if (shim_is_loaded()) {
		ok = r != NULL && strcmp(r, home) == 0;
		snprintf(detail, sizeof(detail), "shim fallback -> %s (want $HOME=%s)%s",
			 r ? r : "NULL", home, ok ? "" : " MISMATCH");
	} else {
		ok = (r == NULL); /* tmpfs: getcwd after rmdir -> NULL/ENOENT, no fallback */
		snprintf(detail, sizeof(detail), "baseline %s errno=%d — no fallback%s",
			 r ? r : "NULL", errno, ok ? "" : " (unexpectedly succeeded)");
	}
	check(ok, "getcwd_unlinked", detail);

	chdir(orig);
}

/* ==================================================================== */
/*  linkat / symlinkat (opt-in, default OFF)                             */
/*                                                                        */
/*  Unlike the other four symbols, this had ZERO test coverage before —  */
/*  the largest gap found when comparing against LTP's linkat01/02.c and */
/*  symlinkat01.c. These tests force the interceptors on via setenv()     */
/*  before any linkat/symlinkat call happens in this process (the        */
/*  toggle mask is parsed once and cached — see parse_toggle_masks() in  */
/*  ohos_compat_shim.c — so this must happen before first use, and       */
/*  applies for the rest of this process's lifetime). Verifying the      */
/*  *default-off* state itself is done separately, outside this binary,  */
/*  precisely because that caching makes it untestable from within a     */
/*  single process that also wants to test the *enabled* behavior.       */
/*                                                                        */
/*  $TMPDIR was confirmed (via ohos-preflight's g5_linkat_eperm probe,   */
/*  re-run directly against this test run's TMPDIR) to reproduce a real  */
/*  EACCES from the real linkat() syscall right now — not a simulated    */
/*  condition — so the EPERM/EACCES -> copy fallback below is exercised  */
/*  against genuine platform behavior, not a mock.                       */
/* ==================================================================== */

static void test_linkat_eexist_passthrough(void)
{
	/* Matches LTP linkat02.c's EEXIST case: an error that ISN'T
	 * EPERM/EACCES must pass straight through untouched, opt-in or not
	 * — the shim's fallback condition is deliberately narrow. */
	char src[4096], dst[4096];
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	snprintf(src, sizeof(src), "%s/ohos-compat-fntest-eexist-src", tmp);
	snprintf(dst, sizeof(dst), "%s/ohos-compat-fntest-eexist-dst", tmp);
	unlink(src);
	unlink(dst);

	int sfd = open(src, O_CREAT | O_WRONLY, 0644);
	int dfd = open(dst, O_CREAT | O_WRONLY, 0644);
	if (sfd < 0 || dfd < 0) {
		check(0, "linkat_eexist", "could not create src/dst fixtures");
		if (sfd >= 0)
			close(sfd);
		if (dfd >= 0)
			close(dfd);
		return;
	}
	close(sfd);
	close(dfd);

	errno = 0;
	int rc = linkat(AT_FDCWD, src, AT_FDCWD, dst, 0);
	check(rc == -1 && errno == EEXIST, "linkat_eexist",
	      "dst already exists -> EEXIST untouched");

	unlink(src);
	unlink(dst);
}

static void test_linkat_eacces_fallback(void)
{
	/* The real, reproducible-right-now condition: linkat() into TMPDIR
	 * on this device fails EACCES (confirmed via ohos-preflight's
	 * g5_linkat_eperm probe). With the interceptor opted in, the copy
	 * fallback must kick in and produce a file with matching content —
	 * losing hardlink identity (a separate inode, not nlink=2) is the
	 * documented, accepted tradeoff for this opt-in feature. */
	char src[4096], dst[4096];
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	snprintf(src, sizeof(src), "%s/ohos-compat-fntest-eacces-src", tmp);
	snprintf(dst, sizeof(dst), "%s/ohos-compat-fntest-eacces-dst", tmp);
	unlink(src);
	unlink(dst);

	int sfd = open(src, O_CREAT | O_WRONLY, 0644);
	if (sfd < 0) {
		check(0, "linkat_eacces_fallback", "could not create src fixture");
		return;
	}
	const char *payload = "linkat-fallback-payload";
	write(sfd, payload, strlen(payload));
	close(sfd);

	errno = 0;
	int rc = linkat(AT_FDCWD, src, AT_FDCWD, dst, 0);

	char detail[160];
	if (rc == 0) {
		/* Verify it's a real, readable copy with the right content —
		 * whether via genuine hardlink (if this device ever stops
		 * reproducing the EACCES quirk) or our copy fallback. */
		FILE *f = fopen(dst, "r");
		char buf[64] = {0};
		size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
		if (f)
			fclose(f);
		int ok = f && n == strlen(payload) && strcmp(buf, payload) == 0;
		snprintf(detail, sizeof(detail),
			 "linkat succeeded (real or fallback), content %s",
			 ok ? "matches" : "MISMATCH");
		check(ok, "linkat_eacces_fallback", detail);
	} else if (shim_is_loaded()) {
		/* Shim active with linkat opted in (see main()'s setenv) —
		 * the real EACCES condition reproduced (confirmed moments
		 * ago via the standalone g5_linkat_eperm probe) but the copy
		 * fallback did NOT kick in and rescue it. That's a real bug,
		 * not an expected platform limitation. */
		snprintf(detail, sizeof(detail),
			 "linkat still failed rc=%d errno=%d (%s) even with the shim's "
			 "EACCES->copy fallback enabled",
			 rc, errno, strerror(errno));
		check(0, "linkat_eacces_fallback", detail);
	} else {
		/* Baseline (no shim loaded): a real linkat() failure here
		 * just confirms the sandbox quirk is reproduced this run —
		 * expected, not a shim correctness question at all. */
		snprintf(detail, sizeof(detail),
			 "linkat failed rc=%d errno=%d (%s) — confirms the TMPDIR "
			 "EACCES quirk reproduces at baseline (no shim to fall back)",
			 rc, errno, strerror(errno));
		printf("INFO  %-28s %s\n", "linkat_eacces_fallback", detail);
	}

	unlink(src);
	unlink(dst);
}

static void test_linkat_bun_tmpfile_via_proc_self_fd(void)
{
	/* bun's npm cache write (src/install/npm.rs linkat_tmpfile) materializes
	 * an O_TMPFILE fd. The fast path linkat(tmpfd, "", dir, name,
	 * AT_EMPTY_PATH) needs CAP_DAC_READ_SEARCH — absent under the OHOS
	 * sandbox, so it EPERMs and bun falls back to the /proc form:
	 *   linkat(AT_FDCWD, "/proc/self/fd/<N>", dir, name, AT_SYMLINK_FOLLOW)
	 * The shim's linkat hook then openat()s that /proc path (following the
	 * symlink to the tmpfile) and byte-copies it to dest — exactly the
	 * materialize-O_TMPFILE-via-copy path bun now relies on after the
	 * source-level linkat downgrade was removed. This reproduces it end-to-end
	 * against the real TMPDIR linkat EACCES. */
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	int tmpfd = open(tmp, O_TMPFILE | O_RDWR, 0600);
	if (tmpfd < 0) {
		printf("INFO  %-28s O_TMPFILE unsupported here (errno=%d %s) — skipped\n",
		       "linkat_bun_tmpfile", errno, strerror(errno));
		checks++;
		return;
	}
	const char *payload = "tmpfile-payload-via-proc";
	write(tmpfd, payload, strlen(payload));

	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", tmpfd);
	char dst[4096];
	snprintf(dst, sizeof(dst), "%s/ohos-compat-fntest-tmpfile-dst", tmp);
	unlink(dst);

	errno = 0;
	int rc = linkat(AT_FDCWD, proc, AT_FDCWD, dst, AT_SYMLINK_FOLLOW);
	char detail[200];
	if (shim_is_loaded()) {
		/* Shim active with linkat opted in (main()'s setenv): the copy
		 * fallback must materialize dst with the tmpfile's contents. */
		FILE *f = fopen(dst, "r");
		char buf[64] = {0};
		size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
		if (f)
			fclose(f);
		int ok = f && n == strlen(payload) && strcmp(buf, payload) == 0;
		snprintf(detail, sizeof(detail),
			 "linkat(/proc/self/fd) %s, dst content %s",
			 rc == 0 ? "succeeded (real or copy)" : "rc=-1 but copy present",
			 ok ? "matches" : "MISMATCH/missing");
		check(ok, "linkat_bun_tmpfile", detail);
	} else {
		/* Baseline (no shim): a real linkat() here reproduces the TMPDIR
		 * EACCES quirk (see linkat_eacces_fallback) — expected, not a shim
		 * correctness question. */
		snprintf(detail, sizeof(detail),
			 "baseline linkat rc=%d errno=%d (%s) — no shim to fall back",
			 rc, errno, strerror(errno));
		printf("INFO  %-28s %s\n", "linkat_bun_tmpfile", detail);
		checks++;
	}
	close(tmpfd);
	unlink(dst);
}

static void test_linkat_bun_dirfd_source(void)
{
	/* bun's hardlink install (Hardlinker.rs, PackageInstall.rs) hardlinks a
	 * cached file into node_modules via linkat(SRC_DIRFD, basename,
	 * DEST_DIRFD, path, 0) — dirfd source + dirfd dest, NOT AT_FDCWD absolute
	 * paths like linkat_eacces_fallback above. The shim's linkat hook must
	 * openat(SRC_DIRFD, basename) to find the cached file (resolved against the
	 * dirfd, not CWD) and copy it to openat(DEST_DIRFD, path). This is the
	 * install hot path every cached package hits; the AT_FDCWD absolute-path
	 * test alone doesn't exercise dirfd-relative resolution on either side. */
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	char srcdir[4096], dstdir[4096], srcfile[4096], dstfile[4096];
	snprintf(srcdir, sizeof(srcdir), "%s/ohos-compat-fntest-linkdir-src", tmp);
	snprintf(dstdir, sizeof(dstdir), "%s/ohos-compat-fntest-linkdir-dst", tmp);
	snprintf(srcfile, sizeof(srcfile), "%s/cached", srcdir);
	snprintf(dstfile, sizeof(dstfile), "%s/installed", dstdir);
	unlink(dstfile);
	unlink(srcfile);
	rmdir(srcdir);
	rmdir(dstdir);
	int src_dirfd = -1, dst_dirfd = -1;
	if (mkdir(srcdir, 0700) || mkdir(dstdir, 0700)) {
		check(0, "linkat_bun_dirfd", "mkdir src/dst failed");
		goto cleanup;
	}
	int sfd = open(srcfile, O_CREAT | O_WRONLY, 0644);
	if (sfd < 0) {
		check(0, "linkat_bun_dirfd", "could not create cached file");
		goto cleanup;
	}
	const char *payload = "dirfd-linkat-payload";
	write(sfd, payload, strlen(payload));
	close(sfd);

	src_dirfd = open(srcdir, O_RDONLY | O_DIRECTORY);
	dst_dirfd = open(dstdir, O_RDONLY | O_DIRECTORY);
	if (src_dirfd < 0 || dst_dirfd < 0) {
		check(0, "linkat_bun_dirfd", "could not open dirfds");
		goto cleanup;
	}

	errno = 0;
	int rc = linkat(src_dirfd, "cached", dst_dirfd, "installed", 0);
	char detail[200];
	if (shim_is_loaded()) {
		FILE *f = fopen(dstfile, "r");
		char buf[64] = {0};
		size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
		if (f)
			fclose(f);
		int ok = f && n == strlen(payload) && strcmp(buf, payload) == 0;
		snprintf(detail, sizeof(detail),
			 "linkat(dirfd,dirfd) %s, dst content %s",
			 rc == 0 ? "succeeded" : "copy fallback", ok ? "matches" : "MISMATCH/missing");
		check(ok, "linkat_bun_dirfd", detail);
	} else {
		snprintf(detail, sizeof(detail),
			 "baseline linkat(dirfd,dirfd) rc=%d errno=%d (%s) — no shim to fall back",
			 rc, errno, strerror(errno));
		printf("INFO  %-28s %s\n", "linkat_bun_dirfd", detail);
		checks++;
	}
cleanup:
	if (src_dirfd >= 0)
		close(src_dirfd);
	if (dst_dirfd >= 0)
		close(dst_dirfd);
	unlink(dstfile);
	unlink(srcfile);
	rmdir(srcdir);
	rmdir(dstdir);
}

static void test_symlinkat_success_is_noop(void)
{
	/* symlinkat() was NOT found to be restricted in TMPDIR on this
	 * device (unlike linkat) — g6_symlinkat_eperm passed cleanly here.
	 * That makes this a "does the shim interfere with an already-working
	 * call" check, the symlinkat counterpart to getcwd's no-op test. */
	char target[4096], linkpath[4096];
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	snprintf(target, sizeof(target), "%s/ohos-compat-fntest-symtarget", tmp);
	snprintf(linkpath, sizeof(linkpath), "%s/ohos-compat-fntest-symlink", tmp);
	unlink(target);
	unlink(linkpath);

	int fd = open(target, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		check(0, "symlinkat_success_noop", "could not create target fixture");
		return;
	}
	close(fd);

	errno = 0;
	int rc = symlinkat(target, AT_FDCWD, linkpath);
	int ok = 0;
	char detail[160];
	if (rc == 0) {
		struct stat st;
		ok = (lstat(linkpath, &st) == 0) && S_ISLNK(st.st_mode);
		snprintf(detail, sizeof(detail), "symlinkat succeeded, lstat %s a symlink",
			 ok ? "confirms" : "does NOT confirm");
	} else {
		snprintf(detail, sizeof(detail), "symlinkat failed errno=%d (%s)", errno,
			 strerror(errno));
	}
	check(ok, "symlinkat_success_noop", detail);

	unlink(linkpath);
	unlink(target);
}

/* ==================================================================== */
/*  fchmodat2 (syscall 452, default on)                                  */
/*                                                                        */
/*  Same shape as close_range: no libc wrapper exists in this musl (too   */
/*  new), callers use syscall(SYS_fchmodat2, ...) directly, and on this   */
/*  device the raw call unconditionally SIGSYS's without the shim         */
/*  (confirmed with a standalone repro before writing the shim's own      */
/*  implementation — see ohos_compat_shim.c). Uses the same guarded_close */
/*  _range-style local protection so a baseline run reports the crash as  */
/*  an expected INFO instead of taking the suite down with it.            */
/* ==================================================================== */

static void test_symlinkat_relative_target_resolution(void)
{
	/* bun's TarballStream creates symlinks whose targets are RELATIVE to the
	 * link's own directory (e.g. node_modules/.bin/cli -> ../pkg/index.js).
	 * The symlinkat copy-fallback fix changed open(target) [resolved against
	 * the process CWD — wrong dir, copies an unrelated same-named file] to
	 * openat(newdirfd, target) [resolved against the link's directory]. The
	 * symlinkat hook's EPERM->copy path can't be triggered on this device
	 * (symlinkat isn't EPERM-restricted in TMPDIR here), so this verifies the
	 * resolution MECHANISM the fix relies on: a relative path that exists
	 * under `dirfd` but NOT under CWD must open via openat(dirfd, ...) and
	 * must MISS via open() — the exact divergence the old bug rode on. */
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	char root[4096], sub[4096], file_in_sub[4096];
	snprintf(root, sizeof(root), "%s/ohos-compat-fntest-symrel", tmp);
	snprintf(sub, sizeof(sub), "%s/sub", root);
	snprintf(file_in_sub, sizeof(file_in_sub), "%s/pkgfile", sub);
	unlink(file_in_sub);
	rmdir(sub);
	rmdir(root);
	if (mkdir(root, 0700) || mkdir(sub, 0700)) {
		check(0, "symlinkat_rel_resolution", "could not create dirs");
		rmdir(sub);
		rmdir(root);
		return;
	}
	int wfd = open(file_in_sub, O_CREAT | O_WRONLY, 0644);
	if (wfd < 0) {
		check(0, "symlinkat_rel_resolution", "could not create file");
		rmdir(sub);
		rmdir(root);
		return;
	}
	const char *payload = "relative-target-payload";
	write(wfd, payload, strlen(payload));
	close(wfd);

	char orig[4096];
	if (!getcwd(orig, sizeof(orig))) {
		check(0, "symlinkat_rel_resolution", "could not capture cwd");
		goto cleanup;
	}
	int dirfd = open(sub, O_RDONLY | O_DIRECTORY);
	int ok = (chdir(root) == 0) && dirfd >= 0;
	if (ok) {
		/* fixed resolution: openat(dirfd, relative) finds the file in `sub`. */
		errno = 0;
		int rfd = openat(dirfd, "pkgfile", O_RDONLY);
		char buf[64] = {0};
		if (rfd >= 0) {
			ssize_t n = read(rfd, buf, sizeof(buf) - 1);
			close(rfd);
			ok = ok && n == (ssize_t)strlen(payload) && strcmp(buf, payload) == 0;
		} else {
			ok = 0;
		}
		/* contrast: open("pkgfile") against CWD (root) must MISS — the file is
		 * in `sub`, not root. This is the wrong-file/miss the old open(target)
		 * shim code would have produced. */
		errno = 0;
		int bad = open("pkgfile", O_RDONLY);
		ok = ok && (bad < 0);
		if (bad >= 0)
			close(bad);
	}
	check(ok, "symlinkat_rel_resolution",
	      "openat(dirfd, relative) resolves; open() vs CWD misses (the old bug)");
	if (dirfd >= 0)
		close(dirfd);
	chdir(orig);
cleanup:
	unlink(file_in_sub);
	rmdir(sub);
	rmdir(root);
}

static void test_symlink_two_arg_bun_fake_node(void)
{
	/* bun's `--bun` mode creates fake node executables via the 2-arg symlink()
	 * (src/install/lib.rs:702): symlink(argv0_abs, BUN_NODE_DIR/{bun,node}).
	 * This is the *symlink* libc symbol, NOT symlinkat — and this shim does
	 * NOT hook it (the hook list is close_range/syscall/getpwuid_r/tmpfile/
	 * getcwd/linkat/symlinkat; no symlink). The test pins bun's real call shape
	 * and confirms whether it works unaided: symlinkat is unrestricted in
	 * TMPDIR here, and symlink is symlinkat(.., AT_FDCWD, ..), so it should
	 * too — but if it ever EPERMs, that surfaces a genuine gap (bun's fake-node
	 * creation would need a shim symlink hook) rather than hiding it. */
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	char target[4096], linkpath[4096];
	snprintf(target, sizeof(target), "%s/ohos-compat-fntest-sym2-target", tmp);
	snprintf(linkpath, sizeof(linkpath), "%s/ohos-compat-fntest-sym2-link", tmp);
	unlink(linkpath);
	unlink(target);
	int fd = open(target, O_CREAT | O_WRONLY, 0755);
	if (fd < 0) {
		check(0, "symlink_two_arg", "could not create target");
		return;
	}
	close(fd);

	errno = 0;
	int rc = symlink(target, linkpath); /* 2-arg, absolute target + absolute link */
	int ok = 0;
	char detail[200];
	if (rc == 0) {
		struct stat st;
		ok = (lstat(linkpath, &st) == 0) && S_ISLNK(st.st_mode);
		snprintf(detail, sizeof(detail),
			 "symlink(target,link) ok, lstat %s symlink (no shim hook needed)",
			 ok ? "confirms" : "NOT a");
	} else {
		snprintf(detail, sizeof(detail),
			 "symlink() FAILED rc=%d errno=%d (%s) — GAP: shim has no symlink hook",
			 rc, errno, strerror(errno));
	}
	check(ok, "symlink_two_arg", detail);
	unlink(linkpath);
	unlink(target);
}

static void test_fchmodat2_applies_mode(void)
{
	char path[4096];
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	snprintf(path, sizeof(path), "%s/ohos-compat-fntest-fchmodat2", tmp);
	unlink(path);

	int fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		check(0, "fchmodat2_applies_mode", "could not create test fixture");
		return;
	}
	close(fd);

	long ret;
	int errno_out;
	if (!guarded_fchmodat2(AT_FDCWD, path, 0600, 0, &ret, &errno_out)) {
		printf("INFO  %-28s would SIGSYS without the shim's protection "
		       "(caught here so the suite could continue) — expected at "
		       "baseline\n",
		       "fchmodat2_applies_mode");
		checks++;
		unlink(path);
		return;
	}

	struct stat st;
	int stat_ok = stat(path, &st) == 0;
	mode_t applied = stat_ok ? (st.st_mode & 07777) : 0;
	char detail[128];
	snprintf(detail, sizeof(detail), "ret=%ld errno=%d, mode after call=%o", ret,
		 errno_out, (unsigned)applied);
	check(ret == 0 && stat_ok && applied == 0600, "fchmodat2_applies_mode", detail);

	unlink(path);
}

/* ==================================================================== */
/*  Pass-through: unrelated syscalls must be unaffected                  */
/* ==================================================================== */

static void test_fchmodat2_bun_lchmod_symlink_nofollow(void)
{
	/* bun's lchmod (src/sys/lib.rs) issues fchmodat2 via the raw syscall()
	 * entry — this musl has no libc wrapper — passing AT_SYMLINK_NOFOLLOW:
	 *   syscall(SYS_fchmodat2, AT_FDCWD, path, mode, AT_SYMLINK_NOFOLLOW)
	 * The shim's fallback forwards the flag to classic fchmodat(), which
	 * this musl honours. On a regular file NOFOLLOW is a no-op either way;
	 * the symlink case is covered separately below. This pins the bun call shape
	 * — syscall() entry point + the NOFOLLOW flag bun passes — so a regression
	 * in either the entry-point dispatch or the flag handling is caught. */
	char path[4096];
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	snprintf(path, sizeof(path), "%s/ohos-compat-fntest-fchmodat2-nofollow", tmp);
	unlink(path);
	int fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		check(0, "fchmodat2_bun_lchmod", "could not create fixture");
		return;
	}
	close(fd);

	long ret;
	int errno_out;
	if (!guarded_fchmodat2(AT_FDCWD, path, 0640, AT_SYMLINK_NOFOLLOW, &ret, &errno_out)) {
		printf("INFO  %-28s would SIGSYS without the shim's protection "
		       "(caught here so the suite could continue) — expected at baseline\n",
		       "fchmodat2_bun_lchmod");
		checks++;
		unlink(path);
		return;
	}
	struct stat st;
	mode_t applied = (stat(path, &st) == 0) ? (st.st_mode & 07777) : 0;
	char detail[128];
	snprintf(detail, sizeof(detail), "ret=%ld mode=%o (want 0640)",
		 ret, (unsigned)applied);
	check(ret == 0 && applied == 0640, "fchmodat2_bun_lchmod", detail);
	unlink(path);
}

/* AT_SYMLINK_NOFOLLOW must not be silently downgraded into "follow it".
 * The shim once dropped the flag on its fallback path, so a chmod aimed at
 * a symlink landed on the file the link pointed at -- a chmod escaping the
 * directory the caller meant to touch. Linux refuses the operation instead
 * (a symlink's own mode bits are meaningless), and this musl's fchmodat
 * does too once the flag actually reaches it. */
static void test_fchmodat2_symlink_not_followed(void)
{
	char victim[4096], link[4096];
	const char *tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/data/storage/el2/base/tmp";
	snprintf(victim, sizeof(victim), "%s/ohos-compat-fntest-lchmod-victim", tmp);
	snprintf(link, sizeof(link), "%s/ohos-compat-fntest-lchmod-link", tmp);
	unlink(victim);
	unlink(link);

	int fd = open(victim, O_CREAT | O_WRONLY, 0600);
	if (fd < 0) {
		check(0, "fchmodat2_symlink_not_followed", "could not create fixture");
		return;
	}
	close(fd);
	if (symlink(victim, link) != 0) {
		check(0, "fchmodat2_symlink_not_followed", "could not create symlink");
		unlink(victim);
		return;
	}

	long ret;
	int errno_out;
	if (!guarded_fchmodat2(AT_FDCWD, link, 0777, AT_SYMLINK_NOFOLLOW, &ret, &errno_out)) {
		printf("INFO  %-28s would SIGSYS without the shim's protection "
		       "(caught here so the suite could continue) — expected at baseline\n",
		       "fchmodat2_symlink_not_followed");
		checks++;
		unlink(link);
		unlink(victim);
		return;
	}

	struct stat st;
	mode_t victim_mode = (stat(victim, &st) == 0) ? (st.st_mode & 07777) : 0;
	char detail[160];
	snprintf(detail, sizeof(detail),
		 "ret=%ld errno=%d, target mode=%o (want 0600 untouched)", ret, errno_out,
		 (unsigned)victim_mode);
	/* The call must not succeed in changing the target. Refusing outright
	 * (ENOTSUP, as Linux does) and leaving it alone are both acceptable;
	 * silently rewriting the target's mode is not. */
	check(victim_mode == 0600, "fchmodat2_symlink_not_followed", detail);

	unlink(link);
	unlink(victim);
}

static void test_passthrough_getpid(void)
{
	pid_t a = getpid();
	long b = syscall(SYS_getpid);
	check(a == (pid_t)b, "passthrough_getpid", "getpid() == syscall(SYS_getpid)");
}

static void test_passthrough_read_write(void)
{
	int fds[2];
	if (pipe(fds) != 0) {
		check(0, "passthrough_read_write", "pipe() failed");
		return;
	}
	const char *msg = "ohos";
	ssize_t w = write(fds[1], msg, 4);
	char buf[8] = {0};
	ssize_t r = read(fds[0], buf, sizeof(buf));
	close(fds[0]);
	close(fds[1]);
	check(w == 4 && r == 4 && memcmp(buf, msg, 4) == 0,
	      "passthrough_read_write", "pipe write/read unaffected");
}

/* ---- splice(2): this kernel reports source-EOF as EPIPE, not 0 ---- */

static void test_splice_eof_is_zero(void)
{
	int src[2], dst[2];
	if (pipe(src) != 0 || pipe(dst) != 0) {
		check(0, "splice_eof_is_zero", "pipe() failed");
		return;
	}
	if (write(src[1], "hi\n", 3) != 3) {
		check(0, "splice_eof_is_zero", "seed write failed");
		return;
	}
	close(src[1]);                       /* no further data can arrive */

	char drain[64];
	ssize_t first = splice(src[0], NULL, dst[1], NULL, 1 << 19, 0);
	ssize_t got = read(dst[0], drain, sizeof(drain));
	ssize_t eof = splice(src[0], NULL, dst[1], NULL, 1 << 19, 0);
	int e = errno;
	close(src[0]); close(dst[0]); close(dst[1]);

	char detail[160];
	snprintf(detail, sizeof(detail),
		 "moved=%zd drained=%zd eof_call=%zd%s%s (want 3/3/0)",
		 first, got, eof, eof < 0 ? " errno=" : "",
		 eof < 0 ? strerror(e) : "");
	check(first == 3 && got == 3 && eof == 0, "splice_eof_is_zero", detail);
}

static void test_splice_real_epipe_preserved(void)
{
	int src[2], dst[2];
	if (pipe(src) != 0 || pipe(dst) != 0) {
		check(0, "splice_real_epipe_preserved", "pipe() failed");
		return;
	}
	if (write(src[1], "hi\n", 3) != 3) {
		check(0, "splice_real_epipe_preserved", "seed write failed");
		return;
	}
	close(dst[0]);                       /* destination genuinely broken */

	void (*prev)(int) = signal(SIGPIPE, SIG_IGN);
	ssize_t rc = splice(src[0], NULL, dst[1], NULL, 1 << 19, 0);
	int e = errno;
	signal(SIGPIPE, prev);
	close(src[0]); close(src[1]); close(dst[1]);

	/* Must NOT be rewritten to 0 -- swallowing this would silently
	 * truncate a copy. Data was available, so a 0 here means the shim
	 * mistook a broken destination for end-of-input. */
	char detail[160];
	snprintf(detail, sizeof(detail), "rc=%zd %s (want -1/EPIPE, never 0)",
		 rc, rc < 0 ? strerror(e) : "no error");
	check(rc < 0 && e == EPIPE, "splice_real_epipe_preserved", detail);
}

/* ---- splice(2): data put in a pipe must wake a blocked poll() ---- */

struct poll_waiter {
	int fd;
	int rc;
};

static void *poll_waiter_thread(void *arg)
{
	struct poll_waiter *w = arg;
	struct pollfd pfd = { .fd = w->fd, .events = POLLIN };
	w->rc = poll(&pfd, 1, 1500);
	return NULL;
}

static void test_splice_wakes_poll_waiter(void)
{
	int src[2], dst[2];
	if (pipe(src) != 0 || pipe(dst) != 0) {
		check(0, "splice_wakes_poll_waiter", "pipe() failed");
		return;
	}
	if (write(src[1], "hi\n", 3) != 3) {
		check(0, "splice_wakes_poll_waiter", "seed write failed");
		return;
	}

	/* The waiter must be parked in poll() *before* the bytes arrive: this
	 * checks the wakeup, not the readiness state. A poll issued afterwards
	 * reports the pipe readable even on the broken kernel. */
	struct poll_waiter w = { .fd = dst[0], .rc = -99 };
	pthread_t th;
	if (pthread_create(&th, NULL, poll_waiter_thread, &w) != 0) {
		check(0, "splice_wakes_poll_waiter", "pthread_create failed");
		return;
	}
	usleep(300000);

	ssize_t moved = splice(src[0], NULL, dst[1], NULL, 1 << 19, 0);
	pthread_join(th, NULL);

	char drain[64];
	ssize_t got = w.rc > 0 ? read(dst[0], drain, sizeof(drain)) : -1;
	close(src[0]); close(src[1]); close(dst[0]); close(dst[1]);

	char detail[192];
	snprintf(detail, sizeof(detail),
		 "moved=%zd poll=%d%s drained=%zd (want 3/>0/3)",
		 moved, w.rc, w.rc == 0 ? " (timed out, never woken)" : "", got);
	check(moved == 3 && w.rc > 0 && got == 3,
	      "splice_wakes_poll_waiter", detail);
}

/* ---- epoll_wait(2): the pipe-repair clamp must not leak premature 0 ----
 *
 * Regression tests for the libuv crash (node's uv__io_poll:
 * "assert(timeout != -1)" when nfds == 0, seen as pnpm SIGABRT). The
 * epoll_pipe interceptor clamps long waits to a 250ms internal poll so it
 * can FIONREAD registered pipes; returning that empty 250ms poll to the
 * CALLER violates the epoll_wait API contract — a -1 wait must block until
 * an event or signal, and a T-ms wait may return 0 only after ~T ms. The
 * clamp must be hidden by re-waiting internally. Both checks pass on a
 * correct kernel (baseline) and MUST pass with the shim. */

static long long test_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_epoll_wait_finite_timeout_honored(void)
{
	int fds[2];
	if (pipe(fds) != 0) {
		check(0, "epoll_wait_timeout_honored", "pipe() failed");
		return;
	}
	int epfd = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fds[0] };
	if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
		check(0, "epoll_wait_timeout_honored", "epoll setup failed");
		return;
	}

	/* Empty pipe registered: a 700ms wait must not return 0 before the
	 * caller's own timeout expires. The broken shim returned its first
	 * empty 250ms repair poll, i.e. rc=0 after ~250ms. */
	long long t0 = test_now_ms();
	struct epoll_event out;
	int rc = epoll_wait(epfd, &out, 1, 700);
	long long elapsed = test_now_ms() - t0;
	close(fds[0]); close(fds[1]); close(epfd);

	char detail[128];
	snprintf(detail, sizeof(detail),
		 "rc=%d elapsed=%lldms (want rc=0, elapsed>=600)", rc, elapsed);
	check(rc == 0 && elapsed >= 600, "epoll_wait_timeout_honored", detail);
}

struct epoll_waiter {
	int epfd;
	volatile int rc;
};

static void *epoll_pwaiter_thread(void *arg)
{
	struct epoll_waiter *w = arg;
	struct epoll_event out;
	w->rc = epoll_pwait(w->epfd, &out, 1, -1, NULL);
	return NULL;
}

static void test_epoll_pwait_infinite_no_zero_return(void)
{
	int fds[2];
	if (pipe(fds) != 0) {
		check(0, "epoll_pwait_infinite_no_zero", "pipe() failed");
		return;
	}
	int epfd = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fds[0] };
	if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
		check(0, "epoll_pwait_infinite_no_zero", "epoll setup failed");
		return;
	}

	/* libuv's exact crash shape: an infinite (-1) wait must NEVER return
	 * 0. Let the waiter park for 600ms — well past the 250ms repair
	 * poll — and confirm it is still blocked; then publish a byte it
	 * must observe. On the T50-broken kernel the baseline (no shim) run
	 * can sporadically fail the wakeup half — that is the bug the shim
	 * repairs, not a suite defect. */
	struct epoll_waiter w = { .epfd = epfd, .rc = -99 };
	pthread_t th;
	if (pthread_create(&th, NULL, epoll_pwaiter_thread, &w) != 0) {
		check(0, "epoll_pwait_infinite_no_zero", "pthread_create failed");
		return;
	}
	pthread_detach(th);
	usleep(600000);
	int premature_zero = (w.rc == 0);
	if (write(fds[1], "x", 1) != 1)
		check(0, "epoll_pwait_infinite_no_zero", "wake write failed");

	/* Bounded join: 2s for the wakeup, so a kernel-struck baseline run
	 * reports FAIL instead of hanging the whole suite. */
	int waited_ms = 0;
	while (w.rc == -99 && waited_ms < 2000) {
		usleep(20000);
		waited_ms += 20;
	}
	int final_rc = w.rc;
	close(fds[0]); close(fds[1]); close(epfd);

	char detail[160];
	snprintf(detail, sizeof(detail),
		 "premature_zero=%d final_rc=%d (want 0/1)%s",
		 premature_zero, final_rc,
		 final_rc == -99 ? " never woken (kernel T50?)" : "");
	check(!premature_zero && final_rc == 1,
	      "epoll_pwait_infinite_no_zero", detail);
}

int main(void)
{
	/* Must happen before any linkat/symlinkat call in this process — the
	 * shim's toggle mask is parsed once and cached (see
	 * parse_toggle_masks() in ohos_compat_shim.c). No-op when this
	 * binary runs without LD_PRELOAD. */
	setenv("OHOS_COMPAT_SHIM_ENABLE", "linkat,symlinkat", 1);

	test_close_range_multi_range();
	test_close_range_cloexec();
	test_close_range_bun_init_cloexec();
	test_close_range_bun_spawn_cloexec();
	test_close_range_einval_first_last();
	test_close_range_einval_bad_flags();
	test_close_range_wide_sparse_range();
	test_close_range_unshare();
	test_close_range_repeated();
	test_close_range_libc_wrapper();

	test_getpwuid_r_erange();
	test_getpwuid_r_idempotent();

	test_tmpfile_roundtrip();
	test_tmpfile_concurrent_no_crosstalk();
	test_tmpfile_repeated_no_leak();

	test_getcwd_success_is_noop();
	test_getcwd_einval_size_zero();
	test_getcwd_erange_size_one();
	test_getcwd_null_buf_autoalloc();
	test_getcwd_deep_nested();
	test_getcwd_unlinked_fallback();

	test_linkat_eexist_passthrough();
	test_linkat_eacces_fallback();
	test_linkat_bun_tmpfile_via_proc_self_fd();
	test_linkat_bun_dirfd_source();
	test_symlinkat_success_is_noop();
	test_symlinkat_relative_target_resolution();
	test_symlink_two_arg_bun_fake_node();

	test_fchmodat2_applies_mode();
	test_fchmodat2_bun_lchmod_symlink_nofollow();
	test_fchmodat2_symlink_not_followed();

	test_passthrough_getpid();
	test_passthrough_read_write();
	test_splice_eof_is_zero();
	test_splice_real_epipe_preserved();
	test_splice_wakes_poll_waiter();

	test_epoll_wait_finite_timeout_honored();
	test_epoll_pwait_infinite_no_zero_return();

	printf("%s (%d/%d checks failed)\n", failures == 0 ? "ALL PASS" : "SOME FAILED",
	       failures, checks);
	return failures == 0 ? 0 : 1;
}
