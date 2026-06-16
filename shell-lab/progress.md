# Shell Lab Progress

## 当前进度

当前实现已经按 `shell_lab_high_level_plan.md` 的阶段顺序推进到阶段 5，并且已在当前 Linux 服务器上重新复核 step1-5。`tsh.c` 中已完成以下能力：

- 阶段 0：shell 主循环、EOF 退出、`quit`
- 阶段 1：前台外部命令执行、参数与环境透传
- 阶段 2：后台作业、作业表登记、`jobs`、`SIGCHLD` 集中回收
- 阶段 3：`SIGINT` / `SIGTSTP` 前台进程组转发、停止/终止消息输出
- 阶段 4：`bg` / `fg`，支持 PID 和 `%JID` 寻址、`SIGCONT` 恢复、前台等待
- 阶段 5：`<` / `>` / 组合重定向、`jobs > file`、基础文件/权限错误处理

当前实现保持了以下约束：

- 没有使用 busy wait、sleep、轮询、`tcsetpgrp`、`tcsetattr`
- 前台等待基于 `sigsuspend`
- 子进程状态回收集中在 `sigchld_handler`
- 作业表访问统一在 `SIGCHLD` / `SIGINT` / `SIGTSTP` 屏蔽区内进行
- 没有偏离 helper API 和现有 skeleton 结构

## 当前验证情况

已在当前 Linux 服务器上基于官方 `sdriver`/`runtrace` 工作流做了 step1-5 复核。为了避开仓库内遗留的 macOS Mach-O 构建产物，复核时在隔离的临时目录中重新编译了 Linux 版 `tsh`、`runtrace` 和 `testprogs`，并使用仓库内的 `tshref` 与 trace 集合做对照验证。

当前服务器复核结果：

- 阶段 1：`trace02` 到 `trace04` 通过
- 阶段 2：`trace05` 到 `trace08` 通过
- 阶段 3：`trace09`、`trace11` 到 `trace21` 通过
- 阶段 4：`trace22` 到 `trace27` 通过
- 阶段 5：`trace28` 到 `trace32` 通过
- 对上述有效 traces 追加做了 3 轮重复回归，总计 `30/30` 通过

本次服务器复核实际覆盖了以下行为：

- shell 生命周期和 `quit`
- 前台/后台命令执行
- `jobs`、`bg`、`fg`
- 前台信号转发与停止/终止消息
- 输入/输出/组合重定向
- `jobs > file`
- 常见缺文件、权限不足、不可执行路径的错误处理

本轮服务器收尾结果：

- 已清理仓库中混入的 macOS 生成旧产物，恢复当前目录下的 Linux 构建环境
- 已在仓库根目录直接执行官方 `make`，构建成功
- 已在仓库根目录直接执行官方 `sdriver`，对所有有效 traces 做 3 轮回归，结果为 `30/30` 通过
- 因此，`shell_lab_high_level_plan.md` 中 step1-5 对应的实现、官方构建和有效 traces 回归都已经在当前服务器上完成

## 下一步

按照 `shell_lab_high_level_plan.md`，当前只剩提交前最后收尾：

1. 运行 `make handin` 生成最终提交产物
2. 检查 handin tar 中只包含要求提交的文件，确认没有混入无关构建产物
3. 如课程流程要求，再执行 `make submit` 或按课程平台完成最终提交

## 当前代码位置

当前主要实现位于：

- `tsh.c`

其余 helper 和测试文件未作为主要实现入口修改。
