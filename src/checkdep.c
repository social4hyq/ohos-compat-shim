/*
 * checkdep.c — tiny companion .so for ohos_compat_check.c's dlopen/.dynsym
 * probe.
 *
 * CLAUDE.md records a system-level finding for this workspace: musl's
 * dynamic linker does NOT resolve the main executable's .dynsym symbols
 * when a dlopen()'d module asks for them via dlsym(RTLD_DEFAULT, ...) —
 * even with -rdynamic/--export-dynamic on the main binary. Software that
 * depends on dlopen-time symbol resolution (e.g. brew zsh's dynamically
 * loaded modules) has had to fall back to `--disable-dynamic` static
 * builds to route around it.
 *
 * There is no libc-level API to ask "has this limitation been lifted on
 * this OS build" directly, so the check program proves it the same way
 * the original bug was found: build a real dlopen()'d module (this file),
 * have it look for a marker symbol that only exists in the main binary,
 * and report whether it found it.
 *
 * ohos_compat_check.c exports `ohos_check_marker` (plain external
 * linkage, no special visibility attribute needed — default visibility
 * already puts it in .dynsym once the check binary is linked with
 * -rdynamic) and links this library at runtime via dlopen(), not at
 * build time — the two are independent translation units glued only
 * through the loader, exactly the scenario the platform limitation is
 * about.
 */
#include <dlfcn.h>
#include <stddef.h>

/* Returns 1 if dlsym(RTLD_DEFAULT, ...) from *inside a dlopen()'d module*
 * can see the main executable's .dynsym (limitation lifted), 0 if not
 * (still blocked — dlopen-dependent software still needs the static-link
 * workaround). */
int ohos_checkdep_probe(void)
{
	return dlsym(RTLD_DEFAULT, "ohos_check_marker") != NULL;
}
