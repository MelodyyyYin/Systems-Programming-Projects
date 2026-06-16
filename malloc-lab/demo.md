# Malloc Lab Demo Notes

下面这份笔记可以直接用来做面试、答辩或录屏讲解。重点不是背诵代码，而是把“我做了什么、为什么这样做、怎么验证它是对的”讲清楚。

## 1. 先用一句话概括项目

这是一个 64-bit dynamic memory allocator，实现了 `malloc`、`free`、`realloc`、`calloc` 和 heap checker `mm_checkheap`。  
核心目标有三个：

1. 正确性：不能重叠、不能破坏数据、不能把已释放块重复使用错误。
2. 利用率：尽量减少 external fragmentation 和 unnecessary heap growth。
3. 吞吐量：查找空闲块、分裂、合并、插入/删除 free list 的开销要低。

讲法建议：

- “我不是只做了能跑的 malloc，而是做了能在标准 driver、debug driver、sparse emulate driver 下都能工作的 allocator。”
- “这个实验里最难的是把 correctness、utilization、throughput 同时维持住。”

## 2. 我的总体设计

我现在的 allocator 不是简单 implicit list，而是：

- segregated explicit free lists
- 支持 mini free block
- regular free block 带 `next/prev`
- mini free block 只保留 `next`
- header 里同时记录当前块状态和前驱块状态

### 2.1 block header 的含义

header 的低位存状态位，大小则放在高位：

- bit 0: 当前块是否 allocated
- bit 1: 前一个物理块是否 allocated
- bit 2: 前一个物理块是否是 mini free block
- bit 3: 前一个物理块是否是 footerless tail free block

这个设计的目的：

- 避免每次 coalesce 都必须读 footer
- 让 mini free block 可以更轻量
- 支持 64-bit / sparse emulate 的场景

### 2.2 为什么要区分 mini block

16-byte 小块非常常见，如果把它们也按普通 free block 处理，会有两个问题：

- 元数据开销太大
- 对小请求的利用率差

所以我把 16-byte free block 单独作为 mini block：

- 只存 header + 一个 next 指针
- 不依赖 footer
- 进入单独的 bucket

### 2.3 为什么要有 segregated free lists

如果所有 free block 都放在一个链表里，查找会越来越慢。  
segregated list 的好处是：

- 小块和大块分开管理
- find-fit 的平均扫描长度更短
- 插入/删除可以保持局部性

我这里还用了：

- exact bins 给小尺寸
- range bins 给较大尺寸
- 最大的 bucket 还会更偏向按 size 组织

## 3. 分配流程怎么讲

### 3.1 `malloc`

可以按这个顺序解释：

1. 把请求大小归整到对齐后的 block size。
2. 在合适的 free list 里找一个能放下的块。
3. 如果找到的块比需求大，就 split。
4. 更新 header 和 successor 的前驱状态位。
5. 返回 payload 指针。

面试时可以强调：

- “我尽量把大块和小块分开找，减少无效扫描。”
- “split 后会同步维护后继块的 metadata，不让链表状态和物理布局脱节。”

### 3.2 `free`

解释重点：

1. 把当前块标记为 free。
2. 检查前后相邻块是否 free。
3. 如果可合并，就 coalesce。
4. 把合并后的块插回正确的 free list。

你可以补一句：

- “free 的难点不是‘放回去’，而是 coalesce 之后还要保证 header、footer、prev bits 和 free list 全都一致。”

### 3.3 `realloc`

建议这样讲：

- `size == 0` 时等价于 `free`
- `ptr == NULL` 时等价于 `malloc`
- 其他情况分配新块、拷贝旧数据、释放旧块

如果对方追问性能：

- “realloc 的重点先是语义正确，后面才是尽量减少搬移成本。”

### 3.4 `calloc`

直接说：

- 分配 `nmemb * size`
- 然后清零
- 重点是乘法溢出和正确初始化

## 4. 我最重视的约束

### 4.1 16-byte alignment

这个实验里所有返回给用户的 payload 都必须 16-byte 对齐。  
这是 malloc lab 的硬约束，不是可选优化。

### 4.2 mm_init 必须可重复调用

driver 会在每个 trace 开始时重新初始化 heap，所以：

- 不能依赖上一轮 trace 的全局残留
- 必须把所有 heap 元数据重置干净

### 4.3 64-bit address space

`mdriver-emulate` 会专门检查这一点。  
所以我在讲实现时可以强调：

- 使用 `size_t` / `uintptr_t` 这类合适的类型
- 注意 size 计算和 round up 的溢出
- 不能默认地址只会落在 32-bit 范围内

## 5. 可以直接讲的实现亮点

下面这些点比较适合做展示重点：

### 5.1 footerless tail block

对于非常大的尾部 free block，我允许它在 heap 尾端不写 footer，并用 successor header 的状态位记录这个事实。  
这样做的好处是：

- 少写一次内存
- 在 sparse emulate 下更省页面触碰

### 5.2 mini free block 的单链表

mini block 只有一个 next 指针，所以：

- 空间更小
- 插入/删除更轻
- 对小请求更友好

### 5.3 检查器 `mm_checkheap`

这个函数的作用不是“装饰”，而是调试核心工具。  
我会用它检查：

- 对齐
- 相邻 free block 是否已经合并
- header/footer 是否一致
- free list 和 heap 中的 block 数量关系

讲法建议：

- “我不是靠运气把代码跑通，而是靠 heap checker 把不变量固定下来。”

## 6. 当前可复现结果

这是这份仓库当前 checkout 能直接复现的结果：

- `make` 成功
- `./mdriver -V`
  - 26/26 traces 通过
  - harmonic mean utilization: `74.0%`
  - harmonic mean throughput: `5608 Kops/sec`
  - perf index: `99.9/100`
- `./mdriver-emulate -V -f traces/syn-giantarray.rep`
  - 通过
  - utilization: `97.0%`

如果现场被问“你怎么证明你的实现不是只在普通 driver 上好看”，可以直接回答：

- “我跑了 normal driver，也跑了 sparse emulate 的 giant trace，两个路径都能通过。”

## 7. 演示时的推荐讲解顺序

如果你要在 3 到 5 分钟内讲完，可以按这个顺序：

1. 先说项目目标：正确性、利用率、吞吐量。
2. 说整体结构：segregated explicit free list + mini block + header metadata。
3. 说一次 `malloc` 的流程。
4. 说一次 `free` 的 coalesce 流程。
5. 说为什么要 `mm_checkheap`。
6. 最后报当前结果：`mdriver -V` 和 `mdriver-emulate` 都通过。

## 8. 面试高频问题怎么答

### 问：为什么不直接用 implicit list？

答：

- implicit list 太容易退化成线性扫描
- 对这个实验的 throughput 不友好
- free list 分层后可以明显减少查找成本

### 问：为什么要支持 mini block？

答：

- 小块请求很多
- mini block 能减少元数据成本
- 对利用率有帮助

### 问：为什么要写 heap checker？

答：

- allocator 的 bug 往往不是立刻崩，而是状态慢慢坏掉
- checker 可以把问题尽早暴露
- 它是我验证 coalesce / split / insert / remove 正确性的主要工具

### 问：你怎么保证 64-bit 兼容？

答：

- 全部 size 和地址相关操作都用合适的宽度类型
- 对大尺寸请求做溢出检查
- 用 `mdriver-emulate` 额外验证 sparse / 64-bit 路径

## 9. 你可以直接用的收尾句

- “这个 allocator 的重点不是某一个技巧，而是把 block 布局、free list、coalesce、checker 和 driver 验证串成一个自洽系统。”
- “我最后用 normal driver 和 emulate driver 两条路径确认了 correctness。”
- “如果继续优化，我会优先看查找策略和大 trace 下的 split/coalesce 成本。”

