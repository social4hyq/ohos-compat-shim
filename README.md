# ohos-compat-shim

用于 HarmonyOS / OpenHarmony 应用沙箱内 **动态链接的 aarch64/musl 程序**的 `LD_PRELOAD` 兼容库。它拦截部分 libc 函数和 `syscall()` 调用，在已知系统行为不兼容时尝试安全降级；不修改目标程序本身。

设备、系统版本和沙箱策略会影响哪些修复仍然需要。安装后可运行 `ohos-shim check` 检查当前设备。完整的可拦截性分析见 [ohos-preload-shim-feasibility.md](../ohos-preflight/docs/ohos-preload-shim-feasibility.md)，各拦截点的去留依据见[审计记录](logs/polyfill-audit/verdicts.md)。

## 支持的拦截点

所有拦截点默认开启，可通过 `OHOS_COMPAT_SHIM_DISABLE` 按需关闭。表中描述的是已观察到的症状，不代表每台设备都会出现。

| 拦截点 | 处理的情况 | 主要限制 |
|---|---|---|
| `close_range` / `syscall(SYS_close_range)` | 系统调用被策略拦截时，使用用户态方式关闭文件描述符；参数先按 Linux 语义校验 | `CLOSE_RANGE_UNSHARE` 依赖 `unshare(CLONE_FILES)`；不可用时返回 `ENOSYS`，不伪造隔离 |
| `getpwuid_r` | 当前 uid 无法从账户数据库解析时，为当前进程合成最小用户记录 | 不为其他 uid 合成账户 |
| `tmpfile` | 原生临时文件创建失败时，在可写目录创建并立即 unlink 临时文件 | 路径受 `TMPDIR`、`HOME` 和应用沙箱权限影响 |
| `linkat` / `link` | 硬链接因沙箱权限失败时，以同目录临时文件复制并改名 | 复制会丢失硬链接身份和共享 inode 语义 |
| `syscall(SYS_fchmodat2)` | 系统调用不可用时回退到 `fchmodat` | 保留 `AT_SYMLINK_NOFOLLOW`；不支持 `AT_EMPTY_PATH` 等无法由旧接口表达的标志 |
| `splice` | 修正源端 EOF 被报为 `EPIPE`；向管道写入时使用缓冲复制以唤醒 poll/epoll 等待者 | 管道路径放弃零拷贝；真实目标端错误仍返回错误 |
| epoll `EPOLLONESHOT` | 修正 FIFO 或 TTY/PTY 写端重复投递 `EPOLLOUT` 的已观察问题 | 对未登记的原始 syscall 调用，部分行为按事件形状识别 |
| 标准流 COPY relocation | 避免特定预编译 musl 程序因标准流符号导致的启动崩溃 | 已确认加载本库可规避该崩溃，但具体生效机制尚未完全定位 |
| `getaddrinfo` | 对非法主机名先本地快速拒绝；异步探测原生解析器行为 | 探测在进程首次调用 `getaddrinfo()` 时启动，不阻塞该次查询 |

`pthread_cancel()` 在目标 musl 环境中是空桩实现，无法由 preload 库补入调用方缺少的协作式取消点，因此不在本项目处理范围内。已经移除的探针和修复见[审计记录](logs/polyfill-audit/verdicts.md)。

## 安装与使用

### Harmonybrew

```sh
brew install social4hyq/core/ohos-compat-shim
ohos-shim check
```

Formula 会安装共享库、`ohos-shim` wrapper 和检查工具。也可从 [GitHub Releases](https://github.com/social4hyq/ohos-compat-shim/releases/latest) 获取发布的共享库。

### 手动设置 `LD_PRELOAD`

```sh
#!/bin/sh
export LD_PRELOAD="/path/to/libohos_compat.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec "/path/to/real-binary" "$@"
```

使用 Homebrew 安装时，优先用 `ohos-shim -- <cmd> [args...]` 启动程序，由 wrapper 查找已安装的共享库：

```sh
ohos-shim -- real-binary arg1 arg2
```

仓库包含 `index.js` 和 `index.d.ts` 的 Node.js wrapper API。当前 npm registry 尚未提供 `@ohos-ports/compat-shim`，因此暂不能通过 `npm install` 安装；可从源码或 Homebrew 使用。单体二进制 wrapper 示例见 [`examples/`](examples/)。

### 运行时开关

```sh
# 关闭一个或多个拦截点，名称以逗号分隔
export OHOS_COMPAT_SHIM_DISABLE=tmpfile,link
```

可用名称：`close_range`、`getpwuid_r`、`tmpfile`、`fchmodat2`、`linkat`、`link`、`splice`、`epoll_pipe`、`getaddrinfo`、`std_streams`。没有对应的 `ENABLE` 变量；默认全部开启。关闭 `linkat`/`link` 可避免使用有损语义的复制降级；关闭 `close_range` 则可能让原调用再次触发 `SIGSYS`。

## 检查当前设备

不同设备或系统版本可能已修复部分问题。用 `ohos-shim check` 在目标设备上检查实际行为，不要仅凭版本号决定关闭哪些拦截点：

```sh
ohos-shim check                     # 表格输出
ohos-shim check --json              # JSON 输出
ohos-shim check --rounds 50         # 增加间歇性 splice 唤醒问题的检查轮数
ohos-shim check --with-shim         # 同时检查加载 shim 后的结果
```

检查结果分为：

- **仍需要**：基线复现了对应问题。
- **可关闭**：基线行为正常，可考虑通过 `OHOS_COMPAT_SHIM_DISABLE` 关闭。
- **不确定**：有限轮检查没有足够证据。未复现间歇性问题不等于系统已修复。

`ohos-shim check` 还会报告若干相关平台能力，但这些项目不属于本 shim 的修复范围。详细平台矩阵见 [ohos-preflight](../ohos-preflight/docs/ohos-preload-shim-feasibility.md)。

## 适用边界

- 仅适用于动态链接程序；静态链接程序不会加载 `LD_PRELOAD` 库。
- 直接通过内联汇编发起、绕过可拦截符号的系统调用不受通用 libc 拦截覆盖，需在源码层处理。
- `linkat`/`link` 的复制回退不保留硬链接语义；只应在原生调用因沙箱权限失败时作为兼容回退。
- `fchmodat2` 回退只能表达旧 `fchmodat` 接口支持的语义。
- `splice` 写入管道时经用户态缓冲区转发，以确保等待者收到唤醒；其开销依工作负载和设备而异。

## 构建与检查

本地构建需要 Harmonybrew 安装的 OHOS NDK。`make` 构建共享库、检查程序和测试/诊断程序；运行各测试目标需要可执行文件签名工具 `binary-sign-tool`。

```sh
make                  # 构建全部目标，不运行测试
make smoke            # 基础 smoke 检查：无 shim 基线及加载 shim
make functional       # 功能检查：无 shim 基线及加载 shim
make check            # 运行 ohos-shim check 的检查程序
make ghost            # 对 epoll ONESHOT 内部状态机运行确定性模拟测试
make bench            # 测量基线与加载 shim 后的开销
make real-vs-fallback # 对比原生实现与 fallback
make clean
```

`make smoke`、`make functional` 和 `make bench` 会显式清除基线进程的 `LD_PRELOAD`，避免继承 shell 环境中已有的预加载库。`make ghost` 是使用模拟 epoll 实现的状态机测试，不是内核故障注入。发布工作流在云端交叉编译和签名，并以真机 smoke 检查作为发布门禁。

设备实测和历史性能测量见[性能记录](logs/performance.md)及[审计记录](logs/polyfill-audit/verdicts.md)；这些结果不保证适用于其他设备。

## License

BSD Zero Clause License (0BSD)，见 [LICENSE](LICENSE)。`scripts/ohos/self-sign.py` 来自 [hqzing/ohos-selfsign](https://github.com/hqzing/ohos-selfsign)，同样采用 0BSD。
