# ohos-compat-shim

面向运行在 HarmonyOS 应用沙箱内的预编译 aarch64/musl 二进制的 `LD_PRELOAD` 兼容 shim。让一个你无法（或不想）重新编译的二进制，在不改动目标二进制任何一个字节的前提下，挺过一批已知的 HarmonyOS 与 OpenHarmony 沙箱差异。

完整的可行性分析（本项目正是从中孵化出来的，包含一张「哪些能修、哪些不能修、为什么」的矩阵）见同级 `ohos-preflight` 仓库中的[`docs/ohos-preload-shim-feasibility.md`](../ohos-preflight/docs/ohos-preload-shim-feasibility.md)。

## 修复了什么

| 符号 | HarmonyOS 上的真实症状 | 默认状态 |
|---|---|---|
| `close_range()` / `syscall(SYS_close_range)` | `SIGSYS` —— 直接杀死进程 | 开启 |
| `getpwuid_r()` | `rc=0, *result=NULL` —— Node 的 `os.userInfo()` 抛异常 | 开启 |
| `tmpfile()` | 返回 `NULL`，`errno=EPERM` —— 沙箱内 `P_tmpdir` 不可写 | 开启 |
| `getcwd()` | hmdfs/tmpfs 父目录缺 `+x` 时 `EACCES`，或 cwd 被 rmdir 后 `ENOENT`（常见于生命周期脚本、`bun run`） | 开启 |
| `linkat()` / `symlinkat()` | 沙箱化的安装目标目录里报 `EPERM`/`EACCES` | **关闭**（需手动开启）|
| `syscall(SYS_fchmodat2)` | 真机 `SIGSYS`，OpenHarmony 容器里是 `ENOSYS` —— 两边都失败，但失败方式不同 | 开启 |
| `splice()` | 源端 EOF 时返回 `-1/EPIPE`，Linux 返回 `0` —— 所有 splice 拷贝循环在文件尾误报错误 | 开启 |

`pthread_cancel()` 在这个平台上是 musl 的空桩实现，刻意**不**做 shim ——一个 preload 库没办法给调用方注入它所需要的协作式取消点。

## 前向兼容：优先自动尝试真实系统调用

每个拦截点都会先通过 `dlsym(RTLD_NEXT, ...)` 解析出真实实现并尝试调用它，而不是先走 fallback 逻辑 —— 这不是一次性的系统版本判断，而是每个新进程都会重新做的实时探测（对 `close_range` 之外的符号来说，甚至是每一次调用都重新判断）。具体来说：

- `close_range`：`cr_probe_syscall()` 每个进程只跑一次；如果真实系统调用成功，该进程就会缓存 `WORKS` 状态，之后再也不会碰用户态 fallback。
- `getpwuid_r` / `tmpfile` / `getcwd` / `linkat` / `symlinkat`：**每次**调用都会先调用真实函数；shim 自己的逻辑只在真实调用失败、且失败特征与上表记录的 HarmonyOS 沙箱症状完全吻合时才会介入。

实际效果：一旦 HarmonyOS 哪天把 `close_range` 加入白名单（ohos-preflight 报告里 441 号被标为 P0，诉求正是这个），或者修复了另外四个症状里的任何一个，**所有新启动的进程都会自动享受到这个改进**——不需要重新编译 shim，不需要重新部署，这个仓库里也不需要改一行代码。

但这**不代表**平台跟上之后开销就会归零。只要消费者仍然 `LD_PRELOAD` 这个库，每次调用依旧要付出进入拦截函数、以及做一次 `dlsym` 缓存过的真实调用尝试的代价（在[性能](#性能)一节中实测约为 10 ns/次的透传税，对已经成功的路径来说几乎可以忽略）。真正做到*完全*零开销的唯一办法，是消费者不再为该符号预加载这个库 ——这是**消费者自己的打包决策**，shim 本身做不到，因为它在编译期根本无法预知运行时某台设备的沙箱究竟允许什么。

### 收口跟踪

沿用本工作区现有的「上游收口」惯例（其它 `@ohos-ports/*` fork 也是这个模式）：一旦上游 / 平台修好了对应问题，就下线这层兼容代码，切回原生路径。

| 符号 | 对应跟踪探针 | 何时收口 |
|---|---|---|
| `close_range` | `ohos-preflight` 探针 `a10_close_range` | HarmonyOS 在所有消费者仍支持的系统版本上放行 436 号系统调用 |
| `getpwuid_r` | 探针 `i9_getpwuid_r` | HarmonyOS 给 HAP 分配的 uid 能通过 `/etc/passwd` 解析，或提供了 `nss_ohos` |
| `tmpfile` | 探针 `g1_tmpfile` | 应用沙箱内 `P_tmpdir` 变为可写 |
| `getcwd` | 探针 `g7_getcwd_unlinked` | 平台层面无法修复（这是符合 POSIX 语义的 `ENOENT` 行为）—— 这一项会一直留着 |
| `linkat`/`symlinkat` | 探针 `g5_linkat_eperm`/`g6_symlinkat_eperm` | 目标安装目录不再对硬链接/符号链接返回 `EPERM`/`EACCES` |
| `fchmodat2` | 探针 `c5_fchmodat2` | HarmonyOS 放行 452 号系统调用（目前是 `both_fail`：OpenHarmony 容器里也是 `ENOSYS`，所以这项收口不光需要 HarmonyOS 放行，容器那边的内核也得先实现这个系统调用）|
| `splice` | 功能测试 `splice_eof_is_zero`（baseline 段即为探针）| 内核修正 `splice()` 的 EOF 语义，源端耗尽时返回 `0` 而不是 `EPIPE` |

定期重跑 `ohos-preflight` 的双轨对比；一旦某个探针稳定地从 `needs_relax`变成 `same`，对应符号的拦截逻辑就可以在后续版本里默认关闭（或直接移除）——等消费者所支持的所有 HarmonyOS 版本都不再需要剩下的任何一个症状时，就应该停止预加载这个库。

## 修不了什么，为什么

这套方案只对通过**动态链接的 libc 符号**解析的调用生效。通过内联汇编发起系统调用的代码——例如 Bun 的 rustix `linux_raw` 后端用于 `openat2`/`epoll_pwait2` 的那部分——根本不会碰这些符号，`LD_PRELOAD` 插桩自然也就够不着它。这些还是得走真正的源码级移植。完整矩阵见可行性文档。

静态链接的二进制同样没法做 shim —— `LD_PRELOAD` 只对动态链接的目标生效。接入前先用 `readelf -d <binary>` 确认一下（找 `NEEDED libc.so`）。

## `splice()` 的 EOF 判据（为什么不能无脑把 EPIPE 当 EOF）

这个内核在**源端 pipe 到达 EOF** 时让 `splice()` 返回 `-1/EPIPE`，而 Linux 返回 `0`；同一个 pipe 上 `read()` 却正确返回 `0`，所以坏的只有 splice 这条路径。后果是每一个 splice 拷贝循环都会把正常的文件尾当成致命错误 —— GNU coreutils 的 `cat` 只要 stdin 和 stdout 同为管道就走这条路，于是打印 `cat: -: Broken pipe` 并以 1 退出。

**真正的 EPIPE（目标端读端已关闭）必须照常报出来**，否则会静默截断拷贝。两者用 `poll()` 分得很干净，而且 `poll()` 是无损的 —— 不像 `read()`，它不会消耗掉我们正要搬运的那个字节：

| 情形 | `poll(fd_in)` | `poll(fd_out)` |
|---|---|---|
| 源端 EOF（内核 bug）| `POLLIN\|POLLHUP` | `POLLOUT` |
| 目标端损坏（真 EPIPE）| `POLLIN` | `POLLOUT\|POLLERR` |
| 两者同时发生 | `POLLIN\|POLLHUP` | `POLLOUT\|POLLERR` |

判据就是目标端的 `POLLERR`。**先查目标端**，这样"两者同时"的歧义情形会落到"真错误"一侧 —— 这是保守的方向：把一个恰好也处于 EOF 的坏管道报成错误没有任何损失，而吞掉一个真 EPIPE 会让拷贝悄悄少写数据。

注意 EOF 的 pipe 上 `POLLIN` 也是置位的（此时 `read()` 会立刻返回 0），所以判 EOF 的依据是 `POLLHUP`，不是"没有 `POLLIN`"。整个分支只在 `splice()` 已经返回 `EPIPE` 时才进入，其余情况原样透传，正常路径的开销就是一次比较。

这个内核的 `splice()` 还有第二个毛病：**对空管道会一直阻塞，无视 `O_NONBLOCK` 和 `SPLICE_F_NONBLOCK`**。该行为不会返回 `EPIPE`，因此碰不到上面的分支，这里没有处理 —— 记在这里是因为它会让"给 splice 加个非阻塞探测"这类想法直接失效。

## 已知平台行为（禁用 `close_range` 前请先读这段）

`close_range` —— 通过原始 `syscall(SYS_close_range, ...)` 符号调用，和 Bun 的调用方式完全一致 —— 在这台设备的真实内核上**无条件抛出 `SIGSYS`**，测试过的所有参数组合（合法调用、`first > last`、乱填 flags）无一例外，而且和这个 shim（或任何其它东西）是否被 `LD_PRELOAD` 完全无关。这和 `ohos-preflight` 自己的 `a10_close_range` 探针从一开始的结论完全一致（OH 容器通过，HM 真机失败）—— 这里面并不存在什么「加载第三方库会改变系统允许什么」的深层故事，事情比那个简单得多。（这段的早期草稿曾经得出过相反的结论，原因是那次测试沿用了一个从此前无关测试里残留下来、忘记清理的 `LD_PRELOAD` 环境变量，悄悄地把本该是干净基线的运行替换成了另一个更窄范围的 preload 库。真正的教训不是关于平台的，而是：**验证时永远显式用 `env -u LD_PRELOAD`**，永远不要假设某个 shell 的环境变量是干净的。Makefile 里的 `smoke`/`functional`/`bench` 几个 target 现在都会自动这么做。）

实际影响：

- 既然真实系统调用在这台设备上永远不会成功，shim 那套「探测一次、缓存结果」的设计（`cr_probe_syscall()`）在这台设备上永远只会走用户态 fallback 路径 —— 这里不存在「委托成功」这条路径可以退回，不过代码依旧保留了这条路径， 留给其它设备/系统版本上 `close_range` 有可能真的能用的情况 （对应 `ohos-preflight` 的 OH 容器那条轨道，这个探针在那边是通过的）。
- `OHOS_COMPAT_SHIM_DISABLE=close_range` **不会**还原出「操作依旧能成功」意义上的真实无 shim 基线行为。它还原出的是「预加载了，但没有保护、没有探测」—— 真实调用每次都会 `SIGSYS`，和完全不加载 shim 时一模一样，也就是说进程会崩溃。 只有在你确实想验证这个崩溃行为本身时，才应该禁用 `close_range`。
- 另外三个默认开启的符号（`getpwuid_r`、`tmpfile`、`getcwd`）完全不表现出这个特性——真实调用有时会成功（`getcwd` 在常见情况下总是成功），禁用它们中的任何一个都能 精确还原出真实、无 shim 时的调用结果，这一点已由 `test/functional.c` 验证。

### close_range() 自己校验参数 —— 因为本机内核校验不了

对照 LTP 的 `close_range02.c`，再读上游 Linux 的 `fs/file.c`（`SYSCALL_DEFINE3(close_range, ...)`），发现了一个纯靠单元测试根本发现不了的真实缺口：上游 Linux 对 `first > last` 以及未知 `flags` 位都会返回 `EINVAL`。而在这台设备上，*任何*参数下的 `close_range` 调用都会 `SIGSYS`（见上面「已知平台行为」）—— 所以无论加不加保护，都不可能从真实内核那里拿到符合标准的 `EINVAL` 行为。shim 的 `cr_validate_args()` 会在任何探测或 fallback 逻辑运行之前提前校验这两种情况，让调用方依旧能拿到正确的 `EINVAL` 错误，而不是要么崩溃、要么（走 fallback 的话）在没有意义的输入上悄悄地继续跑下去。

在 `test/functional.c` 中验证（`close_range_einval_range`、`close_range_einval_flags`），两者都复用了测试套件共享的 `guarded_close_range`辅助函数，这样基线（无 shim）运行时会把底层的崩溃报告成预期内的 `INFO`，而不是把整个测试二进制一起拖垮。

### `CLOSE_RANGE_UNSHARE` 在这台设备上没法真正做到

`unshare(CLONE_FILES)` —— `CLOSE_RANGE_UNSHARE` 用来在关闭之前把某个线程的 fd 表私有化所依赖的原语 —— 经验证在这台设备上会**无条件 `SIGSYS`**，和 `close_range` 本身完全无关，即使完全不加载任何 shim 也能复现。一旦 `unshare()` 本身不可用，真正的 fd 表隔离就没有任何真实的 fallback 可言：如果改为在*共享*的表里直接关闭 fd，会悄悄破坏依赖这张表的其它所有线程，比直接拒绝还要糟糕。所以 `cr_do_fallback()` 也会捕获这个 `SIGSYS`，并如实报告 `ENOSYS`——一个诚实的失败，既不会崩溃，也不会假装做到了隔离。`test/functional.c` 里的 `close_range_unshare` 检查项（用一个真实的 pthread，对照 LTP `close_range02.c` 的 case 5）验证的正是这个「降级但安全」的结果。

### `fchmodat2`（452 号系统调用）—— 第二个原始系统调用级拦截点

这是在审计完 `ohos-preflight` 的完整探针报告（`reports/preflight-summary-2026-06-19-0704.md`）之后加入的，目标是找出和 `close_range` 同一形态的其它 `needs_relax`/`both_fail` 项：这台 musl 里没有对应的 libc 包装函数（这个系统调用号太新了），是直接通过 `syscall(SYS_fchmodat2, ...)` 调用的（比如 Bun 的 `add` 命令设置权限时），真实实现在两条轨道上都会失败，但失败方式*不一样*——

- **真实 HarmonyOS 硬件**：无条件 `SIGSYS`，用和当年为 `close_range` 写 shim 代码之前一样的独立、带保护的复现方式确认过（不是从报告里假设出来的）。
- **OpenHarmony 容器**：一个干净的 `ENOSYS`——容器内核确实完全没有实现这个系统调用，没有 seccomp 参与，不会崩溃。直接验证过：容器里跑出来的 `errno=38` 很干净，不需要在那边捕获 `SIGSYS`。

架构上和 `close_range` 完全一样（探测一次、缓存结果，用 `shim_guarded_syscall`——现在已经抽成两个拦截点共用的 SIGSYS 捕获基础设施），但在一点上更简单：不存在像 `CLOSE_RANGE_UNSHARE` 那样、*不同*参数值会触发另一个底层系统调用、从而改变结果的情况（不会因为 flags 不同而触发第二个系统调用），所以这里信任一次进程级别的探测结果，而不是每次调用都重新做保护。fallback 到经典的 `fchmodat()`（丢弃 `flags` 参数 —— 对符号链接目标会丢失 `AT_SYMLINK_NOFOLLOW` 语义），和 `ohos-preflight` 已验证过的 `solutions/c5_fchmodat2.c` 一致。已经在两条轨道上做过端到端验证：基线在两边都按文档描述的方式失败（真机 `SIGSYS` / 容器 `ENOSYS`，`test/functional.c` 的 `fchmodat2_applies_mode` 在真机场景下报告为 `INFO`——和其它带保护的检查项一样的崩溃恢复模式——在容器里则合理地报告 `FAIL`，因为 `ENOSYS` 不是需要捕获的崩溃）；加了 shim 之后两边都能成功，而且都能应用上正确的权限。

同一轮审计里发现的其它 `needs_relax`/`both_fail` 条目*没有*加进来 ——`epoll_pwait2`/`openat2` 是通过内联汇编发起系统调用的，没有 libc 符号可拦截（和[修不了什么](#修不了什么为什么)里说的是同一个原因）；`seccomp_unotify`可行的绕过方式完全是另一套工具（是要单独拦截各个 libc 调用本身，而不是让 `seccomp()` 这个调用本身能用）；`rseq` 只影响 glibc 消费者（这个项目的目标全都是 musl），就算是 glibc，这个问题也发生在进程启动的太早期，这层拦截根本碰不到；`pidfd_poll_wait` 的竞态条件已经被明确标注为不影响 Bun 实际的代码路径。完整的逐项理由见 `docs/ohos-preload-shim-feasibility.md`。

## 用法

### Shell wrapper（claude-code.rb 模式）

```sh
#!/bin/sh
export LD_PRELOAD="/path/to/libohos_compat.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec "/path/to/real-binary" "$@"
```

### 从 npm 使用

```js
import { preloadEnv } from "@ohos-ports/compat-shim";
import { spawn } from "node:child_process";

spawn("/path/to/real-binary", process.argv.slice(2), {
	env: { ...process.env, LD_PRELOAD: preloadEnv() },
	stdio: "inherit",
});
```

针对 `opencode` 这类单体二进制、可以直接拿来改的 wrapper 示例见 `examples/`。

### 运行时开关

```sh
# 关闭某个默认开启的拦截点（逗号分隔）
export OHOS_COMPAT_SHIM_DISABLE=getcwd,tmpfile

# 打开某个默认关闭的拦截点（语义有损，需按场景手动开启）
export OHOS_COMPAT_SHIM_ENABLE=linkat,symlinkat
```

## 构建

本机 / 真机迭代（需要通过 harmonybrew 安装的 OHOS NDK）：

```sh
make             # 构建 libohos_compat.so + test/smoke + test/functional + test/bench
make smoke       # 基础 3 项检查，先跑基线再跑加 shim
make functional  # 全面的一致性检查，先跑基线再跑加 shim
make bench       # 单次调用开销数据，先跑基线再跑加 shim
```

npm 发布的产物由 `.github/workflows/release.yml` 在云端 `ubuntu-latest` runner 上交叉编译产出，用的是同一套 OHOS NDK clang 调用方式，用 [ohos-bst-light](https://github.com/hqzing/ohos-bst-light) 的 `self-sign.py` 自签名，并且要先通过真机 smoke test 才会进入 GitHub Release / npm 发布环节。不依赖任何 Homebrew formula，也不需要自建构建 runner。

## 测试

`test/functional.c`（`make functional`）覆盖的范围远不止 smoke.c 的 3 项基础检查。测试覆盖度是刻意对照这几个调用现有的参考测试套件来校准的 ——[Linux 内核自测套件](https://github.com/torvalds/linux/blob/master/tools/testing/selftests/core/close_range_test.c)和 [LTP](https://github.com/linux-test-project/ltp) 的 `close_range01/02.c`、`getcwd01-04.c`、`linkat01/02.c`、`symlinkat01.c` —— 而不是随手想到什么测什么。（musl 自己的 `libc-test` 套件和 glibc 的测试套件都没有覆盖 `close_range`/`getcwd`/`linkat`/`symlinkat`/`tmpfile` 的什么实质深度，所以 LTP —— 它测的是这些函数所包装的系统调用本身 —— 是最接近权威参考的东西。`getpwuid_r` 和 `tmpfile` 没有类似的官方参数矩阵可参照：前者是 LTP 系统调用测试范围之外的 NSS/passwd 数据库特性，后者根本不接受任何参数。）直接对照 LTP 在上线前就抓出了两个真实的 bug —— 见上面「已知平台行为」一节里 `close_range` 的 `EINVAL` 校验和 `CLOSE_RANGE_UNSHARE` 降级处理。

- **`close_range`**（8 项检查）：多 fd 区间（关闭中间、两端不动）、一个*稀疏的宽区间*（`fd..fd+90`，对照 LTP 里 dup 到高位 fd 的用例）、 `CLOSE_RANGE_CLOEXEC`、`first > last` → `EINVAL`、乱填 flags → `EINVAL` （而不是崩溃）、通过一个真实的 pthread 触发 `CLOSE_RANGE_UNSHARE` （对照 LTP `close_range02.c` 用 clone(2) 的用例）、连续两次调用结果一致 （验证探测结果缓存的正确性）、以及 `close_range()` 这个 libc 风格包装函数 与原始 `syscall()` 路径结果一致。
- **`getpwuid_r`**（2 项检查）：缓冲区太小时必须返回 `ERANGE`——这台设备上真实实现根本走不到那一步去检查（会先因为一个无关的 `EBADF` 怪癖提前失败），所以这一项只有*加了 shim*才能通过。另外还验证连续两次调用 必须返回逐字节一致的数据。
- **`tmpfile`**（3 项检查）：完整的写入/读回往返、3 个同时存在的临时文件之间互不污染、以及连续 50 次打开+关闭循环全部不失败（对一个零参数函数来说， 这是最接近参数矩阵的东西）。
- **`getcwd`**（5 项检查 + 1 项 info）：核心问题是「是不是真的无操作」——两种不同的调用方式（`getcwd(buf, size)` vs `getcwd(NULL, 0)`）在 cwd 没变的情况下必须返回逐字节一致的字符串。另外还有 `size=0` → `EINVAL`、 `size=1`（真实缓冲区）→ `ERANGE`、以及 `buf=NULL, size=1` 通过 musl 的 GNU 自动分配扩展成功（这三项都是对照这台设备*实际*的 libc 行为校准的，而不是 LTP 那种原始系统调用层面的预期，因为 shim 只拦截 libc 包装函数 —— 具体原因见代码注释）。还有一个不计分的 `INFO` 检查项，用来验证 cwd 被 rmdir 之后的 fallback 行为。
- **`linkat`/`symlinkat`**（3 项检查）：此前**完全没有**覆盖 ——这是这轮排查里发现的最大缺口。`EEXIST` 会原样透传（对照 LTP `linkat02.c`）；在 `$TMPDIR` 里真实、当场复现出来的 `EACCES` （通过重跑 `ohos-preflight` 的 `g5_linkat_eperm` 探针、针对这个测试自己的 `$TMPDIR` 确认，不是模拟出来的条件）会触发 copy fallback，且内容正确； `symlinkat`（在这个 `$TMPDIR` 里不受限制）验证的是成功/无操作路径。
- **bun 调用场景镜像**（8 项检查，跨 close_range/getcwd/linkat/symlinkat/fchmodat2 + symlink）：上面那些是按 LTP 通用语义校准的；这一组是对照 ohos-bun 源码里的**全部真实调用点**逐一补的——① `close_range(4, ~0U, CLOEXEC)` init 期 fd 泄漏防护（`bun_initialize_process`，跳过 stdio 0-2、对 ≥4 的 fd 设 `FD_CLOEXEC` 但保持打开）；② `close_range(3, ~0U, CLOEXEC)` spawn/reload 路径（`BunProcess.cpp` + `on_before_reload_process_linux`，fd 3 **也** CLOEXEC，与 init 的 first=4 区分）；③ `getcwd` 6 层深目录 `getcwd()==readlink("/proc/self/cwd")==构造路径"`；④ `linkat` 经 `/proc/self/fd/<N>` 物化 `O_TMPFILE`（npm `linkat_tmpfile` 无 CAP 回退路径）；⑤ `linkat(SRC_DIRFD, basename, DEST_DIRFD, path, 0)` dirfd 源+dirfd 目的（Hardlinker/PackageInstall hardlink 安装热路径，区别于 AT_FDCWD 绝对路径；基线确认此形状同样 EACCES，shim 的 openat(dirfd) 解析确实被触发）；⑥ `symlinkat` 相对 target 按 `newdirfd` 解析（验证 `open→openat(newdirfd)` 修复；本设备 symlinkat 不受 EPERM 限制，故测 fix 依赖的解析机制本身）；⑦ `symlink(target, link)` 2-arg（bun `--bun` 造假 node 可执行 `lib.rs:702`；**shim 不 hook symlink 这个符号**——测试确认它在 OHOS 上可用，故无需 hook，若受限则在此暴露缺口）；⑧ `fchmodat2` 经 `syscall()` 入口 + `AT_SYMLINK_NOFOLLOW`（bun `lchmod` 唯一调用点，shim 丢弃 flag 走 `fchmodat` 回退）。
- **透传检查**（2 项）：`getpid()` 对比 `syscall(SYS_getpid)`，以及一次管道读写往返 —— 用来证明拦截全局 `syscall()` 符号来处理 `close_range` 不会干扰其它无关的系统调用。

最近一次真机运行（显式用 `env -u LD_PRELOAD` / `env LD_PRELOAD=...`，而不是依赖 shell 环境状态）：**加了 shim 之后 32/32 项计分检查全部通过**（`ALL PASS (0/32 checks failed)`）。真正的基线（无 shim）下，`4/30` 项计分检查失败（`getpwuid_r` 的 `ERANGE`、3 处 `tmpfile`——和文档记录的沙箱症状完全吻合），另外若干项报告 `INFO` 而不贡献通过/失败（`linkat_eacces_fallback`、`linkat_bun_tmpfile`、`linkat_bun_dirfd`——确认 linkat 的 `EACCES` 怪癖在 AT_FDCWD 绝对路径、`/proc/self/fd`、dirfd 三种形状下都会复现，没有东西可以拿来打分；`close_range`/`fchmodat2` 无 shim 时无条件 `SIGSYS`，所以 close_range 的 9 项检查（含 `close_range_bun_init`、`close_range_bun_spawn`）加上 `CLOSE_RANGE_UNSHARE` 和 `fchmodat2_applies_mode`、`fchmodat2_bun_lchmod`，一共 12 项都报告 `INFO`，因为它们没有 shim 的保护根本没法跑完——见「已知平台行为」）；还有一项（`close_range_libc_fn`）报告 `SKIP`，因为不预加载的话这个符号根本不存在。每一项在基线下能有意义地跑起来的检查，要么通过（对应真实、可用的行为），要么精确地失败/报告文档里记录的那个症状 —— 没有意外情况。

**关于"PASS 是否等于真验证了 shim"的诚实说明**——把 8 个 bun 场景测试按"是否真的触发了 shim 的 fallback 路径"分两类：① **真验证**（6 项，设备上 EACCES/SIGSYS 真复现 → shim 回退被触发 → 断言验证结果）：`close_range_bun_init`/`spawn`（CLOEXEC 正确设置 + fd 保持打开 + stdio 不动）、`linkat_bun_tmpfile`/`dirfd`（拷贝内容正确；dirfd 形状若 shim 误用 CWD 会拷不到 → 测试会挂）、`fchmodat2_bun_lchmod`（mode 正确应用）、`getcwd_unlinked`（deleted-cwd → `/proc/self/cwd` → stat 守卫 → `$HOME`，**计分**——这是 getcwd 修复在设备上唯一可复现的分支）。② **过了但不触发 shim 回退**（3 项，已如实标注）：`getcwd_deep_nested`（tmpfs 里 getcwd 本就成功，shim 回退没被触发，只是深路径 + `/proc/self/cwd` 一致性健全性检查；getcwd 修复的 EACCES+真实 cwd 分支需 hmdfs DAC 拒绝，被测试进程权限绕过，**设备上不可复现**，只能靠代码审查 + 此一致性检查）、`symlinkat_rel_resolution`（直接测 `openat(dirfd,相对)` 解析机制，**不调 shim 的 symlinkat hook**；symlinkat 在本设备不受 EPERM 限制，hook 的 copy 回退路径不可复现，此测试守护 fix 依赖的解析机制）、`symlink_two_arg`（shim **不 hook** `symlink` 符号，测的是真 symlink 在 OHOS 可用——确认 bun 造假 node 不需要额外兜底）。

### 双轨确认：OpenHarmony 容器

**这一节是参照信息，不是部署目标。** 容器里完全没有应用沙箱，这个 shim 要修的每一个症状在那里根本就不存在 —— 容器里的任何检查都不需要这个 shim，也没法验证它在 HarmonyOS 真实、更严格的强制策略下是否真的有效。真正能说明问题、决定能不能上线的是上面的真机结果（**「最近一次真机运行」**）。在容器里跑能带来的价值是：提供一种独立的、机械化的方式，通过对照一个宽松的参照系来确认 shim 的 fallback 逻辑本身是不是正确的，并且在某项检查表现异常时，能把「shim 有 bug」和「平台本身就强制这个限制」这两种情况区分开（下面 `close_range_unshare` 测试 bug 的发现正是靠这个方式）。

按本工作区标准的双轨方法论（OH 容器 = 一台原生 Linux 6.6 内核上的能力上限，没有应用沙箱；HM 真机 = 实际被强制执行的东西 —— 也是这个 shim 真正要应对的、更难的目标），把完全相同的 `test/functional.c` 在 `openharmony` 容器里编译并运行了一遍：

- **基线（无 shim）：22 项计分检查里通过 21 项**（2 项 `SKIP`——一项测试固件假设了一个只在真实 HarmonyOS 设备上存在的临时路径， 容器里没有；`close_range_libc_fn` 照例在不预加载的情况下跳过）。 `close_range` 的 `EINVAL`/`CLOSE_RANGE_UNSHARE` 检查在这里*不需要* shim 就能通过 —— 这个容器的内核正确实现了上游 Linux 的 close_range 语义， 和真机不一样。`getpwuid_r` 也能干净地解析：容器是以真正的 root 身份运行的（uid 0，且存在于 `/etc/passwd` 中），不像沙箱化 HAP 的 uid。 唯一真正的失败项：**`fchmodat2_applies_mode` 在基线下失败，报告一个 干净的 `ENOSYS`** —— 这和 `ohos-preflight` 对 `c5_fchmodat2` 的定性 完全一致（`both_fail`：容器内核确实完全没有实现这个系统调用， 失败方式和真机的 `SIGSYS` 不一样，但两边都是失败）。
- **加了 shim：23/23 全部通过** —— 基线下就能通过的项目依旧逐字节一致地通过（这正是「真实平台已经能用时就是真无操作」这个设计目标，对每一个 真实调用在这里能成功的符号都成立），而 `fchmodat2_applies_mode`—— 唯一的基线失败项——现在也通过了，证明 shim 的 fallback 在容器里遇到 `ENOSYS` 时，和真机遇到 `SIGSYS` 时一样能正确介入：同一套派发逻辑， 两种不同的真实触发条件。

这和 `ohos-preflight` 自己对这几个探针的双轨定性（`a10_close_range`、`i9_getpwuid_r`、`g1_tmpfile`：OH 通过 / HM 失败）完全吻合 ——这是从另一个角度（对一整套探针跑 shim）对最初那轮探针调查结论的独立确认。

在第二套环境里跑测试还顺带发现了一个测试本身的 bug：`close_range_unshare`原本无条件断言「只要加载了 shim，就一定是真机特有的*降级*结果（`ENOSYS`）」。而在容器里，`unshare(CLONE_FILES)` 是真的能用的，所以 shim 在那里正确地实现了*真正*的隔离 —— 旧的断言错误地把这个结果判成了失败。现在已经修复为：既接受一个真实、经过验证的成功结果，也接受一个诚实的 `ENOSYS` 降级结果，这才符合 shim 实际的契约，而不是只认某一个平台的特定限制。

## 性能

`test/bench.c`（`make bench`）测量单次调用的开销。下面的数字是在真实 HarmonyOS 硬件上跑 3 次取平均（基线 vs. `LD_PRELOAD` 加 shim，都显式用 `env -u LD_PRELOAD` / `env LD_PRELOAD=...`，而不是依赖 shell 状态 —— 原因见「已知平台行为」）。这台设备的数字本身单次运行间波动很大（单次运行之间能差 ±20-40%），所以这些数字只能当作数量级参考，不是精确值：

| 测试项 | 基线 | 加 shim | 差值 |
|---|---|---|---|
| `syscall()` 透传（任意*其它*系统调用号） | 258.5 ns/次 | 327.1 ns/次 | **+68.6 ns（+27%）** |
| `close_range` | **N/A —— 每次都崩溃，没有 shim 就没法 fallback** | 108 250 ns/次 | 只有加了 shim 才能跑完这个操作 |
| `close_range` fallback 算法（单独测量） | 106 512 ns/次 | 113 715 ns/次 | 数量级相同（代码本身完全一样）|
| `getpwuid_r` | 68 530 ns/次 | 72 128 ns/次 | **+3598 ns（+5.2%）** |
| `tmpfile` | 3 011 710 ns/次 | 4 231 301 ns/次 | **+1 219 591 ns（+40%）** |
| `getcwd`（成功路径） | 1922.8 ns/次 | 1838.3 ns/次 | 在噪声范围内 |

要点：

- **`close_range` 没有「基线」数字**—— 真实系统调用在这台设备上无条件 `SIGSYS`（见「已知平台行为」），所以一个没加 shim 的进程一次都没法完成 这个操作。约 108–114 μs 的加 shim 开销完全来自 `/proc/self/fd` 枚举 （`cr_do_fallback()`），而且是这台设备上**每一次** `close_range` 调用 都要付出的代价，不是偶尔才发生的最坏情况 —— 目前没有更快的路径可用。 「close_range」和「close_range fallback 算法」两行落在同一个量级 （而不是其中一个可以忽略不计）正是「每次加了 shim 的调用都走了 fallback 路径」这个预期结论的印证。
- 一旦预加载这个库，**每一次**原始 `syscall()` 调用都要付出的税——不只是 `close_range` 调用——在这里是真实存在的、两位数百分比的 相对开销（虽然绝对值仍在亚微秒级别）。对一个大量使用 `syscall()` 的消费者（比如 Bun 的 `c-bindings.cpp`，`close_range`、`pwritev2`、 `exit_group` 全都是走公开的 `syscall()` 符号）影响最大。
- `getcwd` 成功路径的差值在这台设备的运行间噪声范围内 ——与「真实调用已经能用时就是无操作」这个设计目标一致（但不算确凿证明）。
- `tmpfile` 的相对差值是这四个 libc 级符号里最大的，但两个数字都被真实文件系统 I/O 主导（无论哪种情况都是 3-4+ 毫秒）；多出来的开销 来自先尝试真实调用（很快就会失败）、然后再 fallback 到 `mkstemp()`—— 这是「始终优先尝试真实实现」这个设计固有的代价。

### 容器对比：fallback 到底要花多少代价

仅作参照 —— 容器不是部署目标（见上文），它是一个宽松的基线，用来单独衡量 fallback 路径在真机上到底要花多少代价。`test/bench.c`，同一个二进制，在 OpenHarmony 容器里运行（真实系统调用在那里能原生成功，跑 3 次，不需要 shim，因为基线本身走的就是真实路径）：

| 测试项 | HM 真机（强制走 fallback） | OH 容器（原生） | 倍数 |
|---|---|---|---|
| `close_range` | 108 250 ns/次 | ~1867 ns/次 | **真机慢约 58 倍** |
| `getpwuid_r` | 68 530 ns/次 | ~5080 ns/次 | **慢约 13 倍** |
| `tmpfile` | 3 011 710 ns/次 | ~33 500 ns/次 | **慢约 90 倍** |
| `getcwd` | 1922.8 ns/次 | ~272 ns/次 | **慢约 7 倍** |

这个差距不是 shim 本身的开销 —— 而是「真实系统调用/libc 调用直接就能用」和「每次调用都需要一个用户态的变通方案」（`close_range` 靠 `/proc/self/fd` 枚举、`tmpfile` 靠 `mkstemp()`、`getpwuid_r` 靠拼接环境变量）之间真实存在的成本差异。这也是上面收口跟踪表为什么重要的最直接证据：HarmonyOS 补上的每一个缺口，对用了 shim 的消费者来说，都是一次实打实的、两位数倍数级别的性能提升，而不仅仅是修正确性。

### 容器内部：加不加 shim

和上面那张表问的是不同的问题：既然真实调用在容器里本来就都会成功，那单纯*加载了 shim*、但它从来不需要走 fallback 时，到底要花多少代价？

最初跑的 5 轮基线接着 5 轮加 shim 的方案里，加 shim 的数字在每一项指标上都*一致地更低*——这在物理上是不可能的（shim 只能多做一次 `dlsym` 缓存过的真实调用尝试再加一次分支判断，不可能凭空省掉工作），是一个测量伪影的信号：先把 5 轮基线全跑完、再跑 5 轮加 shim，会让这段时间里任何正在「热身」的东西（页缓存、容器/会话级别的一次性开销）都渗进来，造成一种「加了 shim 反而更快」的假象。改成严格**交替**采样（基线/加 shim/基线/加 shim/……），按每一轮的配对差值取平均 —— 这样比拿两个独立的区块做对比，能更好地抵消随时间的漂移。6 轮和 20 轮都还是功效不足（95% 置信区间和点估计本身差不多宽，甚至更宽，`tmpfile` 在 n=6 时看起来的一点点倾向到了 n=20 直接反了个方向）。扩大到**100 轮交替**，每个进程都用 `env -i` 加一份显式的变量白名单启动（`PATH`/`TMPDIR`/`HOME`，`LD_PRELOAD` 只在加 shim 的那一半才加），而不是继承任何 shell 状态，得到一份功效足够的读数：

| 测试项 | 基线均值 | 加 shim 均值 | 配对差值 | 95% 置信区间 | 是否显著 |
|---|---|---|---|---|---|
| `syscall()` 透传 | 168.9 ns | 182.3 ns | **+13.5 ns（+8.0%）** | **±6.8 ns** | **是（t=3.92）** |
| `close_range` | 1814.4 ns | 1824.5 ns | +10.2 ns | ±82.4 ns | 否（t=0.24）|
| `close_range` fallback 算法（独立测量） | 5575.6 ns | 5558.6 ns | -17.0 ns | ±184.3 ns | 否（t=-0.18）|
| `getpwuid_r` | 4212.4 ns | 4216.8 ns | +4.4 ns | ±132.5 ns | 否（t=0.07）|
| `getcwd` | 240.5 ns | 249.8 ns | +9.3 ns | ±11.8 ns | 否（t=1.57，最接近阈值）|
| `tmpfile` | 19 726.9 ns | 19 329.6 ns | -397.3 ns | ±697.4 ns | 否（t=-1.13）|

（配对 t 检验，df=99，临界 \|t\|=1.984。）

**在 n=100 时，终于有一个真实效应从噪声里分离出来了：`syscall()` 透传。**+13.5 ns/次，95% 置信区间 [+6.7, +20.3] ns —— 完全在零以上。这正是进程里每一次非 `close_range` 的 `syscall()` 调用都无条件要经过的路径（shim 的 `syscall()` 覆盖实现里那次系统调用号判断+分支），所以这里出现一个小而真实、始终要付出的固定成本，正是设计所预期的。这和真机上同一测试项的发现（[性能](#性能)一节：+27%）互相印证——同一个底层机制，只是幅度不同（这个容器里基线调用本身就更便宜，而且真机上其它开销来源在这里都不适用），这本身也是一次合理性检验：一个真实的、有物理原因的效应，理应在两种环境下都表现为*某种*正的差值，而这里确实如此。

**其余五项即便到 n=100 依旧不显著**，不过现在置信区间已经窄到有意义了（比如 `getcwd` 在约 240 ns 基线上的 ±11.8 ns，`close_range` 在约 1814 ns 基线上的 ±82.4 ns）—— 这已经不再是「探测不到效应」，而是「在这个精度下确实探测不到效应」。结合真机上的数字一起看（那边 shim 的 fallback 逻辑在大多数调用里是真的会介入的），这两组数据一致地表明：shim 真实世界里的开销几乎全都来自 fallback 路径本身的执行，而不是插桩这个动作本身，也不是任何超出那一个共用的 `syscall()` 入口点之外的、按符号区分的派发逻辑——每个进程都要为经过这个入口付出一份小而固定、现在已经量化出来的成本。

### 真实实现 vs. fallback 实现，直接对比

上面所有数字比较的都是「加了 shim」vs.「没加 shim」——但在容器里，真实调用总是成功，所以 shim 自己的 fallback 逻辑根本不会自然触发，也就没法被测量或者做行为对比。`test/real_vs_fallback.c` 直接、无条件地调用两种实现（fallback 是从 `ohos_compat_shim.c` 逐字复刻出来的，不经过 shim 的派发逻辑），用来回答一个不一样、更直接的问题：**当 fallback 真的在真机上介入时，它的输出和成本，跟它所替代的真实实现比起来怎么样？**

**功能差异**（`--dump` 模式；这个容器没有设置 `LOGNAME`/`USER`，这也符合实际情况 —— 一个沙箱化的 HAP 通常也不会设置）：

| 字段 | `getpwuid_r` 真实实现 | `getpwuid_r` fallback |
|---|---|---|
| `pw_name` | `root` | `u0`（基于 uid 的占位符 —— 没有环境变量可以拿来当真实用户名）|
| `pw_passwd` | `x` | ``（空 —— 没有对应物）|
| `pw_gecos` | `root` | `u0`（和 `pw_name` 保持一致）|
| `pw_dir` | `/root` | `/root`（一致 —— 两边都读的是 `$HOME`）|
| `pw_shell` | `/bin/sh` | `/bin/false`（写死的 —— 没办法知道真实 shell 是什么）|
| `pw_uid` / `pw_gid` | `0` / `0` | `0` / `0`（一致 —— 来自真实系统调用，不是环境变量）|

所以 fallback 在数值型身份字段和 `pw_dir` 上是准确的（只要 `$HOME`设置了，实践中它基本总是设置的），但字符串型身份字段（name/gecos/shell）会退化成合成的占位符。这本来就是已经写明的权衡，只是现在有了逐字段的确认，而不是靠假设。`tmpfile` 的写入/读回往返在真实实现和 fallback 之间完全一致（在这个测试深度下没发现功能差异）。`getcwd` 的 fallback 现在优先 `readlink("/proc/self/cwd")` 返回**真实 cwd**（内核 `d_path()` 不受用户态 `+x` 限制，正是 `getcwd()` 父目录遍历失败而它能成功的原因），用 `stat()` 校验路径仍存在；只有当路径确已消失（rmdir 后 `stat` 返回 `ENOENT`）才回落 `$HOME`。所以对 hmdfs `EACCES` 这类「cwd 有效但 `getcwd()` 走不通」的场景，fallback 给的是正确路径而非 `$HOME` 猜测——这正是让 bun 能撤掉 `ohos_set_pwd`/`cd-prefix` 的关键。

在 5 次重复的 `--dump` 运行之间做了交叉复核，两两互相比对：真实实现和 fallback 两边的输出每次都逐字节完全一致。这些是确定性的、结构性的差异（纯靠 `getenv()` 合成能填哪些字段、不能填哪些字段），不是抖动或者跟时间相关的输出。

**性能**（6 轮，单进程对比 —— 这里的效应大小已经足够大，不像上面加 shim vs. 基线的数字那样需要重的统计处理才能相信）：

| 符号 | 真实实现均值 | fallback 均值 | 倍数 |
|---|---|---|---|
| `getpwuid_r` | 5844.3 ns/次 | 322.6 ns/次 | **fallback 快约 18.1 倍** |
| `getcwd` | 253.6 ns/次 | 14.1 ns/次 | **fallback 快约 17.9 倍** |
| `tmpfile` | 33 893.3 ns/次 | 29 980.1 ns/次 | 约 1.13 倍（每次跑方向都会反转 —— 基本打平）|

这和 `close_range` 正好是**相反**的方向 —— `close_range` 的 fallback（`/proc/self/fd` 枚举）比真实系统调用要慢 40-90 倍（见[容器对比](#容器对比fallback-到底要花多少代价)）。规律是：一个 fallback 到底比真实调用快还是慢，完全取决于真实调用做了多少 fallback 不需要重做的工作——`getpwuid_r` 的真实路径要做一次 NSS/`/etc/passwd` 查找，`getcwd` 的真实路径要做一次真实的内核路径解析系统调用，这两者 fallback 都完全跳过（纯 `getenv()` + 字符串操作，不涉及任何系统调用）；相比之下，`close_range` 的 fallback 得*主动去枚举并关闭*文件描述符，来近似内核本来会直接做的事情；`tmpfile` 的 fallback 依旧要做真实的文件系统 I/O（`mkstemp()`），所以它和真实调用处在同一个成本量级，而不是彻底跳过了工作。这里不存在一条「fallback 就是更慢」或者「fallback 就是更快」的通用规律——得按每个符号具体去核实。

**同一个工具，在真实 HarmonyOS 设备上运行**（`make real-vs-fallback`，单次运行 —— 下面真机上的数字，方向上和 shim 自己在[性能](#性能)一节里 averaged 出的真机基准数据是一致的，这正是用来确认这些不是偶然波动的关键）：

| 符号 | 真实实现（失败）均值 | fallback 均值 | 倍数 |
|---|---|---|---|
| `getpwuid_r` | 58 023.9 ns/次 | 673.1 ns/次 | fallback 快约 86 倍 |
| `tmpfile` | 2 665 842.2 ns/次 | 107 955.5 ns/次 | fallback 快约 25 倍 |
| `getcwd` | 1750.6 ns/次（真实调用在这里*成功*了 —— cwd 存在） | 50.8 ns/次 | fallback 快约 34 倍 |

真机讲的是同一个故事更极端的版本，原因是容器展示不出来的：在真实硬件上，`getpwuid_r` 和 `tmpfile` 的「真实」调用不只是比 fallback 做了更多工作——它们会**直接失败**，而失败本身也是要花真实时间的（分别是 58 μs 和 2.7 ms，仅仅是为了走到 `ENOENT`/`EPERM` 然后放弃），这些时间发生在 shim 的派发逻辑真正开始尝试 fallback 之前。这份「尝试后失败」的成本，是这个 shim 在真机上每一次 `getpwuid_r`/`tmpfile` 调用都要背负的固定开销——这也是为什么这两项在[性能](#性能)一节的「加 shim vs. 基线」数字里显示出最大的相对开销，而且这不是 fallback 设计能够避免的：shim 始终优先尝试真实实现，依据的判断是「一次缓慢但保证结果新鲜的检查，好过一次快但可能出错的假设」（见[前向兼容](#前向兼容优先自动尝试真实系统调用)）。这里 `getcwd` 展示的是*成功*的场景（这次运行时这台真机的 cwd 是存在的），所以它比另外两项便宜，尽管依旧是 fallback 成本的约 34 倍——真实的内核路径解析终归比读一个缓存的环境变量要贵得多。

## License

MIT —— 见 [LICENSE](LICENSE)。`close_range` 的探测再 fallback 模式改编自 [close-range-shim](https://github.com/hqzing/close-range-shim)（MIT）。`scripts/ohos/self-sign.py` 引入自[ohos-bst-light](https://github.com/hqzing/ohos-bst-light)（MIT）。
