/*
 * ohos_compat_shim.c — LD_PRELOAD compatibility shim for HarmonyOS-sandboxed
 * aarch64/musl processes.
 *
 * HarmonyOS's application sandbox seccomp-filters several Linux syscalls that
 * OpenHarmony itself (and the upstream kernel) supports, and returns
 * unexpected errno values from a few libc calls that assume a traditional
 * /etc/passwd-backed uid or a writable P_tmpdir. This library intercepts the
 * *libc-symbol* level of those calls and falls back to a userspace
 * implementation when the real one fails in the HarmonyOS-specific way.
 *
 * IMPORTANT — SCOPE: this only works for calls that resolve through a
 * dynamically-linked libc symbol (syscall(), getpwuid_r(), tmpfile(), ...).
 * Code that issues the syscall via inline assembly (e.g. Bun's rustix
 * `linux_raw` backend for openat2/epoll_pwait2) never touches these symbols
 * and cannot be reached by LD_PRELOAD interposition — see the sibling
 * ohos-preflight repo's docs/ohos-preload-shim-feasibility.md for the full
 * matrix. Nothing here replaces a source-level port; it only helps
 * *prebuilt* dynamic-musl binaries survive the sandbox without a rebuild.
 *
 * Design rule for every intercepted symbol: resolve the real implementation
 * via dlsym(RTLD_NEXT, ...) first and prefer it. Most fall back only when
 * the real call fails with the specific HarmonyOS-sandbox symptom (SIGSYS /
 * ENOENT / EPERM as documented per-symbol below), making the shim a safe
 * no-op on any target where the real call already works. Two exceptions
 * never take that probe-first path because their symptom is a real,
 * consistently-reproducing kernel behavior rather than something to detect
 * per call: close_range()/syscall(SYS_close_range) (unconditional SIGSYS on
 * this class of device, independent of whether anything is preloaded —
 * matching ohos-preflight's own a10_close_range probe) and the epoll_pipe
 * ONESHOT-enforcement cluster (epoll_ctl/epoll_wait/epoll_pwait; see that
 * section's own comment).
 *
 * close_range()/syscall(SYS_close_range) probes the native syscall and
 * falls back to userspace handling when the operation is unavailable or
 * blocked.
 *
 * Build: see ../Makefile (OHOS NDK clang, -shared -fPIC -ldl).
 * Usage: LD_PRELOAD=/path/to/libohos_compat.so <program> ...
 *
 * Runtime toggles (comma-separated symbol names):
 *   OHOS_COMPAT_SHIM_DISABLE — turn OFF a default-on interceptor
 *                              (close_range, getpwuid_r, tmpfile,
 *                               fchmodat2, link, linkat, splice,
 *                               epoll_pipe, getaddrinfo, std_streams)
 *
 * Deliberately NOT implemented: pthread_cancel (musl stub is a no-op;
 * emulating cancellation needs cooperative checkpoints in the target
 * program, which a preload shim cannot inject).
 */

#define _GNU_SOURCE
#include <elf.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "ohos_compat_shim.c: only supports aarch64 (HarmonyOS/OHOS target)"
#endif

/* /tmp is read-only in the OHOS sandbox; default TMPDIR so the program's own
 * mkstemp/getenv temp calls land on a writable, auto-cleaned path. Preserves
 * any user- or wrapper-set TMPDIR. */
__attribute__((constructor))
static void ohos_shim_init_tmpdir(void)
{
	if (!getenv("TMPDIR"))
		setenv("TMPDIR", "/data/storage/el2/base/cache", 1);
}


/* ------------------------------------------------------------------ */
/*  Runtime toggle parsing                                            */
/* ------------------------------------------------------------------ */

/* Returns 1 if `name` appears in the comma-separated list env var `var`. */
static int env_list_has(const char *var, const char *name)
{
	const char *list = getenv(var);
	if (!list || !*list)
		return 0;

	size_t name_len = strlen(name);
	const char *p = list;
	while (*p) {
		const char *comma = strchr(p, ',');
		size_t seg_len = comma ? (size_t)(comma - p) : strlen(p);
		if (seg_len == name_len && strncmp(p, name, name_len) == 0)
			return 1;
		if (!comma)
			break;
		p = comma + 1;
	}
	return 0;
}

/*
 * shim_disabled() are called from every interceptor
 * entry point — including syscall(), which every non-close_range syscall in
 * the process also routes through. A naive getenv()+scan on every single
 * call would tax the hottest path in the library for no reason: the toggle
 * env vars can't change under a running process, so parse them into a
 * bitmask exactly once (idempotent even if two threads race into this
 * before the mask is set — same env, same result, no lock needed) and turn
 * every subsequent check into a cheap strcmp + bitwise AND.
 */
enum {
	SD_CLOSE_RANGE = 1 << 0,
	SD_GETPWUID_R  = 1 << 1,
	SD_TMPFILE     = 1 << 2,
	SD_FCHMODAT2   = 1 << 3,
	SD_LINKAT      = 1 << 4,
	SD_SPLICE      = 1 << 5,
	SD_EPOLL_PIPE  = 1 << 6,
	SD_GETADDRINFO = 1 << 7,
	SD_LINK        = 1 << 8,
	SD_STD_STREAMS = 1 << 9,
};

static int g_disable_mask = -1;

static void parse_toggle_masks(void)
{
	if (g_disable_mask >= 0)
		return;

	int d = 0;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "close_range"))
		d |= SD_CLOSE_RANGE;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "getpwuid_r"))
		d |= SD_GETPWUID_R;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "tmpfile"))
		d |= SD_TMPFILE;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "fchmodat2"))
		d |= SD_FCHMODAT2;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "linkat"))
		d |= SD_LINKAT;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "splice"))
		d |= SD_SPLICE;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "epoll_pipe"))
		d |= SD_EPOLL_PIPE;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "getaddrinfo"))
		d |= SD_GETADDRINFO;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "link"))
		d |= SD_LINK;
	if (env_list_has("OHOS_COMPAT_SHIM_DISABLE", "std_streams"))
		d |= SD_STD_STREAMS;
	g_disable_mask = d; /* set last: non-negative value doubles as "done" */
}

/* Takes the SD_* bit directly rather than a name string: every call site
 * knows its own toggle at compile time, so there is no reason to pay a
 * string-compare chain on what is often a hot path (syscall()'s dispatcher
 * runs this on every raw syscall the process makes once this library is
 * preloaded). */
static int shim_disabled(int bit)
{
	parse_toggle_masks();
	return !!(g_disable_mask & bit);
}


/* ------------------------------------------------------------------ */
/*  std_streams: backfill stdout/stderr/stdin COPY-reloc slots         */
/* ------------------------------------------------------------------ */

/*
 * Dynamically-linked musl executables that reference stdout/stderr/stdin
 * get R_AARCH64_COPY relocations whose dynsym entries also *define* the
 * symbol at the copy target (a .bss slot) in the executable. glibc's ld.so
 * resolves the copy against the libc definition, but HarmonyOS's musl
 * ld.so resolves it against the executable's own definition — an 8-byte
 * self-copy that leaves the slot at its .bss initial value NULL. The first
 * stdio call (e.g. bun's setvbuf(stdout, NULL, _IOLBF, 0)) then dies in
 * the hardened libc assert "setvbuf: parameter is null". Verified against
 * official prebuilt claude-code linux-arm64-musl binaries.
 *
 * This constructor scans the main executable's .rela.dyn for COPY relocs
 * against those three names and writes libc's real FILE* into each NULL
 * slot found (pure no-op for executables without such relocations). That
 * patch is confirmed to make the crash go away when this .so is preloaded.
 * What is NOT confirmed: that this patch is the (or the only) reason —
 * disabling it via OHOS_COMPAT_SHIM_DISABLE=std_streams, and even building
 * a stripped-down .so with none of this file's other interceptors, still
 * avoided the crash in testing, while a truly minimal preload .so did not.
 * Something about this file's full symbol table/relocation footprint
 * appears to matter independent of this constructor's own logic, and that
 * mechanism was not further isolated. Kept as the one documented, coded
 * fix rather than removed, since it is correct and harmless on its own
 * terms even if it isn't the full explanation.
 */

#define OHOS_R_AARCH64_COPY 1024

struct std_fixup {
	const char *name;
	Elf64_Addr slot;
};

/* dl_iterate_phdr visits the main program first; on musl its dlpi_name is
 * the exe path rather than glibc's "", so track the first reported phdr
 * pointer instead of comparing names. */
static int phdr_is_main_exe(const struct dl_phdr_info *info)
{
	static const Elf64_Phdr *first;
	if (!first) {
		first = info->dlpi_phdr;
		return 1;
	}
	return info->dlpi_phdr == first;
}

static int std_streams_collect(struct dl_phdr_info *info, size_t sz, void *data)
{
	struct std_fixup *f = data;

	(void)sz;
	if (!phdr_is_main_exe(info))
		return 1;

	for (int j = 0; j < info->dlpi_phnum; j++) {
		const Elf64_Phdr *ph = &info->dlpi_phdr[j];
		if (ph->p_type != PT_DYNAMIC)
			continue;

		Elf64_Addr base = info->dlpi_addr;
		Elf64_Dyn *d = (Elf64_Dyn *)(base + ph->p_vaddr);
		Elf64_Sym *symtab = NULL;
		const char *strtab = NULL;
		const Elf64_Rela *rela = NULL;
		Elf64_Xword relasz = 0;

		for (; d->d_tag != DT_NULL; d++) {
			switch (d->d_tag) {
			case DT_SYMTAB: symtab = (Elf64_Sym *)(base + d->d_un.d_ptr); break;
			case DT_STRTAB: strtab = (const char *)(base + d->d_un.d_ptr); break;
			case DT_RELA:   rela = (const Elf64_Rela *)(base + d->d_un.d_ptr); break;
			case DT_RELASZ: relasz = d->d_un.d_val; break;
			}
		}
		if (!symtab || !strtab || !rela || !relasz)
			return 1;

		for (Elf64_Xword o = 0; o + sizeof(Elf64_Rela) <= relasz;
		     o += sizeof(Elf64_Rela)) {
			const Elf64_Rela *r = (const Elf64_Rela *)((const char *)rela + o);
			if (ELF64_R_TYPE(r->r_info) != OHOS_R_AARCH64_COPY)
				continue;
			const Elf64_Sym *s = &symtab[ELF64_R_SYM(r->r_info)];
			const char *name = strtab + s->st_name;
			for (int k = 0; k < 3; k++)
				if (strcmp(f[k].name, name) == 0)
					f[k].slot = base + r->r_offset;
		}
		return 1;
	}
	return 1;
}

/* Returns libc's FILE* for the named stream, or NULL. The executable
 * exports its own (still-NULL) definition, so a plain dlsym() would find
 * that one — RTLD_NEXT from this preloaded library resolves inside libc
 * instead. musl defines the variable as `FILE *const stdout = ...`, so
 * dlsym returns the address of the variable and we dereference it. */
static void *std_streams_libc_file(const char *name)
{
	void *var = dlsym(RTLD_NEXT, name);
	if (var && *(void **)var)
		return *(void **)var;
	return NULL;
}

__attribute__((constructor))
static void ohos_shim_init_std_streams(void)
{
	if (shim_disabled(SD_STD_STREAMS))
		return;

	struct std_fixup f[3] = {
		{ "stdout", 0 },
		{ "stderr", 0 },
		{ "stdin",  0 },
	};
	dl_iterate_phdr(std_streams_collect, f);

	for (int k = 0; k < 3; k++) {
		if (!f[k].slot)
			continue;
		void **slot = (void **)f[k].slot;
		if (*slot) /* already resolved — nothing to do */
			continue;
		void *file = std_streams_libc_file(f[k].name);
		if (file)
			*slot = file;
	}
}

/* ------------------------------------------------------------------ */
/*  close_range flags (kernel UAPI, may be missing from older headers) */
/* ------------------------------------------------------------------ */
#ifndef CLOSE_RANGE_UNSHARE
# define CLOSE_RANGE_UNSHARE  (1U << 1)
#endif
#ifndef CLOSE_RANGE_CLOEXEC
# define CLOSE_RANGE_CLOEXEC  (1U << 2)
#endif
#ifndef __NR_close_range
# define __NR_close_range 436
#endif
#ifndef __NR_fchmodat2
# define __NR_fchmodat2 452
#endif

/* ==================================================================== */
/*  1. close_range() / syscall(SYS_close_range, ...)                    */
/*     Probe-once + SIGSYS catch + userspace fallback via /proc/self/fd */
/*     enumeration.                                                     */
/*                                                                       */
/*  IMPORTANT — confirmed on-device with a guarded standalone repro,    */
/*  not just reading docs: syscall(SYS_close_range, ...) unconditionally */
/*  raises SIGSYS on this HarmonyOS device's real kernel, for every      */
/*  parameter combination tried (a plain valid call, first>last,         */
/*  garbage flags alike) — independent of whether this shim, or ANY      */
/*  other library, is LD_PRELOAD-ed. This matches ohos-preflight's own   */
/*  a10_close_range probe (OH-container pass, HM-device fail) exactly;   */
/*  there is no more complicated "loading a third-party .so changes      */
/*  what's allowed" story here — an earlier draft of this comment        */
/*  claimed there was, based on a test run where the shell's ambient     */
/*  LD_PRELOAD had been silently left pointing at a *different*,         */
/*  narrower preload library by unrelated earlier testing, making every  */
/*  "baseline" run in that session actually shimmed by something else.   */
/*  Lesson, not a platform finding: verify with `env -u LD_PRELOAD`,     */
/*  never trust a shell's ambient state to be clean.                     */
/*                                                                        */
/*  Practical effect: on this class of device, the probe below always    */
/*  lands on CR_PROBE_FALLBACK — there is no "real syscall succeeds"     */
/*  path to take here, though the code still supports one for other      */
/*  devices/OS versions where close_range might actually work (matching  */
/*  ohos-preflight's OH-container track, where this probe passes).       */
/*  OHOS_COMPAT_SHIM_DISABLE=close_range does NOT reproduce true         */
/*  no-shim baseline behavior in the sense of "the operation still       */
/*  works" — the real call SIGSYS's every time regardless, so this       */
/*  reproduces "preloaded but unprotected, no probe", which crashes.     */
/*  See README's Known Platform Behavior section.                        */
/* ==================================================================== */

enum { CR_PROBE_UNKNOWN, CR_PROBE_WORKS, CR_PROBE_FALLBACK };
static int cr_probe_state = CR_PROBE_UNKNOWN;
/* Thread-local: multiple threads can independently be inside a guarded
 * SIGSYS-catching attempt at once (proven by test/functional.c's
 * pthread-based CLOSE_RANGE_UNSHARE test) — a shared jmp_buf across
 * threads would let one thread's siglongjmp corrupt another's stack. */
static __thread sigjmp_buf cr_sigsys_jmp;
/* Thread-local "this thread is currently inside a guarded window" flag.
 * Without it, a *genuine* SIGSYS delivered to a thread that isn't inside
 * shim_guarded_syscall() — the handler is process-wide, installed the
 * instant any thread enters its first guarded call — would siglongjmp into
 * that thread's cr_sigsys_jmp before it has ever been initialized: undefined
 * behavior, worse than the crash the real signal was reporting. */
static __thread int cr_sigsys_armed;

static void cr_sigsys_handler(int sig)
{
	if (!cr_sigsys_armed) {
		/* Not one of ours: restore the default disposition and
		 * re-raise, so the process dies the same way it would with
		 * no shim loaded at all — signal()/raise() are both on the
		 * async-signal-safe list (signal-safety(7)), safe to call
		 * from here. */
		signal(SIGSYS, SIG_DFL);
		raise(sig);
		return;
	}
	siglongjmp(cr_sigsys_jmp, 1);
}

/* sigaction(SIGSYS, ...) install/restore used to happen unconditionally on
 * every shim_guarded_syscall() call, saving/restoring whatever disposition
 * happened to be current *on that call* — with multiple threads racing
 * through guarded windows concurrently (the whole reason cr_sigsys_jmp is
 * thread-local), the first thread to exit could restore a disposition that
 * was never the true pre-shim original, and could tear the handler out from
 * under a second thread still inside its own guarded window. Refcounted
 * install fixes both: the handler goes in once (first 0->1 transition,
 * caching the real original) and comes out once (last ->0 transition,
 * restoring that same original), no matter how threads interleave. */
static pthread_mutex_t cr_sigsys_lock = PTHREAD_MUTEX_INITIALIZER;
static int cr_sigsys_refcount = 0;
static struct sigaction cr_sigsys_old_sa;

static void cr_sigsys_ref(void)
{
	pthread_mutex_lock(&cr_sigsys_lock);
	if (cr_sigsys_refcount++ == 0) {
		struct sigaction sa;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sa.sa_handler = cr_sigsys_handler;
		sigaction(SIGSYS, &sa, &cr_sigsys_old_sa);
	}
	pthread_mutex_unlock(&cr_sigsys_lock);
}

static void cr_sigsys_unref(void)
{
	pthread_mutex_lock(&cr_sigsys_lock);
	if (--cr_sigsys_refcount == 0)
		sigaction(SIGSYS, &cr_sigsys_old_sa, NULL);
	pthread_mutex_unlock(&cr_sigsys_lock);
}

typedef long (*real_syscall_fn)(long, ...);
static real_syscall_fn real_syscall = NULL;

/* Resolve and call the real (next-in-chain) syscall() with up to 6 args.
 * Always passes 6 positional args regardless of how many the real
 * syscall actually uses — harmless, matches how syscall() is normally
 * called with a variable argument count. */
static long cr_real_syscall(long n, long a0, long a1, long a2, long a3,
			    long a4, long a5)
{
	if (!real_syscall)
		real_syscall = (real_syscall_fn)dlsym(RTLD_NEXT, "syscall");
	if (!real_syscall) {
		errno = ENOSYS;
		return -1;
	}
	return real_syscall(n, a0, a1, a2, a3, a4, a5);
}

/* Attempt a real raw-syscall delegate call guarded by a fresh SIGSYS catch,
 * rather than trusting any cached "this syscall number works" state
 * blindly. Generic over syscall number — shared by every raw-syscall-level
 * interceptor in this file (currently close_range and fchmodat2; see each
 * symbol's own section for why a per-call guard, not just a one-time
 * process probe, is necessary). On-device testing found that even a
 * *different argument combination* of an already-probed-safe syscall
 * number can independently SIGSYS (e.g. CLOSE_RANGE_UNSHARE crashed an
 * unprotected call after a flags=0 probe had already reported success),
 * so this is deliberately called around every attempt that hasn't itself
 * been individually probed, not just once per process. Returns 1 with
 * *out_ret set if the syscall returned normally (0 or -1, doesn't matter
 * which); returns 0 if it was caught via SIGSYS instead (*out_ret
 * untouched). */
static int shim_guarded_syscall(long nr, long a0, long a1, long a2, long a3,
				long a4, long a5, long *out_ret)
{
	cr_sigsys_ref();
	cr_sigsys_armed = 1;

	int ok;
	if (sigsetjmp(cr_sigsys_jmp, 1) == 0) {
		*out_ret = cr_real_syscall(nr, a0, a1, a2, a3, a4, a5);
		ok = 1;
	} else {
		ok = 0;
	}

	cr_sigsys_armed = 0;
	cr_sigsys_unref();
	return ok;
}

static void cr_probe_syscall(void)
{
	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		cr_probe_state = CR_PROBE_FALLBACK;
		return;
	}

	long ret;
	if (shim_guarded_syscall(__NR_close_range, (long)fd, (long)fd, 0L, 0, 0, 0, &ret) &&
	    ret == 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF)
		cr_probe_state = CR_PROBE_WORKS;
	else
		cr_probe_state = CR_PROBE_FALLBACK;
	close(fd);
}

/* Userspace fallback: enumerate actually-open fds via /proc/self/fd instead
 * of a linear scan to RLIMIT_NOFILE (typically 1024-65536) — matters because
 * Bun's real call pattern is close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) (see
 * bun/src/jsc/bindings/BunProcess.cpp) — a range that clamps to
 * RLIMIT_NOFILE-1, so a linear scan would mean tens of thousands of
 * close()/fcntl() calls on fds that were never open. Pattern matches
 * ohos-preflight's own validated solutions/a10_close_range.c. Precondition:
 * first <= last and flags is already validated — both call sites below
 * (close_range() and the syscall() override) check this before ever
 * reaching here, matching upstream Linux's close_range(2) semantics
 * (fs/file.c SYSCALL_DEFINE3) up front rather than relying on this
 * function to re-derive it. */
static int cr_do_fallback(unsigned int first, unsigned int last, unsigned int flags)
{
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && last >= rl.rlim_cur)
		last = rl.rlim_cur > 0 ? (unsigned int)rl.rlim_cur - 1 : 0;

	if (flags & CLOSE_RANGE_UNSHARE) {
		/* unshare(CLONE_FILES) itself was found to unconditionally
		 * SIGSYS on this device — independent of close_range's own
		 * gating, and reproduced even completely unshimmed. There is
		 * no real fd-table-privatizing fallback available: closing
		 * fds in the *shared* table instead would silently break the
		 * isolation CLOSE_RANGE_UNSHARE callers rely on (other
		 * threads/whatever this table is shared with would lose
		 * those fds too), which is worse than an honest failure. So
		 * catch the SIGSYS and report ENOSYS/EPERM rather than either
		 * crashing or silently doing the wrong thing. Routed through
		 * shim_guarded_syscall() (via __NR_unshare) rather than a
		 * second hand-rolled sigaction/sigsetjmp pair, now that the
		 * helper is refcounted/thread-armed-flag safe — two copies of
		 * this pattern is exactly what let them race against each
		 * other and against close_range's own guarded calls. */
		long ret;
		if (!shim_guarded_syscall(__NR_unshare, (long)CLONE_FILES, 0, 0, 0, 0, 0, &ret)) {
			errno = ENOSYS;
			return -1;
		}
		if (ret == -1 && errno != EINVAL)
			return -1;
	}

	/* Deliberately NOT opendir()/readdir()/closedir(): this path runs from
	 * close_range()/the syscall() override, which Bun (the primary
	 * consumer) calls right after fork() to clean up the child's fd table
	 * before exec — exactly the window where only async-signal-safe calls
	 * are guaranteed correct. opendir()/readdir() malloc internally, and
	 * the loop used to call this shim's own close(), which now takes
	 * g_ep_pipes_lock via shim_forget_fd(): if that lock (or malloc's own
	 * arena lock) was held by a *different* thread at fork time, the
	 * single surviving child thread deadlocks on its first iteration.
	 * Raw syscalls only, close() bypassed via cr_real_syscall(__NR_close,
	 * ...) so no lock this shim owns is ever touched from here — the
	 * registry itself is separately reset via cr_atfork_child() below. */
	int dfd = (int)cr_real_syscall(__NR_openat, (long)AT_FDCWD,
				       (long)"/proc/self/fd",
				       (long)(O_RDONLY | O_DIRECTORY), 0, 0, 0);
	if (dfd < 0) {
		/* Extreme fallback if /proc isn't mounted/visible: linear scan,
		 * same as ohos-preflight's own documented fallback-of-the-
		 * fallback. Slow, but correct. */
		if (flags & CLOSE_RANGE_CLOEXEC) {
			for (unsigned int i = first; i <= last; i++)
				cr_real_syscall(__NR_fcntl, (long)i, F_SETFD,
						(long)FD_CLOEXEC, 0, 0, 0);
		} else {
			for (unsigned int i = first; i <= last; i++)
				cr_real_syscall(__NR_close, (long)i, 0, 0, 0, 0, 0);
		}
		return 0;
	}

	/* Kernel getdents64(2) ABI struct — deliberately not <dirent.h>'s
	 * `struct dirent`, which is libc's (different-shaped, allocated-by-
	 * readdir) view. Flexible array member holds the NUL-terminated name. */
	struct cr_kernel_dirent64 {
		uint64_t d_ino;
		int64_t  d_off;
		unsigned short d_reclen;
		unsigned char  d_type;
		char     d_name[];
	};
	char buf[4096];
	for (;;) {
		long n = cr_real_syscall(__NR_getdents64, (long)dfd, (long)buf,
					 (long)sizeof(buf), 0, 0, 0);
		if (n <= 0)
			break;
		long off = 0;
		while (off < n) {
			struct cr_kernel_dirent64 *de =
				(struct cr_kernel_dirent64 *)(buf + off);
			if (de->d_name[0] >= '0' && de->d_name[0] <= '9') {
				/* Manual parse, not atoi(): not on the
				 * async-signal-safe list, this one is. */
				unsigned int fd = 0;
				const char *p = de->d_name;
				while (*p >= '0' && *p <= '9')
					fd = fd * 10 + (unsigned int)(*p++ - '0');
				if (fd >= first && fd <= last && (int)fd != dfd) {
					if (flags & CLOSE_RANGE_CLOEXEC)
						cr_real_syscall(__NR_fcntl, (long)fd,
								F_SETFD, (long)FD_CLOEXEC,
								0, 0, 0);
					else
						cr_real_syscall(__NR_close, (long)fd,
								0, 0, 0, 0, 0);
				}
			}
			off += de->d_reclen;
		}
	}
	cr_real_syscall(__NR_close, (long)dfd, 0, 0, 0, 0, 0);
	return 0;
}

/* Validate against upstream Linux's close_range(2) semantics (fs/file.c:
 * SYSCALL_DEFINE3(close_range, ...) — `if (flags & ~(...)) return -EINVAL`,
 * `if (fd > max_fd) return -EINVAL`) BEFORE ever asking the real kernel or
 * touching the fallback. Two things forced this to be upfront validation
 * rather than "just ask the real syscall and pass its answer through":
 *
 *   1. On-device testing found this device's kernel does NOT enforce
 *      `first > last` -> EINVAL the way upstream Linux does — it silently
 *      returns success for e.g. close_range(4, 3, 0). Deferring to "real"
 *      behavior here would leak that platform quirk to every caller
 *      instead of the standards-conformant behavior a consumer coded
 *      against Linux (like Bun) actually expects.
 *   2. Forwarding genuinely invalid flags (e.g. all bits set) to the raw
 *      syscall on this device raises an *unhandled* SIGSYS and kills the
 *      process — worse than a graceful EINVAL. Rejecting bad flags here
 *      means we never issue that syscall at all.
 */
static int cr_validate_args(unsigned int first, unsigned int last, unsigned int flags)
{
	if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) {
		errno = EINVAL;
		return -1;
	}
	if (first > last) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* Shared dispatch for both entry points below (once args are validated).
 * flags==0 is the one exact combination cr_probe_syscall() already proved
 * safe, so it takes the cheap unguarded path once WORKS is cached; any
 * other flags value gets its own guarded attempt (see
 * shim_guarded_syscall) since e.g. CLOSE_RANGE_UNSHARE was found to
 * independently SIGSYS on this device even after a flags=0 probe
 * succeeded — and a SIGSYS there downgrades the cached state to FALLBACK
 * so later calls skip straight to the safe path instead of re-discovering
 * the same crash. */
static int cr_dispatch(unsigned int first, unsigned int last, unsigned int flags)
{
	if (__builtin_expect(cr_probe_state == CR_PROBE_UNKNOWN, 0))
		cr_probe_syscall();

	if (cr_probe_state == CR_PROBE_WORKS) {
		if (flags == 0) {
			long ret = cr_real_syscall(__NR_close_range, (long)first,
						  (long)last, 0, 0, 0, 0);
			if (ret == 0 || errno != ENOSYS)
				return (int)ret;
		} else {
			long ret;
			if (shim_guarded_syscall(__NR_close_range, (long)first, (long)last,
						 (long)flags, 0, 0, 0, &ret)) {
				if (ret == 0 || errno != ENOSYS)
					return (int)ret;
			} else {
				cr_probe_state = CR_PROBE_FALLBACK;
			}
		}
	}
	return cr_do_fallback(first, last, flags);
}

int close_range(unsigned int first, unsigned int last, unsigned int flags)
{
	/* Disabled means "don't intercept" — defer entirely to the real
	 * syscall, same as if this shim weren't loaded at all, including
	 * skipping our own EINVAL validation above. No probe, no fallback
	 * safety net: if the real call gets SIGSYS'd, so does the caller,
	 * exactly matching genuine no-shim behavior. */
	if (shim_disabled(SD_CLOSE_RANGE)) {
		return (int)cr_real_syscall(__NR_close_range, (long)first,
					    (long)last, (long)flags, 0, 0, 0);
	}
	if (cr_validate_args(first, last, flags) != 0)
		return -1;
	return cr_dispatch(first, last, flags);
}

/* Forward-declared: fc2_dispatch() is implemented in fchmodat2's own
 * section below (after syscall(), which needs to call it here). */
static int fc2_dispatch(int dirfd, const char *path, mode_t mode, int flags);

/* Forward-declared: ep_shim_ctl_done() lives in the epoll_pipe section
 * below — it runs the registry bookkeeping after a successful real
 * epoll_ctl(), no matter which entry symbol carried the call. */
static int ep_shim_ctl_done(int rc, int epfd, int op, int fd,
			    struct epoll_event *ev);

/* Forward-declared: shim_forget_fd() lives next to close() below — see its
 * own comment there. Needed here because bun's Rust event loop issues
 * close()/dup2()/dup3() as raw syscalls sometimes rather than the libc
 * symbols, same reasoning as ep_shim_ctl_done() above. */
static void shim_forget_fd(int fd);

/* syscall() override — dispatches the small set of raw-syscall-numbers
 * this file intercepts (currently close_range and fchmodat2 — see each
 * one's own section); every other number passes straight through to the
 * real (RTLD_NEXT) syscall() unmodified. */
long syscall(long number, ...)
{
	va_list ap;
	long a0, a1, a2, a3, a4, a5;

	va_start(ap, number);
	a0 = va_arg(ap, long);
	a1 = va_arg(ap, long);
	a2 = va_arg(ap, long);
	a3 = va_arg(ap, long);
	a4 = va_arg(ap, long);
	a5 = va_arg(ap, long);
	va_end(ap);

	if (number == __NR_close_range && !shim_disabled(SD_CLOSE_RANGE)) {
		unsigned int first = (unsigned int)a0;
		unsigned int last = (unsigned int)a1;
		unsigned int flags = (unsigned int)a2;
		if (cr_validate_args(first, last, flags) != 0)
			return -1;
		return cr_dispatch(first, last, flags);
	}

	if (number == __NR_fchmodat2 && !shim_disabled(SD_FCHMODAT2)) {
		return fc2_dispatch((int)a0, (const char *)a1, (mode_t)a2, (int)a3);
	}

	/* Bun's Rust event loop (src/sys/linux_syscall.rs) issues epoll_ctl
	 * as a raw syscall(SYS_epoll_ctl, ...) rather than the libc symbol,
	 * so the epoll_ctl() override below never sees those calls. Route
	 * them through the same registry bookkeeping here. cr_real_syscall
	 * is libc's syscall(): libc-convention return, errno set on -1. */
	if (number == __NR_epoll_ctl && !shim_disabled(SD_EPOLL_PIPE)) {
		return ep_shim_ctl_done(
			(int)cr_real_syscall(__NR_epoll_ctl, a0, a1, a2, a3, 0, 0),
			(int)a0, (int)a1, (int)a2, (struct epoll_event *)a3);
	}

	/* Same reasoning as epoll_ctl above: bun's Rust event loop closes and
	 * dup3()s fds via raw syscalls, not the close()/dup3() libc symbols
	 * below, so those overrides alone would miss it -- leaving stale
	 * epoll_pipe registry / fifo-cache entries for a recycled fd number.
	 * (No __NR_dup2 case: aarch64 has no dup2 syscall number at all --
	 * libc's dup2() is itself implemented as dup3(old, new, 0), which the
	 * dup3 case below already covers regardless of which symbol the
	 * caller used to get there.) */
	if (number == __NR_close) {
		shim_forget_fd((int)a0);
		return cr_real_syscall(__NR_close, a0, 0, 0, 0, 0, 0);
	}
	if (number == __NR_dup3) {
		shim_forget_fd((int)a1);
		return cr_real_syscall(__NR_dup3, a0, a1, a2, 0, 0, 0);
	}

	return cr_real_syscall(number, a0, a1, a2, a3, a4, a5);
}

/* ==================================================================== */
/*  1.5. fchmodat2() — syscall 452, chmod with AT_* flags (mainly         */
/*       AT_SYMLINK_NOFOLLOW). No libc wrapper exists for this in this   */
/*       musl (too new; same situation close_range was in) — callers use */
/*       syscall(SYS_fchmodat2, ...) directly, e.g. Bun's `add` command   */
/*       setting permissions. Confirmed on-device: raises an unhandled   */
/*       SIGSYS on real HarmonyOS hardware regardless of arguments, same */
/*       failure mode as close_range (not a graceful ENOSYS) — verified  */
/*       with a standalone guarded repro before writing this. Falls back */
/*       to the classic fchmodat(), forwarding AT_SYMLINK_NOFOLLOW —     */
/*       this musl honours it (measured below). Simpler than             */
/*       close_range: no evidence different flag values change the       */
/*       SIGSYS outcome here (there's no second underlying syscall like  */
/*       unshare() to independently trip), so one process-wide probe is  */
/*       trusted rather than re-guarding every call.                     */
/* ==================================================================== */

enum { FC2_PROBE_UNKNOWN, FC2_PROBE_WORKS, FC2_PROBE_FALLBACK };
static int fc2_probe_state = FC2_PROBE_UNKNOWN;

static void fc2_probe(int dirfd, const char *path)
{
	long ret;
	struct stat st;
	if (fstatat(dirfd, path, &st, 0) != 0) {
		fc2_probe_state = FC2_PROBE_FALLBACK;
		return;
	}
	if (shim_guarded_syscall(__NR_fchmodat2, dirfd, (long)path,
				 (long)(st.st_mode & 07777), 0, 0, 0, &ret) &&
	    ret == 0)
		fc2_probe_state = FC2_PROBE_WORKS;
	else
		fc2_probe_state = FC2_PROBE_FALLBACK;
}

static int fc2_dispatch(int dirfd, const char *path, mode_t mode, int flags)
{
	if (__builtin_expect(fc2_probe_state == FC2_PROBE_UNKNOWN, 0))
		fc2_probe(dirfd, path);

	if (fc2_probe_state == FC2_PROBE_WORKS) {
		long ret;
		if (shim_guarded_syscall(__NR_fchmodat2, dirfd, (long)path, (long)mode,
					 flags, 0, 0, &ret)) {
			if (ret == 0 || errno != ENOSYS)
				return (int)ret;
		} else {
			fc2_probe_state = FC2_PROBE_FALLBACK;
		}
	}

	/* Fallback: classic fchmodat(), forwarding AT_SYMLINK_NOFOLLOW.
	 *
	 * This used to drop the flag, on the assumption that classic fchmodat()
	 * could not honour it. Measured on this device, it can:
	 *
	 *   regular file + NOFOLLOW -> rc=0, mode applied
	 *   directory    + NOFOLLOW -> rc=0, mode applied
	 *   symlink      + NOFOLLOW -> rc=-1 ENOTSUP, target untouched
	 *
	 * which is exactly Linux's contract (a symlink's own mode bits are
	 * meaningless, so chmod on one is refused). Dropping the flag was
	 * therefore not just lossy but unsafe: it turned "do not follow this
	 * symlink" into "follow it", so a chmod aimed at a link landed on
	 * whatever the link pointed at. bun's installer relies on the refusal
	 * to avoid chmod-ing a file outside the package via a symlinked bin
	 * target (test/cli/install/symlink-path-traversal.test.ts).
	 *
	 * Other fchmodat2-only flags (AT_EMPTY_PATH) stay dropped: classic
	 * fchmodat() would reject them outright and no caller here uses them. */
	return fchmodat(dirfd, path, mode, flags & AT_SYMLINK_NOFOLLOW);
}

/* ==================================================================== */
/*  2. getpwuid_r() — HarmonyOS HAP uids (2002xxxx) aren't in /etc/passwd */
/*     Real impl returns ENOENT/0-with-NULL-result; synthesize a minimal */
/*     passwd record matching Node's os.userInfo() needs. When the query */
/*     is for the caller's own uid, prefer the real OS-account name from */
/*     libos_account_ndk.so (OH_OsAccount_GetName, API 12+), then env    */
/*     vars, then a uid-derived placeholder. Queries for any other uid   */
/*     pass the real ENOENT through — the synthesized record is only     */
/*     valid for the current account.                                    */
/* ==================================================================== */

#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX 256
#endif

typedef int (*getpwuid_r_fn)(uid_t, struct passwd *, char *, size_t, struct passwd **);

/* Lazily resolve OH_OsAccount_GetName; cache the handle and symbol for the
 * process lifetime (getpwuid_r() is called repeatedly, e.g. by Node's
 * os.userInfo()). Returns NULL when the library or symbol is unavailable,
 * so callers transparently fall back to env-var synthesis. */
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

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
	      struct passwd **result)
{
	static getpwuid_r_fn real = NULL;
	if (!real)
		real = (getpwuid_r_fn)dlsym(RTLD_NEXT, "getpwuid_r");

	if (!shim_disabled(SD_GETPWUID_R) && real) {
		int rc = real(uid, pwd, buf, buflen, result);
		if (rc == 0 && *result != NULL)
			return 0; /* real lookup succeeded, use it */
		if (rc != 0 && rc != ENOENT)
			return rc; /* a different real error, don't mask it */
	} else if (real) {
		return real(uid, pwd, buf, buflen, result);
	}

	if (shim_disabled(SD_GETPWUID_R)) {
		*result = NULL;
		return ENOENT;
	}

	/* Only synthesize for the caller's own uid — the fallback record IS the
	 * current OS account, so handing it out for an arbitrary uid would mask
	 * the ENOENT that callers rely on (Node's process.initgroups() numeric
	 * pre-resolve expects ERR_UNKNOWN_CREDENTIAL for unknown uids). */
	if (uid != getuid() && uid != geteuid()) {
		*result = NULL;
		return ENOENT;
	}

	/* Fallback: synthesize from the OS-account name (only valid for the
	 * caller's own uid — the account API has no uid parameter), then
	 * environment, mirroring ohos-preflight/solutions/i9_getpwuid_r.c.
	 * A set-but-empty var (e.g. `export LOGNAME=`) is treated the same
	 * as unset. */
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
		return ERANGE;
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
	return 0;
}

/* ==================================================================== */
/*  2b. getaddrinfo() — a hostname with characters outside the DNS/       */
/*     hostname alphabet is forwarded to the network instead of          */
/*     rejected locally, so a lookup that should fail instantly          */
/*     (EAI_NONAME) instead blocks for a full resolver timeout (several  */
/*     seconds, observed). Probed once per process in a background       */
/*     thread, never on the caller's path: if the real resolver already  */
/*     rejects a synthetic invalid hostname fast, this interceptor       */
/*     becomes a pure passthrough for the rest of the process's life.    */
/*     Until the probe resolves (or if it confirms the slow behavior),   */
/*     a local syntax pre-check matching glibc's accepted alphabet       */
/*     stands in. Disable via OHOS_COMPAT_SHIM_DISABLE=getaddrinfo       */
/*     (skips both the probe and the pre-check).                        */
/* ==================================================================== */

typedef int (*getaddrinfo_fn)(const char *, const char *,
			      const struct addrinfo *, struct addrinfo **);

static getaddrinfo_fn g_real_getaddrinfo;

static getaddrinfo_fn gai_real(void)
{
	if (!g_real_getaddrinfo)
		g_real_getaddrinfo = (getaddrinfo_fn)dlsym(RTLD_NEXT, "getaddrinfo");
	return g_real_getaddrinfo;
}

static int hostname_has_invalid_chars(const char *node)
{
	for (const unsigned char *p = (const unsigned char *)node; *p; p++) {
		unsigned char c = *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '-' ||
		    c == '_' || c == '%' || c == ':')
			continue;
		return 1;
	}
	return 0;
}

/* 0 = still checking (safe default: the local pre-check stays active);
 * 1 = this host's real resolver already rejects fast, confirmed by the
 * probe thread -- never reset back to 0 once set. */
static _Atomic int g_gai_native_rejects_fast = 0;
static _Atomic int g_gai_probe_launched = 0;

/* GAI_PROBE_SLOW_MS is deliberately generous: it only has to tell "rejected
 * locally, no network touched" (single-digit to double-digit ms, measured)
 * apart from "forwarded to the network" (multi-second timeout, measured) --
 * not chase a tight bound. */
#define GAI_PROBE_SLOW_MS 500

static void *gai_probe_thread(void *arg)
{
	(void)arg;
	struct addrinfo hints = { 0 };
	struct addrinfo *res = NULL;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int rc = gai_real()("bad host!", NULL, &hints, &res);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (rc == 0)
		freeaddrinfo(res);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		 (t1.tv_nsec - t0.tv_nsec) / 1000000;
	if (rc != 0 && ms < GAI_PROBE_SLOW_MS)
		atomic_store_explicit(&g_gai_native_rejects_fast, 1,
				      memory_order_release);
	return NULL;
}

/* Launched from the first getaddrinfo() call this process ever makes, not
 * from a library-load constructor: a constructor would race the host
 * program's own startup for no benefit, since nothing needs the answer
 * until the first real lookup. Detached and fire-and-forget -- the result
 * is read via the atomic above, never joined. */
static void gai_probe_launch_once(void)
{
	if (atomic_exchange_explicit(&g_gai_probe_launched, 1,
				     memory_order_acq_rel))
		return;
	pthread_t th;
	if (pthread_create(&th, NULL, gai_probe_thread, NULL) == 0)
		pthread_detach(th);
}

int getaddrinfo(const char *node, const char *service,
		const struct addrinfo *hints, struct addrinfo **res)
{
	getaddrinfo_fn real = gai_real();
	if (!real) {
		errno = ENOSYS;
		return EAI_SYSTEM;
	}
	if (shim_disabled(SD_GETADDRINFO))
		return real(node, service, hints, res);

	gai_probe_launch_once();
	if (!atomic_load_explicit(&g_gai_native_rejects_fast, memory_order_acquire) &&
	    node && *node && hostname_has_invalid_chars(node))
		return EAI_NONAME;

	return real(node, service, hints, res);
}

/* ==================================================================== */
/*  3. tmpfile() — P_tmpdir is unwritable in the HarmonyOS app sandbox;  */
/*     fall back to an unlinked file under $TMPDIR (or $HOME if unset). */
/* ==================================================================== */

typedef FILE *(*tmpfile_fn)(void);

FILE *tmpfile(void)
{
	static tmpfile_fn real = NULL;
	if (!real)
		real = (tmpfile_fn)dlsym(RTLD_NEXT, "tmpfile");

	if (!shim_disabled(SD_TMPFILE) && real) {
		FILE *f = real();
		if (f)
			return f;
		/* fall through to userspace fallback below */
	} else if (real) {
		return real();
	}

	if (shim_disabled(SD_TMPFILE))
		return NULL;

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
	unlink(tmpl); /* auto-cleanup on close/exit, matches tmpfile() semantics */

	FILE *f = fdopen(fd, "w+");
	if (!f) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return NULL;
	}
	return f;
}


/* ==================================================================== */
/*  5. linkat() — sandboxed target dirs return EPERM/EACCES for          */
/*     hardlinks; fall back to a byte copy. Semantically lossy (loses    */
/*     hardlink identity: the copy is a separate inode, not a second     */
/*     name for the same one). Disable via OHOS_COMPAT_SHIM_DISABLE=     */
/*     linkat if true hardlink semantics matter for your workload.       */
static int copy_fd_contents(int src_fd, int dst_fd)
{
	char buf[65536];
	ssize_t n;
	while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(dst_fd, buf + off, (size_t)(n - off));
			if (w < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			off += w;
		}
	}
	return (n < 0) ? -1 : 0;
}

/* Copy src_fd to newdirfd/newpath atomically: write a hidden sibling temp
 * file, then renameat() into place. The previous direct O_CREAT|O_EXCL +
 * copy made the destination visible at 0 bytes the moment it was created,
 * so a concurrent quick_exit (bun install aborting on a resolution error
 * while a worker thread is still flushing its npm manifest cache) left
 * permanently corrupt 0-byte cache entries ("manifest is invalid" on the
 * next load — bun-install-registry prereleases-* tests). With tmp+rename
 * the destination appears complete or not at all; a quick_exit mid-copy
 * only litters a hidden temp file.
 *
 * EEXIST semantics: real linkat fails if newpath exists. renameat would
 * silently replace it, so pre-check with fstatat; the tiny TOCTOU window
 * between check and rename is inherent to an emulation and harmless for
 * the cache-writer workloads this shim serves (they unlink+retry anyway). */
static int copy_fd_to_path_atomic(int src_fd, int newdirfd, const char *newpath,
				  mode_t mode)
{
	/* The temp file MUST live in newpath's own directory: renameat is
	 * same-filesystem only, and an absolute newpath with a bare temp name
	 * would put the temp in the process CWD (possibly another fs → EXDEV). */
	char tmp[PATH_MAX];
	const char *slash = strrchr(newpath, '/');
	size_t dirlen = slash ? (size_t)(slash - newpath) + 1 : 0;
	if (dirlen >= sizeof(tmp))
		return -1;
	int len = snprintf(tmp, sizeof(tmp), "%.*s.ohos-linkat-tmp.%d",
			   (int)dirlen, newpath, (int)getpid());
	if (len <= 0 || len >= (int)sizeof(tmp))
		return -1;

	int dst = -1;
	for (int i = 0; i < 100; i++) {
		char *p = tmp + len;
		if (i > 0)
			snprintf(p, sizeof(tmp) - len, ".%d", i);
		dst = openat(newdirfd, tmp,
			     O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, mode);
		if (dst >= 0)
			break;
		if (errno != EEXIST)
			return -1;
	}
	if (dst < 0)
		return -1;

	int copy_rc = copy_fd_contents(src_fd, dst);
	int e = errno;
	close(dst);
	if (copy_rc != 0) {
		unlinkat(newdirfd, tmp, 0);
		errno = e;
		return -1;
	}

	struct stat st;
	if (fstatat(newdirfd, newpath, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		unlinkat(newdirfd, tmp, 0);
		errno = EEXIST;
		return -1;
	}

	if (renameat(newdirfd, tmp, newdirfd, newpath) != 0) {
		e = errno;
		unlinkat(newdirfd, tmp, 0);
		errno = e;
		return -1;
	}
	return 0;
}

typedef int (*linkat_fn)(int, const char *, int, const char *, int);

/* ------------------------------------------------------------------ */
/*  splice(2): EOF on the source is reported as EPIPE instead of 0      */
/* ------------------------------------------------------------------ */
/*
 * This kernel returns -1/EPIPE from splice() when the *source* pipe is at
 * end-of-file, where Linux returns 0. read() on that same pipe correctly
 * returns 0, so only the splice path is affected. Every splice-based copy
 * loop therefore mistakes a normal EOF for a fatal error: GNU coreutils'
 * cat takes that path whenever stdin and stdout are both pipes and prints
 * "cat: -: Broken pipe" then exits 1. Measured with a standalone probe,
 * no Bun involved.
 *
 * A genuine EPIPE (the destination's read end is gone) must still be
 * reported. poll() separates the two cleanly and is non-destructive --
 * unlike read(), it cannot consume the byte we are asked to move:
 *
 *   source at EOF        poll(fd_in)=POLLIN|POLLHUP  poll(fd_out)=POLLOUT
 *   destination broken   poll(fd_in)=POLLIN          poll(fd_out)=POLLOUT|POLLERR
 *   both at once         poll(fd_out)=POLLOUT|POLLERR
 *
 * So POLLERR on the destination decides it. Checking the destination FIRST
 * makes the ambiguous "both" case resolve to the real error, which is the
 * conservative direction: reporting a broken pipe that also happens to be
 * at EOF loses nothing, whereas swallowing a real EPIPE would silently
 * truncate a copy.
 *
 * Only reached when splice() has already failed with EPIPE; every other
 * outcome is passed through untouched, so the cost on the normal path is
 * one comparison. Note POLLIN is set on an EOF pipe too (a read would
 * return 0 immediately), which is why POLLHUP -- not the absence of
 * POLLIN -- is the EOF signal.
 */
typedef ssize_t (*splice_fn)(int, off_t *, int, off_t *, size_t, unsigned int);

/* ------------------------------------------------------------------ */
/*  splice(2), second defect: writing to a pipe wakes no poll()/epoll   */
/* ------------------------------------------------------------------ */
/*
 * Bytes that splice() places into a pipe never wake a poll() or
 * epoll_wait() that is already blocked on that pipe's read end. The data
 * really is there -- a later poll, or a read(), sees it immediately -- so
 * the pipe's readiness *state* is right and only the *wakeup* is missing.
 * A reader blocked in read() is woken correctly, which is why the defect
 * hides behind anything that reads synchronously.
 *
 * It deadlocks any pipeline whose consumer polls: GNU cat feeds stdout
 * with splice() when it is a pipe, so `cat big | bun script.js` hangs
 * forever -- Bun sits in epoll_wait, cat fills the pipe and then blocks
 * in splice() too. `cat big | wc -c` is fine (wc blocks in read), and
 * `dd ... | bun script.js` is fine (dd uses write).
 *
 * Fix: when the destination is a pipe, move the bytes through a userspace
 * buffer so the pipe is fed by write(), whose wakeup works. That costs one
 * copy and gives up splice's zero-copy property on this path, which is the
 * right trade for a shim whose job is correctness.
 *
 * Holding the last byte back and sending only that one with write() would
 * have kept the zero-copy bulk transfer, and a probe confirmed it wakes the
 * poller -- but it does not survive the real case: a full-pipe splice
 * returns short, and the trailing write() is never reached. A wakeup
 * scheme that fails exactly when the pipe is full is no use, since that is
 * when the reader is guaranteed to be waiting.
 */
#ifndef SPLICE_F_NONBLOCK
#define SPLICE_F_NONBLOCK 2
#endif

static int splice_fd_is_fifo(int fd)
{
	struct stat st;
	return fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
}

/*
 * Cache splice_fd_is_fifo()'s result per fd, direct-mapped by fd number so a
 * hit is a single atomic load, no lock: a slot holding a *different* fd (or
 * an empty slot) is simply treated as a cache miss and falls through to the
 * real fstat() -- a wrong/stale hit is structurally impossible, only a
 * missed opportunity to skip the syscall. Correctness rests entirely on
 * invalidating a slot whenever its fd number can start meaning a different
 * file, which is why every place that can do that -- close(fd),
 * dup2/dup3(_, newfd), and the raw-syscall paths bun's Rust event loop uses
 * instead of those libc symbols -- calls shim_forget_fd() below before the
 * fd changes meaning.
 *
 * Slot encoding: 0 = empty. A live entry is ((fd+1) << 1) | is_fifo, so
 * fd 0 (often stdin) is representable and distinct from "empty".
 */
#define FIFO_CACHE_SIZE 256
static _Atomic uint32_t g_fifo_cache[FIFO_CACHE_SIZE];

static int splice_fd_is_fifo_cached(int fd)
{
	if (fd < 0)
		return 0;
	unsigned idx = (unsigned)fd % FIFO_CACHE_SIZE;
	uint32_t slot = atomic_load_explicit(&g_fifo_cache[idx], memory_order_relaxed);
	if (slot != 0 && (int)(slot >> 1) - 1 == fd)
		return (int)(slot & 1);
	int is_fifo = splice_fd_is_fifo(fd);
	uint32_t encoded = (((uint32_t)fd + 1) << 1) | (is_fifo ? 1u : 0u);
	atomic_store_explicit(&g_fifo_cache[idx], encoded, memory_order_relaxed);
	return is_fifo;
}

static void fifo_cache_forget_fd(int fd)
{
	if (fd < 0)
		return;
	unsigned idx = (unsigned)fd % FIFO_CACHE_SIZE;
	uint32_t slot = atomic_load_explicit(&g_fifo_cache[idx], memory_order_relaxed);
	if (slot != 0 && (int)(slot >> 1) - 1 == fd)
		atomic_store_explicit(&g_fifo_cache[idx], 0, memory_order_relaxed);
}

/*
 * ONESHOT-enforcement eligibility beyond FIFOs: PTY/tty write ends. The same
 * kernel defect family ignores EPOLLONESHOT auto-disarm there too -- measured
 * 2026-09-27 on an interactive claude-code (musl) session: stdout/stderr are
 * /dev/pts entries (S_ISCHR, invisible to S_ISFIFO), registered 0x4000001c, and the
 * loop spun at ~240k immediate epoll_pwait returns/s, 99.98% pure EPOLLOUT
 * (7.0M OUT vs 1.3K IN over ~10s; main thread 129% CPU + scavenger 56%).
 * Detection is TCGETS (exactly isatty(3)'s own probe): PTY master and slave
 * both answer it; /dev/null, /dev/urandom, eventfd/signalfd/timerfd
 * (anon_inode) and sockets all fail it, so none become eligible.
 */
static int splice_fd_is_tty(int fd)
{
	struct termios tio;
	return ioctl(fd, TCGETS, &tio) == 0;
}

static _Atomic uint32_t g_tty_cache[FIFO_CACHE_SIZE];

static int splice_fd_is_tty_cached(int fd)
{
	if (fd < 0)
		return 0;
	unsigned idx = (unsigned)fd % FIFO_CACHE_SIZE;
	uint32_t slot = atomic_load_explicit(&g_tty_cache[idx], memory_order_relaxed);
	if (slot != 0 && (int)(slot >> 1) - 1 == fd)
		return (int)(slot & 1);
	int is_tty = splice_fd_is_tty(fd);
	uint32_t encoded = (((uint32_t)fd + 1) << 1) | (is_tty ? 1u : 0u);
	atomic_store_explicit(&g_tty_cache[idx], encoded, memory_order_relaxed);
	return is_tty;
}

static void tty_cache_forget_fd(int fd)
{
	if (fd < 0)
		return;
	unsigned idx = (unsigned)fd % FIFO_CACHE_SIZE;
	uint32_t slot = atomic_load_explicit(&g_tty_cache[idx], memory_order_relaxed);
	if (slot != 0 && (int)(slot >> 1) - 1 == fd)
		atomic_store_explicit(&g_tty_cache[idx], 0, memory_order_relaxed);
}

/*
 * One bounded chunk, source -> userspace -> destination. Returning less than
 * `len` is allowed by splice(2) and every splice-based copy loop already
 * handles it, so a 64KB ceiling costs nothing but bounds the stack.
 */
static ssize_t splice_through_buffer(int fd_in, off_t *off_in, int fd_out,
				     size_t len, unsigned int flags)
{
	char buf[65536];
	if (len > sizeof buf)
		len = sizeof buf;

	/* SPLICE_F_NONBLOCK promises not to block on the source; read() alone
	   would, unless fd_in itself is O_NONBLOCK. POLLHUP-without-POLLIN
	   still reports ready, so EOF keeps flowing through to the read(). */
	if (flags & SPLICE_F_NONBLOCK) {
		struct pollfd pfd = { .fd = fd_in, .events = POLLIN };
		if (poll(&pfd, 1, 0) <= 0) {
			errno = EAGAIN;
			return -1;
		}
	}

	ssize_t got = off_in ? pread(fd_in, buf, len, *off_in)
			     : read(fd_in, buf, len);
	if (got <= 0)
		return got;	/* 0 is EOF -- the answer splice() should give */

	/* These bytes are already out of the source, so a short write cannot
	   be reported back as "not consumed" the way splice() would: it has to
	   be retried or the data is gone. That means a SPLICE_F_NONBLOCK caller
	   can still block here on a full destination. Losing bytes is the worse
	   failure, and the alternative (peek at pipe space first) has no
	   portable form. */
	size_t done = 0;
	while (done < (size_t)got) {
		ssize_t w = write(fd_out, buf + done, (size_t)got - done);
		if (w > 0) {
			done += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pfd = { .fd = fd_out, .events = POLLOUT };
			if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
				break;
			continue;
		}
		break;		/* EPIPE and friends: report what did land */
	}
	if (done == 0)
		return -1;	/* errno still set by write() */
	if (off_in)
		*off_in += (off_t)done;
	return (ssize_t)done;
}

ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
	       size_t len, unsigned int flags)
{
	static splice_fn real = NULL;
	if (!real)
		real = (splice_fn)dlsym(RTLD_NEXT, "splice");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}

	/* off_out must be NULL when the destination is a pipe (kernel rule);
	   testing it as well keeps this path off any call the kernel would
	   have rejected anyway. */
	if (len > 0 && off_out == NULL && !shim_disabled(SD_SPLICE) &&
	    splice_fd_is_fifo_cached(fd_out))
		return splice_through_buffer(fd_in, off_in, fd_out, len, flags);

	ssize_t rc = real(fd_in, off_in, fd_out, off_out, len, flags);
	if (rc >= 0 || errno != EPIPE)
		return rc;
	if (shim_disabled(SD_SPLICE))
		return rc;

	int saved = errno;

	/* Destination genuinely broken? Then EPIPE is the correct answer. */
	struct pollfd out_pfd = { .fd = fd_out, .events = POLLOUT };
	if (poll(&out_pfd, 1, 0) > 0 && (out_pfd.revents & (POLLERR | POLLNVAL))) {
		errno = saved;
		return rc;
	}

	/* Source hung up with nothing left to move: that is a plain EOF. */
	struct pollfd in_pfd = { .fd = fd_in, .events = POLLIN };
	if (poll(&in_pfd, 1, 0) > 0 && (in_pfd.revents & POLLHUP))
		return 0;

	errno = saved;
	return rc;
}

/* ==================================================================== */
/*  epoll_pipe: EPOLLONESHOT is not honored for EPOLLOUT registrations   */
/* ==================================================================== */
/*
 * This kernel ignores EPOLLONESHOT's auto-disarm for a FIFO write end or a
 * PTY/TTY fd registered with EPOLLOUT|EPOLLONESHOT (detected via TCGETS,
 * the same probe isatty(3) uses): after the first delivery, which should be
 * the only one until an explicit re-arm, the kernel keeps re-delivering the
 * same event on every subsequent epoll_wait/epoll_pwait -- observed at
 * several hundred thousand pure-EPOLLOUT returns per second on an idle
 * interactive session, pinning a core. Detection is TCGETS for TTYs and
 * S_ISFIFO for pipes; sockets, eventfd/signalfd/timerfd, and other anon
 * inodes are never eligible.
 *
 * Repair: the first delivery for a tracked entry passes through untouched
 * (matches a compliant kernel) and marks the entry disarmed; every
 * subsequent delivery for that entry is stripped from the returned event
 * array (see ep_shim_after_wait_ex()) and the entry is also removed
 * kernel-side (EPOLL_CTL_DEL) so the kernel stops re-delivering it, not
 * just this shim. A later ADD/MOD from the application re-arms it (see
 * ep_shim_ctl_done()'s ENOENT translation, for a kernel where DEL on this
 * exact registration itself returns ENOENT despite still listing the fd in
 * /proc/self/fdinfo -- del_tried caps that one failing attempt to once per
 * arm instead of retrying it on every re-fire).
 *
 * A second, narrower case: some EPOLLOUT|EPOLLONESHOT registrations are
 * armed through a path this shim never sees at all (e.g. Bun's Rust event
 * loop issuing epoll_ctl via a raw inlined syscall), so no registry entry
 * ever exists for them. Those are caught by pattern instead of by
 * registration: a pure-EPOLLOUT event recurring for the same (epfd, data)
 * key with gaps under EP_STORM_GAP_MS, EP_STORM_STREAK times in a row, is
 * treated as the same defect and stripped/CTL_DEL'd the same way.
 *
 * Either way, every strip requests a pacing sleep before the next re-wait
 * (see ep_shim_wait()) so a kernel that keeps re-delivering even after the
 * CTL_DEL cannot turn this repair into the same spin it replaces.
 * OHOS_COMPAT_SHIM_DISABLE=epoll_pipe turns the whole interceptor off.
 */

#define EP_REG_MAX 64

typedef struct {
	int used;
	int epfd;
	int fd;
	struct epoll_event ev;	/* registration mask + udata */
	int disarmed;		/* ONESHOT: first delivery already passed through */
	int kdel;		/* we removed this entry kernel-side (ONESHOT
				 * enforcement); re-arm must reach the kernel
				 * as ADD, and an app DEL must not fail */
	int del_tried;		/* ONESHOT enforcement already attempted a
				 * kernel-side DEL for the current arm (see
				 * the 2026-09-28 note on ep_shim_after_wait_ex
				 * below: on some HarmonyOS kernels DEL on this
				 * exact, still-registered tfd returns ENOENT
				 * every time -- retrying it on every re-fire
				 * just adds a failing syscall to what's
				 * already a busy-loop). One attempt per arm;
				 * cleared on the next ADD/MOD so a genuinely
				 * re-armed entry gets a fresh try. */
	int ghost_streak;	/* consecutive ghost re-fires stripped since
				 * this arm (see EP_GHOST_SLEEP_MAX_NS above);
				 * grows the caller's pacing sleep, resets on
				 * the next ADD/MOD */
} ep_pipe_reg_t;

static ep_pipe_reg_t g_ep_pipes[EP_REG_MAX];
static pthread_mutex_t g_ep_pipes_lock = PTHREAD_MUTEX_INITIALIZER;

/* ONESHOT-enforcement eligibility: FIFO registrations that ask for
 * EPOLLOUT under EPOLLONESHOT -- Bun's PosixPipeWriter shape. This kernel
 * ignores EPOLLONESHOT auto-disarm for such entries and re-delivers
 * EPOLLOUT on every wait (measured 2026-09-27: ~510k immediate
 * epoll_pwait returns/s, 100% pure EPOLLOUT, on the official
 * claude-code linux-arm64-musl binary; see
 * logs/2026-09-27-claude-code-musl-idle-spin.md). EPOLLIN-bearing
 * registrations are deliberately NOT enforcement-eligible: they are the
 * existing synthesis repair's domain and no re-fire storm was ever
 * observed in that direction. */
static int ep_reg_oneshot_watch(const ep_pipe_reg_t *r)
{
	return (r->ev.events & (EPOLLOUT | EPOLLONESHOT)) ==
	       (EPOLLOUT | EPOLLONESHOT);
}

/* Count of currently-registered enforcement-eligible entries (atomic so
 * the wait-side fast path can skip the lock entirely when zero -- the
 * wait path never needs the exact value, only ever "are there any").
 * Maintained by recount under g_ep_pipes_lock on every registry mutation. */
static _Atomic int g_ep_oneshot_watch_count;

static void ep_oneshot_recount_locked(void)
{
	int i, n = 0;
	for (i = 0; i < EP_REG_MAX; i++)
		if (g_ep_pipes[i].used && ep_reg_oneshot_watch(&g_ep_pipes[i]))
			n++;
	atomic_store_explicit(&g_ep_oneshot_watch_count, n,
			      memory_order_release);
}

static long long ep_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ==================================================================== */
/*  Unknown-key EPOLLOUT storm suppression                                */
/*                                                                        */
/*  The ONESHOT enforcement above keys on registrations the shim SAW --   */
/*  but some arms never reach any interceptable path: bun's Rust code     */
/*  issues epoll_ctl through rustix's linux_raw inline svc, which never   */
/*  touches a libc symbol, so the registry never learns the entry.        */
/*  Measured 2026-09-28 on an interactive claude-code (musl) TUI          */
/*  session: an eventfd registered ONESHOT|EPOLLOUT (mask 0x4000001c,     */
/*  visible in fdinfo) that no interceptable epoll_ctl ever armed,        */
/*  re-fired a pure ev=0x4 event on EVERY epoll_pwait -- one pinned core  */
/*  plus mimalloc's scavenger ~50% downstream. Same defect family as      */
/*  ohos-bun 408a29c0b4's on-device trace ("kernel keeps delivering       */
/*  ready events for that fd indefinitely, even after CTL_DEL reports     */
/*  success"): ONESHOT discipline on this kernel is flaky over time, and  */
/*  for the TTY/writer shape userspace cannot always make it stop.        */
/*                                                                        */
/*  Detection mirrors ohos-bun's accepted signature: the same (epfd,      */
/*  data) pure-EPOLLOUT event arriving with <=10ms gaps, 20+ times in a   */
/*  row, is a storm; legitimate drain-then-idle cycles are ms-to-seconds  */
/*  apart, and the FIRST event after a >10ms gap always passes (streak    */
/*  resets to 1 -- ONESHOT's one-delivery-per-arm stays honored for       */
/*  genuinely re-armed entries). On storm onset the shim (a) reverse-maps */
/*  data -> fd via /proc/self/fdinfo/<epfd> and CTL_DELs it kernel-side   */
/*  (stops the still-registered case cold on today's kernel), and (b) if  */
/*  the kernel keeps delivering anyway (the traced ghost-delivery case),  */
/*  strips the event and paces the re-wait loop with a 2ms sleep per      */
/*  stripped iteration -- bounded latency, only ever paid mid-storm.      */
/* ==================================================================== */

#define EP_STORM_MAX 16
#define EP_STORM_GAP_MS 10
#define EP_STORM_STREAK 20
#define EP_STORM_SLEEP_NS 2000000LL

/* Exponential pacing cap for the ONESHOT-enforcement matched-entry ghost
 * path (see the 2026-09-28 note on ep_shim_after_wait_ex): unlike the
 * unknown-key storm above, this ghost can be a PERMANENT property of one
 * registration for the rest of its arm's lifetime -- on the affected
 * kernel, DEL for the exact, still-registered tfd returns ENOENT every
 * time, so nothing ever silences it kernel-side. Paying a flat 2ms forever
 * for a storm that never ends measured ~5.5% steady-state CPU on an
 * interactive claude-code (musl) TUI session; growing the pace while it
 * stays uninterrupted (reset on the next real ADD/MOD arm) brought that
 * down further without adding perceptible input latency (32ms is well
 * under normal human-perceived keystroke-to-echo budgets). */
#define EP_GHOST_SLEEP_MAX_NS 32000000LL
#define EP_GHOST_STREAK_CAP 4 /* 2ms << 4 == 32ms == the cap above */

typedef struct {
	int used;
	int epfd;
	uint64_t data;
	int streak;
	long long last_ms;
	int storm;
	int del_tried;
} ep_storm_t;

static ep_storm_t g_ep_storm[EP_STORM_MAX];

/* Pure-EPOLLOUT events are the storm's shape: OUT and nothing else. */
static int ev_is_pure_epollout(uint32_t e)
{
	return (e & EPOLLOUT) &&
	       !(e & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP));
}

/* Reverse-map (epfd, data) -> registered fd by parsing /proc/self/fdinfo.
 * Returns the fd or -1. Called at storm onset only (del_tried gates), so
 * the procfs read cost is one-shot per storm key. */
static int ep_storm_find_fd(int epfd, uint64_t data)
{
	char path[64];
	snprintf(path, sizeof path, "/proc/self/fdinfo/%d", epfd);
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	int found = -1;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		int tfd;
		unsigned ev;
		unsigned long long d;
		if (sscanf(line, "tfd: %d events: %x data: %llx",
			   &tfd, &ev, &d) == 3 && d == data) {
			found = tfd;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Observe one pure-EPOLLOUT event that matched no registry entry; sets
 * *suppress when this key is in storm mode. The table update runs under
 * g_ep_pipes_lock; the one-shot fdinfo scan + CTL_DEL run outside it. */
static void ep_storm_observe(int epfd, uint64_t data, int *suppress)
{
	long long now = ep_now_ms();
	int i, slot = -1, oldest = -1;
	long long oldest_ms = 0;

	*suppress = 0;
	pthread_mutex_lock(&g_ep_pipes_lock);
	for (i = 0; i < EP_STORM_MAX; i++) {
		if (!g_ep_storm[i].used) {
			if (slot < 0)
				slot = i;
			continue;
		}
		if (g_ep_storm[i].epfd == epfd && g_ep_storm[i].data == data)
			break;
	}
	if (i >= EP_STORM_MAX && slot >= 0) {
		i = slot;
	} else if (i >= EP_STORM_MAX) {
		/* full: recycle the quietest slot */
		for (i = 0; i < EP_STORM_MAX; i++) {
			if (!g_ep_storm[i].used)
				continue;
			if (oldest < 0 || g_ep_storm[i].last_ms < oldest_ms) {
				oldest = i;
				oldest_ms = g_ep_storm[i].last_ms;
			}
		}
		i = oldest;
	}
	if (i >= 0 && i < EP_STORM_MAX) {
		if (!g_ep_storm[i].used || g_ep_storm[i].epfd != epfd ||
		    g_ep_storm[i].data != data) {
			memset(&g_ep_storm[i], 0, sizeof g_ep_storm[i]);
			g_ep_storm[i].used = 1;
			g_ep_storm[i].epfd = epfd;
			g_ep_storm[i].data = data;
		}
		ep_storm_t *s = &g_ep_storm[i];
		s->streak = (now - s->last_ms <= EP_STORM_GAP_MS) ?
				    s->streak + 1 :
				    1;
		s->last_ms = now;
		if (s->streak >= EP_STORM_STREAK)
			s->storm = 1; /* sticky until a >gap quiet period */
		*suppress = s->storm;
	}
	int try_del = *suppress && !g_ep_storm[i].del_tried;
	if (try_del)
		g_ep_storm[i].del_tried = 1;
	pthread_mutex_unlock(&g_ep_pipes_lock);

	if (try_del) {
		int fd = ep_storm_find_fd(epfd, data);
		if (fd >= 0) {
			static int (*real_ctl)(int, int, int,
					       struct epoll_event *) = NULL;
			if (!real_ctl)
				real_ctl = (int (*)(int, int, int,
						    struct epoll_event *))dlsym(
					RTLD_NEXT, "epoll_ctl");
			if (real_ctl)
				real_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
		}
	}
}


/* fork() only carries the calling thread into the child; if some *other*
 * thread held g_ep_pipes_lock at the instant of fork(), it stays locked
 * forever in the child (its owner doesn't exist there to unlock it) --
 * every close()/epoll_ctl()/epoll_wait() the child ever makes would then
 * deadlock on its very first call, since they all touch this lock via
 * shim_forget_fd()/ep_shim_ctl_done()/ep_shim_wait(). pthread_atfork's
 * child callback runs in the child immediately post-fork, before any other
 * shim entry point can run there, so reinitializing unconditionally here is
 * safe even though the mutex was never destroyed -- POSIX doesn't strictly
 * sanction re-init of a non-destroyed mutex, but this is the standard,
 * widely-used pattern for exactly this problem (glibc's own malloc arena
 * locks do the same via their fork handlers). Registrations don't survive
 * fork meaningfully anyway (the child's epfds/pipe fds are the same numbers
 * but a fresh execve is coming right behind close_range's cleanup in the
 * common case), so clearing the table alongside the lock is correct, not
 * just convenient. */
static void cr_atfork_child(void)
{
	pthread_mutex_init(&g_ep_pipes_lock, NULL);
	memset(g_ep_pipes, 0, sizeof(g_ep_pipes));
	atomic_store_explicit(&g_ep_oneshot_watch_count, 0,
			      memory_order_release);
	/* cr_sigsys_lock (shim_guarded_syscall's refcounted SIGSYS handler
	 * install/restore, above) has the identical fork hazard -- reset the
	 * same way. cr_sigsys_refcount resetting to 0 is safe: the forking
	 * thread is never itself inside a guarded window across a fork() call
	 * (nothing in this file forks from inside shim_guarded_syscall), and
	 * no other thread survives the fork to have been counted. */
	pthread_mutex_init(&cr_sigsys_lock, NULL);
	cr_sigsys_refcount = 0;
	memset(g_ep_storm, 0, sizeof(g_ep_storm));
}

__attribute__((constructor))
static void cr_atfork_register(void)
{
	pthread_atfork(NULL, NULL, cr_atfork_child);
}

static int ep_pipe_active(void)
{
	return !shim_disabled(SD_EPOLL_PIPE);
}

/* All registry mutations happen under g_ep_pipes_lock; Bun runs one
 * epoll instance per event-loop thread, so epoll_ctl for different epfds
 * really can arrive concurrently. */

static void ep_reg_update_locked(int epfd, int fd, const struct epoll_event *ev)
{
	int free_slot = -1, i;
	for (i = 0; i < EP_REG_MAX; i++) {
		if (!g_ep_pipes[i].used) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (g_ep_pipes[i].epfd == epfd && g_ep_pipes[i].fd == fd) {
			g_ep_pipes[i].ev = *ev;
			g_ep_pipes[i].disarmed = 0;
			/* kdel deliberately cleared: an ADD/MOD reaching the
			 * registry bookkeeping means the app (re)armed this
			 * entry, so any kernel-side removal we did for ONESHOT
			 * enforcement is superseded (the caller translated the
			 * op to reach the kernel correctly; see
			 * ep_shim_ctl_done). */
			g_ep_pipes[i].kdel = 0;
			/* del_tried cleared alongside kdel: a fresh arm
			 * deserves a fresh DEL attempt if it later ghost-
			 * refires, same reasoning as kdel above. */
			g_ep_pipes[i].del_tried = 0;
			g_ep_pipes[i].ghost_streak = 0;
			ep_oneshot_recount_locked();
			return;
		}
	}
	if (free_slot < 0) {
		return;	/* full: this pipe just never gets repaired */
	}
	g_ep_pipes[free_slot].used = 1;
	g_ep_pipes[free_slot].epfd = epfd;
	g_ep_pipes[free_slot].fd = fd;
	g_ep_pipes[free_slot].ev = *ev;
	g_ep_pipes[free_slot].disarmed = 0;
	g_ep_pipes[free_slot].kdel = 0;
	g_ep_pipes[free_slot].del_tried = 0;
	g_ep_pipes[free_slot].ghost_streak = 0;
	ep_oneshot_recount_locked();
}

static void ep_reg_del_locked(int epfd, int fd)
{
	int i;
	for (i = 0; i < EP_REG_MAX; i++)
		if (g_ep_pipes[i].used && g_ep_pipes[i].epfd == epfd &&
		    g_ep_pipes[i].fd == fd)
			g_ep_pipes[i].used = 0;
	ep_oneshot_recount_locked();
}

static void ep_reg_forget_fd_locked(int fd)
{
	int i;
	for (i = 0; i < EP_REG_MAX; i++)
		if (g_ep_pipes[i].used &&
		    (g_ep_pipes[i].fd == fd || g_ep_pipes[i].epfd == fd))
			g_ep_pipes[i].used = 0;
	ep_oneshot_recount_locked();
}

/* Registry bookkeeping after a real epoll_ctl(), shared by the libc
 * symbol override and the syscall(SYS_epoll_ctl) dispatcher. */
static int ep_shim_ctl_done(int rc, int epfd, int op, int fd,
			    struct epoll_event *ev)
{
	static int (*real_ctl)(int, int, int, struct epoll_event *) = NULL;
	if (!real_ctl)
		real_ctl = (int (*)(int, int, int,
				    struct epoll_event *))dlsym(
			RTLD_NEXT, "epoll_ctl");
	if (!ep_pipe_active())
		return rc;

	/* ONESHOT-enforcement masking: entries we removed kernel-side
	 * (kdel) still exist as far as the app knows, so its re-arm MOD
	 * would hit ENOENT -- translate it to ADD -- and its DEL would
	 * likewise fail -- suppress to success. Foreign ENOENTs (entries
	 * we never touched) pass through untouched. */
	if (rc != 0 && errno == ENOENT) {
		int i, ours = 0;
		pthread_mutex_lock(&g_ep_pipes_lock);
		for (i = 0; i < EP_REG_MAX && !ours; i++)
			if (g_ep_pipes[i].used && g_ep_pipes[i].epfd == epfd &&
			    g_ep_pipes[i].fd == fd && g_ep_pipes[i].kdel)
				ours = 1;
		if (ours) {
			if (op == EPOLL_CTL_DEL) {
				ep_reg_del_locked(epfd, fd);
				rc = 0;
			} else if (op == EPOLL_CTL_MOD && ev && real_ctl) {
				rc = real_ctl(epfd, EPOLL_CTL_ADD, fd, ev);
				if (rc == 0)
					ep_reg_update_locked(epfd, fd, ev);
			}
		}
		pthread_mutex_unlock(&g_ep_pipes_lock);
		return rc;
	}

	if (rc != 0)
		return rc;
	pthread_mutex_lock(&g_ep_pipes_lock);
	if (op == EPOLL_CTL_DEL) {
		ep_reg_del_locked(epfd, fd);
	} else if (ev &&
		   ((ev->events & (EPOLLOUT | EPOLLONESHOT)) ==
		    (EPOLLOUT | EPOLLONESHOT)) &&
		   (splice_fd_is_fifo_cached(fd) || splice_fd_is_tty_cached(fd))) {
		/* ONESHOT-enforcement-eligible: a FIFO write end or TTY/PTY
		 * fd armed with EPOLLOUT|EPOLLONESHOT. */
		ep_reg_update_locked(epfd, fd, ev);
	} else {
		/* No repair interest: make sure a recycled fd number doesn't
		 * stay tracked from a previous registration. */
		ep_reg_del_locked(epfd, fd);
	}
	pthread_mutex_unlock(&g_ep_pipes_lock);
	return rc;
}

typedef int (*epoll_ctl_fn)(int, int, int, struct epoll_event *);

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	static epoll_ctl_fn real = NULL;
	if (!real)
		real = (epoll_ctl_fn)dlsym(RTLD_NEXT, "epoll_ctl");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	return ep_shim_ctl_done(real(epfd, op, fd, event), epfd, op, fd, event);
}

/* Strips ONESHOT-enforcement re-fires (both the registered-entry case and
 * the unknown-key storm case) from a real wait's results, requesting a
 * pacing sleep via *out_sleep_ns whenever it strips at least one event --
 * see the section comment above and ep_shim_wait() below for how the sleep
 * is used. Zero cost when no eligible entry is registered and no storm key
 * is active (the atomic count gates the lock for the first case; the
 * pure-EPOLLOUT shape check is cheap for the second). */
static int ep_shim_after_wait_ex(int rc, int epfd, struct epoll_event *events,
				 int maxevents, long long *out_sleep_ns)
{
	int i, j;
	*out_sleep_ns = 0;
	if (maxevents <= 0 || !ep_pipe_active() || rc <= 0)
		return rc;

	{
		static int (*real_ctl)(int, int, int,
				       struct epoll_event *) = NULL;
		if (!real_ctl)
			real_ctl = (int (*)(int, int, int,
					    struct epoll_event *))dlsym(
				RTLD_NEXT, "epoll_ctl");
		unsigned char *matched = NULL;
		if (atomic_load_explicit(&g_ep_oneshot_watch_count,
					 memory_order_acquire) > 0) {
			pthread_mutex_lock(&g_ep_pipes_lock);
			/* matched[] needs one byte per returned event; rc is
			 * small in practice (the storm itself returns 1-2).
			 * malloc-free cap: skip matching entirely for
			 * absurdly wide returns. */
			if (rc <= 256) {
				matched = alloca((size_t)rc);
				memset(matched, 0, (size_t)rc);
			}
			for (j = 0; j < rc; j++) {
				for (i = 0; i < EP_REG_MAX; i++) {
					ep_pipe_reg_t *r = &g_ep_pipes[i];
					if (!r->used || r->epfd != epfd ||
					    !ep_reg_oneshot_watch(r) ||
					    r->ev.data.u64 !=
						    events[j].data.u64)
						continue;
					if (matched)
						matched[j] = 1;
					if (!r->disarmed) {
						r->disarmed = 1;
					} else {
						if (!r->kdel && !r->del_tried &&
						    real_ctl) {
							r->del_tried = 1;
							if (real_ctl(epfd,
								     EPOLL_CTL_DEL,
								     r->fd,
								     NULL) == 0) {
								r->kdel = 1;
							}
							/* A failure here (this
							 * kernel can return
							 * ENOENT for a tfd
							 * fdinfo still lists)
							 * is not retried --
							 * del_tried stays set
							 * until the next
							 * ADD/MOD -- but the
							 * event is still a
							 * ghost re-fire either
							 * way, so it's still
							 * stripped and still
							 * paced below. */
						}
						memmove(&events[j],
							&events[j + 1],
							(size_t)(rc - j - 1) *
								sizeof(events[0]));
						rc--;
						j--;
						{
							int shift = r->ghost_streak;
							if (shift > EP_GHOST_STREAK_CAP)
								shift = EP_GHOST_STREAK_CAP;
							long long want =
								EP_STORM_SLEEP_NS
								<< shift;
							if (want > EP_GHOST_SLEEP_MAX_NS)
								want = EP_GHOST_SLEEP_MAX_NS;
							if (want > *out_sleep_ns)
								*out_sleep_ns = want;
							if (r->ghost_streak <
							    EP_GHOST_STREAK_CAP)
								r->ghost_streak++;
						}
					}
					break;
				}
			}
			pthread_mutex_unlock(&g_ep_pipes_lock);
		}

		/* Unknown-key EPOLLOUT storm pass (see the storm block below
		 * ep_now_ms): pure-EPOLLOUT events that no interceptable
		 * epoll_ctl ever armed -- bun registers some via rustix's
		 * inline-svc path, invisible to LD_PRELOAD. Observing them
		 * here is the only hook the shim has. */
		for (j = 0; j < rc; j++) {
			if (matched && matched[j])
				continue;
			if (!ev_is_pure_epollout(events[j].events))
				continue;
			int suppress = 0;
			ep_storm_observe(epfd, events[j].data.u64,
					 &suppress);
			if (suppress) {
				memmove(&events[j], &events[j + 1],
					(size_t)(rc - j - 1) *
						sizeof(events[0]));
				rc--;
				j--;
				if (EP_STORM_SLEEP_NS > *out_sleep_ns)
					*out_sleep_ns = EP_STORM_SLEEP_NS;
			}
		}
	}

	return rc;
}

typedef int (*epoll_pwait_fn)(int, struct epoll_event *, int, int,
			      const sigset_t *);

/* Calls the real wait once with the caller's own timeout, unmodified --
 * there is no periodic slicing to hide here, unlike the missed-wakeup
 * repair this file used to also carry. If ep_shim_after_wait_ex() strips
 * every event down to 0, that must not be handed back as a premature
 * timeout (a genuine timeout == -1 wait may only return with an event or a
 * signal — libuv's uv__io_poll asserts exactly that, and a leaked
 * premature 0 crashed pnpm with SIGABRT), so this re-waits for the
 * caller's remaining time instead, paced by *out_sleep_ns whenever the
 * strip was a storm/ghost re-fire. timeout == 0 (non-blocking) is the one
 * shape that returns whatever it gets immediately, stripped or not --
 * the caller asked not to block, so a 0 here is a legitimate answer, not a
 * leaked internal detail. */
static int ep_shim_wait(int epfd, struct epoll_event *events, int maxevents,
			int timeout, epoll_pwait_fn real,
			const sigset_t *sigmask)
{
	if (timeout == 0) {
		long long sleep_ns;
		return ep_shim_after_wait_ex(
			real(epfd, events, maxevents, 0, sigmask),
			epfd, events, maxevents, &sleep_ns);
	}

	long long deadline = timeout > 0 ? ep_now_ms() + timeout : 0;
	for (;;) {
		long long sleep_ns = 0;
		int rc = ep_shim_after_wait_ex(
			real(epfd, events, maxevents, timeout, sigmask),
			epfd, events, maxevents, &sleep_ns);
		if (rc != 0)
			return rc;
		if (sleep_ns > 0) {
			struct timespec zzz = { .tv_sec = 0, .tv_nsec = sleep_ns };
			nanosleep(&zzz, NULL);
		}
		if (timeout > 0) {
			long long left = deadline - ep_now_ms();
			if (left <= 0)
				return 0;
			timeout = left > INT_MAX ? INT_MAX : (int)left;
		}
	}
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
	       int timeout)
{
	/* Route through the real epoll_pwait with a NULL mask — that IS
	 * epoll_wait at the syscall level — so both overrides share the
	 * same hidden-slicing loop. */
	static epoll_pwait_fn real = NULL;
	if (!real)
		real = (epoll_pwait_fn)dlsym(RTLD_NEXT, "epoll_pwait");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	return ep_shim_wait(epfd, events, maxevents, timeout, real, NULL);
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
		int timeout, const sigset_t *sigmask)
{
	static epoll_pwait_fn real = NULL;
	if (!real)
		real = (epoll_pwait_fn)dlsym(RTLD_NEXT, "epoll_pwait");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	return ep_shim_wait(epfd, events, maxevents, timeout, real, sigmask);
}

/*
 * Shared cleanup for every place an fd number can start meaning a
 * different file: the real close() below, dup2()/dup3() onto an
 * already-open newfd (which implicitly closes whatever newfd used to be),
 * and the raw-syscall paths bun's Rust event loop uses instead of those
 * libc symbols (see the syscall() override's __NR_close/__NR_dup3 cases).
 * Forgets both the epoll_pipe registry entry (if that interceptor is
 * active) and the fifo cache (unconditionally -- splice()'s own is-fifo
 * check uses the cache too, independent of epoll_pipe). Call this *before*
 * the fd's old meaning is gone, matching the timing the epoll registry
 * cleanup already used: afterwards the number can be handed out again for
 * a different file at any moment, so forgetting late leaves a window where
 * a concurrent lookup can be caught with a stale answer.
 *
 * This is what closes the fd-reuse staleness risk the epoll_pipe registry
 * only partially guarded against before (close() alone missed dup2/dup3
 * and raw-syscall closes -- flagged in the 2026-08-18 shim validation
 * pass) and what makes the fifo cache above safe to introduce at all.
 */
static void shim_forget_fd(int fd)
{
	if (fd < 0)
		return;
	if (ep_pipe_active()) {
		pthread_mutex_lock(&g_ep_pipes_lock);
		ep_reg_forget_fd_locked(fd);
		pthread_mutex_unlock(&g_ep_pipes_lock);
	}
	fifo_cache_forget_fd(fd);
	tty_cache_forget_fd(fd);
}

typedef int (*close_fn)(int);

int close(int fd)
{
	static close_fn real = NULL;
	if (!real)
		real = (close_fn)dlsym(RTLD_NEXT, "close");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	shim_forget_fd(fd);
	return real(fd);
}

/*
 * dup2()/dup3() onto an already-open newfd implicitly close it -- the same
 * "this fd number is about to mean a different file" event close() handles
 * above. Forgetting unconditionally (even when the call will turn out to
 * be a no-op, e.g. dup2(fd, fd), or fails) is deliberate: an unnecessary
 * forget just costs one extra cache-miss fstat() on the next lookup,
 * whereas forgetting only after a confirmed-successful call would leave a
 * window, between the real syscall completing and this running, where a
 * concurrent lookup on newfd could see the cache's old (now wrong) answer.
 */
typedef int (*dup2_fn)(int, int);

int dup2(int oldfd, int newfd)
{
	static dup2_fn real = NULL;
	if (!real)
		real = (dup2_fn)dlsym(RTLD_NEXT, "dup2");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	shim_forget_fd(newfd);
	return real(oldfd, newfd);
}

typedef int (*dup3_fn)(int, int, int);

int dup3(int oldfd, int newfd, int flags)
{
	static dup3_fn real = NULL;
	if (!real)
		real = (dup3_fn)dlsym(RTLD_NEXT, "dup3");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	shim_forget_fd(newfd);
	return real(oldfd, newfd, flags);
}

int linkat(int olddirfd, const char *oldpath, int newdirfd,
	  const char *newpath, int flags)
{
	static linkat_fn real = NULL;
	if (!real)
		real = (linkat_fn)dlsym(RTLD_NEXT, "linkat");

	int rc = real ? real(olddirfd, oldpath, newdirfd, newpath, flags) : -1;
	if (rc == 0)
		return 0;
	if (shim_disabled(SD_LINKAT))
		return rc;
	if (errno != EPERM && errno != EACCES)
		return rc;

	int src = openat(olddirfd, oldpath,
			 O_RDONLY | (flags & AT_SYMLINK_FOLLOW ? 0 : O_NOFOLLOW));
	if (src < 0)
		return -1;

	struct stat st;
	if (fstat(src, &st) != 0) {
		int e = errno;
		close(src);
		errno = e;
		return -1;
	}

	if (copy_fd_to_path_atomic(src, newdirfd, newpath,
				   st.st_mode & 0777) != 0) {
		int e = errno;
		close(src);
		errno = e;
		return -1;
	}
	close(src);
	return 0;
}

typedef int (*link_fn)(const char *, const char *);

/* link() — same sandbox EPERM/EACCES story as linkat, but musl's link()
 * goes through an inline syscall(SYS_linkat) that bypasses the linkat
 * *dynamic symbol*, so the linkat hook above never sees fs.link / libuv
 * callers — they reach musl's link() symbol instead, which this hook
 * intercepts. Fall back to the same atomic byte-copy as linkat. Semantically
 * lossy (loses hardlink identity), like linkat. Disable via
 * OHOS_COMPAT_SHIM_DISABLE=link. */
int link(const char *oldpath, const char *newpath)
{
	static link_fn real = NULL;
	if (!real)
		real = (link_fn)dlsym(RTLD_NEXT, "link");

	int rc = real ? real(oldpath, newpath) : -1;
	if (rc == 0)
		return 0;
	if (shim_disabled(SD_LINK))
		return rc;
	if (errno != EPERM && errno != EACCES)
		return rc;

	int src = openat(AT_FDCWD, oldpath, O_RDONLY);
	if (src < 0)
		return -1;

	struct stat st;
	if (fstat(src, &st) != 0) {
		int e = errno;
		close(src);
		errno = e;
		return -1;
	}

	if (copy_fd_to_path_atomic(src, AT_FDCWD, newpath,
				   st.st_mode & 0777) != 0) {
		int e = errno;
		close(src);
		errno = e;
		return -1;
	}
	close(src);
	return 0;
}
