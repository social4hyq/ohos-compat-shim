/** Absolute path to the prebuilt, self-signed aarch64-unknown-linux-ohos .so. */
export declare const libPath: string;

/**
 * Build an LD_PRELOAD value that prepends this shim ahead of any existing
 * preload chain.
 */
export declare function preloadEnv(existing?: string): string;
