/*
 * test/bench.c — measures per-call overhead added by this shim, run once
 * without LD_PRELOAD (baseline) and once with it (`make bench`). Compare
 * the two ns/call numbers by hand — this prints raw measurements, it does
 * not itself compute a diff (each run is a separate process/exec).
 *
 * What each benchmark actually measures:
 *
 *   syscall_passthrough — a cheap syscall (getpid) issued via the raw
 *     syscall() symbol, looped many times. Baseline hits libc's syscall()
 *     directly; shimmed goes through our syscall() override's
 *     number-check + (cached) toggle-mask lookup before delegating via
 *     dlsym(RTLD_NEXT). This is the tax paid by *every* raw syscall() call
 *     in the process once this library is preloaded, not just close_range
 *     — the number that matters most for a syscall()-heavy consumer like
 *     Bun's c-bindings.cpp (close_range, pwritev2, exit_group all go
 *     through the public syscall() symbol).
 *
 *   close_range — open+close a fresh fd via close_range each iteration.
 *     On this device the real syscall unconditionally raises SIGSYS
 *     (confirmed independent of LD_PRELOAD — see README's "Known platform
 *     behavior"), so an unprotected baseline call cannot complete even one
 *     iteration. The first attempt is guarded to detect this and bail out
 *     with an honest "N/A" instead of a misleading number (or, on a device
 *     where it doesn't crash, proceeds unguarded for accurate timing).
 *
 *   getpwuid_r / tmpfile / getcwd — baseline always takes the real
 *     (failing, for the first two) codepath; shimmed measures real-call
 *     attempt + fallback synthesis.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_close_range
#define __NR_close_range 436
#endif

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void report(const char *name, long iters, double ms)
{
	printf("%-22s %8ld iters  %10.3f ms total  %10.1f ns/call\n", name, iters,
	       ms, (ms * 1e6) / (double)iters);
}

static void bench_syscall_passthrough(void)
{
	long iters = 200000;
	double t0 = now_ms();
	for (long i = 0; i < iters; i++)
		syscall(SYS_getpid);
	report("syscall_passthrough", iters, now_ms() - t0);
}

static sigjmp_buf bench_jmp;

static void bench_sigsys_handler(int sig)
{
	(void)sig;
	siglongjmp(bench_jmp, 1);
}

static void bench_close_range(void)
{
	long iters = 2000;

	/* Probe iteration 1 guarded — on this device it never completes
	 * without a shim, and letting that crash take out the whole bench
	 * binary would silently lose every subsequent benchmark too. */
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		printf("%-22s could not open a test fd\n", "close_range");
		return;
	}

	struct sigaction old_sa, sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = bench_sigsys_handler;
	sigaction(SIGSYS, &sa, &old_sa);

	int crashed = 0;
	if (sigsetjmp(bench_jmp, 1) == 0) {
		syscall(__NR_close_range, fd, fd, 0);
	} else {
		crashed = 1;
	}
	sigaction(SIGSYS, &old_sa, NULL);

	if (crashed) {
		printf("%-22s N/A — unconditionally SIGSYS's without shim protection "
		       "on this device (see README)\n",
		       "close_range");
		return;
	}

	/* First call survived — proceed unguarded for accurate steady-state
	 * timing (matches how the shim's own WORKS-path behaves: guarded
	 * once, then trusted). */
	double t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		fd = open("/dev/null", O_RDONLY);
		if (fd < 0)
			continue;
		syscall(__NR_close_range, fd, fd, 0);
	}
	report("close_range", iters, now_ms() - t0);
}

/* Standalone measurement of the userspace /proc/self/fd fallback path in
 * isolation (same steps as ohos_compat_shim.c's cr_do_fallback), regardless
 * of whether THIS run's close_range benchmark above actually triggered it.
 * Whether the real delegate succeeds or gets SIGSYS-blocked appears to vary
 * with the exact LD_PRELOAD chain in ways not fully pinned down (see
 * README's "Known platform behavior") — this gives an honest worst-case
 * number for the fallback branch either way. */
static void bench_close_range_fallback_path(void)
{
	long iters = 2000;
	double t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		int fd = open("/dev/null", O_RDONLY);
		if (fd < 0)
			continue;
		DIR *d = opendir("/proc/self/fd");
		int dirfd_ = d ? dirfd(d) : -1;
		if (!d) {
			close(fd);
			continue;
		}
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] < '0' || e->d_name[0] > '9')
				continue;
			unsigned int cand = (unsigned int)atoi(e->d_name);
			if ((int)cand == fd && (int)cand != dirfd_)
				close((int)cand);
		}
		closedir(d);
	}
	report("close_range_fallback", iters, now_ms() - t0);
}

static void bench_getpwuid_r(void)
{
	long iters = 5000;
	struct passwd pwd, *result;
	char buf[1024];
	double t0 = now_ms();
	for (long i = 0; i < iters; i++)
		getpwuid_r(geteuid(), &pwd, buf, sizeof(buf), &result);
	report("getpwuid_r", iters, now_ms() - t0);
}

static void bench_tmpfile(void)
{
	long iters = 200;
	double t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		FILE *f = tmpfile();
		if (f)
			fclose(f);
	}
	report("tmpfile", iters, now_ms() - t0);
}

static void bench_getcwd(void)
{
	long iters = 5000;
	char buf[4096];
	double t0 = now_ms();
	for (long i = 0; i < iters; i++)
		getcwd(buf, sizeof(buf));
	report("getcwd", iters, now_ms() - t0);
}

int main(void)
{
	bench_syscall_passthrough();
	bench_close_range();
	bench_close_range_fallback_path();
	bench_getpwuid_r();
	bench_tmpfile();
	bench_getcwd();
	return 0;
}
