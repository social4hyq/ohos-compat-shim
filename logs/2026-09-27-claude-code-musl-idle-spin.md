# 诊断：musl 版 claude-code（bun 单文件）经 LD_PRELOAD 运行时闲时 CPU ~150-180%

日期：2026-09-27/28 · 环境：HarmonyOS 真机应用沙箱 · 结论级别：已实测定性（v4，撤回 v3 的「已知极限」结论）

> **v4 更新（2026-09-28，`fix/musl-tui-idle` 分支）：v3 结论错误，已修复，不是
> LD_PRELOAD 结构性极限。** v3 把交互 TUI 闲时仍打满一核归因于「bun 主循环走
> raw 内联 `epoll_pwait2`，整段不可拦截」。这次用 PC 采样直接证伪：在 v3 已
> 装的 v0.6.1（含 ①②③④⑤ 全部五层修复）上，`mainprobe`（10ms 间隔 `SIGPROF`
> 采样，4946 个主线程样本）**95.6% 的 PC 落在 `ld-musl` 的 `epoll_ctl+0x4c`**
> （即 svc 返回后的用户态尾段），对应 LR（去 PAC 位）解析到
> `libohos_compat.so` 的 `0x6fb4`——落在 `ep_shim_after_wait_ex()` 的
> ONESHOT 强制段内部，不是任何裸 syscall 路径。也就是说等待本身完全走的是
> shim 已经在拦截的 libc 符号，只是拦截器自己有两个 bug：
>
> 1. **剥事件的已知（matched）分支从不告知调用方要限速**：`*out_suppressed`
>    只在「未知 key 风暴」分支（v3 的⑤）被置位，v3 之前就存在、覆盖面更广的
>    「已知条目重发」分支（disarmed 后收到重发事件、剥除+尝试 DEL 的那段）
>    完全没碰这个输出参数，于是 `ep_shim_wait()` 现有的 2ms 步进从未对这条
>    路径生效——事件确实被剥掉了（app 从未看到假事件），但 shim 自己的重等
>    循环全速空转，把自旋从「调用方」搬进了「shim 内部」，恰是最初设计这层
>    强制时自己在注释里就点名要避免的失败模式。
> 2. **DEL 在这台设备上对仍在 fdinfo 里的合法 tfd 也会返回 `ENOENT`**：用一个
>    观测型 preload（`test/delprobe.c`，串在 compat 之后接管
>    `dlsym(RTLD_NEXT,"epoll_ctl")`）实测交互 TUI 20 秒闲时窗口：
>    `epoll_ctl(epfd=4, EPOLL_CTL_DEL, fd=10)` 被调用 **860561 次，100%
>    返回 -1/ENOENT**（约 43000 次/秒，每次 ~7us 纯 syscall 开销），而同一
>    时刻 `/proc/self/fdinfo/4` 里 `tfd 10` 始终**原样挂着**、掩码
>    `4000001c`（`ONESHOT|EPOLLOUT|ERR|HUP`）分毫不差——不是 fd 用错了（重
>    查 fdinfo 也是同一个 tfd），是这台内核的 `epoll_ctl(DEL)` 对这类注册
>    干脆不可靠。旧代码没有对失败的 DEL 做任何限流，于是在每一次重发事件上
>    都重新尝试这个注定失败的 DEL。
>
> 两个 bug 叠加：剥事件不限速 + 对必败的 DEL 无限重试 = 主线程常驻 R 态、
> `epoll_ctl`/`epoll_pwait` 全速空转。**修法**（详见 README「已知极限」段落
> 修订）：`del_tried` 把 DEL 尝试限制到每次重臂最多一次；已缴械条目的每次
> 重发剥除都无条件请求限速（不管 DEL 成功、失败还是幽灵投递），且请求的
> sleep 随连续重发指数增长（2ms 起步、封顶 32ms，收到下一次 ADD/MOD 复位到
> 2ms）——这个 ghost 对这类注册可能是**永久性**的（DEL 永远 ENOENT，内核侧
> 从未真正安静过），固定步进要为整段空闲会话付费，指数退避把稳态成本压到
> 最低同时不影响真实按键的感知延迟（32ms 远低于人体感知的输入延迟阈值）。
>
> **真机 A/B**（`test/ab_compare.py`：同一份官方 `claude-code` 2.1.283 musl
> 二进制、同一 TUI 空闲画面（主题选择/diff 预览静态帧），新旧 `.so` 交替跑
> 3 轮、掐头 4 秒进程启动开销、对稳态区间算 jiffies/秒配对差值）：
>
> | 轮次 | v0.6.1（旧） | 本修复（新） | 差值 |
> |---|---|---|---|
> | 0 | 100.45% | 1.24% | 99.20pp |
> | 1 | 100.45% | 1.32% | 99.13pp |
> | 2 | 100.69% | 1.06% | 99.63pp |
>
> 三轮几乎零方差、每轮差值 ~99 个百分点——与「换 tap 版 bun」路线实测的 ~1%
> 打平。回归：`make ghost`（新增的确定性内核故障注入单测，直接复现「DEL 一直
> ENOENT」与「DEL 提前被真删」两种场景，共 4 项，修复前 4 项全部 FAIL——1ms
> 内调用 1001 次；修复后 4 项 PASS，调用数降到个位数/十位数）全绿；
> `make functional` 46/46 不回归。
>
> **结论修正**：musl 逃生通道对交互 TUI 与非交互形态（`-p`/`mcp serve`/
> 管道）现在**同样是完整修复**，不再需要为交互场景默认改回 tap bun；v3 提出
> 的「LD_PRELOAD 结构上无解」判断是过早下的，实际问题在 shim 自己的限速逻辑
> 有遗漏，不在等待路径本身摸不到。

> **v3 增补（交互 TUI 形态，已被 v4 部分推翻——见上）**：用户报告「musl 版
> 闲时 CPU 仍很高」属实——交互
> TUI（PTY stdio）存在**第四层**自旋。修复链（分支 `fix/epoll-oneshot-tty`，
> commit 9ce9472）：①PTY 写端登记（TCGETS 缓存判定）；②未知 key 纯 EPOLLOUT
> 风暴抑制（rustix 内联 arm 不可见 → 同 (epfd,data) ≤10ms 间隔连击 ≥20 判定
> 风暴 → fdinfo 反查 fd 内核侧 CTL_DEL + 剥除 + re-wait 2ms 步进）。修复后
> 风暴活跃 session 实测 **129%+56% → 12 秒 0 ticks**，functional 46/46。这一
> 层修复本身没错（诊断的是另一个真实缺陷，未知 key 风暴），错的只是它之后
> 附带的「结构性极限」判断——见 v4。
> **但存在结构性极限**：TUI 主循环的等待本身可能走 bun raw 内联
> `epoll_pwait2`（svc 直发）——该层风暴整段不跨任何可拦截符号（10s trace
> 0 字节、stime 主导 80%），**LD_PRELOAD 无解，只有源码修复（tap bun）能治**。
> 且内核行为间歇漂移（同一晚上有无自旋两种 session 并存，与 ohos-bun
> 408a29c0b4 revert 记录的"探针结果互相冲突"一致）。
> **运维结论：`~/.shellrc` 默认改回 `CLAUDE_CODE_RUNTIME=bun`（TUI 日常），
> musl 逃生通道仅用于非交互形态（`-p` / `mcp serve` / 管道——这些已被
> shim 完整治好，实测 1%/0%）。**

> v2 修订：初版报告有两处错误，已由追加实验修正——
> ①「等待走 raw syscall、LD_PRELOAD 不可见」**错**：waitprobe 实测主循环等待走
> libc `epoll_pwait` 符号（~51 万次/秒，全部立即返回），compat-shim 完全可见；
> ②「LD_PRELOAD 无解」过强：内核违反的是 EPOLLONESHOT 缴械语义，shim 侧存在
> 原则性修法（见「shim 侧可修性评估」）。**最终建议不变：换 tap 版 claude-code。**

## 一句话结论

自旋根源是 bun PipeWriter 的 **EPOLLOUT 假就绪风暴**：stdout/stderr 管道写端以
`EPOLLONESHOT|EPOLLOUT|ERR|HUP` 注册后，内核无视 ONESHOT 缴械、对空管道
（level-true 可写）在**每次** `epoll_pwait` 上重发 2 个 EPOLLOUT 事件（实测
22.8M 事件/40s，100% 纯 EPOLLOUT），上游 bun 空缓冲分支不做 unregister →
永续重触发（主线程 120% + mimalloc scavenger 下游 ~50%，futex 唤醒风暴
~12.5 万次/秒）。**不是 FIONREAD 可修的 POLLIN 方向假就绪（IN 事件占比
0.0002%）；推荐直接换 tap 版 claude-code（同负载实测闲时 1%）。**

## 实验矩阵（全部同机同夜，配置/HOME 隔离，未触碰现役会话）

| # | 实例 | stdout 形态 | preload | 主线程 | scavenger | 关键观测 |
|---|---|---|---|---|---|---|
| 1 | musl 2.1.220 `mcp serve` | 管道 | compat | **114-126%**（R 240/240，从不入睡） | **52-67%** | 合计 ≈167-176% 单核 |
| 2 | 同上 + `OHOS_COMPAT_SHIM_DISABLE=epoll_pipe` | 管道 | compat | 120% | 56% | **消融：自旋不变，shim 拦截器非诱因非刹车** |
| 3 | 同上 + trace-shim | 管道 | trace+compat | 113% | 67% | 10s/213 万条：**99.97% futex(98)**；epoll_ctl 仅 7 次；read/write 近零 |
| A | 同上 | **普通文件** | probe+compat | **1%**（wchan=EVENTPOLL 安静阻塞） | 0% | **因果差分：管道写端在→风暴在；不在→灭** |
| B | tap `claude mcp serve`（bun 2.1.283，含修复） | 管道 | 无（tap 形态） | **1%**（wchan=EVENTPOLL） | 0% | **解药 A/B：同负载形态下零自旋** |

对照：现役 tap 会话进程 9577/10849 闲时 10s utime+stime 零增长。

## 直接证据

1. **事件掩码实测**（waitprobe：仅观测不改行为的 epoll/poll 符号探针，
   叠在 compat 之前）：
   ```
   epoll_pwait(4,1024,14) = 2: [ev=0x4 data=0x80000052bf3a800] [ev=0x4 data=0x80000052bf3a828]
   ```
   每次等待立刻返回 2 个 `ev=0x4`（纯 EPOLLOUT）；累计 **OUT=22,799,968 /
   IN=40 / ERR=HUP=RDHUP=0**。两个 data u64 与 fdinfo 注册的 tfd 8/9
   （stdout/stderr 管道写端 PipeWriter watcher）一一对应。
2. **注册表形态**（/proc/PID/fdinfo/3）：
   - musl：`tfd 8/9: events=0x4000001c`（ONESHOT|EPOLLOUT|ERR|HUP）**常驻**，
     缓冲为空、全程 epoll_ctl 仅 7 次（无 MOD 重臂）→ 内核违反 ONESHOT 缴械。
   - tap（含修复）：fd 8/9 **不在注册表**——OHOS bun 修复第一层
     （空缓冲唤醒即 `poll.unregister()`）的形态证据。
3. **等待路径**：musl 二进制动态导入 `epoll_pwait` 等符号（JUMP_SLOT 重定位）；
   waitprobe 计数 1140 万次/22s ≈ 51 万次/秒、99.99% 立即返回 >0——主循环等待
   走 libc 符号（trace-shim 看不见只因它没 wrap 这些符号，不代表不可拦截）。
4. **futex 风暴是下游**：主线程 125.5 万次 `FUTEX_WAKE`（70.9 万 rv=0 + 54.6 万
   rv=1）↔ scavenger 85.9 万次 `FUTEX_WAIT`（10s 窗口）——由自旋循环的分配行为
   驱动，非独立根源。
5. **权威交叉验证**：ohos-bun `85f998bf38`（真机 A/B 后经 `15affff046` 保留）
   记录同一机制并修复（空缓冲唤醒 force-unregister + 同 fd 唤醒连击退避；
   11s 窗口 CPU 16.45s→0.04s）。

## 判定与 shim 侧可修性评估

- 任务假设 A（密集 poll/ppoll/epoll_wait 假就绪 → FIONREAD 校验可修）：
  **形态命中（epoll_pwait 立即返回假就绪，51 万次/秒）但方向不符**——假就绪
  100% 是 EPOLLOUT（可写），FIONREAD 校验的是读侧字节，结构上不适用
  （POLLIN 事件全程仅 40 次）。**该 interceptor 无处可加。**
- 任务假设 B（futex 唤醒风暴/mimalloc scavenger → 无解）：风暴真实存在但为
  **下游**；主线程从不入睡 + 每轮先有 epoll 事件后有 futex WAKE。
- **真正的内核缺陷**：EPOLLONESHOT 缴械不被执行（注册时明确请求了
  0x40000000 位）。理论上 compat-shim 可修：`ep_shim_ctl_done()` 目前只登记
  EPOLLIN 的 FIFO（0x4000001c 无 EPOLLIN 位被丢弃），需扩为同时登记
  ONESHOT+EPOLLOUT 的 FIFO 条目，并在 `ep_shim_wait()` 的真实返回路径
  （`ep_shim_after_wait_ex`）对已缴械条目剥除事件、MOD 重臂——把 ONESHOT
  语义在用户态还原。但这给所有消费者的热路径加新语义风险（剥事件一旦误判
  会饿死真实写者），而修复版 bun 已存在且经真机 A/B；性价比上应直接用 tap 版。
- futex 限速、剥 EPOLLOUT 无差别过滤等路线均不可行（正确性原因见 v1 表）。

## 结论

**【2026-09-27 深夜更新：shim 侧修复已实现并实测通过——回答是「能修」。】**
分支 `fix/epoll-oneshot-enforce`（commit 91e4f4d）在 epoll_pipe 拦截器内实现了
ONESHOT 缴械强制：扩登记 ONESHOT|EPOLLOUT 的 FIFO 条目；首投递放行并缴械，
对已缴械条目的重发事件剥除并**内核侧 CTL_DEL**（纯用户态剥除只会把自旋搬进
shim 内部）；app 的重臂 MOD 由 shim 翻译回 ADD、DEL 的 ENOENT 被吞掉，app
对 enforcement 全程无感。实测：musl claude 空闲 CPU **120%+50% → 1%+0%**，
fdinfo 中风暴注册消失，kdel 后 MCP 请求/响应仍正常走 stdout 管道，functional
45/45、smoke 3/3 全绿。未合并 main，待按仓库评审纪律走 PR。

（原始结论保留：换 tap 版 claude-code 依然是更省事的路线——修好的 bun 已经
存在；shim 修复适合「必须保留 musl 单文件」的场景。）

**换 tap 版 claude-code**（`brew install claude-code`，跑在含 CPU 修复的 OHOS
bun 上）——同负载实测闲时 1%。musl 单文件若因故必须保留，唯一原则性 shim 修法
是 ONESHOT 缴械强制（上述扩登记+剥事件），需按本仓「polyfill discipline」标准
单独评审，不建议作为首选。

## 复现/采样材料

- 原始工件（trace 日志、采样器、fdinfo 快照、waitprobe 源码）为会话本地产物，
  不入库；`waitprobe.c`（epoll/poll 符号观测探针）与关键数字均已摘录在本文档，
  可按需重建
- musl 二进制需复制到 EL2 并 `selfsign` 后方可 exec（hmdfs 属主不可执行）；
  tap 版 `CLAUDE_CODE_RUNTIME=musl` 路径会自动完成下载+签名+经 ohos-shim 启动
- 采样方法：分线程读 `/proc/PID/task/*/stat`（utime/stime 差分）+ `wchan`/state
  分布；syscall 形态用 ohos-trace-shim（futex 风暴）+ 自写 waitprobe（epoll 符号
  路径与事件掩码）
