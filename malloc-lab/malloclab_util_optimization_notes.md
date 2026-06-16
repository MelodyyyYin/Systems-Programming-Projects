# Utilization Optimization Notes

## 1. 背景

目标是把当前 allocator 的：

- Harmonic mean utilization

从约 `72.3%` 提升到至少 `74%`，同时尽量不把：

- throughput
- perf index

打回不可接受的水平。

我把实验时的稳定参考点当作：

- `Harmonic mean utilization = 72.3%`
- `Harmonic mean throughput (Kops/sec) = 5201`
- `Perf index = 54.5 (util) + 40.0 (thru) = 94.5/100`

这个参考点的特点是：

- correctness 通过
- `mdriver-emulate` 通过
- throughput 有较大安全余量

---

## 2. 实验总结表

| 实验 | 主要改动 | 预期 | 实测结果 | 结论 |
|---|---|---|---|---|
| A | 减小 `chunksize`，减少扩堆过量 | 提高 utilization，少浪费尾部空闲空间 | utilization 基本不变，throughput 小幅下降 | 不是主因 |
| B | 扩大 sorted bucket 覆盖范围 | 更接近 best-fit，提高 utilization | utilization 只小幅上涨，但 throughput 明显下降 | 性价比差 |
| C | 所有 regular bucket 都排序 | 进一步提高利用率 | utilization 到约 `73.0%`，但 throughput 掉到约 `1466 Kops/s` | 不可接受 |
| D | 14 buckets 下重调阈值 | 用更细的尺寸分桶改善 fit | 提升不明显，部分配置 throughput 继续恶化 | 效果不稳定 |
| E | 把 `heap_last` 元数据移到 heap，恢复 15 buckets | 在 128B globals 内换更多 size classes | correctness 还能过，但 utilization/throughput 都明显退化 | 当前实现方向失败 |

---

## 3. 详细实验记录

### A. 缩小扩堆粒度

### 改动

- 把默认 `chunksize` 从 `4096` 缩到 `2048`
- 去掉 `malloc` 里对 `asize >= 1024` 时额外放大到 `2 * chunksize` 的逻辑

### 思路

怀疑当前 utilization 偏低的原因之一是：

- 每次找不到 fit 时扩堆过多
- 导致峰值时刻 heap 尾部保留了较多暂时没用上的空闲空间

### 实测

结果大致是：

- utilization 仍然约 `72.3%`
- throughput 仍然很高，约 `5.2M Kops/s`

### 判断

这个实验说明：

- utilization 的主要瓶颈不是简单的“尾部扩堆过量”
- 至少不是靠把 `chunksize` 从 `4096` 调到 `2048` 就能解决

另外，`ngram` 这类低利用率 trace 在这个实验里几乎没动，也进一步支持这个判断。

---

### B. 让更多 bucket 使用 sorted insertion + sorted first-fit

### 改动

- 把 `use_sorted_insert()` 的范围从只覆盖最后一个 bucket，扩大到更多中大尺寸 bucket
- 在 sorted bucket 中直接使用 first-fit，因为链表已经按 size 排序

### 思路

希望把：

- 原本偏 LIFO / limited best-fit 的查找

推向：

- 更接近 best-fit 的放置策略

这样可能减少：

- 中大尺寸 block 被过大 free block 吞掉
- 由此带来的外部碎片

### 实测

这个方向出现过两类结果：

1. 较温和版本：
   - utilization 只能涨到 `72.3% ~ 72.8%`
   - throughput 会从高位明显下降，最差时只剩略高于门槛

2. 更激进版本：
   - utilization 最多到约 `73.0%`
   - throughput 直接掉到约 `1466 Kops/s`
   - perf index 退化到约 `56.9/100`

### 判断

这个方向能提高利用率，但：

- 提升幅度不足以把 `72.3%` 推到 `74%`
- 对 throughput 的伤害太大

所以它可以作为“轻度调参”的工具，但不能作为主解法。

---

### C. 所有 regular bucket 都排序

### 改动

- 除 mini bucket 外，其余 free list 全部按 block size 排序
- `find_fit()` 在这些 bucket 中使用 sorted first-fit

### 思路

这是对实验 B 的进一步极化：

- 既然 sorted bucket 能让 fit 更接近 best-fit
- 那就把这种策略推广到所有 regular free lists

### 实测

结果很明确：

- `Harmonic mean utilization = 73.0%`
- `Harmonic mean throughput (Kops/sec) = 1466`
- `Perf index = 56.9/100`

### 判断

虽然 utilization 相比 `72.3%` 有提升，但：

- 没到 `74%`
- throughput 已经掉到 final 明显不可接受的区间

因此这条路不能保留。

---

### D. 在 14 buckets 内重调阈值

### 改动

尝试过几类更细的阈值划分，例如把中等尺寸区间拆得更密：

- 把 `112/128/160/224/320/...`
- 或 `2048`

这类分界重新拉回到更常见的中等尺寸请求区间

### 思路

希望在不增加 globals 的前提下：

- 用更细的 size class 减少 bucket 内部跨度
- 让 limited search 能找到更贴近请求大小的 free block

### 实测

结果整体不理想：

- 有些 trace 的单独 utilization 会微涨
- 但总 harmonic mean utilization 提升不稳定
- 很多配置下 throughput 明显下降

其中比较典型的一组结果是：

- utilization 仍然只在 `73.0%` 左右
- throughput 在 `1463 Kops/s` 左右

### 判断

单靠 14 buckets 内的阈值重调，不能稳定把利用率推过 `74%`。

---

### E. 把 `heap_last` 元数据移到 heap 中，恢复 15 buckets

### 改动

为了绕开 128-byte writable globals 限制，我尝试：

- 不再把 `heap_last` 放在全局变量中
- 改成把它写在 heap 开头的固定元数据槽里
- 从而把 `SEG_SIZE` 从 `14` 恢复到 `15`

### 思路

这是一个更结构性的尝试：

- 多一个 bucket
- 就能在中大尺寸区间再插入一个 size class
- 理论上 fit 精度会更高

### 实测

这条路在 correctness 上并没有直接炸掉：

- `syn-giantarray.rep` 这类 giant emulate 仍然能过

但整体性能非常差，代表性结果是：

- `Harmonic mean utilization = 68.6%`
- `Harmonic mean throughput (Kops/sec) = 1584 ~ 1601`

### 判断

这个想法本身未必永远错误，但当前实现版本明显失败了。

原因可能包括：

- 15 buckets 的新阈值方案本身不合适
- 与当时的 `sorted bucket + first-fit` 组合不兼容
- 新元数据布局虽然 correctness 过了，但没有带来预期中的 fit 改善

当前状态下，不值得继续沿这个版本往前推。

---

## 4. 这几轮实验得到的结论

### 4.1 低利用率的主因不是简单的扩堆过量

把 `chunksize` 从 `4096` 改到 `2048` 后：

- `ngram` 等低利用率 trace 基本没改善
- 总 utilization 也几乎不变

这说明：

- 当前的低利用率不是靠“把扩堆缩小一点”就能解决

### 4.2 参数微调能把 utilization 拉高一点，但很容易把 throughput 打穿

特别是：

- 更 aggressive 的 sorted insertion
- 更接近 best-fit 的查找

确实能把 utilization 从 `72.3%` 拉到 `72.8%` 或 `73.0%` 左右，但代价是：

- throughput 下降非常明显

### 4.3 74% 更像是结构问题，不像是常数问题

当前实验结果的信号很一致：

- 只改 `chunksize`
- 只改 search limit
- 只改 bucket 阈值
- 只扩大 sorted 范围

都没有把 utilization 稳定推到 `74%`。

这说明如果还要继续追 `74%`，更可能需要：

- allocator 结构层面的变化
- 而不是继续盲调几个常数

---

## 5. 我当前的想法

如果目标是：

- 保住 `mdriver-emulate`
- 保住 final throughput
- 再把 utilization 推到 `74%+`

那我认为接下来最值得做的，不是继续小修小补，而是做更有针对性的结构调整。

我目前的判断是：

- 当前低 utilization 的一部分是“小对象内部碎片”带来的，这部分靠参数基本很难抹掉
- 所以剩下能优化的空间，主要在：
  - 中尺寸 block 的放置质量
  - free block 的复用质量
  - bucket 设计是否真正贴合 trace 分布

---

## 6. 未来可以做的方案

### 方案 1：做一版“面向 trace 分布”的 bucket 设计

不是继续凭感觉调阈值，而是先统计：

- 常见请求大小
- 常见 free block 大小
- 哪些 bucket 最长
- 哪些 bucket 最容易出现“大块吃小请求”

然后按真实分布重新设计 size classes。

这是我认为最现实、最值得优先做的方向。

---

### 方案 2：只对中大尺寸 bucket 做更精细的 best-fit，但保留 O(1) 级插入

当前主要矛盾是：

- 更接近 best-fit 的策略能提高 utilization
- 但 sorted insertion 太伤 throughput

下一步可以考虑：

- 保留 LIFO / cheap insertion
- 但对中大尺寸 bucket 在 `find_fit()` 里增加更聪明的有限搜索策略
- 例如按请求大小动态调 search window，而不是固定 `8/12/16/24`

这比“全桶排序”更有希望保住 throughput。

---

### 方案 3：针对中等尺寸 block 试验不同的 split policy

现在 allocator 默认是：

- 只要 remainder 足够形成合法 free block，就 split

可以继续试验：

- 某些尺寸区间下不立刻 split
- 或者对 remainder 很小、很难复用的情况采用不同处理

这类策略对 utilization 的帮助未必大，但成本比重写数据结构低。

---

### 方案 4：重新审视“15 buckets”路线，但要彻底换一种实现方式

我不建议继续沿用这次的 15-bucket 试验版本，但不代表“更多 bucket”这件事本身没有价值。

如果以后再做这条路，建议：

- 先把 metadata 布局单独设计清楚
- 再做一版更系统的阈值设计
- 不要和激进的 sorted policy 一起同时改

也就是说：

- 一次只改一个维度

否则很难知道到底是哪一部分把利用率和吞吐一起拖坏了。

---

### 方案 5：先接受 `72.3%`，把时间投入到更高确定性的收益点

如果目标不是“非要追到 74%”，而是：

- 稳定提交
- 保住 correctness
- 保住 emulate
- 保住高 perf index

那么当前 `72.3% / 5201 Kops/s / 94.5/100` 这条线已经很稳。

从工程性价比上看，这可能比继续冒险试更多利用率调参更合理。

---

## 7. 最后结论

目前已经尝试过的方向表明：

- 想把 `72.3%` 推到 `74%+`
- 只靠常数级参数微调
- 成功概率不高

更现实的判断是：

- 如果继续做，应该转向“基于 trace 分布的 bucket 设计”或“中大尺寸 block 的更聪明复用策略”
- 而不是继续反复改 `chunksize`、bucket 阈值和 sorted 范围

如果只看“稳定可交”和“最终 perf index”，当前的稳定版本已经是一个很好的停点。

---

## 8. 新一轮分析补充

这一轮没有继续做大范围调参，而是先补齐了 checker 和 spec 对齐问题，再重新看 trace 分布，得到几个更明确的信号。

### 8.1 `mm_checkheap` 补强后抓到了一个真实 bug

新增了：

- block size 合法性检查
- free-list link 边界检查
- `heap_last` 与实际最后物理块一致性检查

结果马上抓到了一个以前没被发现的问题：

- `realloc` 在处理尾块 shrink / grow 时，某些路径没有同步更新 `heap_last`

这说明：

- 更严格的 checker 是有价值的
- 当前 allocator 虽然在正式 trace 上 correctness 通过，但内部元数据维护仍然存在“过去没被检查到”的薄弱点

### 8.2 真正拖 utilization 的 trace 主要是 `ngram-*`

从正式计分里最低的几条 trace 看：

- `ngram-gulliver1`：`61.7%`
- `ngram-gulliver2`：`58.3%`
- `ngram-moby1`：`59.4%`
- `ngram-shake1`：`60.0%`

而这四条 `ngram` trace 的请求尺寸分布非常集中：

- 大约 `50%` 的请求是 `24` bytes
- 大约 `28.8%` 的请求落在 `9..16` bytes
- 大约 `19.7%` 的请求落在 `<= 8` bytes

换句话说，`ngram` 的主体不是“大块 fit 不够准”，而是：

- 极多的小对象
- 其中大量对象正好落在当前 block layout 最尴尬的区间（`9..16`）

### 8.3 一个轻量级“remainder-aware fit”实验没有效果

我做了一个很小的实验：

- 在 `find_fit()` 中，针对 `asize == 32`
- 轻微惩罚把 `48`-byte free block 切成 `32 + 16`
- 希望减少对 `16`-byte mini remainder 的制造

理由是：

- `ngram` 里很多请求最终都映射到 `32`-byte allocated block
- `16`-byte mini remainder 只能服务极小请求
- 理论上它的复用价值可能低于 `32`-byte remainder

但实测结果是：

- harmonic mean utilization 仍然是 `72.3%`
- throughput 基本不变

这说明：

- 问题不是“某一种 splinter 选择错了”这么简单
- 至少不能靠一个局部 fit heuristic 就把总利用率推到 `74%+`

### 8.4 当前最值得怀疑的是“小对象布局本身”

由于当前 allocated block 采用：

- `8-byte header`
- `16-byte alignment`

所以很多请求会映射到这些 payload 容量：

- block `16` -> payload `8`
- block `32` -> payload `24`
- block `48` -> payload `40`

这意味着：

- `9..16` bytes 的请求都会落到 `32`-byte block
- 这是 `ngram` 中一大批高频请求

因此如果还要继续追 `74%+`，我现在更倾向于认为真正有希望的方向是：

- 不是继续调 bucket 常数
- 而是想办法改变“小对象的布局成本”或者“小对象的复用方式”

### 8.5 下一步最值得做的方向

按优先级看，我现在认为最值得试的是：

1. 针对 `9..16` bytes 小对象引入更结构化的处理方式  
   例如单独的小对象页 / slab / run 思路，把 metadata 从每个小对象里挪出去，而不是继续让这些对象都走普通 boundary-tag block。

2. 把优化重点只放在 `asize == 32` 这一类请求上  
   不是全局更 aggressive 地 best-fit，而是只为最常见、最拖利用率的 block size 设计专门复用策略。

3. 如果不愿意做结构性重构，就不要再花太多时间在：
   - `chunksize`
   - bucket 阈值
   - sorted 范围
   - 轻量局部 fit 打分
   这些方向上反复调参

当前这轮分析最重要的结论是：

- `72.3%` 到 `74%+` 的差距，更像是“小对象表示方式”的问题
- 不太像是继续把现有 segregated-list 参数磨一磨就能解决的问题

---

## 9. 后续结构性实验更新

在上面这些分析之后，我继续做了几轮更大的结构调整。这里记录最终真正有价值的实验过程，而不是只保留最后成功版本。

### F. `asize == 32` 专用 slab 路线

### 改动

- 为 `32`-byte block class 单独增加 slab page
- 把该尺寸请求从普通 boundary-tag allocator 中分流出去
- 尝试过 `4096` 和 `1024` 两种 slab page 粒度

### 思路

目标很直接：

- `ngram` / `bdd` / `syn-*` 里有大量请求最终会落到 `32`-byte block
- 如果把这类小对象的 per-block header 成本挪到 page metadata，也许能显著提高 utilization

### 实测

整体结果很差，代表性结果是：

- `Harmonic mean utilization = 64.2%`
- `Harmonic mean throughput (Kops/sec) = 2192`
- `Perf index = 29.1/100`

单 trace 上也能直接看出问题：

- `ngram-gulliver1.rep` utilization 掉到约 `33.8%`
- `bdd-aa32.rep` utilization 掉到约 `63.9%`

### 判断

这条路在当前 lab 约束下失败了。根因不是实现崩溃，而是：

- slab page 粒度导致峰值时保留了太多尚未完全利用的小对象页
- 这类“页级内部碎片”比原先 block-level internal fragmentation 更糟

所以这条路线虽然是结构性变化，但方向不对，已经完全放弃。

---

### G. 把 segregated-list metadata 搬进 heap，换更多 exact-size bins

### 改动

- 不再把 free-list 头数组放在 writable globals
- 只保留两个全局指针：`heap_start` 和 `heap_last`
- 把 bucket heads 放进 heap 开头的 metadata 区
- 为了保持 payload 仍然 16-byte aligned，额外加入按 `SEG_SIZE` 奇偶自动计算的 pad word

在这个布局上，重新组织 size classes：

- `32` 个 exact-size bins，覆盖 `16..512`
- `6` 个 range bins，覆盖更大的 block
- mini block 仍然是 bucket `0`

### 思路

这次的核心不是“多一个 bucket”这种小改，而是：

- 用 heap 内 metadata 释放 writable globals 配额
- 把真正高频的小中尺寸请求尽量变成 exact-size 命中
- 让最常见的分配路径退化成：
  - O(1) 取 exact bin 头
  - 否则再做有限 range-bin 搜索

### 第一版结果

第一版 exact-bin 结构已经是净收益：

- `Harmonic mean utilization = 72.7%`
- `Harmonic mean throughput (Kops/sec) = 6085`
- `Perf index = 95.8/100`

比旧稳定基线：

- `72.3%`
- `~5201 Kops/s`
- `94.5/100`

明显更强，但还没到 `74%`。

### 失败的子分支

在 exact-bin 结构上又试了几条变体：

1. 所有 range bins 都做 sorted insertion
   - utilization 最多只到约 `72.8%`
   - throughput 掉到约 `3334`
   - 结论：不值

2. 把 exact bins 扩到 `16..1024`
   - `Harmonic mean utilization = 71.6%`
   - throughput 约 `6236`
   - 结论：exact 区做得太大，反而破坏了整体复用

3. 只把最大的几个 range bins 排序
   - `Harmonic mean utilization = 73.0%`
   - throughput 约 `3619`
   - 结论：对大块 fit 有帮助，但 throughput 代价仍然过大

这些子实验说明：

- 新结构是对的
- 但“靠排序去逼近 best-fit”依然不是最优路线

---

### H. 最终保留版本：exact bins + 更激进的有限 range search

### 改动

最终保留的是：

- `32` 个 exact bins 覆盖 `16..512`
- `6` 个 range bins：`768, 1024, 1536, 2048, 4096, 16384`
- exact bins 的 `find_fit()` 完全走 O(1) 头结点命中
- range bins 不排序，只扩大有限搜索窗口
- `close_enough_fit()` 从 `32` 收紧到 `16`
- 最后一个超大 bucket 仍保留 sorted insertion，避免 giant path 退化

### 思路

这版的 tradeoff 很明确：

- 不在热路径上引入排序开销
- 但愿意拿掉一部分过剩 throughput，换更好的 fit 质量

### 实测结果

最终结果是：

- `Harmonic mean utilization = 74.0%`
- `Harmonic mean throughput (Kops/sec) = 5542`
- `Perf index = 99.9/100`

格式化后的最终确认结果为：

- `Harmonic mean utilization = 74.0%`
- `Harmonic mean throughput (Kops/sec) = 5406`
- `Perf index = 99.9/100`

这里 throughput 的轻微波动是正常的，本质结论没变：

- utilization 已经达到目标
- throughput 仍远高于本机 `2348` 的满分门槛

### 单 trace 变化信号

这版结构调整最明显改善的是原来被 free-list 粗粒度 fit 拖累的 synthetic traces：

- `syn-array.rep` 提升到 `87.9%`
- `syn-mix.rep` 提升到 `92.6%`

而 `ngram-*` 仍然大致停留在它们的理论上限附近：

- `58% ~ 61%`

这和前面分析是一致的：

- `ngram` 的剩余低利用率主要来自小对象内部碎片
- 不是 free-list 复用失败

---

## 10. 最终结论更新

截至目前，真正把 utilization 从 `72.3%` 推到 `74.0%` 的，不是：

- `chunksize`
- bucket 阈值微调
- sorted 范围扩大
- 32-byte slab

而是：

- 把 metadata 放进 heap
- 把小中尺寸请求尽量转成 exact-size bins
- 在 range bins 上使用更积极但仍然有限的搜索

也就是说，这次实验最后证明了一个更明确的结论：

- `72.3% -> 74.0%` 确实需要结构级变化
- 但不需要把 allocator 改成完全不同的体系
- 一个“仍然是 boundary-tag + segregated lists”的版本，只要把 metadata 布局和 size-class 组织方式做对，也能过线

---

## 11. 按 spec 的最终审计结论

我又对照 `malloclab_spec.md` 做了一轮完整检查。当前最终版结论是：

### 11.1 提交范围与接口

- 提交物仍然只有 `mm.c`
- 没有修改 `mm.h`
- `mm_init` / `malloc` / `free` / `realloc` / `calloc` / `mm_checkheap` 都已实现

### 11.2 行为正确性

- 返回指针始终满足 16-byte alignment
- `free(NULL)` 无副作用
- `realloc(ptr, 0)` 等价于 `free(ptr); return NULL`
- `realloc(NULL, size)` 等价于 `malloc(size)`
- `calloc` 会做乘法溢出检查，并把返回 payload 置零

### 11.3 64-bit 与 overflow

- `mdriver-emulate -V` 全套通过
- 大小计算使用 `size_t` / `intptr_t` / `word_t`
- 关键路径存在显式 overflow guard：
  - `checked_add`
  - `checked_round_up`
  - `request_to_block_size`
  - `extend_heap`
  - `extend_heap_for_malloc`

### 11.4 writable globals

- 当前 writable globals 只有：
  - `heap_start`
  - `heap_last`
- 总量 `16 bytes`
- 明显低于 spec 的 `128 bytes` 上限

### 11.5 `mm_checkheap`

- 正常情况下静默返回 `true`
- 只有失败时才打印错误
- 检查内容覆盖：
  - prologue / epilogue
  - block 对齐
  - block size 合法性
  - header / footer 一致性
  - prev bits 一致性
  - 相邻 free block 是否已合并
  - free-list link 是否有效
  - bucket range 是否正确
  - heap/free-list free block 数量是否一致
  - `heap_last` 与 free-list head 的合法性

### 11.6 本地验证

最终格式化后的提交物已经通过：

- `make -j4`
- `make mm-check`
- `make .format-checked`
- `./mdriver -v 0`
- `./mdriver-emulate -V`
- `./mdriver-dbg -d 2 -f traces/syn-mix-realloc.rep`
- `./mdriver-dbg -d 2 -f traces/syn-array-short.rep`

### 11.7 剩余风险判断

按 spec 来看，当前没有发现新的 blocker。

唯一不能绝对控制的是：

- Autolab 机器上的 throughput 可能与本地略有波动

但当前吞吐量离满分门槛仍有很大安全余量，因此这不构成现实风险。
