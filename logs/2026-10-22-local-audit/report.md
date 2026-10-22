# 本机 shim 精简审计

## 结论

本次没有发现可以直接删除或在本机全局关闭的完整拦截项。现有十个运行时开关对应的功能，都有本次真机复现证据；`std_streams` 由真实 Claude 二进制补充验证，不能依赖通用检查器判断。

可推进的是构建分层、测试清理和实现重构，不是按旧记录删功能。本轮未修改运行时代码、formula 或全局环境，也没有重新编译或发布。

## 验证对象和边界

- 本机：HarmonyOS，HongMeng Kernel 1.13.0，aarch64；完整环境和产物 SHA256 见 `environment.txt`。
- 实际加载：Harmonybrew 已安装的 `ohos-compat-shim/0.8.1`，formula 指向提交 `83aed114e1273d5cd2bcedaf459c061fa77c53d6`。
- 工作树 HEAD 是 `d6c3237`，仍在 v0.8.0。与已安装版本的源码差异只有 epoll ghost 等待上限从 32ms 改到 8ms及相应注释。本次结果不能冒充对 HEAD 新构建产物的完整回归。
- 已安装检查器随包安装的源码与工作树 `src/ohos_compat_check.c` 通过 `cmp` 核对一致。
- 仓库残留的 `test/functional` 二进制早于当前源码，包含已删除的 getcwd 兜底预期。它用于辅助复现，不能作为当前源码的完整测试门禁。
- 全部基线显式清除 `LD_PRELOAD`；测试进程未设置禁用开关，专项 A/B 除外。日志位于本目录。

## 逐项实测

执行 `ohos-shim check --json --rounds 50 --with-shim`，两遍输出分别是无 shim 基线和加载 shim 的结果。

| 功能 | 无 shim | 加载已安装 shim | 建议 |
|---|---|---|---|
| close_range | SIGSYS，包括无效参数；unshare(CLONE_FILES) 也 SIGSYS | 基础关闭及参数校验正常；UNSHARE 返回 ENOSYS | 保留，不可把 UNSHARE 简化成伪成功 |
| fchmodat2 | SIGSYS | 兼容调用成功 | 保留 |
| getpwuid_r | rc=0，但 result=NULL | 返回当前用户记录 | 保留 |
| tmpfile | EACCES | 创建成功 | 保留；设置 TMPDIR 并不能替代 libc tmpfile 拦截 |
| linkat / link | cache、HOME、工作目录等候选路径均 EPERM/EACCES | 复制降级成功 | 两者都保留；复制不等于硬链接，不应向依赖 inode 身份的应用承诺等价 |
| splice EOF | EOF 被报告为 EPIPE | 返回 0 | 保留 |
| splice 唤醒 | 压力下 50/50 未正确唤醒 | 压力下 0/50 复现 | 保留缓冲转发，不能只留 EOF 修正 |
| epoll_pipe | FIFO、PTY 写端 ONESHOT 均重复投递 | 两者均不再重复投递 | 保留；本次不能证明未知注册 storm/ghost 处理可删 |
| getaddrinfo | 非法主机名约 2135ms 后失败 | 约 3ms 拒绝 | 保留；与历史审计中的本机快速拒绝结果不同 |
| std_streams | 通用检查器无法覆盖 | 见真实应用 A/B | 保留 |

检查器第二遍的 `DROPPABLE` 代表加载 shim 后症状消失，不是删除建议。第一遍的 `disable_recommendation` 为空。

### DNS 独立交叉验证

使用 musl Python 的 `socket.getaddrinfo()`，同一进程交替查 `bad host!` 和 `example.com`，重复三次：

- 无 shim：非法主机名分别耗时 2105、2044、2033ms，正常域名三次均解析成功。
- 有 shim：非法主机名分别耗时 4、0、0ms，正常域名三次均解析成功。

这排除了“网络完全不可用，所有查询都快速失败”的解释；但没有抓包，不能把耗时直接当成已证明发出了某种 DNS 数据包。该补丁仍有本机收益，原生优先的后台探测暂不建议删除。

日志：`native-dns.txt`、`shim-dns.txt`。

### std_streams 真实应用 A/B

绕过 wrapper，直接运行本机 Claude Code 2.1.306 的 musl 可执行文件 `--version`。按基线、默认、禁用 std_streams、全部禁用的顺序交替执行三轮，每次设 15 秒超时。

| 模式 | 三轮结果 |
|---|---|
| 无 LD_PRELOAD | 全部 abort，exit 134 |
| 默认 shim | 全部正常打印版本，exit 0 |
| 仅禁用 std_streams，其他默认 | 全部 abort，exit 134 |
| 全部开关禁用 | 全部 abort，exit 134 |
| 补充：只保留 std_streams | 全部 SIGSYS，exit 159 |

对这个版本，std_streams 开关已明确改变启动结果，不能再拿历史“禁用也能正常启动”的记录作为精简依据。这仍不等于完整定位了每一步 ELF 重定位机制。只保留它仍不足以让应用启动；此次没有追踪只保留模式的具体 SIGSYS 系统调用。

日志：`claude-version-ab.txt`、`claude-only-stdio.txt`。仅覆盖版本输出，不代表交互、spawn、网络等全流程验收。

### smoke 和旧 functional 二进制

- smoke：无 shim 的账户查询和 tmpfile 两项失败；有 shim 全部通过。
- 旧 functional：无 shim 7/43 失败，另有 SIGSYS 捕获及跳过项；有 shim 1/46 失败。
- 加 shim 的唯一失败是 `getcwd_unlinked`，旧二进制要求 cwd 删除后回退 HOME，当前源码已明确删除该行为。这是测试产物陈旧，不能为了把旧测试变绿恢复该补丁。

日志：`smoke-*.txt`、`functional-*.txt`。

## 哪些内容值得精简

以下为源码审查提出的候选，不是已经验证可删除的代码。应先写测试，再经 formula/CI 构建后真机回归。

1. **先清理过期测试产物及注释。** 当前约 2365 行运行时源码不等于有同等数量的有效逻辑。旧 functional 还测 getcwd/symlinkat 旧行为；源码仍有“EPOLLIN synthesis”遗留描述，TMPDIR 构造函数仍写 `/tmp` 只读。清理这些内容不改变行为，也不会丢兼容能力。
2. **glibc 构建按编译期裁剪。** `parse_toggle_masks()` 已让 glibc 默认关闭 std_streams、getpwuid_r、tmpfile、link/linkat、getaddrinfo，但相应实现和导出符号仍编进库。可研究将这些 musl 专用段编译排除，减少 glibc 库的无用实现和符号拦截。不能据此删除 musl 实现；改变符号表后还需真实 glibc 消费者验证。
3. **保留共享复制实现，不要把 link 当作 linkat 的冗余入口。** musl 的 link 可以直接发原始 syscall，绕过 linkat 符号；本次两条路径都实际失败，两个入口都有价值。可以整理调度代码，不能只删一个入口。
4. **epoll 可拆模块，不宜直接删兜底。** 它是主要复杂度来源，包含已登记 ONESHOT、无法拦截注册的 storm、内核删除后仍重复投递的 ghost，以及 fd 生命周期和 fork 状态。普通 FIFO/PTY 检查只证明核心修复需要，不足以证明其他路径不需要。精简前必须补齐直接 syscall/内联 svc、ADD/MOD 重挂、fd 复用、fork、DEL 后 ghost、正常高频 EPOLLOUT 不误杀等回归。
5. **缓存账号库探测失败，属于优化而非删功能。** `ohos_get_name_resolve()` 在 dlopen 失败时不缓存失败，下次继续尝试；可以用一次性初始化统一成功和失败缓存。需要先确定运行期间库可用性不会改变，并测试并发及 fork，不能仅凭历史容器开销结论改动。

不建议通过硬编码本机永远阻止 close_range/fchmodat2，来删除 SIGSYS 探测；这会牺牲对其他设备的原生优先行为。也不建议以“全部禁用”作为零开销对照：构造函数、导出符号和部分 fd 清理路径仍在，禁用开关不等于没有加载库。

## 后续顺序

先修复测试产物与源码版本的可追溯性，再单独做 glibc 编译期裁剪，最后考虑 epoll 内部整理。若目标是减少维护负担，优先拆分责任和补测试；若目标是减少体积或运行开销，应先确定体积、启动时间或热点调用的指标，做交替 A/B，不能用行数替代收益。
