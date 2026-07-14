#!/bin/sh
# Example launcher wrapper for a single-binary OHOS release (e.g. opencode),
# following the same pattern as claude-code.rb's bin/claude wrapper.
#
# Usage: copy next to the real binary (renamed e.g. `opencode.bin`), then
# make this script the thing users actually invoke as `opencode`.
#
#   your-release/
#     opencode            <- this wrapper, executable, named what users run
#     opencode.bin        <- the real dynamic-musl binary
#     libohos_compat.so   <- from @ohos-ports/compat-shim or GitHub Release

DIR="$(cd "$(dirname "$0")" && pwd)"
SHIM="$DIR/libohos_compat.so"

if [ -f "$SHIM" ]; then
	export LD_PRELOAD="$SHIM${LD_PRELOAD:+:$LD_PRELOAD}"
fi

exec "$DIR/opencode.bin" "$@"
