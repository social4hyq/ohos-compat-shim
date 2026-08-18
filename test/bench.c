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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
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

/*
 * bench_poll_patch_* / bench_epoll_wait_no_pipe / bench_epoll_ctl_churn —
 * added for the 2026-08-18 shim performance/stability validation pass
 * (epoll_pipe interceptor had zero benchmark coverage before this).
 *
 * poll_patch_idle_fds / poll_patch_pipe_fds isolate ep_shim_patch_pollfds()
 * (ohos_compat_shim.c): whenever the epoll_pipe interceptor is enabled at
 * all -- gated only by the global shim_disabled("epoll_pipe") check, NOT by
 * whether any pipe was ever actually registered -- every poll()/ppoll()
 * return runs an uncached fstat() (splice_fd_is_fifo()) on every fd that
 * asked for POLLIN and came back with revents==0. That is an O(N) tax on
 * the caller's *entire idle fd set*, paid on every call, process-wide, for
 * every LD_PRELOAD consumer. N is swept via argv so the O(N) shape shows up
 * directly in the ns/call trend rather than needing algebra on one number.
 *
 * epoll_wait_no_pipe isolates the global-mutex-plus-O(64)-scan cost
 * (g_ep_pipes_lock, ep_reg_any_locked) an epoll_wait/epoll_pwait call pays
 * even when the epfd has zero registered pipes.
 *
 * epoll_ctl_churn isolates the same lock+scan cost under repeated
 * ADD/MOD/DEL on an fd that *does* register (a real pipe), which is the
 * shape a churning connection pool would produce.
 */

static void bench_poll_patch_idle_fds(int n)
{
	char name[32];
	snprintf(name, sizeof(name), "poll_patch_idle_n%d", n);

	int *fds = calloc((size_t)n, sizeof(int));
	struct pollfd *pfds = calloc((size_t)n, sizeof(struct pollfd));
	if (!fds || !pfds) {
		printf("%-22s calloc failed\n", name);
		free(fds);
		free(pfds);
		return;
	}
	for (int i = 0; i < n; i++) {
		fds[i] = eventfd(0, EFD_NONBLOCK);
		pfds[i].fd = fds[i];
		pfds[i].events = POLLIN;
	}

	long iters = 5000;
	double t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		for (int j = 0; j < n; j++)
			pfds[j].revents = 0;
		poll(pfds, (nfds_t)n, 0);
	}
	report(name, iters, now_ms() - t0);

	for (int i = 0; i < n; i++)
		if (fds[i] >= 0)
			close(fds[i]);
	free(fds);
	free(pfds);
}

static void bench_poll_patch_pipe_fds(int n)
{
	char name[32];
	snprintf(name, sizeof(name), "poll_patch_pipe_n%d", n);

	int (*fds)[2] = calloc((size_t)n, sizeof(int[2]));
	struct pollfd *pfds = calloc((size_t)n, sizeof(struct pollfd));
	if (!fds || !pfds) {
		printf("%-22s calloc failed\n", name);
		free(fds);
		free(pfds);
		return;
	}
	for (int i = 0; i < n; i++) {
		if (pipe(fds[i]) != 0) {
			printf("%-22s pipe() failed\n", name);
			for (int k = 0; k < i; k++) {
				close(fds[k][0]);
				close(fds[k][1]);
			}
			free(fds);
			free(pfds);
			return;
		}
		fcntl(fds[i][0], F_SETFL, O_NONBLOCK);
		pfds[i].fd = fds[i][0];
		pfds[i].events = POLLIN;
	}

	long iters = 5000;
	double t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		for (int j = 0; j < n; j++)
			pfds[j].revents = 0;
		poll(pfds, (nfds_t)n, 0);
	}
	report(name, iters, now_ms() - t0);

	for (int i = 0; i < n; i++) {
		close(fds[i][0]);
		close(fds[i][1]);
	}
	free(fds);
	free(pfds);
}

static void bench_epoll_wait_no_pipe(void)
{
	int efd = eventfd(0, EFD_NONBLOCK);
	int epfd = epoll_create1(0);
	if (efd < 0 || epfd < 0) {
		printf("%-22s setup failed\n", "epoll_wait_no_pipe");
		return;
	}
	struct epoll_event ev = {0};
	ev.events = EPOLLIN;
	ev.data.fd = efd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);

	long iters = 20000;
	struct epoll_event out[4];
	double t0 = now_ms();
	for (long i = 0; i < iters; i++)
		epoll_wait(epfd, out, 4, 0);
	report("epoll_wait_no_pipe", iters, now_ms() - t0);

	close(epfd);
	close(efd);
}

static void bench_epoll_ctl_churn(void)
{
	int pfd[2];
	if (pipe(pfd) != 0) {
		printf("%-22s pipe() failed\n", "epoll_ctl_churn");
		return;
	}
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	int epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("%-22s epoll_create1 failed\n", "epoll_ctl_churn");
		close(pfd[0]);
		close(pfd[1]);
		return;
	}

	long iters = 5000;
	struct epoll_event ev = {0};
	ev.data.fd = pfd[0];
	double t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		ev.events = EPOLLIN;
		epoll_ctl(epfd, EPOLL_CTL_ADD, pfd[0], &ev);
		ev.events = EPOLLIN | EPOLLPRI;
		epoll_ctl(epfd, EPOLL_CTL_MOD, pfd[0], &ev);
		epoll_ctl(epfd, EPOLL_CTL_DEL, pfd[0], NULL);
	}
	report("epoll_ctl_churn", iters, now_ms() - t0);

	close(epfd);
	close(pfd[0]);
	close(pfd[1]);
}

/* NOTE on the epoll_pipe adaptive backoff (2a): deliberately NOT
 * benchmarked here by calling epoll_wait(-1) directly. A pipe registered
 * but never fed data means every slice is empty, so ep_shim_wait's
 * infinite-wait loop (by design -- an infinite wait must only ever return
 * with a real event or a signal, never a premature 0, see the 8daab67
 * SIGABRT history above ep_shim_wait) never returns at all: there is no
 * bounded way to observe "N calls completing" from outside. A FINITE
 * outer timeout does return, but its own deadline caps total elapsed time
 * regardless of how the interval grew internally, which masks growth
 * rather than revealing it -- any wall-clock measurement attempted here
 * either hangs or measures the wrong thing. Backoff growth is instead
 * verified functionally (test/functional.c's bounded-deadline regression
 * test) and by code inspection (the comment block above g_ep_backoff's
 * declaration in src/ohos_compat_shim.c).
 */

/* Replaces the README's historical prose throughput number ("100MB through
 * two-hop pipes: 101ms -> 129ms, 28% slower") with a reproducible bench
 * entry. splice() to a FIFO destination with off_out==NULL is the one
 * interceptor in this shim that's unconditionally replaced rather than
 * probe-then-fallback (see splice()'s own comment: it never even attempts
 * the real syscall on that path) -- so under LD_PRELOAD, every one of
 * these chunks goes through splice_through_buffer's userspace bounce
 * (read()+write() instead of zero-copy), which is exactly the cost this
 * measures. 16KB chunks (comfortably under any plausible default pipe
 * capacity, avoiding capacity-dependent partial-write/blocking edge cases
 * from a value nearer 64KB) x 20MB total, single-threaded splice-then-
 * drain each iteration so neither pipe's buffer can back up across
 * iterations. */
static void bench_splice_pipe_to_pipe(void)
{
	const char *name = "splice_pipe_to_pipe_20mb";
	int p1[2], p2[2];
	if (pipe(p1) != 0 || pipe(p2) != 0) {
		printf("%-26s pipe() failed\n", name);
		return;
	}

	const size_t chunk = 16384;
	const size_t total = 20 * 1024 * 1024;
	char *src = malloc(chunk);
	char *drain = malloc(chunk);
	if (!src || !drain) {
		printf("%-26s malloc failed\n", name);
		free(src);
		free(drain);
		close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
		return;
	}
	memset(src, 'x', chunk);

	double t0 = now_ms();
	size_t remaining = total;
	int broke = 0;
	while (remaining > 0 && !broke) {
		size_t want = remaining < chunk ? remaining : chunk;
		if (write(p1[1], src, want) != (ssize_t)want) {
			broke = 1;
			break;
		}
		size_t spliced = 0;
		while (spliced < want) {
			ssize_t m = splice(p1[0], NULL, p2[1], NULL, want - spliced, SPLICE_F_MOVE);
			if (m <= 0) {
				broke = 1;
				break;
			}
			spliced += (size_t)m;
		}
		size_t drained = 0;
		while (drained < spliced) {
			ssize_t r = read(p2[0], drain, chunk);
			if (r <= 0) {
				broke = 1;
				break;
			}
			drained += (size_t)r;
		}
		remaining -= want;
	}
	double elapsed = now_ms() - t0;
	if (broke) {
		printf("%-26s aborted early (errno=%d %s)\n", name, errno, strerror(errno));
	} else {
		double mbps = elapsed > 0 ? ((double)total / 1024.0 / 1024.0) / (elapsed / 1000.0) : 0;
		printf("%-26s %zu bytes in %.1fms (%.1f MB/s)\n", name, total, elapsed, mbps);
	}

	free(src);
	free(drain);
	close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
}

/* poll(fds, n, -1) with NO fifo fd present -- the case Phase 2b's fix
 * specifically targets paying nothing extra for: has_fifo is computed
 * once (a cache-only scan) and, finding nothing, skips the post-call
 * ioctl scan entirely rather than the old always-scan-after-return
 * behavior. fds[0] is pre-signaled so every iteration returns immediately
 * instead of actually blocking -- this measures shim call overhead, not
 * real wait time. */
static void bench_poll_infinite_no_fifo(int n)
{
	char name[40];
	snprintf(name, sizeof(name), "poll_infinite_no_fifo_n%d", n);

	int *fds = calloc((size_t)n, sizeof(int));
	struct pollfd *pfds = calloc((size_t)n, sizeof(struct pollfd));
	if (!fds || !pfds) {
		printf("%-26s calloc failed\n", name);
		free(fds);
		free(pfds);
		return;
	}
	for (int i = 0; i < n; i++) {
		fds[i] = eventfd(0, EFD_NONBLOCK);
		pfds[i].fd = fds[i];
		pfds[i].events = POLLIN;
	}
	uint64_t one = 1;
	if (write(fds[0], &one, sizeof(one)) < 0) {
		printf("%-26s eventfd write failed\n", name);
	}

	long iters = 5000;
	double t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		for (int j = 0; j < n; j++)
			pfds[j].revents = 0;
		poll(pfds, (nfds_t)n, -1); /* infinite, but fds[0] is always ready */
	}
	report(name, iters, now_ms() - t0);

	for (int i = 0; i < n; i++)
		if (fds[i] >= 0)
			close(fds[i]);
	free(fds);
	free(pfds);
}

int main(void)
{
	bench_syscall_passthrough();
	bench_close_range();
	bench_close_range_fallback_path();
	bench_getpwuid_r();
	bench_tmpfile();
	bench_getcwd();
	bench_poll_patch_idle_fds(1);
	bench_poll_patch_idle_fds(8);
	bench_poll_patch_idle_fds(32);
	bench_poll_patch_idle_fds(128);
	bench_poll_patch_pipe_fds(1);
	bench_poll_patch_pipe_fds(8);
	bench_poll_patch_pipe_fds(32);
	bench_poll_patch_pipe_fds(128);
	bench_epoll_wait_no_pipe();
	bench_epoll_ctl_churn();
	bench_poll_infinite_no_fifo(1);
	bench_poll_infinite_no_fifo(128);
	bench_splice_pipe_to_pipe();
	return 0;
}
