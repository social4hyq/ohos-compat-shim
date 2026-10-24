#!/usr/bin/env bash
D=$(cd "$(dirname "$0")" && pwd); OUT=$1; N=$2; PROXY=http://127.0.0.1:7890
: >"$OUT"
for round in $(seq 1 "$N"); do
  if [ $((round % 2)) -eq 1 ]; then conds="direct proxy"; else conds="proxy direct"; fi
  for cond in $conds; do
    if [ "$cond" = direct ]; then
      j=$(env -u LD_PRELOAD -u http_proxy -u https_proxy -u all_proxy -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY ohos-shim check --rounds 1 --json 2>/dev/null)
    else
      j=$(env -u LD_PRELOAD http_proxy=$PROXY https_proxy=$PROXY all_proxy=$PROXY HTTP_PROXY=$PROXY HTTPS_PROXY=$PROXY ALL_PROXY=$PROXY ohos-shim check --rounds 1 --json 2>/dev/null)
    fi
    printf '%s\t%s\t%s\n' "$round" "$cond" "$(printf '%s' "$j" | python3 -I -c "import json,sys;d=json.load(sys.stdin);r=[x for x in d['rows'] if x['id']=='getaddrinfo'][0];print(r['verdict'],'|',r['note'])")" >>"$OUT"
  done
done
echo done >"$OUT.done"
