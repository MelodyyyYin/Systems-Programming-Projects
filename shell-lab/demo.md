# Shell Lab Demo 讲解稿

这份文档用于面试或课堂展示时快速回顾 Shell Lab 的实现思路、关键代码和演示顺序。目标不是背代码，而是能够清楚说明：

1. 这个 shell 做了什么。
2. 为什么要这样设计。
3. 竞态和信号问题是怎么处理的。
4. 如何用仓库里的命令复现结果。

---

## 1. 项目一句话概述

我实现的是 CS:APP Shell Lab 的 tiny shell `tsh`。它支持：

- 交互式命令循环
- 前台和后台作业
- `quit`、`jobs`、`bg`、`fg`
- `SIGCHLD`、`SIGINT`、`SIGTSTP`
- 外部命令的 `<`、`>` 重定向
- `jobs > file`

核心实现都在 [`tsh.c`](/afs/andrew.cmu.edu/usr16/melodyyi/private/18613-s26/shell-lab-MelodyyyYin/tsh.c)。

---

## 2. 仓库里最重要的文件

- [`tsh.c`](/afs/andrew.cmu.edu/usr16/melodyyi/private/18613-s26/shell-lab-MelodyyyYin/tsh.c): 主要实现文件。
- [`tsh_helper.c`](/afs/andrew.cmu.edu/usr16/melodyyi/private/18613-s26/shell-lab-MelodyyyYin/tsh_helper.c): 解析命令行、维护作业表的 helper。
- [`tsh_helper.h`](/afs/andrew.cmu.edu/usr16/melodyyi/private/18613-s26/shell-lab-MelodyyyYin/tsh_helper.h): helper API。
- [`traces/`](/afs/andrew.cmu.edu/usr16/melodyyi/private/18613-s26/shell-lab-MelodyyyYin/traces): 官方 trace。
- [`testprogs/`](/afs/andrew.cmu.edu/usr16/melodyyi/private/18613-s26/shell-lab-MelodyyyYin/testprogs): trace 会拉起的小程序。
- [`Makefile`](/afs/andrew.cmu.edu/usr16/melodyyi/private/18613-s26/shell-lab-MelodyyyYin/Makefile): 构建、格式检查、打包提交。

---

## 3. 我实现时的整体思路

### 3.1 主循环

`main()` 做三件事：

1. 初始化环境和作业表。
2. 安装信号处理函数。
3. 进入读取命令、解析命令、执行命令的循环。

遇到 EOF 时直接退出，这对应最基础的 shell 生命周期。

### 3.2 命令分流

每一行命令先交给 `parseline()` 解析，再按结果分成两类：

- builtin：`quit`、`jobs`、`bg`、`fg`
- external：普通程序路径，比如 `/bin/ls`

这样做的好处是逻辑清晰，builtin 不需要 fork，而外部命令统一走子进程。

### 3.3 作业模型

作业表由 helper 管理，shell 只负责：

- 添加作业
- 查询作业
- 修改作业状态
- 根据 PID 或 `%JID` 定位作业

我没有自己重写一套作业系统，而是严格沿用 helper 的接口，这样和 trace 的语义保持一致。

---

## 4. 关键实现点

### 4.1 前台和后台

- 没有 `&` 的命令作为前台作业执行。
- 带 `&` 的命令作为后台作业执行。
- 后台作业启动后要立刻返回提示符，并打印 `[jid] (pid) cmdline`。

### 4.2 `quit`

`quit` 直接退出 shell，不需要 fork。

### 4.3 `jobs`

`jobs` 会遍历作业表，把当前存在的 job 打印出来。

我这里还支持了 `jobs > file`，这样结果会输出到文件而不是标准输出。

### 4.4 `bg` 和 `fg`

`bg` / `fg` 接受两种参数形式：

- `1234`：PID
- `%3`：JID

处理流程通常是：

1. 解析参数。
2. 找到对应 job。
3. 给整个进程组发 `SIGCONT`。
4. 修改 job 状态。
5. 如果是 `fg`，继续等待它不再处于前台。

### 4.5 信号处理

我把 child 回收集中放在 `SIGCHLD` handler 里：

- 退出的子进程直接删除 job。
- 被信号终止的子进程打印终止信息并删除 job。
- 被停止的子进程改成 `ST` 状态并打印停止信息。

`SIGINT` 和 `SIGTSTP` 则只负责把信号转发给当前前台作业的进程组。

### 4.6 竞态控制

这是 Shell Lab 最容易出问题的地方。我做了几件事来降低竞态：

- 在访问作业表前屏蔽 `SIGCHLD`、`SIGINT`、`SIGTSTP`
- 前台等待使用 `sigsuspend`，而不是忙等
- 子进程在 `execve` 前恢复信号屏蔽和默认信号处理
- 父进程先登记 job，再恢复信号屏蔽

这样可以避免子进程过快退出时，父进程还没来得及把它加进作业表。

### 4.7 重定向

外部命令支持：

- 输入重定向 `<`
- 输出重定向 `>`
- 两者同时出现

实现上是在子进程 `execve` 之前完成 `open()` 和 `dup2()`。

---

## 5. 演示时可以按这个顺序讲

### 5.1 先说明构建方式

```bash
make
```

可以说明仓库里的代码、driver 和 helper 程序都能正常构建。

### 5.2 再演示最基础功能

```bash
./tsh
quit
```

可以说明 shell 能启动、接受输入、退出。

### 5.3 演示前台命令

```bash
./tsh
/bin/echo hello
```

可以说明普通命令会被解析并执行。

### 5.4 演示后台作业和 `jobs`

```bash
./tsh
/bin/sleep 2 &
jobs
```

可以说明后台 job 会立即返回提示符，并且 `jobs` 能看到它。

### 5.5 演示 `bg` / `fg`

```bash
./tsh
/bin/sleep 10 &
fg %1
```

或者先用 `Ctrl-Z` 停止前台作业，再用 `bg` 继续。

讲解重点是：

- `bg` 让作业继续跑，但留在后台
- `fg` 把作业拉回前台，并等待它结束

### 5.6 演示信号转发

```bash
./tsh
/bin/sleep 10
```

然后按 `Ctrl-C` 或 `Ctrl-Z`，说明 shell 自己不应该被杀掉，而是把信号转发给前台作业。

### 5.7 演示重定向

```bash
./tsh
/bin/cat < input.txt > output.txt
jobs > jobs.out
```

可以说明：

- 外部命令的输入/输出重定向有效
- `jobs` 也支持输出重定向

### 5.8 用 driver 复现 trace

```bash
./sdriver 2 -s ./tsh
```

如果要批量说明，可以挑几个代表性的 trace：

- `trace00` / `trace01`: 启动和退出
- `trace02` - `trace04`: 前台外部命令
- `trace05` - `trace08`: 后台作业和 `jobs`
- `trace09`, `trace11` - `trace21`: 信号处理
- `trace22` - `trace27`: `bg` / `fg`
- `trace28` - `trace32`: 重定向

---

## 6. 面试里可能会被问到的问题

### 问题 1：为什么要屏蔽 `SIGCHLD`？

因为子进程可能比父进程更快退出。如果不屏蔽，父进程还没把 job 加进表，`SIGCHLD` 就已经来了，容易出现丢 job 或误删 job 的竞态。

### 问题 2：为什么前台等待用 `sigsuspend`？

因为前台作业是否结束，本质上是由 `SIGCHLD` 驱动的。`sigsuspend` 可以在等待期间原子地放开信号，然后在信号到来后醒来，避免忙等。

### 问题 3：为什么 `SIGINT` / `SIGTSTP` 要发给进程组？

Shell Lab 的目标是让前台 job 整体接收信号，而不是只给单个进程。发给负 PID 就能把信号送到整个进程组。

### 问题 4：为什么 `jobs` 要支持重定向？

因为 trace 和手工测试里会检查 builtin 的输出行为，`jobs > file` 是规范要求的一部分。

### 问题 5：这份实现里最关键的地方是什么？

我会回答两点：

- 正确的 job control 语义
- 竞态安全的信号处理

这两个地方决定了 shell 能不能稳定通过 driver 和 trace。

---

## 7. 我可以怎么收尾总结

一个简短总结可以这样说：

> 我实现了一个符合 Shell Lab 规范的 tiny shell。它支持前台/后台作业、`jobs`、`bg`、`fg`、信号转发和重定向，并且通过 helper 提供的作业表接口把竞态处理收敛到 `SIGCHLD` 和受控的临界区里。这样能让 shell 在 trace 驱动下稳定运行，也便于解释每一步为什么安全。

---

## 8. 现场准备建议

- 先背清楚命令流：`parse -> builtin/external -> job control -> signal handling`
- 重点讲 `SIGCHLD`、`SIGINT`、`SIGTSTP` 的职责分离
- 如果时间不够，优先讲：
  - job 表怎么维护
  - 为什么要屏蔽信号
  - 为什么前台等待不能忙等
- 演示时尽量先跑 `make`，再跑一个最小命令和一个 trace
