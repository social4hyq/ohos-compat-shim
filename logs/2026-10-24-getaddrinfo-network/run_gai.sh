#!/usr/bin/env bash
# usage: run_gai.sh <rounds> <out.tsv>
D=$(cd "$(dirname "$0")" && pwd)
N=$1; OUT=$2
SHIM=$HOME/.harmonybrew/opt/ohos-compat-shim/lib/libohos_compat.so
PROXY=http://127.0.0.1:7890
names=("bad host!" "nonexistent-gai-test.invalid" "example.com")
run() { # cond shim name
  local cond=$1 shim=$2 name=$3 out
  if [ "$cond" = direct ]; then
    out=$(env -u LD_PRELOAD -u http_proxy -u https_proxy -u all_proxy -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
      ${shim:+LD_PRELOAD=$SHIM} timeout 30 "$D/gai_once" "$name" 2>&1)
  else
    out=$(env -u LD_PRELOAD http_proxy=$PROXY https_proxy=$PROXY all_proxy=$PROXY HTTP_PROXY=$PROXY HTTPS_PROXY=$PROXY ALL_PROXY=$PROXY \
      ${shim:+LD_PRELOAD=$SHIM} timeout 30 "$D/gai_once" "$name" 2>&1)
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$round" "$cond" "$([ -n "$shim" ] && echo shim || echo native)" "$name" $out >>"$OUT"
}
: >"$OUT"
for round in $(seq 1 "$N"); do
  if [ $((round % 2)) -eq 1 ]; then conds=(direct proxy); else conds=(proxy direct); fi
  for cond in "${conds[@]}"; do
    for name in "${names[@]}"; do run "$cond" "" "$name"; done
    run "$cond" 1 "bad host!"
  done
done
echo done >>"$OUT.done"
