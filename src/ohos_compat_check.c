/*
 * ohos_compat_check.c — `ohos-shim check`: probes whether the symptoms
 * ohos_compat_shim.c works around are still reproducible on THIS device.
 *
 * ohos_compat_shim.c was written against real HarmonyOS 6.1 hardware. The
 * platform is expected to relax some of these sandbox restrictions over
 * subsequent OHOS API levels, but nothing publishes *which* ones on *which*
 * build — the only reliable way to find out is to reproduce the original
 * symptom directly, on-device. That is what this program does: replay each
 * interceptor's exact trigger condition against the real libc/kernel (no
 * shim preloaded) and report NEEDED / DROPPABLE / INCONCLUSIVE per symbol,
 * ending with a ready-to-export `OHOS_COMPAT_SHIM_DISABLE=...` line.
 *
 * DELIBERATELY NOT sharing code with ohos_compat_shim.c: that file must
 * stay byte-identical with ohos-bun's embedded copy (see CLAUDE.md), so a
 * diagnostic feature has no business forcing a shim/bun rebuild. Trigger
 * conditions are re-derived here from the shim's own doc comments and
 * test/functional.c, the same "hand-copied, not shared" approach
 * test/real_vs_fallback.c already uses for its fallback-logic comparison.
 *
 * Two independent probe groups:
 *
 *   A — the shim's own 9 interceptors. Each gets a NEEDED/DROPPABLE/
 *       INCONCLUSIVE verdict and feeds the DISABLE recommendation line.
 *   B — surrounding platform capabilities this workspace has documented
 *       workarounds for (raw syscalls Bun's rustix backend can't be
 *       LD_PRELOAD-shimmed for, ptrace, prctl(PR_SET_PTRACER), the musl
 *       dlopen/.dynsym limitation, ...). Informational only — group B
 *       does not appear in the DISABLE recommendation, because none of
 *       it is something this shim (or any LD_PRELOAD shim) can fix.
 *
 * Anything that can raise an *uncaught* SIGSYS on real hardware (close_range,
 * fchmodat2, ptrace, prctl(PR_SET_PTRACER), the raw B-group syscalls) is run
 * in a forked child, never in this process — see run_guarded() below. That
 * is a harder guarantee than the shim's own sigsetjmp/siglongjmp approach:
 * a crash just shows up as the child dying, cleanly separated from "the
 * call returned an errno".
 *
 * Baseline hygiene: this program forcibly unsetenv("LD_PRELOAD") at start
 * (unless it is its own --with-shim re-exec pass) rather than trusting the
 * caller to have run `env -u LD_PRELOAD` — the shim's own Makefile/README
 * both record a real incident where an ambient LD_PRELOAD silently
 * invalidated a "baseline" run.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __OHOS__
#include <info/device_api_version.h>
#include <info/application_target_sdk_version.h>
#endif

#if !defined(__aarch64__)
#error "ohos_compat_check.c: only supports aarch64 (HarmonyOS/OHOS target)"
#endif

/* ------------------------------------------------------------------ */
/*  Raw syscall numbers missing from older/narrower musl headers        */
/*  (same rationale and same numbers as ohos_compat_shim.c)             */
/* ------------------------------------------------------------------ */
#ifndef __NR_close_range
#define __NR_close_range 436
#endif
#ifndef __NR_fchmodat2
#define __NR_fchmodat2 452
#endif
#ifndef CLOSE_RANGE_UNSHARE
#define CLOSE_RANGE_UNSHARE (1U << 1)
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_statx
#define SYS_statx 291
#endif
#ifndef SYS_renameat2
#define SYS_renameat2 276
#endif
#ifndef SYS_copy_file_range
#define SYS_copy_file_range 285
#endif
#ifndef SYS_memfd_create
#define SYS_memfd_create 279
#endif
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61 /* "Yama" -- not always in sys/prctl.h */
#endif

/* Exported (via -rdynamic) for checkdep.so's dlopen probe -- see that
 * file's header comment for the full story. Never called directly here. */
int ohos_check_marker(void) { return 42; }

/* ==================================================================== */
/*  Report model                                                        */
/* ==================================================================== */

typedef enum { V_NEEDED, V_DROPPABLE, V_INCONCLUSIVE, V_INFO } verdict_t;

typedef struct {
	char id[32];
	char group[2];   /* "A" or "B" */
	char desc[160];
	verdict_t verdict;
	char note[512];
	int has_disable_flag; /* A-group only: eligible for the DISABLE line */
} report_row_t;

#define MAX_ROWS 32
static report_row_t g_rows[MAX_ROWS];
static int g_nrows = 0;

static report_row_t *add_row(const char *id, const char *group, const char *desc)
{
	report_row_t *r = &g_rows[g_nrows++];
	memset(r, 0, sizeof(*r));
	snprintf(r->id, sizeof(r->id), "%s", id);
	snprintf(r->group, sizeof(r->group), "%s", group);
	snprintf(r->desc, sizeof(r->desc), "%s", desc);
	return r;
}

static const char *verdict_str(verdict_t v)
{
	switch (v) {
	case V_NEEDED: return "仍需要";
	case V_DROPPABLE: return "可关闭";
	case V_INCONCLUSIVE: return "不确定";
	case V_INFO: default: return "信息";
	}
}

/* ==================================================================== */
/*  fork-guarded probe execution                                        */
/*                                                                        */
/*  Every probe that might raise an uncaught SIGSYS on real hardware runs */
/*  in its own forked child; the parent only ever observes the outcome    */
/*  through waitpid()+pipe, so a crash here can never take this program   */
/*  down with it (unlike the shim's own sigsetjmp approach, which has to  */
/*  run in-process because it's intercepting calls made BY the host       */
/*  process -- this program has no such constraint, so the strictly       */
/*  safer fork is used everywhere, not just for the historically riskiest */
/*  two symbols).                                                         */
/* ==================================================================== */

typedef struct {
	int crashed;   /* child died from an uncaught signal */
	int term_sig;  /* valid iff crashed */
	int wrote;     /* child ran to completion and reported back */
	int rc;
	int err;
	int aux;       /* probe-specific extra field (see individual probes) */
} child_result_t;

typedef void (*child_fn)(void *arg, child_result_t *out);

static void run_guarded(child_fn fn, void *arg, child_result_t *out)
{
	memset(out, 0, sizeof(*out));

	int pfd[2];
	if (pipe(pfd) != 0) {
		out->crashed = 1;
		return;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		out->crashed = 1;
		return;
	}

	if (pid == 0) {
		close(pfd[0]);
		child_result_t res;
		memset(&res, 0, sizeof(res));
		fn(arg, &res);
		res.wrote = 1;
		const char *p = (const char *)&res;
		size_t off = 0;
		while (off < sizeof(res)) {
			ssize_t n = write(pfd[1], p + off, sizeof(res) - off);
			if (n <= 0)
				break;
			off += (size_t)n;
		}
		close(pfd[1]);
		_exit(0);
	}

	close(pfd[1]);
	char *buf = (char *)out;
	size_t total = 0;
	while (total < sizeof(*out)) {
		ssize_t n = read(pfd[0], buf + total, sizeof(*out) - total);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	close(pfd[0]);

	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)) {
		out->crashed = 1;
		out->term_sig = WTERMSIG(status);
	} else if (total < sizeof(*out) || !out->wrote) {
		out->crashed = 1;
	}
}

/* ==================================================================== */
/*  Candidate path set — the four path-sensitive interceptors           */
/*  (getcwd/tmpfile/linkat/symlinkat) depend on WHICH filesystem a path  */
/*  lives on, not just the OS version, so every probe below runs once    */
/*  per candidate directory rather than once globally.                   */
/* ==================================================================== */

#define NUM_CANDIDATE_DIRS 5

static const char *candidate_dir(int idx, char *scratch, size_t scratchsz)
{
	switch (idx) {
	case 0: {
		const char *v = getenv("TMPDIR");
		return (v && *v) ? v : NULL;
	}
	case 1: {
		const char *v = getenv("HOME");
		return (v && *v) ? v : NULL;
	}
	case 2:
		return getcwd(scratch, scratchsz);
	case 3:
		return "/data/storage/el2/base";
	case 4:
		return "/storage/Users/currentUser"; /* hmdfs, may not exist */
	default:
		return NULL;
	}
}

/* ==================================================================== */
/*  A-group: the shim's 9 interceptors                                  */
/* ==================================================================== */

/* -- close_range --------------------------------------------------- */

static void child_close_range_basic(void *arg, child_result_t *out)
{
	(void)arg;
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		out->rc = -1;
		out->err = errno;
		return;
	}
	errno = 0;
	long ret = syscall(__NR_close_range, (long)fd, (long)fd, 0L);
	out->rc = (int)ret;
	out->err = errno;
	if (ret == 0 && (fcntl(fd, F_GETFD) != -1 || errno != EBADF)) {
		/* claimed success but the fd is still open -- treat as broken */
		out->rc = -100;
	}
}

static void child_close_range_einval(void *arg, child_result_t *out)
{
	(void)arg;
	errno = 0;
	/* first > last: upstream Linux's fs/file.c rejects this with EINVAL.
	 * The shim validates this itself up front because an earlier probe
	 * found this device's kernel does NOT enforce it -- see whether that
	 * is still true here. */
	long ret = syscall(__NR_close_range, 4L, 3L, 0L);
	out->rc = (int)ret;
	out->err = errno;
}

static void child_unshare_close_files(void *arg, child_result_t *out)
{
	(void)arg;
	errno = 0;
	out->rc = unshare(CLONE_FILES);
	out->err = errno;
}

static void child_close_range_unshare_flag(void *arg, child_result_t *out)
{
	(void)arg;
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		out->rc = -1;
		out->err = errno;
		return;
	}
	errno = 0;
	long ret = syscall(__NR_close_range, (long)fd, (long)fd,
			   (long)CLOSE_RANGE_UNSHARE);
	out->rc = (int)ret;
	out->err = errno;
	close(fd);
}

static void probe_close_range(void)
{
	report_row_t *r = add_row("close_range", "A",
		"close_range()/syscall(SYS_close_range) 批量关 fd");
	r->has_disable_flag = 1;
	child_result_t res;
	char note[512];
	int needed = 0;
	size_t off = 0;

	run_guarded(child_close_range_basic, NULL, &res);
	if (res.crashed) {
		needed = 1;
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"基本调用被信号杀死(sig=%d)；", res.term_sig);
	} else if (res.rc == 0) {
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"基本调用成功且真的关闭了 fd；");
	} else {
		needed = 1;
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"基本调用失败 rc=%d errno=%d(%s)；", res.rc, res.err,
			strerror(res.err));
	}

	run_guarded(child_close_range_einval, NULL, &res);
	if (res.crashed) {
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"first>last 参数校验：内核对非法参数也 SIGSYS（更糟，"
			"shim 的前置校验仍必要）；");
	} else if (res.rc != 0 && res.err == EINVAL) {
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"first>last 参数校验：内核已按上游语义返回 EINVAL；");
	} else {
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"first>last 参数校验：内核未按上游语义处理(rc=%d errno=%d)"
			"——关闭 shim 会丢失这层保护；", res.rc, res.err);
	}

	int unshare_needed = 0;
	run_guarded(child_unshare_close_files, NULL, &res);
	if (res.crashed) {
		unshare_needed = 1;
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"unshare(CLONE_FILES) 被信号杀死(sig=%d)；", res.term_sig);
	} else if (res.rc == 0 || res.err == EINVAL) {
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"unshare(CLONE_FILES) 可用；");
	} else {
		unshare_needed = 1;
		off += (size_t)snprintf(note + off, sizeof(note) - off,
			"unshare(CLONE_FILES) 失败 errno=%d(%s)；", res.err,
			strerror(res.err));
	}

	run_guarded(child_close_range_unshare_flag, NULL, &res);
	if (res.crashed) {
		unshare_needed = 1;
		snprintf(note + off, sizeof(note) - off,
			"CLOSE_RANGE_UNSHARE 标志被信号杀死(sig=%d)", res.term_sig);
	} else if (res.rc == 0) {
		snprintf(note + off, sizeof(note) - off,
			"CLOSE_RANGE_UNSHARE 标志可用");
	} else {
		unshare_needed = 1;
		snprintf(note + off, sizeof(note) - off,
			"CLOSE_RANGE_UNSHARE 标志失败 errno=%d(%s)", res.err,
			strerror(res.err));
	}

	needed = needed || unshare_needed;
	r->verdict = needed ? V_NEEDED : V_DROPPABLE;
	snprintf(r->note, sizeof(r->note), "%s", note);
}

/* -- fchmodat2 -------------------------------------------------------- */

static void child_fchmodat2(void *arg, child_result_t *out)
{
	(void)arg;
	char path[PATH_MAX];
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/data/storage/el2/base";
	snprintf(path, sizeof(path), "%s/ohos-check-fchmodat2-%d", dir, (int)getpid());
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		out->rc = -1;
		out->err = errno;
		return;
	}
	close(fd);
	errno = 0;
	long ret = syscall(__NR_fchmodat2, AT_FDCWD, path, 0600, 0);
	out->rc = (int)ret;
	out->err = errno;
	unlink(path);
}

static void probe_fchmodat2(void)
{
	report_row_t *r = add_row("fchmodat2", "A",
		"syscall(SYS_fchmodat2) chmod 的 AT_* flags 变体");
	r->has_disable_flag = 1;
	child_result_t res;
	run_guarded(child_fchmodat2, NULL, &res);
	if (res.crashed) {
		r->verdict = V_NEEDED;
		snprintf(r->note, sizeof(r->note), "被信号杀死(sig=%d)，真机典型症状",
			res.term_sig);
	} else if (res.rc == 0) {
		r->verdict = V_DROPPABLE;
		snprintf(r->note, sizeof(r->note), "真实系统调用成功");
	} else {
		r->verdict = V_NEEDED;
		snprintf(r->note, sizeof(r->note),
			"返回错误 errno=%d(%s)（容器典型是干净的 ENOSYS）", res.err,
			strerror(res.err));
	}
}

/* -- getpwuid_r ------------------------------------------------------- */

static void probe_getpwuid_r(void)
{
	report_row_t *r = add_row("getpwuid_r", "A",
		"os.userInfo() 依赖的 uid→用户名解析");
	r->has_disable_flag = 1;
	struct passwd pwd, *result = NULL;
	char buf[1024];
	/* POSIX getpwuid_r returns the error number directly (rc), unlike
	 * most libc calls -- it does NOT go through errno. */
	int rc = getpwuid_r(geteuid(), &pwd, buf, sizeof(buf), &result);
	if (rc == 0 && result != NULL) {
		r->verdict = V_DROPPABLE;
		snprintf(r->note, sizeof(r->note), "真实调用成功，pw_name=%s", pwd.pw_name);
	} else {
		r->verdict = V_NEEDED;
		if (rc == 0)
			snprintf(r->note, sizeof(r->note),
				"真实调用返回成功(rc=0)但 *result=NULL——HAP uid 仍未命中 "
				"/etc/passwd 记录（shim README 记录的确切症状）");
		else
			snprintf(r->note, sizeof(r->note),
				"真实调用失败 rc=%d(%s)，HAP uid 仍不在 /etc/passwd", rc,
				strerror(rc));
	}
}

/* -- tmpfile ------------------------------------------------------------ */

static void probe_tmpfile(void)
{
	report_row_t *r = add_row("tmpfile", "A", "P_tmpdir 可写性 (tmpfile())");
	r->has_disable_flag = 1;
	errno = 0;
	FILE *f = tmpfile();
	if (f) {
		fclose(f);
		r->verdict = V_DROPPABLE;
		snprintf(r->note, sizeof(r->note), "真实 tmpfile() 成功");
	} else {
		r->verdict = V_NEEDED;
		snprintf(r->note, sizeof(r->note), "真实 tmpfile() 失败 errno=%d(%s)",
			errno, strerror(errno));
	}
}

/* -- getcwd -------------------------------------------------------------- */

static void probe_getcwd(void)
{
	report_row_t *r = add_row("getcwd", "A",
		"cwd 不可解析时的兜底（hmdfs 祖先目录缺 +x / cwd 被删除）");
	r->has_disable_flag = 1;

	char orig[PATH_MAX];
	if (!getcwd(orig, sizeof(orig)))
		snprintf(orig, sizeof(orig), "/");

	int any_needed = 0;
	char detail[400] = "";
	size_t off = 0;

	/* (a) plain getcwd from each candidate dir -- catches hmdfs ancestor
	 * EACCES the same way the shim's guard condition does. */
	for (int i = 0; i < NUM_CANDIDATE_DIRS; i++) {
		char scratch[PATH_MAX];
		const char *dir = candidate_dir(i, scratch, sizeof(scratch));
		if (!dir)
			continue;
		struct stat st;
		if (stat(dir, &st) != 0)
			continue;
		if (chdir(dir) != 0)
			continue;
		char cwdbuf[PATH_MAX];
		errno = 0;
		char *ok = getcwd(cwdbuf, sizeof(cwdbuf));
		if (!ok && (errno == EACCES || errno == ENOENT)) {
			any_needed = 1;
			off += (size_t)snprintf(detail + off, sizeof(detail) - off,
				"[%s: errno=%d] ", dir, errno);
		}
		chdir(orig);
	}

	/* (b) cwd rmdir'd out from under the process -- reproducible on any
	 * writable candidate, not filesystem-specific. */
	for (int i = 0; i < NUM_CANDIDATE_DIRS; i++) {
		char scratch[PATH_MAX];
		const char *dir = candidate_dir(i, scratch, sizeof(scratch));
		if (!dir || access(dir, W_OK) != 0)
			continue;
		char tmpl[PATH_MAX];
		snprintf(tmpl, sizeof(tmpl), "%s/ohos-check-cwd-XXXXXX", dir);
		char *made = mkdtemp(tmpl);
		if (!made)
			continue;
		if (chdir(made) != 0) {
			rmdir(made);
			continue;
		}
		rmdir(made);
		char cwdbuf[PATH_MAX];
		errno = 0;
		char *ok = getcwd(cwdbuf, sizeof(cwdbuf));
		chdir(orig);
		if (!ok && errno == ENOENT) {
			any_needed = 1;
			snprintf(detail + off, sizeof(detail) - off,
				"[rmdir'd-cwd@%s] ", dir);
		}
		break; /* this scenario isn't filesystem-dependent, one is enough */
	}

	r->verdict = any_needed ? V_NEEDED : V_DROPPABLE;
	snprintf(r->note, sizeof(r->note),
		"%s（注：cwd-被删的兜底是主动防御，不是平台缺陷——即使平台正常也"
		"建议保留开启，别跟着 DROPPABLE 一起关）",
		any_needed ? detail : "所有探测路径下真实 getcwd() 均成功");
}

/* -- linkat / symlinkat --------------------------------------------------- */

static void probe_linkat(void)
{
	report_row_t *r = add_row("linkat", "A", "hmdfs/沙箱安装目标目录的硬链接");
	r->has_disable_flag = 1;
	int any_needed = 0, tested = 0;
	char detail[400] = "";
	size_t off = 0;

	for (int i = 0; i < NUM_CANDIDATE_DIRS; i++) {
		char scratch[PATH_MAX];
		const char *dir = candidate_dir(i, scratch, sizeof(scratch));
		if (!dir || access(dir, W_OK) != 0)
			continue;
		char src[PATH_MAX], dst[PATH_MAX];
		snprintf(src, sizeof(src), "%s/ohos-check-link-src-%d", dir, (int)getpid());
		snprintf(dst, sizeof(dst), "%s/ohos-check-link-dst-%d", dir, (int)getpid());
		int fd = open(src, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (fd < 0)
			continue;
		close(fd);
		unlink(dst);
		errno = 0;
		int rc = linkat(AT_FDCWD, src, AT_FDCWD, dst, 0);
		tested = 1;
		if (rc != 0 && (errno == EPERM || errno == EACCES)) {
			any_needed = 1;
			off += (size_t)snprintf(detail + off, sizeof(detail) - off,
				"[%s: errno=%d] ", dir, errno);
		}
		unlink(dst);
		unlink(src);
	}

	if (!tested) {
		r->verdict = V_INCONCLUSIVE;
		snprintf(r->note, sizeof(r->note), "没有可写的候选目录，未能测试");
		return;
	}
	r->verdict = any_needed ? V_NEEDED : V_DROPPABLE;
	snprintf(r->note, sizeof(r->note),
		"%s（有损语义 fallback：一旦可关闭就应尽快关闭，别图省事留着）",
		any_needed ? detail : "所有可写候选目录下真实 linkat() 均成功");
}

static void probe_symlinkat(void)
{
	report_row_t *r = add_row("symlinkat", "A", "hmdfs/沙箱安装目标目录的符号链接");
	r->has_disable_flag = 1;
	int any_needed = 0, tested = 0;
	char detail[400] = "";
	size_t off = 0;

	for (int i = 0; i < NUM_CANDIDATE_DIRS; i++) {
		char scratch[PATH_MAX];
		const char *dir = candidate_dir(i, scratch, sizeof(scratch));
		if (!dir || access(dir, W_OK) != 0)
			continue;
		char dst[PATH_MAX];
		snprintf(dst, sizeof(dst), "%s/ohos-check-symlink-dst-%d", dir, (int)getpid());
		unlink(dst);
		errno = 0;
		int rc = symlinkat("target-does-not-need-to-exist", AT_FDCWD, dst);
		tested = 1;
		if (rc != 0 && (errno == EPERM || errno == EACCES)) {
			any_needed = 1;
			off += (size_t)snprintf(detail + off, sizeof(detail) - off,
				"[%s: errno=%d] ", dir, errno);
		}
		unlink(dst);
	}

	if (!tested) {
		r->verdict = V_INCONCLUSIVE;
		snprintf(r->note, sizeof(r->note), "没有可写的候选目录，未能测试");
		return;
	}
	r->verdict = any_needed ? V_NEEDED : V_DROPPABLE;
	snprintf(r->note, sizeof(r->note),
		"%s（有损语义 fallback：一旦可关闭就应尽快关闭，别图省事留着）",
		any_needed ? detail : "所有可写候选目录下真实 symlinkat() 均成功");
}

/* -- splice / epoll_pipe: multi-round + memory-pressure stress ---------- */
/*
 * Both defects are load-dependent (the shim's own comments record the T50
 * epoll bug's repro rate tracking memory pressure). A single clean pass
 * proves nothing, so these two run --rounds times under a background
 * memcpy/sched_yield stress pool, and a full clean pass is reported
 * INCONCLUSIVE, never DROPPABLE — "didn't reproduce" is not "fixed" for an
 * intermittent bug.
 */

static int g_rounds = 20;

#define STRESS_BUF_BYTES (4 * 1024 * 1024)
#define STRESS_MAX_THREADS 64

static volatile int g_stress_stop = 0;
static pthread_t g_stress_threads[STRESS_MAX_THREADS];
static int g_nstress = 0;

static void *stress_thread_fn(void *arg)
{
	(void)arg;
	char *a = malloc(STRESS_BUF_BYTES);
	char *b = malloc(STRESS_BUF_BYTES);
	if (a && b) {
		memset(a, 0xAA, STRESS_BUF_BYTES);
		while (!g_stress_stop) {
			memcpy(b, a, STRESS_BUF_BYTES);
			sched_yield();
		}
	}
	free(a);
	free(b);
	return NULL;
}

static void start_stress(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n < 1)
		n = 2;
	if (n > STRESS_MAX_THREADS)
		n = STRESS_MAX_THREADS;
	g_stress_stop = 0;
	g_nstress = 0;
	for (long i = 0; i < n; i++) {
		if (pthread_create(&g_stress_threads[g_nstress], NULL, stress_thread_fn, NULL) == 0)
			g_nstress++;
	}
}

static void stop_stress(void)
{
	g_stress_stop = 1;
	for (int i = 0; i < g_nstress; i++)
		pthread_join(g_stress_threads[i], NULL);
	g_nstress = 0;
}

/* splice EOF-on-source reported as EPIPE instead of 0 — deterministic, not
 * load-dependent, so this runs once (not under --rounds). */
static int splice_eof_is_broken(void)
{
	int p1[2], p2[2];
	if (pipe(p1) != 0 || pipe(p2) != 0)
		return -1;
	close(p1[1]); /* immediate EOF on the read end */
	errno = 0;
	ssize_t n = splice(p1[0], NULL, p2[1], NULL, 65536, 0);
	int broken = !(n == 0);
	close(p1[0]);
	close(p2[0]);
	close(p2[1]);
	return broken;
}

typedef struct {
	int fd;
	int woke_ms; /* -1 = timed out, -2 = poll() itself errored */
} poll_wait_arg_t;

static void *poll_waiter_thread(void *arg)
{
	poll_wait_arg_t *a = (poll_wait_arg_t *)arg;
	struct pollfd pfd = { .fd = a->fd, .events = POLLIN };
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int rc = poll(&pfd, 1, 2000);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (rc > 0)
		a->woke_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
				   (t1.tv_nsec - t0.tv_nsec) / 1000000);
	else
		a->woke_ms = (rc == 0) ? -1 : -2;
	return NULL;
}

/* Returns 1 if the "splice into a pipe doesn't wake poll()" defect
 * reproduced this round, 0 if the waiter woke promptly, -1 on setup error. */
static int splice_wake_defect_once(void)
{
	int src[2], dst[2];
	if (pipe(src) != 0)
		return -1;
	if (pipe(dst) != 0) {
		close(src[0]);
		close(src[1]);
		return -1;
	}

	poll_wait_arg_t arg = { .fd = dst[0], .woke_ms = -2 };
	pthread_t th;
	if (pthread_create(&th, NULL, poll_waiter_thread, &arg) != 0) {
		close(src[0]); close(src[1]); close(dst[0]); close(dst[1]);
		return -1;
	}
	usleep(300 * 1000);

	char buf[4096];
	memset(buf, 'x', sizeof(buf));
	write(src[1], buf, sizeof(buf));
	close(src[1]);
	splice(src[0], NULL, dst[1], NULL, sizeof(buf), 0);
	close(dst[1]);

	pthread_join(th, NULL);
	close(src[0]);
	close(dst[0]);

	/* woke_ms in [0, 1500) counts as "woke promptly" -- 300ms feed delay
	 * plus generous scheduling slack, well under the 2000ms poll timeout
	 * that a genuinely-hung waiter would hit. */
	return (arg.woke_ms >= 0 && arg.woke_ms < 1500) ? 0 : 1;
}

static void probe_splice(void)
{
	report_row_t *r = add_row("splice", "A",
		"splice() EOF 语义 + 写管道唤醒 poll/epoll");
	r->has_disable_flag = 1;

	int eof_needed = splice_eof_is_broken();

	start_stress();
	int wake_needed_rounds = 0;
	for (int i = 0; i < g_rounds; i++) {
		int res = splice_wake_defect_once();
		if (res > 0)
			wake_needed_rounds++;
	}
	stop_stress();

	if (eof_needed > 0 || wake_needed_rounds > 0) {
		r->verdict = V_NEEDED;
		snprintf(r->note, sizeof(r->note),
			"EOF 语义%s；poll 唤醒缺陷在 %d/%d 轮复现",
			eof_needed > 0 ? "仍报 EPIPE" : "正常", wake_needed_rounds,
			g_rounds);
	} else if (eof_needed < 0) {
		r->verdict = V_INCONCLUSIVE;
		snprintf(r->note, sizeof(r->note), "管道创建失败，未能测试");
	} else {
		r->verdict = V_INCONCLUSIVE;
		snprintf(r->note, sizeof(r->note),
			"%d 轮加压下未复现 poll 唤醒缺陷，EOF 语义正常——间歇性缺陷，"
			"未复现不等于已修复，必要时用更大的 --rounds 重跑",
			g_rounds);
	}
}

static int epoll_pipe_defect_once(void)
{
	int p[2];
	if (pipe(p) != 0)
		return -1;
	int epfd = epoll_create1(0);
	if (epfd < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = p[0];
	epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev);

	write(p[1], "x", 1);

	struct epoll_event out[1];
	int n = epoll_wait(epfd, out, 1, 2000);
	close(epfd);
	close(p[0]);
	close(p[1]);
	return (n > 0) ? 0 : 1;
}

static void probe_epoll_pipe(void)
{
	report_row_t *r = add_row("epoll_pipe", "A",
		"epoll/poll 丢失管道可读状态 (T50)");
	r->has_disable_flag = 1;

	start_stress();
	int needed_rounds = 0, errors = 0;
	for (int i = 0; i < g_rounds; i++) {
		int res = epoll_pipe_defect_once();
		if (res < 0)
			errors++;
		else if (res > 0)
			needed_rounds++;
	}
	stop_stress();

	if (needed_rounds > 0) {
		r->verdict = V_NEEDED;
		snprintf(r->note, sizeof(r->note), "在 %d/%d 轮复现（%d 轮建立失败）",
			needed_rounds, g_rounds, errors);
	} else {
		r->verdict = V_INCONCLUSIVE;
		snprintf(r->note, sizeof(r->note),
			"%d 轮加压下未复现（%d 轮建立失败）——间歇性缺陷，未复现不等于"
			"已修复，必要时用更大的 --rounds 重跑", g_rounds, errors);
	}
}

static void run_a_group_probes(void)
{
	probe_close_range();
	probe_fchmodat2();
	probe_getpwuid_r();
	probe_tmpfile();
	probe_getcwd();
	probe_linkat();
	probe_symlinkat();
	probe_splice();
	probe_epoll_pipe();
}

/* ==================================================================== */
/*  B-group: surrounding platform capabilities (informational only)     */
/* ==================================================================== */

static void child_openat2(void *arg, child_result_t *out)
{
	(void)arg;
	struct open_how how;
	memset(&how, 0, sizeof(how));
	how.flags = O_RDONLY | O_CLOEXEC;
	how.resolve = 0x40; /* RESOLVE_BENEATH */
	errno = 0;
	long ret = syscall(SYS_openat2, AT_FDCWD, ".", &how, sizeof(how));
	out->rc = (int)ret;
	out->err = errno;
	if (ret >= 0)
		close((int)ret);
}

static void child_epoll_pwait2(void *arg, child_result_t *out)
{
	(void)arg;
	int epfd = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event events[1];
	struct timespec timeout = { 0, 0 };
	errno = 0;
	long ret = syscall(SYS_epoll_pwait2, epfd, events, 1, &timeout, NULL, (size_t)8);
	out->rc = (int)ret;
	out->err = errno;
	close(epfd);
}

struct probe_clone_args {
	uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls;
};

static void child_clone3(void *arg, child_result_t *out)
{
	(void)arg;
	struct probe_clone_args ca;
	memset(&ca, 0, sizeof(ca));
	ca.exit_signal = SIGCHLD;
	errno = 0;
	long pid = syscall(SYS_clone3, &ca, sizeof(ca));
	if (pid == 0)
		_exit(0); /* we are the new child -- must not fall through into
			   * the caller's own pipe-report logic */
	out->rc = (int)pid;
	out->err = errno;
	if (pid > 0) {
		int st;
		waitpid((pid_t)pid, &st, 0);
	}
}

static void child_statx(void *arg, child_result_t *out)
{
	(void)arg;
	unsigned char stxbuf[256];
	memset(stxbuf, 0, sizeof(stxbuf));
	errno = 0;
	long ret = syscall(SYS_statx, AT_FDCWD, ".", 0, 0x7ffu, stxbuf);
	out->rc = (int)ret;
	out->err = errno;
}

static void child_renameat2(void *arg, child_result_t *out)
{
	(void)arg;
	char a[PATH_MAX], b[PATH_MAX];
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/data/storage/el2/base";
	snprintf(a, sizeof(a), "%s/ohos-check-rn2-a-%d", dir, (int)getpid());
	snprintf(b, sizeof(b), "%s/ohos-check-rn2-b-%d", dir, (int)getpid());
	int fd = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		out->rc = -1;
		out->err = errno;
		return;
	}
	close(fd);
	unlink(b);
	errno = 0;
	long ret = syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 0);
	out->rc = (int)ret;
	out->err = errno;
	unlink(a);
	unlink(b);
}

static void child_copy_file_range(void *arg, child_result_t *out)
{
	(void)arg;
	char a[PATH_MAX], b[PATH_MAX];
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/data/storage/el2/base";
	snprintf(a, sizeof(a), "%s/ohos-check-cfr-a-%d", dir, (int)getpid());
	snprintf(b, sizeof(b), "%s/ohos-check-cfr-b-%d", dir, (int)getpid());
	int fa = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fa < 0) {
		out->rc = -1;
		out->err = errno;
		return;
	}
	write(fa, "hello", 5);
	int fb = open(b, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fb < 0) {
		out->rc = -1;
		out->err = errno;
		close(fa);
		unlink(a);
		return;
	}
	errno = 0;
	long long off_in = 0, off_out = 0;
	long ret = syscall(SYS_copy_file_range, fa, &off_in, fb, &off_out, 5, 0);
	out->rc = (int)ret;
	out->err = errno;
	close(fa);
	close(fb);
	unlink(a);
	unlink(b);
}

static void child_sendfile(void *arg, child_result_t *out)
{
	(void)arg;
	int p[2];
	if (pipe(p) != 0) {
		out->rc = -1;
		out->err = errno;
		return;
	}
	char a[PATH_MAX];
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = "/data/storage/el2/base";
	snprintf(a, sizeof(a), "%s/ohos-check-sendfile-%d", dir, (int)getpid());
	int fa = open(a, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fa < 0) {
		out->rc = -1;
		out->err = errno;
		close(p[0]);
		close(p[1]);
		return;
	}
	write(fa, "hello", 5);
	off_t off = 0;
	errno = 0;
	ssize_t n = sendfile(p[1], fa, &off, 5);
	out->rc = (int)n;
	out->err = errno;
	close(fa);
	close(p[0]);
	close(p[1]);
	unlink(a);
}

static void child_memfd_create(void *arg, child_result_t *out)
{
	(void)arg;
	errno = 0;
	long fd = syscall(SYS_memfd_create, "ohos-check", 0);
	out->rc = (int)fd;
	out->err = errno;
	if (fd >= 0)
		close((int)fd);
}

static void child_pidfd_open(void *arg, child_result_t *out)
{
	(void)arg;
	errno = 0;
	long fd = syscall(SYS_pidfd_open, getpid(), 0);
	out->rc = (int)fd;
	out->err = errno;
	if (fd >= 0)
		close((int)fd);
}

static void probe_b_generic(const char *id, const char *desc, child_fn fn)
{
	report_row_t *r = add_row(id, "B", desc);
	child_result_t res;
	run_guarded(fn, NULL, &res);
	r->verdict = V_INFO;
	if (res.crashed) {
		snprintf(r->note, sizeof(r->note), "被信号杀死(sig=%d)——仍被拦截",
			res.term_sig);
	} else if (res.rc >= 0) {
		snprintf(r->note, sizeof(r->note), "可用（rc=%d）", res.rc);
	} else {
		snprintf(r->note, sizeof(r->note), "受限：errno=%d(%s)", res.err,
			strerror(res.err));
	}
}

static void child_ptrace(void *arg, child_result_t *out)
{
	(void)arg;
	pid_t pid = fork();
	if (pid < 0) {
		out->rc = -1;
		out->err = errno;
		return;
	}
	if (pid == 0) {
		/* Smuggle PTRACE_TRACEME's real errno out through the exit code
		 * (0-255) -- this grandchild has no pipe of its own, only the
		 * outer forked child (below) does. A fixed sentinel here would
		 * have hidden the actual failure reason (e.g. EPERM) behind a
		 * meaningless one. */
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
			_exit(errno > 0 && errno < 256 ? errno : 255);
		raise(SIGSTOP);
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, WUNTRACED);
	if (WIFSIGNALED(status)) {
		out->rc = -2;
		out->aux = WTERMSIG(status);
		return;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		out->rc = -1;
		out->err = WEXITSTATUS(status); /* PTRACE_TRACEME's real errno */
		return;
	}
	if (!WIFSTOPPED(status)) {
		out->rc = -1;
		out->err = ESRCH; /* genuinely unexpected: neither stopped, signaled,
				   * nor an exit we already handled above */
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return;
	}
	ptrace(PTRACE_DETACH, pid, NULL, NULL);
	waitpid(pid, &status, 0);
	out->rc = 0;
}

static void probe_ptrace(void)
{
	report_row_t *r = add_row("ptrace", "B",
		"调试器 attach 基础能力（PTRACE_TRACEME）——决定 strace 能不能用");
	child_result_t res;
	run_guarded(child_ptrace, NULL, &res);
	r->verdict = V_INFO;
	if (res.crashed) {
		snprintf(r->note, sizeof(r->note), "探测进程本身被信号杀死(sig=%d)",
			res.term_sig);
	} else if (res.rc == 0) {
		snprintf(r->note, sizeof(r->note),
			"PTRACE_TRACEME/DETACH 均成功——真 ptrace 可用，ohos-trace-shim/"
			"qemu -strace 变通理论上可以退回真 ptrace");
	} else if (res.rc == -2) {
		snprintf(r->note, sizeof(r->note),
			"tracee 被信号杀死(sig=%d)——沙箱仍拦截 ptrace", res.aux);
	} else {
		snprintf(r->note, sizeof(r->note), "失败 rc=%d errno=%d(%s)", res.rc,
			res.err, strerror(res.err));
	}
}

static void child_prctl_ptracer(void *arg, child_result_t *out)
{
	(void)arg;
	errno = 0;
	out->rc = (int)prctl(PR_SET_PTRACER, 0, 0, 0, 0);
	out->err = errno;
}

static void probe_prctl_ptracer(void)
{
	report_row_t *r = add_row("prctl_ptracer", "B",
		"prctl(PR_SET_PTRACER)——OfficeCLI(.NET) 被 SIGSYS 杀死的根因");
	child_result_t res;
	run_guarded(child_prctl_ptracer, NULL, &res);
	r->verdict = V_INFO;
	if (res.crashed) {
		snprintf(r->note, sizeof(r->note),
			"被信号杀死(sig=%d)——和已知 OfficeCLI 根因一致，仍受限",
			res.term_sig);
	} else if (res.rc == 0) {
		snprintf(r->note, sizeof(r->note), "调用成功——这条限制已放开");
	} else {
		snprintf(r->note, sizeof(r->note), "调用失败 errno=%d(%s)", res.err,
			strerror(res.err));
	}
}

/* -- dlopen 是否已能解析主二进制 .dynsym（见 checkdep.c） -------------- */

static int resolve_sibling_lib(const char *filename, char *out, size_t outsz)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = '\0';
		char *slash = strrchr(exe, '/');
		if (slash)
			*slash = '\0';
		/* Formula layout: libexec/ohos-compat-check + lib/<filename>. */
		snprintf(out, outsz, "%s/../lib/%s", exe, filename);
		if (access(out, F_OK) == 0)
			return 1;
		/* Flat dev layout: `make check` builds everything straight into
		 * the repo root (see Makefile) -- no libexec/lib split. */
		snprintf(out, outsz, "%s/%s", exe, filename);
		if (access(out, F_OK) == 0)
			return 1;
	}
	const char *prefix = getenv("HOMEBREW_PREFIX");
	if (prefix && *prefix) {
		snprintf(out, outsz, "%s/opt/ohos-compat-shim/lib/%s", prefix, filename);
		if (access(out, F_OK) == 0)
			return 1;
	}
	const char *home = getenv("HOME");
	if (home) {
		snprintf(out, outsz, "%s/.harmonybrew/opt/ohos-compat-shim/lib/%s", home,
			filename);
		if (access(out, F_OK) == 0)
			return 1;
	}
	return 0;
}

static void probe_dlopen_dynsym(void)
{
	report_row_t *r = add_row("dlopen_dynsym", "B",
		"musl dlopen 是否已能解析主二进制 .dynsym（决定 zsh 等还要不要 "
		"--disable-dynamic 静态内建）");
	r->verdict = V_INFO;

	char path[PATH_MAX];
	if (!resolve_sibling_lib("libohos_compat_checkdep.so", path, sizeof(path))) {
		snprintf(r->note, sizeof(r->note),
			"未找到 libohos_compat_checkdep.so，跳过（需要配套探测库同装）");
		return;
	}
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		snprintf(r->note, sizeof(r->note), "dlopen(%s) 失败：%s", path, dlerror());
		return;
	}
	typedef int (*probe_fn)(void);
	probe_fn fn = (probe_fn)dlsym(h, "ohos_checkdep_probe");
	if (!fn) {
		snprintf(r->note, sizeof(r->note), "checkdep.so 里找不到探测符号，跳过");
		dlclose(h);
		return;
	}
	int resolved = fn();
	dlclose(h);
	if (resolved)
		snprintf(r->note, sizeof(r->note),
			"已能解析——限制已放开，静态内建变通理论上可以退回动态版");
	else
		snprintf(r->note, sizeof(r->note),
			"仍不能解析——dlopen 依赖此符号解析的软件仍需静态内建（如 brew "
			"zsh --disable-dynamic）");
}

static void run_b_group_probes(void)
{
	probe_b_generic("openat2", "openat2(RESOLVE_BENEATH)——bun rustix 走裸 syscall，"
		"LD_PRELOAD 打不到，只能等平台放开或改源码", child_openat2);
	probe_b_generic("epoll_pwait2", "bun 事件循环纳秒级超时——同上，裸 syscall",
		child_epoll_pwait2);
	probe_b_generic("clone3", "glibc/部分工具链创建进程的首选接口", child_clone3);
	probe_b_generic("statx", "扩展 stat", child_statx);
	probe_b_generic("renameat2", "原子重命名/RENAME_NOREPLACE", child_renameat2);
	probe_b_generic("copy_file_range", "内核态零拷贝文件复制", child_copy_file_range);
	probe_b_generic("sendfile", "内核态零拷贝 fd→fd", child_sendfile);
	probe_ptrace();
	probe_prctl_ptracer();
	probe_b_generic("memfd_create", "匿名内存文件", child_memfd_create);
	probe_b_generic("pidfd_open", "进程句柄 fd 化", child_pidfd_open);
	probe_dlopen_dynsym();
}

/* ==================================================================== */
/*  Output                                                              */
/* ==================================================================== */

/* Captured once in main() so both the human header and --json's "meta"
 * object can present the same facts without printing them twice. */
static struct utsname g_uname;
static int g_api_level, g_target_sdk;
static char g_orig_ld_preload[PATH_MAX];

static void capture_header_facts(const char *orig_ld_preload)
{
	uname(&g_uname);
#ifdef __OHOS__
	g_api_level = get_device_api_version();
	g_target_sdk = get_application_target_sdk_version();
#else
	g_api_level = -1;
	g_target_sdk = -1;
#endif
	snprintf(g_orig_ld_preload, sizeof(g_orig_ld_preload), "%s",
		orig_ld_preload ? orig_ld_preload : "");
}

/* Human-readable form, stdout — only called when NOT --json, so stdout in
 * --json mode is a single parseable document with no stray text prefix. */
static void print_header(int is_pass2)
{
	printf("=== ohos-compat-shim 平台能力自检%s ===\n",
		is_pass2 ? "（第二遍：带 shim）" : "");
	printf("uname: %s %s %s\n", g_uname.sysname, g_uname.release, g_uname.machine);
	if (g_api_level >= 0)
		printf("OHOS API level: %d (target_sdk=%d)\n", g_api_level, g_target_sdk);
	printf("uid=%d gid=%d euid=%d\n", (int)getuid(), (int)getgid(), (int)geteuid());
	printf("原 LD_PRELOAD: %s\n", *g_orig_ld_preload ? g_orig_ld_preload : "(未设置)");
	printf("\n");
}

static void print_table(int is_pass2)
{
	printf("%-16s %-4s %-8s %s\n", "拦截点/能力", "组", "结论", "说明");
	printf("%-16s %-4s %-8s %s\n", "----------------", "----", "--------",
		"----------------------------------------------------------");
	for (int i = 0; i < g_nrows; i++) {
		report_row_t *r = &g_rows[i];
		printf("%-16s %-4s %-8s %s\n", r->id, r->group, verdict_str(r->verdict),
			r->note);
	}
	if (is_pass2)
		return;

	printf("\n建议：export OHOS_COMPAT_SHIM_DISABLE=");
	int first = 1;
	for (int i = 0; i < g_nrows; i++) {
		report_row_t *r = &g_rows[i];
		if (r->has_disable_flag && r->verdict == V_DROPPABLE) {
			printf("%s%s", first ? "" : ",", r->id);
			first = 0;
		}
	}
	if (first)
		printf("（无——当前没有可安全关闭的拦截点）");
	printf("\n");
	printf("注：仍需要/不确定的拦截点不建议关闭；B 组是信息性的周边平台能力，"
		"不产出关闭建议；完整 90+ 项平台能力矩阵见 ohos-preflight。\n");
}

static void json_escaped(const char *s)
{
	putchar('"');
	for (const char *p = s; *p; p++) {
		if (*p == '"' || *p == '\\')
			putchar('\\');
		if ((unsigned char)*p < 0x20) {
			printf("\\u%04x", *p);
			continue;
		}
		putchar(*p);
	}
	putchar('"');
}

static void print_json(int is_pass2)
{
	printf("{\"meta\":{\"pass2_with_shim\":%s,\"uname_sysname\":", is_pass2 ? "true" : "false");
	json_escaped(g_uname.sysname);
	printf(",\"uname_release\":");
	json_escaped(g_uname.release);
	printf(",\"uname_machine\":");
	json_escaped(g_uname.machine);
	printf(",\"api_level\":%d,\"target_sdk\":%d", g_api_level, g_target_sdk);
	printf(",\"uid\":%d,\"gid\":%d,\"euid\":%d", (int)getuid(), (int)getgid(), (int)geteuid());
	printf(",\"orig_ld_preload\":");
	json_escaped(g_orig_ld_preload);
	printf("},\"rows\":[");
	for (int i = 0; i < g_nrows; i++) {
		report_row_t *r = &g_rows[i];
		if (i)
			printf(",");
		printf("{\"id\":");
		json_escaped(r->id);
		printf(",\"group\":");
		json_escaped(r->group);
		printf(",\"verdict\":");
		json_escaped(verdict_str(r->verdict));
		printf(",\"has_disable_flag\":%d", r->has_disable_flag);
		printf(",\"note\":");
		json_escaped(r->note);
		printf("}");
	}
	printf("],\"disable_recommendation\":\"");
	int first = 1;
	for (int i = 0; i < g_nrows; i++) {
		report_row_t *r = &g_rows[i];
		if (r->has_disable_flag && r->verdict == V_DROPPABLE) {
			if (!first)
				printf(",");
			printf("%s", r->id);
			first = 0;
		}
	}
	printf("\"}\n");
}

static void print_usage(void)
{
	printf(
		"usage: ohos-compat-check [--json] [--rounds N] [--with-shim] [--help]\n"
		"\n"
		"对当前设备逐项探测 ohos-compat-shim 的每个拦截点在真实内核/沙箱上是否\n"
		"还会复现所修的症状，帮助判断哪些可以通过 OHOS_COMPAT_SHIM_DISABLE 关闭。\n"
		"同时汇报若干与本工作区已知 workaround 挂钩的周边平台限制（仅供参考，\n"
		"不产出 DISABLE 建议）。\n"
		"\n"
		"  --json         机器可读输出\n"
		"  --rounds N     splice/epoll_pipe 等间歇性缺陷的重复探测轮数（默认 20）\n"
		"  --with-shim    追加第二遍：预加载 libohos_compat.so 后重跑一遍，验证 shim 修好了\n"
		"\n"
		"通常通过 `ohos-shim check` 调用（自动 env -u LD_PRELOAD）；也可以直接把\n"
		"这份源码 scp 去别的机器用 ohos-sdk clang 单独编译，见项目 Makefile 的\n"
		"`check` target 与同目录 checkdep.c。\n");
}

int main(int argc, char **argv)
{
	int is_pass2 = getenv("_OHOS_COMPAT_CHECK_PASS2") != NULL;
	int json = 0, with_shim = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0) {
			json = 1;
		} else if (strcmp(argv[i], "--with-shim") == 0) {
			with_shim = 1;
		} else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
			g_rounds = atoi(argv[++i]);
		} else if (strncmp(argv[i], "--rounds=", 9) == 0) {
			g_rounds = atoi(argv[i] + 9);
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage();
			return 0;
		} else {
			fprintf(stderr, "ohos-compat-check: 未知参数 %s（--help 看用法）\n",
				argv[i]);
			return 64;
		}
	}
	if (g_rounds < 1)
		g_rounds = 1;
	if (g_rounds > 500)
		g_rounds = 500;

	char orig_preload[PATH_MAX] = "";
	const char *ep = getenv("LD_PRELOAD");
	if (ep)
		snprintf(orig_preload, sizeof(orig_preload), "%s", ep);

	if (!is_pass2)
		unsetenv("LD_PRELOAD"); /* enforce a clean baseline regardless of caller */

	capture_header_facts(orig_preload);
	if (!json)
		print_header(is_pass2);

	g_nrows = 0;
	run_a_group_probes();
	if (!is_pass2)
		run_b_group_probes();

	if (json)
		print_json(is_pass2);
	else
		print_table(is_pass2);

	if (!is_pass2 && with_shim) {
		char shim_path[PATH_MAX];
		if (resolve_sibling_lib("libohos_compat.so", shim_path, sizeof(shim_path))) {
			/* Non-JSON mode gets a human banner; JSON mode stays a bare
			 * stream of one JSON object per pass (each individually valid,
			 * not one combined document) so a consumer can still parse
			 * each with a per-line/per-object JSON reader. */
			if (!json)
				printf("\n=== 二次运行：带 shim（%s）===\n\n", shim_path);
			else
				printf("\n");
			fflush(stdout);
			setenv("LD_PRELOAD", shim_path, 1);
			setenv("_OHOS_COMPAT_CHECK_PASS2", "1", 1);
			execv("/proc/self/exe", argv);
			fprintf(stderr, "ohos-compat-check: 无法重新执行自身验证 --with-shim: %s\n",
				strerror(errno));
			return 1;
		}
		if (!json)
			printf("\n(--with-shim: 未找到 libohos_compat.so，跳过二次验证)\n");
	}

	return 0;
}
