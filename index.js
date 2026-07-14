import { fileURLToPath } from "node:url";
import path from "node:path";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

/** Absolute path to the prebuilt, self-signed aarch64-unknown-linux-ohos .so. */
export const libPath = path.join(__dirname, "lib", "libohos_compat.so");

/**
 * Build an LD_PRELOAD value that prepends this shim ahead of any existing
 * preload chain, matching the pattern used by claude-code.rb / opencode
 * wrapper scripts:
 *   export LD_PRELOAD="<shim>${LD_PRELOAD:+:$LD_PRELOAD}"
 *
 * @param {string} [existing] Existing LD_PRELOAD value; defaults to
 *   process.env.LD_PRELOAD.
 * @returns {string}
 */
export function preloadEnv(existing) {
	const prev = existing ?? process.env.LD_PRELOAD ?? "";
	return prev ? `${libPath}:${prev}` : libPath;
}
