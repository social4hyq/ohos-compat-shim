# ohos-compat-shim

面向运行在 HarmonyOS 应用沙箱内的预编译 aarch64/musl 二进制的 `LD_PRELOAD` 兼容 shim。让一个你无法（或不想）重新编译的二进制，在不改动目标二进制任何一个字节的前提下，挺过一批已知的 HarmonyOS 与 OpenHarmony 沙箱差异。

完整的可行性分析（本项目正是从中孵化出来的，包含一张「哪些能修、哪些不能修、为什么」的矩阵）见同级 `ohos-preflight` 仓库中的[`docs/ohos-preload-shim-feasibility.md`](../ohos-preflight/docs/ohos-preload-shim-feasibility.md)。

## 修复了什么

| 符号 | HarmonyOS 上的真实症状 | 默认状态 |
|---|---|---|
| `close_range()` / `syscall(SYS_close_range)` | `SIGSYS` —— 直接杀死进程 | 开启 |
| `getpwuid_r()` | `rc=0, *result=NULL` —— Node 的 `os.userInfo()` 抛异常 | 开启 |
| `tmpfile()` | 返回 `NULL`，`errno=EPERM` —— 沙箱内 `P_tmpdir` 不可写 | 开启 |
| `linkat()` | 沙箱化的安装目标目录里报 `EPERM`/`EACCES` | 开启 |
| `link()` | 同上（hard link 被禁）；musl 的 `link()` 走内联 `syscall(SYS_linkat)` 绕过 `linkat` 动态符号，故 `linkat` 拦截够不着 Node `fs.link`/libuv 这类经 `link()` 符号调用的调用者——本拦截点补这条路径（同样的 copy fallback） | 开启 |
| `syscall(SYS_fchmodat2)` | 真机 `SIGSYS`，OpenHarmony 容器里是 `ENOSYS` —— 两边都失败，但失败方式不同 | 开启 |
| `splice()` | 两个独立缺陷：① 源端 EOF 时返回 `-1/EPIPE`，Linux 返回 `0` —— 所有 splice 拷贝循环在文件尾误报错误；② 写进管道的数据**不唤醒**任何已阻塞的 `poll()`/`epoll_wait()`，导致轮询型消费端的管道永久死锁 | 开启 |
| `epoll_ctl`/`epoll_wait`/`epoll_pwait` | 对 FIFO 写端或 TTY/PTY 注册 `EPOLLOUT\|EPOLLONESHOT` 时，内核不遵守 ONESHOT 的自动缴械，每次等待都重发同一个事件——某些注册（Bun rustix 内联 syscall 发出的）连本拦截点的登记表都学不到，靠"同 key 纯 EPOLLOUT 高频连击"这一形状模式识别 | 开启 |
| `stdout`/`stderr`/`stdin` COPY-reloc 槽位 | 预编译 musl 可执行文件引用标准流时，链接器生成 `R_AARCH64_COPY` 重定位并在 dynsym 里**自定义**该符号；HarmonyOS 的 musl ld.so 按"可执行文件自身定义"解析这份拷贝——8 字节自拷贝，槽位停留在 .bss 初始值 `NULL`。第一次 stdio 调用（如 bun 启动的 `setvbuf(stdout, NULL, _IOLBF, 0)`）即触发加固 libc 断言 `setvbuf: parameter is null` abort。加载期扫描主程序 `.rela.dyn` 回填 libc 真实 `FILE*`——但已确认这段补丁逻辑并非（或不是唯一）让官方 claude-code musl 二进制免于崩溃的原因，真正生效的机制与本 `.so` 完整的符号表/重定位图有关，具体哪一步生效未完全定位；崩溃本身与"用这个 `.so`" 修复本身都是 100% 可复现的确定结论，只是补丁代码与结果之间的因果链没有完全坐实 | 开启 |
| `getaddrinfo()` | 含非法字符（DNS/hostname 字母表之外）的主机名被转发到网络等超时，而非本地立即拒绝；本地进程启动时后台起一个一次性探测线程，探测通过（本地已原生修好）后本拦截点对该进程剩余生命周期变成纯透传 | 开启 |

`pthread_cancel()` 在这个平台上是 musl 的空桩实现，刻意**不**做 shim ——一个 preload 库没办法给调用方注入它所需要的协作式取消点。

`getcwd()`、`symlinkat()`、`getaddrinfo()` 的 AI_ADDRCONFIG 补丁，以及 epoll 管道可读状态合成逻辑和定时切片，已根据真机/容器复测移除；对应探针与专属测试也已清理。历史判断和复测依据见[拦截点去留记录](logs/polyfill-audit/verdicts.md)。

## 前向兼容：优先自动尝试真实系统调用

**大多数**拦截点都会先通过 `dlsym(RTLD_NEXT, ...)` 解析出真实实现并尝试调用它，而不是先走 fallback 逻辑。具体来说：

- `close_range`：`cr_probe_syscall()` 每个进程只跑一次；如果真实系统调用成功，该进程就会缓存 `WORKS` 状态，之后再也不会碰用户态 fallback。同样的探测一次、缓存结果模式也用在 `fchmodat2`（`fc2_probe()`）。
- `getpwuid_r` / `tmpfile` / `linkat` / `link`：**每次**调用都会先调用真实函数；shim 自己的逻辑只在真实调用失败、且失败特征与上表记录的 HarmonyOS 沙箱症状完全吻合时才会介入。
- `getaddrinfo`：本地进程启动后台起一个一次性探测线程（不阻塞调用方任何一次真实查询），用合成的非法字符主机名判断本机真实解析器是否已经本地快速拒绝；探测通过后，这个进程剩余生命周期里 `getaddrinfo()` 变成对真实符号的纯透传。
- `splice()` 的 EOF 语义修正：每次都先调用真实 `splice()`，只在返回 `-1/EPIPE` 且经 `poll()` 判定源端确实是 EOF（而非目标端真损坏）时才改写返回值。

**只有一簇是例外，从不尝试"探测通过就关闭"**：`epoll_ctl`/`epoll_wait`/`epoll_pwait` 的 EPOLLONESHOT 强制（含 FIFO 写端与 TTY/PTY 写端登记、未知 key 的纯 EPOLLOUT 高频连击识别），以及 `splice()` 写入管道从不唤醒轮询等待方这条修复——这两个症状都在真机上稳定 100% 复现，是真实、非负载相关的内核行为差异，不是"探测一次决定要不要修"的候选：只要 `OHOS_COMPAT_SHIM_DISABLE` 没有关闭对应开关就常驻生效。`splice` 写入管道的修复代价见[性能记录](logs/performance.md)中的 `splice_pipe_to_pipe_20mb` 基准，量化了放弃零拷贝的成本。

`getpwuid_r` 的 fallback 用户名来源：优先调用 `OH_OsAccount_GetName()`（`libos_account_ndk.so`，运行时 dlopen、句柄缓存，编译期零 SDK 依赖）取当前系统账号名——但只在查询的 uid 等于进程自身 uid 时（账号 API 没有 uid 参数）；失败或非自身 uid 时回落 `$LOGNAME`/`$USER`，最后退化为 `u<uid>` 占位符。其余字段（`pw_dir`/`pw_shell`/`pw_uid`/`pw_gid`）逻辑不变，账号 API 无法提供。

实际效果：一旦 HarmonyOS 修复某个仍保留的真实探测/尝试型症状，**所有新启动的进程都会自动享受到这个改进**——不需要重新编译 shim 或重新部署。

但这**不代表**平台跟上之后开销就会归零。只要消费者仍然 `LD_PRELOAD` 这个库，每次调用依旧要付出进入拦截函数、以及做一次 `dlsym` 缓存过的真实调用尝试的代价（在[性能记录](logs/performance.md)中实测约为 10 ns/次的透传税，对已经成功的路径来说几乎可以忽略）。真正做到*完全*零开销的唯一办法，是消费者不再为该符号预加载这个库 ——这是**消费者自己的打包决策**，shim 本身做不到，因为它在编译期根本无法预知运行时某台设备的沙箱究竟允许什么。

### 收口跟踪

沿用本工作区现有的「上游收口」惯例（其它 `@ohos-ports/*` fork 也是这个模式）：一旦上游 / 平台修好了对应问题，就下线这层兼容代码，切回原生路径。

| 符号 | 对应跟踪探针 | 何时收口 |
|---|---|---|
| `close_range` | `ohos-preflight` 探针 `a10_close_range` | HarmonyOS 在所有消费者仍支持的系统版本上放行 436 号系统调用 |
| `getpwuid_r` | 探针 `i9_getpwuid_r` | HarmonyOS 给 HAP 分配的 uid 能通过 `/etc/passwd` 解析，或提供了 `nss_ohos` |
| `tmpfile` | 探针 `g1_tmpfile` | 应用沙箱内 `P_tmpdir` 变为可写 |
| `linkat` | 探针 `g5_linkat_eperm` | 目标安装目录不再对硬链接返回 `EPERM`/`EACCES` |
| `link` | 同 `linkat`（同源 hard-link 限制，同一 `g5_linkat_eperm` 探针即判定） | 同 `linkat` |
| `fchmodat2` | 探针 `c5_fchmodat2` | HarmonyOS 放行 452 号系统调用（目前是 `both_fail`：OpenHarmony 容器里也是 `ENOSYS`，所以这项收口不光需要 HarmonyOS 放行，容器那边的内核也得先实现这个系统调用）|
| `splice`（EOF 语义）| 功能测试 `splice_eof_is_zero`（baseline 段即为探针）| 内核修正 `splice()` 的 EOF 语义，源端耗尽时返回 `0` 而不是 `EPIPE` |
| `splice`（poll 唤醒）| 功能测试 `splice_wakes_poll_waiter`（baseline 段即为探针）| 内核让写入管道的 splice 唤醒 poll/epoll 等待者。收口后应删掉 bounce buffer 路径，恢复零拷贝 |
| `epoll_ctl`/`epoll_wait`/`epoll_pwait`（ONESHOT 强制）| 功能测试 `epoll_oneshot_tty_write_end` | 内核对 `EPOLLOUT\|EPOLLONESHOT` 注册正确执行自动缴械，FIFO 写端与 TTY/PTY 均不再重发 |
| `getaddrinfo` | `ohos-shim check` 自带探针 `getaddrinfo`（`ohos_compat_check.c`）| 已在每个受影响进程里自动完成：一次性后台探测确认原生行为已修好后即转为透传，自检探针只用于确认受影响设备的当前行为 |


**copy fallback（`linkat`/`link`）的原子性**：字节拷贝经同目录隐藏临时文件 + `renameat` 落位，目标路径要么完整出现、要么不出现。直接 `O_CREAT|O_EXCL` + 拷贝会让目标在 0 字节时即可见——一个消费进程解析失败 `quick_exit`、而另一线程还在刷缓存时，会留下永久性 0 字节缓存文件（下次加载报 "manifest is invalid" 一类的错误）。临时文件放在 `newpath` 同目录是硬性要求：`renameat` 不能跨文件系统，绝对路径的 `newpath` 配裸临时文件名会把临时文件落到进程 CWD（可能异 fs → `EXDEV`）。

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

这个内核的 `splice()` 还有第三个毛病：**对空管道会一直阻塞，无视 `O_NONBLOCK` 和 `SPLICE_F_NONBLOCK`**。该行为不会返回 `EPIPE`，因此碰不到上面的分支，这里没有处理 —— 记在这里是因为它会让"给 splice 加个非阻塞探测"这类想法直接失效。

## `splice()` 写入管道不唤醒 poll/epoll（为什么这里放弃了零拷贝）

`splice()` 放进管道的数据**不会唤醒任何已经阻塞在该管道读端的 `poll()` 或 `epoll_wait()`**。数据确实进去了 —— 之后再 poll 一次、或者直接 `read()`，立刻就能看到 —— 所以管道的就绪**状态**是对的，坏掉的只有**唤醒**。阻塞在 `read()` 上的读者能被正常唤醒，这就是这个缺陷长期藏在同步读背后的原因。

独立探针实测（等待者先阻塞，300ms 后送 4096 字节）：

| 等待方式 | 由 `splice()` 送入 | 由 `write()` 送入 |
|---|---|---|
| `poll` | 2000ms 超时，不唤醒 | 300ms 唤醒 |
| `epoll` LT | 2000ms 超时，不唤醒 | 301ms 唤醒 |
| `epoll` ET | 2000ms 超时，不唤醒 | 301ms 唤醒 |
| 阻塞 `read` | 301ms 唤醒 | 301ms 唤醒 |

任何"消费端用轮询"的管道都会因此永久死锁。GNU `cat` 在 stdout 是管道时用 `splice()` 供数，于是 `cat big | bun script.js` 永远挂着：bun 阻塞在 `epoll_wait`，cat 填满 512KB 管道后也阻塞在 `splice()`。`cat big | wc -c` 没事（wc 阻塞在 `read`），`dd ... | bun script.js` 也没事（dd 用 `write`）。

**修法**：目标是管道时，把字节经用户态缓冲区搬过去，让管道由 `write()` 供数 —— 它的唤醒是好的。代价是多一次内存拷贝、这条路径上不再零拷贝。可复跑基准见 `make bench` 的 `splice_pipe_to_pipe_20mb`（20MB、16KB 分块，逐块 splice 后立即排空对端管道，避免任一侧缓冲区跨块堆积）——本机最近一次实测：基线（真实零拷贝 `splice`）~2069 MB/s，shimmed（bounce buffer）~1957 MB/s，**慢约 5%**。对一个以正确性为职责的 shim 来说这个交换是划算的，但收口时应当第一时间删掉。

曾经考虑过保住零拷贝的方案：**只扣下最后 1 个字节用 `write()` 送**，靠它产生唤醒。探针确认这个办法确实能唤醒 poll，但它**在真实场景里不成立**：cat 要往 512KB 的管道里搬 524288 字节，`splice()` 填满管道就短返回，那个收尾的 `write()` 根本走不到。一个"恰好在管道满时失效"的唤醒方案没有意义 —— 管道满正是读者一定在等的时刻。

## 已知平台行为（禁用 `close_range` 前请先读这段）

`close_range` —— 通过原始 `syscall(SYS_close_range, ...)` 符号调用，和 Bun 的调用方式完全一致 —— 在这台设备的真实内核上**无条件抛出 `SIGSYS`**，测试过的所有参数组合（合法调用、`first > last`、乱填 flags）无一例外，而且和这个 shim（或任何其它东西）是否被 `LD_PRELOAD` 完全无关。这和 `ohos-preflight` 自己的 `a10_close_range` 探针从一开始的结论完全一致（OH 容器通过，HM 真机失败）—— 这里面并不存在什么「加载第三方库会改变系统允许什么」的深层故事，事情比那个简单得多。（这段的早期草稿曾经得出过相反的结论，原因是那次测试沿用了一个从此前无关测试里残留下来、忘记清理的 `LD_PRELOAD` 环境变量，悄悄地把本该是干净基线的运行替换成了另一个更窄范围的 preload 库。真正的教训不是关于平台的，而是：**验证时永远显式用 `env -u LD_PRELOAD`**，永远不要假设某个 shell 的环境变量是干净的。Makefile 里的 `smoke`/`functional`/`bench` 几个 target 现在都会自动这么做。）

实际影响：

- 既然真实系统调用在这台设备上永远不会成功，shim 那套「探测一次、缓存结果」的设计（`cr_probe_syscall()`）在这台设备上永远只会走用户态 fallback 路径 —— 这里不存在「委托成功」这条路径可以退回，不过代码依旧保留了这条路径， 留给其它设备/系统版本上 `close_range` 有可能真的能用的情况 （对应 `ohos-preflight` 的 OH 容器那条轨道，这个探针在那边是通过的）。
- `OHOS_COMPAT_SHIM_DISABLE=close_range` **不会**还原出「操作依旧能成功」意义上的真实无 shim 基线行为。它还原出的是「预加载了，但没有保护、没有探测」—— 真实调用每次都会 `SIGSYS`，和完全不加载 shim 时一模一样，也就是说进程会崩溃。 只有在你确实想验证这个崩溃行为本身时，才应该禁用 `close_range`。
- 其它探测/尝试型符号（`getpwuid_r`、`tmpfile`、`linkat`、`link`、`fchmodat2`、`getaddrinfo`）完全不表现出这个特性——真实调用有时会成功，禁用它们中的任何一个都能精确还原出真实、无 shim 时的调用结果，这一点已由 `test/functional.c` 验证。

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
# 关闭某个默认开启的拦截点（逗号分隔）——close_range/getpwuid_r/tmpfile/
# fchmodat2/linkat/link/splice/epoll_pipe/getaddrinfo/std_streams 十个
# 全部默认开启，没有对应的 OHOS_COMPAT_SHIM_ENABLE：只有这一个开关
export OHOS_COMPAT_SHIM_DISABLE=tmpfile,link
```

关哪些合适，别靠猜——见下一节 `ohos-shim check`，它会在当前设备上逐项实测
每个拦截点是否还会复现所修的症状，直接给出一行可复制的
`OHOS_COMPAT_SHIM_DISABLE=...`。

## `ohos-shim check` —— 平台能力回归自检

这套 shim 是针对 HarmonyOS 6.1 真机沙箱写的；后续系统版本会逐步放开这些限制，
但没有文档能告诉你**具体哪台设备上放开了哪几项**——只能实测。
`ohos-shim check`（`src/ohos_compat_check.c`，`make check` 本地构建）就是干这个的：

```sh
ohos-shim check                          # 默认表格输出
ohos-shim check --json                   # 机器可读
ohos-shim check --rounds 50              # 加大 splice 写入唤醒缺陷的探测轮数
ohos-shim check --with-shim              # 追加第二遍：预加载 shim 后重跑，验证 shim 修好了
```

逐拦截点给出三态结论：

- **仍需要**——基线复现了 shim 所修的症状，别关。
- **可关闭**——基线行为已经正常，可以安全地塞进 `OHOS_COMPAT_SHIM_DISABLE`。
- **不确定**——`splice` 写入唤醒是间歇性缺陷（复现率随内存压力上升），
  `--rounds` 轮全部没复现也只判「不确定」而不是「可关闭」——**未复现不等于已修复**，
  想确认就加大 `--rounds` 或在真实负载下重跑。

一个例外，即使平台放开也不建议留着不关：`linkat`/`link` 的拷贝 fallback 是有损
语义（丢硬链接身份），一旦判「可关闭」就应该尽快真的关掉，不要因为「反正能用」
就留着。

另外报告一组周边平台能力（`openat2`/`epoll_pwait2`/`clone3` 等裸 syscall、
`ptrace`、`prctl(PR_SET_PTRACER)`、musl 的 dlopen/`.dynsym` 限制……），只供参考，
不产出关闭建议——这些不是这个 shim（或任何 `LD_PRELOAD` shim）能修的东西，完整
90+ 项平台能力矩阵还是要靠 `ohos-preflight`。

依赖 `libexec/ohos-compat-check` + `lib/libohos_compat_checkdep.so`（brew 安装自带）；
也可以只把 `src/ohos_compat_check.c` 和 `src/checkdep.c` 这两份源码 scp 到没装
harmonybrew 的机器上，用 OHOS NDK clang 单独编译（见 `Makefile` 的 `check` target）。

## 构建

本机 / 真机迭代（需要通过 harmonybrew 安装的 OHOS NDK）：

```sh
make             # 构建 libohos_compat.so + test/smoke + test/functional + test/bench + check
make smoke       # 基础 3 项检查，先跑基线再跑加 shim
make functional  # 全面的一致性检查，先跑基线再跑加 shim
make bench       # 单次调用开销数据，先跑基线再跑加 shim
make check       # 跑 ohos-compat-check（`ohos-shim check` 的 payload），强制干净 LD_PRELOAD 基线
make ghost       # epoll ONESHOT 重发的确定性内核故障注入单测（无需 LD_PRELOAD，直接驱动内部状态机）
```

npm 发布的产物由 `.github/workflows/release.yml` 在云端 `ubuntu-latest` runner 上交叉编译产出，用的是同一套 OHOS NDK clang 调用方式，用 [ohos-bst-light](https://github.com/hqzing/ohos-bst-light) 的 `self-sign.py` 自签名，并且要先通过真机 smoke test 才会进入 GitHub Release / npm 发布环节。不依赖任何 Homebrew formula，也不需要自建构建 runner。

## 测试

`test/functional.c`（`make functional`）覆盖的范围远不止 smoke.c 的 3 项基础检查。测试覆盖度是刻意对照这几个调用现有的参考测试套件来校准的——[Linux 内核自测套件](https://github.com/torvalds/linux/blob/master/tools/testing/selftests/core/close_range_test.c)和 [LTP](https://github.com/linux-test-project/ltp) 的 `close_range01/02.c`、`linkat01/02.c`——而不是随手想到什么测什么。直接对照 LTP 在上线前就抓出了两个真实的 bug——见上面「已知平台行为」一节里 `close_range` 的 `EINVAL` 校验和 `CLOSE_RANGE_UNSHARE` 降级处理。

- **`close_range`**（12 项检查）：多 fd 区间（关闭中间、两端不动）、一个*稀疏的宽区间*（`fd..fd+90`，对照 LTP 里 dup 到高位 fd 的用例）、`CLOSE_RANGE_CLOEXEC`、`first > last` → `EINVAL`、乱填 flags → `EINVAL`（而不是崩溃）、通过一个真实的 pthread 触发 `CLOSE_RANGE_UNSHARE`（对照 LTP `close_range02.c` 用 clone(2) 的用例）、并发压力（8 线程 × 100 次 `CLOSE_RANGE_UNSHARE`）、fork 安全性（30 次 fork，另一线程同时churn `epoll_ctl`）、连续两次调用结果一致（验证探测结果缓存的正确性）、以及 `close_range()` 这个 libc 风格包装函数与原始 `syscall()` 路径结果一致，加上两个照搬 Bun 真实调用形状的用例（init 期与 spawn/reload 期的 fd 泄漏防护）。
- **`getpwuid_r`**（2 项检查）：缓冲区太小时必须返回 `ERANGE`；连续两次调用必须返回逐字节一致的数据。
- **`tmpfile`**（3 项检查）：完整的写入/读回往返、3 个同时存在的临时文件之间互不污染、以及连续 50 次打开+关闭循环全部不失败。
- **`linkat`/`link`**（5 项检查）：`EEXIST` 会原样透传（对照 LTP `linkat02.c`）；在 `$TMPDIR` 里真实复现出来的 `EACCES` 会触发 copy fallback 且内容正确（`linkat`/`link` 各一项）；`linkat` 经 `/proc/self/fd/<N>` 物化 `O_TMPFILE`、以及 dirfd 源+dirfd 目的两种照搬 Bun 真实调用形状的用例。
- **`fchmodat2`**（3 项检查）：应用请求的 mode；对符号链接目标不跟随（`AT_SYMLINK_NOFOLLOW`）；照搬 Bun `lchmod` 唯一调用点的形状。
- **`splice`**（3 项检查）：源端 EOF 返回 `0` 而非 `-1/EPIPE`；目标端真损坏时 `EPIPE` 仍然照常报出；写入管道后能唤醒一个已经先阻塞的 `poll()` 等待方。
- **`epoll`**：`EPOLLOUT|EPOLLONESHOT` 注册在 FIFO 与 TTY 写端上都恰好只投递一次（第二次等待不会重发）。
- **透传检查**（2 项）：`getpid()` 对比 `syscall(SYS_getpid)`，以及一次管道读写往返——用来证明拦截全局 `syscall()` 符号来处理 `close_range` 不会干扰其它无关的系统调用。

最近一次真机运行（显式用 `env -u LD_PRELOAD` / `env LD_PRELOAD=...`，而不是依赖 shell 环境状态）：**加了 shim 之后 31/31 项计分检查全部通过**。真正的基线（无 shim）下，`7/28` 项计分检查失败（`getpwuid_r` 的 `ERANGE`、3 处 `tmpfile`、`splice` 的 EOF 语义与写唤醒、`epoll` 的 ONESHOT 强制——都和文档记录的沙箱症状完全吻合），另外若干项报告 `INFO` 而不贡献通过/失败（`close_range`/`fchmodat2` 无 shim 时无条件 `SIGSYS`，相关检查没有 shim 的保护根本没法跑完——见「已知平台行为」），还有 3 项报告 `SKIP`（不预加载的话 `close_range()` 这个符号根本不存在）。每一项在基线下能有意义地跑起来的检查，要么通过（对应真实、可用的行为），要么精确地失败/报告文档里记录的那个症状——没有意外情况。

### 双轨确认：OpenHarmony 容器

**这一节是参照信息，不是部署目标。** 容器里完全没有应用沙箱，这个 shim 要修的每一个症状在那里根本就不存在 —— 容器里的任何检查都不需要这个 shim，也没法验证它在 HarmonyOS 真实、更严格的强制策略下是否真的有效。真正能说明问题、决定能不能上线的是上面的真机结果（**「最近一次真机运行」**）。在容器里跑能带来的价值是：提供一种独立的、机械化的方式，通过对照一个宽松的参照系来确认 shim 的 fallback 逻辑本身是不是正确的，并且在某项检查表现异常时，能把「shim 有 bug」和「平台本身就强制这个限制」这两种情况区分开（下面 `close_range_unshare` 测试 bug 的发现正是靠这个方式）。

按本工作区标准的双轨方法论（OH 容器 = 一台原生 Linux 6.6 内核上的能力上限，没有应用沙箱；HM 真机 = 实际被强制执行的东西 —— 也是这个 shim 真正要应对的、更难的目标），把完全相同的 `test/functional.c` 在 `openharmony` 容器里编译并运行：

- **基线（无 shim）**：`close_range` 的 `EINVAL`/`CLOSE_RANGE_UNSHARE` 检查在这里*不需要* shim 就能通过——这个容器的内核正确实现了上游 Linux 的 close_range 语义，和真机不一样。`getpwuid_r` 也能干净地解析：容器是以真正的 root 身份运行的（uid 0，且存在于 `/etc/passwd` 中），不像沙箱化 HAP 的 uid。`fchmodat2_applies_mode` 在基线下失败，报告一个干净的 `ENOSYS`——这和 `ohos-preflight` 对 `c5_fchmodat2` 的定性完全一致（`both_fail`：容器内核确实完全没有实现这个系统调用，失败方式和真机的 `SIGSYS` 不一样，但两边都是失败）；`splice`/`epoll` 的相关缺陷在容器里均不复现（不同内核，符合预期）。
- **加了 shim**：基线下就能通过的项目依旧逐字节一致地通过（这正是「真实平台已经能用时就是真无操作」这个设计目标），`fchmodat2_applies_mode` 现在也通过，证明 shim 的 fallback 在容器里遇到 `ENOSYS` 时，和真机遇到 `SIGSYS` 时一样能正确介入：同一套派发逻辑，两种不同的真实触发条件。

这和 `ohos-preflight` 自己对这几个探针的双轨定性（`a10_close_range`、`i9_getpwuid_r`、`g1_tmpfile`：OH 通过 / HM 失败）完全吻合 ——这是从另一个角度（对一整套探针跑 shim）对最初那轮探针调查结论的独立确认。

在第二套环境里跑测试还顺带发现了一个测试本身的 bug：`close_range_unshare`原本无条件断言「只要加载了 shim，就一定是真机特有的*降级*结果（`ENOSYS`）」。而在容器里，`unshare(CLONE_FILES)` 是真的能用的，所以 shim 在那里正确地实现了*真正*的隔离 —— 旧的断言错误地把这个结果判成了失败。现在已经修复为：既接受一个真实、经过验证的成功结果，也接受一个诚实的 `ENOSYS` 降级结果，这才符合 shim 实际的契约，而不是只认某一个平台的特定限制。

## 性能

真机与 OpenHarmony 容器的基准数据、fallback 对比和测量方法属于历史参考，详见[性能记录](logs/performance.md)。这些数据用于说明开销数量级，不能代替目标设备上的复测。

## License

MIT —— 见 [LICENSE](LICENSE)。`close_range` 的探测再 fallback 模式改编自 [close-range-shim](https://github.com/hqzing/close-range-shim)（MIT）。`scripts/ohos/self-sign.py` 引入自[ohos-bst-light](https://github.com/hqzing/ohos-bst-light)（MIT）。
