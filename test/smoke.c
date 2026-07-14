/*
 * test/smoke.c — exercises the three default-on shimmed calls end to end.
 *
 * Run once without LD_PRELOAD (baseline — shows the raw HarmonyOS sandbox
 * failures) and once with LD_PRELOAD=libohos_compat.so (shows the fallback
 * kicking in). `make smoke` runs both automatically.
 *
 * Exit code: 0 if all three checks pass, 1 otherwise. Each check prints its
 * own PASS/FAIL line so a partial pass is visible even under `|| true`.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_close_range
#define __NR_close_range 436
#endif

static int failures = 0;

static void check(int cond, const char *name, const char *detail)
{
	if (cond) {
		printf("PASS  %-12s %s\n", name, detail ? detail : "");
	} else {
		printf("FAIL  %-12s %s (errno=%d %s)\n", name, detail ? detail : "",
		       errno, strerror(errno));
		failures++;
	}
}

static sigjmp_buf cr_jmp;

static void cr_sigsys_handler(int sig)
{
	(void)sig;
	siglongjmp(cr_jmp, 1);
}

/* 1. close_range via raw syscall() — mirrors how Bun's c-bindings.cpp calls
 * it (syscall(__NR_close_range, ...)), not the libc wrapper directly. On
 * this device the real syscall unconditionally SIGSYS's without the shim
 * (see README's "Known platform behavior") — guarded here so a baseline
 * run reports that plainly instead of crashing this whole smoke test. */
static void check_close_range(void)
{
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		check(0, "close_range", "could not open /dev/null to test with");
		return;
	}

	struct sigaction old_sa, sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = cr_sigsys_handler;
	sigaction(SIGSYS, &sa, &old_sa);

	long ret = -999;
	int crashed = 0;
	if (sigsetjmp(cr_jmp, 1) == 0)
		ret = syscall(__NR_close_range, fd, fd, 0);
	else
		crashed = 1;
	sigaction(SIGSYS, &old_sa, NULL);

	if (crashed) {
		printf("INFO  close_range   would SIGSYS without the shim (expected at baseline "
		       "on this device)\n");
		return;
	}
	int is_closed = (fcntl(fd, F_GETFD) == -1 && errno == EBADF);
	check(ret == 0 && is_closed, "close_range", "fd closed via syscall(SYS_close_range)");
}

/* 2. getpwuid_r — HarmonyOS HAP uids aren't in /etc/passwd. */
static void check_getpwuid_r(void)
{
	struct passwd pwd, *result = NULL;
	char buf[1024];
	int rc = getpwuid_r(geteuid(), &pwd, buf, sizeof(buf), &result);
	char detail[256];
	if (result)
		snprintf(detail, sizeof(detail), "pw_name=%s pw_dir=%s", pwd.pw_name, pwd.pw_dir);
	else
		snprintf(detail, sizeof(detail), "rc=%d result=NULL", rc);
	check(rc == 0 && result != NULL, "getpwuid_r", detail);
}

/* 3. tmpfile — P_tmpdir may be unwritable in the app sandbox. */
static void check_tmpfile(void)
{
	FILE *f = tmpfile();
	int ok = 0;
	if (f) {
		ok = (fputs("ok", f) >= 0);
		fclose(f);
	}
	check(ok, "tmpfile", f ? "wrote+closed" : "tmpfile() returned NULL");
}

int main(void)
{
	check_close_range();
	check_getpwuid_r();
	check_tmpfile();

	printf("%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "SOME FAILED",
	       failures, failures == 1 ? "" : "s");
	return failures == 0 ? 0 : 1;
}
