/*
 * real_vs_fallback.c — directly compares the REAL libc implementation vs
 * this shim's own FALLBACK (substitute) implementation for getpwuid_r,
 * tmpfile, and getcwd — both performance AND functional output — by
 * re-implementing the exact fallback logic from ohos_compat_shim.c
 * standalone, so it runs unconditionally regardless of whether the real
 * call would naturally fail on this platform (it doesn't, in the
 * OpenHarmony container, which is exactly why this needs to be forced).
 *
 * This does NOT link against or exercise ohos_compat_shim.c's actual
 * dispatch logic — the three fallback_*() functions below are hand-copied
 * from it. If the shim's fallback logic changes, update these to match or
 * the comparison drifts out of sync with what actually ships.
 *
 * Usage:
 *   ./real_vs_fallback          -> performance: real vs fallback, ns/call
 *   ./real_vs_fallback --dump   -> functional: full field/value dump of both
 *
 * close_range isn't covered here — it already has a real-vs-fallback
 * comparison in test/bench.c (bench_close_range vs
 * bench_close_range_fallback_path), since its fallback needs the same
 * SIGSYS-guard machinery the shim itself uses, which doesn't fit this
 * file's simpler direct-call model.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void report(const char *name, long iters, double ms)
{
	printf("%-28s %8ld iters  %10.3f ms total  %10.1f ns/call\n", name, iters,
	       ms, (ms * 1e6) / (double)iters);
}

/* ==================================================================== */
/* getpwuid_r: real vs fallback (verbatim from shim)                    */
/* ==================================================================== */

#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX 256
#endif

typedef int (*ohos_get_name_fn)(char *, size_t);

static ohos_get_name_fn ohos_get_name_resolve(void)
{
	static void *handle = NULL;
	static ohos_get_name_fn fn = NULL;
	if (!handle) {
		handle = dlopen("libos_account_ndk.so", RTLD_NOW | RTLD_LOCAL);
		if (handle)
			fn = (ohos_get_name_fn)dlsym(handle, "OH_OsAccount_GetName");
	}
	return fn;
}

static void fallback_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
				size_t buflen, struct passwd **result)
{
	const char *username = NULL;
	char account_name[LOGIN_NAME_MAX];
	if (uid == getuid()) {
		ohos_get_name_fn get_name = ohos_get_name_resolve();
		if (get_name && get_name(account_name, sizeof(account_name)) == 0)
			username = account_name;
	}
	if (!username) {
		username = getenv("LOGNAME");
		if (username && !*username)
			username = NULL;
	}
	if (!username) {
		username = getenv("USER");
		if (username && !*username)
			username = NULL;
	}
	const char *homedir = getenv("HOME");
	if (homedir && !*homedir)
		homedir = NULL;
	if (!homedir)
		homedir = "/data/storage/el2/base";
	const char *shell = getenv("SHELL");
	if (shell && !*shell)
		shell = NULL;
	if (!shell)
		shell = "/bin/false";

	char uid_buf[32];
	if (!username) {
		snprintf(uid_buf, sizeof(uid_buf), "u%u", (unsigned)uid);
		username = uid_buf;
	}

	size_t need = strlen(username) + 1 + strlen(homedir) + 1 + strlen(shell) + 1;
	if (buflen < need) {
		*result = NULL;
		return;
	}

	char *p = buf;
	pwd->pw_name = p;
	p = stpcpy(p, username) + 1;
	pwd->pw_dir = p;
	p = stpcpy(p, homedir) + 1;
	pwd->pw_shell = p;
	stpcpy(p, shell);

	pwd->pw_passwd = (char *)"";
	pwd->pw_uid = uid;
	pwd->pw_gid = getegid();
	pwd->pw_gecos = pwd->pw_name;

	*result = pwd;
}

static void bench_getpwuid_r(void)
{
	long iters = 5000;
	struct passwd pwd, *result;
	char buf[1024];
	uid_t uid = geteuid();

	double t0 = now_ms();
	for (long i = 0; i < iters; i++)
		getpwuid_r(uid, &pwd, buf, sizeof(buf), &result);
	report("getpwuid_r REAL", iters, now_ms() - t0);

	t0 = now_ms();
	for (long i = 0; i < iters; i++)
		fallback_getpwuid_r(uid, &pwd, buf, sizeof(buf), &result);
	report("getpwuid_r FALLBACK", iters, now_ms() - t0);
}

static void dump_getpwuid_r(void)
{
	struct passwd pwd, *result;
	char buf[1024];
	uid_t uid = geteuid();

	printf("\n--- getpwuid_r REAL ---\n");
	getpwuid_r(uid, &pwd, buf, sizeof(buf), &result);
	if (result) {
		printf("pw_name=%s pw_passwd=%s pw_uid=%u pw_gid=%u pw_gecos=%s "
		       "pw_dir=%s pw_shell=%s\n",
		       pwd.pw_name, pwd.pw_passwd, pwd.pw_uid, pwd.pw_gid, pwd.pw_gecos,
		       pwd.pw_dir, pwd.pw_shell);
	} else {
		printf("result=NULL\n");
	}

	printf("--- getpwuid_r FALLBACK ---\n");
	fallback_getpwuid_r(uid, &pwd, buf, sizeof(buf), &result);
	if (result) {
		printf("pw_name=%s pw_passwd=%s pw_uid=%u pw_gid=%u pw_gecos=%s "
		       "pw_dir=%s pw_shell=%s\n",
		       pwd.pw_name, pwd.pw_passwd, pwd.pw_uid, pwd.pw_gid, pwd.pw_gecos,
		       pwd.pw_dir, pwd.pw_shell);
	} else {
		printf("result=NULL\n");
	}
}

/* ==================================================================== */
/* tmpfile: real vs fallback (mkstemp+HOME, verbatim from shim)          */
/* ==================================================================== */

static FILE *fallback_tmpfile(void)
{
	const char *dir = getenv("TMPDIR");
	if (!dir)
		dir = getenv("HOME");
	if (!dir)
		dir = "/data/storage/el2/base";

	char tmpl[PATH_MAX];
	int n = snprintf(tmpl, sizeof(tmpl), "%s/ohos-compat-tmp-XXXXXX", dir);
	if (n < 0 || (size_t)n >= sizeof(tmpl)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	int fd = mkstemp(tmpl);
	if (fd < 0)
		return NULL;
	unlink(tmpl);
	FILE *f = fdopen(fd, "w+");
	if (!f) {
		int e = errno;
		close(fd);
		errno = e;
		return NULL;
	}
	return f;
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
	report("tmpfile REAL", iters, now_ms() - t0);

	t0 = now_ms();
	for (long i = 0; i < iters; i++) {
		FILE *f = fallback_tmpfile();
		if (f)
			fclose(f);
	}
	report("tmpfile FALLBACK", iters, now_ms() - t0);
}

static void dump_tmpfile(void)
{
	printf("\n--- tmpfile REAL ---\n");
	FILE *f = tmpfile();
	if (f) {
		fputs("hello", f);
		rewind(f);
		char buf[16] = {0};
		fread(buf, 1, sizeof(buf) - 1, f);
		printf("write/read roundtrip: '%s'\n", buf);
		fclose(f);
	} else {
		printf("tmpfile() returned NULL, errno=%d (%s)\n", errno, strerror(errno));
	}

	printf("--- tmpfile FALLBACK ---\n");
	f = fallback_tmpfile();
	if (f) {
		fputs("hello", f);
		rewind(f);
		char buf[16] = {0};
		fread(buf, 1, sizeof(buf) - 1, f);
		printf("write/read roundtrip: '%s'\n", buf);
		fclose(f);
	} else {
		printf("fallback_tmpfile() returned NULL, errno=%d (%s)\n", errno,
		       strerror(errno));
	}
}

/* ==================================================================== */
/* getcwd: real vs fallback (ENOENT->HOME, verbatim from shim)           */
/* ==================================================================== */

static char *fallback_getcwd(char *buf, size_t size)
{
	const char *home = getenv("HOME");
	if (!home)
		home = "/data/storage/el2/base";
	size_t need = strlen(home) + 1;

	if (buf) {
		if (size < need) {
			errno = ERANGE;
			return NULL;
		}
		memcpy(buf, home, need);
		return buf;
	}
	size_t alloc_size = size ? size : need;
	if (alloc_size < need) {
		errno = ERANGE;
		return NULL;
	}
	char *out = malloc(alloc_size);
	if (!out)
		return NULL;
	memcpy(out, home, need);
	return out;
}

static void bench_getcwd(void)
{
	long iters = 5000;
	char buf[4096];

	double t0 = now_ms();
	for (long i = 0; i < iters; i++)
		getcwd(buf, sizeof(buf));
	report("getcwd REAL", iters, now_ms() - t0);

	t0 = now_ms();
	for (long i = 0; i < iters; i++)
		fallback_getcwd(buf, sizeof(buf));
	report("getcwd FALLBACK", iters, now_ms() - t0);
}

static void dump_getcwd(void)
{
	char buf[4096];
	printf("\n--- getcwd REAL ---\n");
	char *r = getcwd(buf, sizeof(buf));
	printf("result=%s\n", r ? r : "(NULL)");

	printf("--- getcwd FALLBACK ---\n");
	r = fallback_getcwd(buf, sizeof(buf));
	printf("result=%s\n", r ? r : "(NULL)");
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
		dump_getpwuid_r();
		dump_tmpfile();
		dump_getcwd();
		return 0;
	}
	bench_getpwuid_r();
	bench_tmpfile();
	bench_getcwd();
	return 0;
}
