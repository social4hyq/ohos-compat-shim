# Historical validation: epoll ONESHOT idle spin in musl Claude Code

This device-specific investigation records the evidence behind the FIFO
`EPOLLOUT|EPOLLONESHOT` repair in `src/ohos_compat_shim.c`. It is not a
performance guarantee for other devices or application versions.

## Finding

On the tested HarmonyOS device, the kernel repeatedly delivered `EPOLLOUT`
for an empty pipe writer registered with `EPOLLONESHOT`. An official
Claude Code 2.1.283 musl binary produced about 22.8 million pure-`EPOLLOUT`
events in 40 seconds. The registration remained present in
`/proc/<pid>/fdinfo`, despite the application not re-arming it. The wait path
used the interposable libc `epoll_pwait` symbol; this was not an inline raw
syscall that `LD_PRELOAD` could not see.

The initial shim repair suppressed repeated events, but the first interactive
TUI test exposed two issues in that repair: matched-event suppression did
not request pacing, and repeated kernel-side `EPOLL_CTL_DEL` attempts returned
`ENOENT` while the registration remained visible. A 20-second observation
counted 860,561 failed deletes. The implementation was corrected to attempt
delete at most once per arm and back off repeated ghost events. The deterministic
state-machine regression cases are in `test/epoll_ghost.c`.

## Device A/B result

The same official binary and idle TUI screen were run in alternating order,
three times each. CPU usage excludes the first four seconds of startup.

| Run | Before pacing fix | After pacing fix |
|---|---:|---:|
| 1 | 100.45% | 1.24% |
| 2 | 100.45% | 1.32% |
| 3 | 100.69% | 1.06% |

Interactive and non-interactive musl runs both stopped exhibiting the measured
spin after the fix. These numbers describe this device and test setup only.

## Follow-up: residual application CPU

A later, longer sample measured roughly 0.8–1.2% steady-state CPU after the
shim fix. The main thread remained blocked; periodic groups of 7–8 short-lived
threads appeared about every 30 seconds and ran for roughly 10 seconds. The
pattern points to periodic Claude Code or runtime activity, but the worker
threads were not individually sampled and the same run was not compared with
the tap Bun runtime. This follow-up therefore does not establish the source
of that residual CPU and is not evidence of an epoll shim regression.

## Method notes

- The first poll/epoll probes closed the pipe writer before joining the waiter,
  allowing `HUP` to create a false wakeup. Corrected probes kept the writer
  open until after the waiter joined.
- CPU measurements used per-thread `/proc/<pid>/task/*/stat` deltas. Event
  masks were observed with a separate preload probe; `fdinfo` snapshots were
  used to confirm the registrations remained present.
- Earlier raw-syscall and “preload cannot fix the TUI” conclusions were
  disproved by the later PC sample and are intentionally omitted here.
