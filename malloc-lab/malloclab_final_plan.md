# Malloc Lab Final 计划（修订版）

## 约束与目标

- 最终提交只会采纳 `mm.c`，实现阶段必须把所有改动限制在 `mm.c`。
- 本文档只是开发计划，不参与提交。
- 必须继续满足文档里的硬约束：16-byte alignment、`mm_init` 可重复初始化、完整 64-bit address space、可写全局变量不超过 128 bytes、无 warning 编译。
- 目标不是“重写一个新 allocator”，而是在当前 `mm.c` 框架上，把 final 真正卡分的风险逐个清掉。

## 当前基线

我先对当前版本做了本地验证，结论如下：

- `./mdriver -V`：26/26 个常规 trace 全部 correctness 通过。
- 同一次运行的总体结果：
  - utilization：`67.6%`
  - throughput：约 `1100 Kops/s`
  - Perf index：`39.9/100`
- `./mdriver-emulate -V -f traces/syn-giantarray.rep`：稳定失败。
  - 报错：`FAILURE. Ran out of memory for emulation`
- 其他 giant trace 的局部结果：
  - `syn-giantarray-short.rep`：通过
  - `syn-giantarray-med.rep`：通过
  - `syn-giantmix.rep`：通过

这说明当前状态基本是：

- 基础接口行为已经成立。
- final 的主要问题不在 correctness，而在：
  - `mdriver-emulate` 下的大地址/大块场景
  - throughput 明显低于 final 门槛
  - utilization 还有提升空间，但不是第一优先级

## 对当前实现的判断

当前 `mm.c` 不是 handout 起始版 implicit list，而是一个已经比较成熟的实现：

- segregated explicit free lists
- 16-byte mini free block 特判
- header 里保存 `prev_alloc` / `prev_mini`
- regular free block 带 footer，mini free block 不带 footer

这套结构本身可以继续做 final，不建议推倒重写。更稳的路线是保留框架，只修关键路径和策略。

## 关键风险判断

### 1. `mdriver-emulate` 失败是第一 blocker

这是 final 的硬伤，必须先清掉。

当前最可能的原因不止一个，计划不能只押宝单一猜测：

- 高概率原因：giant trace 中触碰了过多 sparse pages。
  - 最可疑的是大块路径上的尾部写入和中间态写入：
    - `extend_heap`
    - `write_block`
    - `split_block`
    - `coalesce_block`
    - `realloc`
  - 现在的逻辑在“扩堆后立刻拿整个大块去分配”时，常常会先把新空间写成 free block，再写 footer，再挂进 free list，再取出来变成 allocated block。
  - 对普通 trace 这没问题，但对 sparse emulate，额外写 block 尾页和链表指针都会消耗页面预算。
- 另一类可能原因：大块复用不积极，导致 heap 增长过多。
  - 这会同时伤 utilization，也会在 emulate 里放大成页数耗尽。
- 还必须排除 64-bit / 大 size 算术问题。
  - 规范要求的是完整 64-bit address space 支持，不是只要不 OOM 就算过关。
  - `malloc` / `realloc` 里的 size 计算、round up、指针偏移都必须单独审计。

因此，Phase 1 不能只写“减少页触碰”，还必须包含大块复用和 64-bit 算术审计。

### 2. throughput 是第二 blocker，而且已经严重到会吞吐分归零

当前 `throughput ≈ 1100 Kops/s`，明显低于本机 final 的最低参考门槛 `2348 Kops/s`。

最慢 trace 很集中：

- `syn-array.rep`
- `syn-array-scaled.rep`
- `syn-string-scaled.rep`
- `syn-struct-scaled.rep`

这说明瓶颈更像是 free-list 搜索/插入策略，而不是单个 API 的常数项。

### 3. utilization 还有差距，但优先级低于前两项

当前 `67.6%` 不算差，但离 final 满分线 `74%` 还有距离。

不过很多吞吐优化和大块复用优化本身就会影响 utilization，所以不适合完全割裂开做。

### 4. 当前 `mm_checkheap` 只能作为护栏，不能当成充分证明

当前 checker 已经能检查：

- 对齐
- `prev_alloc` / `prev_mini`
- footer 一致性
- 相邻 free block 未合并
- heap/free-list free block 数量一致

但它还不足以覆盖后续计划里要修改的所有高风险点，例如：

- free-list duplicate
- free-list cycle
- sorted bucket 是否仍保持有序
- 同一个 free block 是否被插入错误 bucket 后仍侥幸通过计数检查

因此，后续如果要改 `add_block` / `remove_block` / `find_fit` / sorted insertion，就不能只依赖“`-d 2` 没报错”来判断安全。

## 执行原则

- 每次只改 `mm.c` 的一类逻辑，改完立刻回归。
- 不同时修改“大块扩堆路径”和“bucket 策略”，避免问题来源混在一起。
- 所有中间态设计都要围绕以下不变量：
  - 每个物理块的 `prev_alloc` / `prev_mini` 必须和真实前驱一致。
  - regular free block 必须有 footer，mini free block 不得依赖 footer。
  - heap 中不得存在相邻未合并 free blocks。
  - 每个 free block 必须且只能在 free list 中出现一次。
  - epilogue 的 prev bits 必须与最后一个真实块一致。
- 任何为 emulate 做的优化，都不能破坏普通 `mdriver` 和 `mdriver-emulate` 间 utilization 一致性。

## 实施计划

### Phase 0: 先补护栏和验证集

这一步不追求提分，目标是防止后面“修一个坏两个”。

具体动作：

1. 扩大最小回归集。
   - 每做完一类改动，至少跑：
     - `./mdriver-dbg -d 2 -f traces/syn-array-short.rep`
     - `./mdriver-dbg -d 2 -f traces/syn-mix-realloc.rep`
     - `./mdriver -c traces/syn-mix-realloc.rep`
     - `./mdriver-emulate -V -f traces/syn-giantarray-short.rep`
     - `./mdriver-emulate -V -f traces/syn-giantarray.rep`
     - `./mdriver -v 0`
   - 原因：
     - `syn-giantarray*` 需要放在 `mdriver-emulate` 下覆盖 giant-address blocker；它们不适合作为 dense `mdriver-dbg` 的 smoke test
     - `syn-mix-realloc.rep` 覆盖 `realloc` 和 split/coalesce 副作用
     - `-c` 模式会重复初始化，有助于验证 `mm_init` 真正清空状态

2. 明确 64-bit / overflow 审计清单。
   - 逐项检查：
     - `malloc` 中 `size + wsize` 是否可能溢出
     - `realloc` 中 `size + wsize` 是否可能溢出
     - `round_up` 是否会在超大 size 下产生 wraparound
     - 所有 block size / diff / extend size 是否都保持 `size_t`
     - 是否存在依赖 32-bit 常量或隐式窄化的表达式
   - 结论要落实到代码，不要只凭“当前 trace 没炸”判断。

3. 把 checker 当作“调试护栏”而不是“证明器”。
   - 如果后续要修改 free-list 策略，优先考虑把以下检查补进 `mm_checkheap`：
     - bucket 中 block 是否重复出现
     - 非 mini bucket 的链表是否无断链
     - 若保留 sorted insertion，排序是否仍成立
   - 如果不打算先补 checker，就不要在同一轮里同时改 free-list 插入策略和 giant block 路径。

Phase 0 完成标准：

- 形成固定 smoke test 套件
- 明确列出需要审计的 64-bit/overflow 风险点
- 后续每轮改动都遵守“一类修改，一次回归”

### Phase 1: 优先修 `mdriver-emulate`

这一步的目标是：在不破坏 correctness 的前提下，让 giant trace 在 sparse emulate 下稳定通过。

具体动作：

1. 审计所有会写 block 尾部或引入中间 free 状态的路径。
   - 重点检查：
     - `extend_heap`
     - `write_block`
     - `split_block`
     - `coalesce_block`
     - `realloc`
   - 区分：
     - 这个 footer / 链指针写入是后续逻辑必需的
     - 还是只是一个马上会被覆盖的中间态写入

2. 设计“扩堆后直接放置”的专用路径，但必须先写清楚前驱三种情况。
   - 情况 A：旧 epilogue 显示前一块已分配。
     - 这是最适合 direct place 的情况。
     - 可以在新空间里直接构造 allocated block，只有 remainder 真能作为 free block 留在 heap 中时，才物化 remainder。
   - 情况 B：前一块是 mini free block。
     - 不能简单跳过 coalesce，否则会留下相邻 free block 或搞错 backward traversal。
     - 更稳的做法是：要么先与前驱逻辑合并后再 place，要么禁止这种情况下走 direct place。
   - 情况 C：前一块是 regular free block。
     - 同样不能盲目 direct place。
     - 必须保证最终状态仍满足“所有相邻 free block 已合并”。

3. 避免不会停留为 free 状态的块进入 free list。
   - 如果一个新扩出来的块最终会立刻分配给当前请求，就不要先插入 segregated list 再删掉。
   - 这既减少元数据写入，也减少 emulate 下的页面触碰。

4. 精确更新 `prev_alloc` / `prev_mini` / epilogue。
   - direct place 路径最容易把 next block 的 prev bits 和 epilogue 写错。
   - 这部分必须逐路径核对，而不是依赖“最后调用一次 `update_next_prev_info`”糊过去。

5. 同步完成 64-bit/overflow 防护。
   - `malloc` / `realloc` 的 size 归整逻辑必须在本阶段补上 overflow guard。
   - 否则 giant trace 即使不再 OOM，仍可能在 emulate 或隐藏测试里出问题。

Phase 1 完成标准：

- `./mdriver-emulate -V -f traces/syn-giantarray.rep` 通过
- 完整 `./mdriver-emulate -V` 通过
- 不仅没有 `Ran out of memory for emulation`，也没有 dense/emulate utilization 不一致等额外 deduction
- 常规 `./mdriver -V` correctness 不回退

### Phase 2: 先收吞吐的明显悬崖，同时监控 heap 增长

在当前基线下，throughput 已经低到会直接丢分，所以 emulate 过后应先处理吞吐大坑，而不是先单独追 utilization。

具体动作：

1. 缩短 `find_fit` 的平均扫描长度。
   - 当前实现更接近“受限 best-fit”。
   - 对 `syn-array*` 这类长 trace，通常会比简单 first-fit 更慢。
   - 可以优先尝试：
     - 小 bucket 直接 first-fit
     - 大 bucket 才保留有限搜索
   - 但每做一轮都要重新看 giant trace，确认没有因为“找不到合适大块”而让 heap 继续膨胀。

2. 放宽 sorted insertion 的使用范围。
   - 现在 `index >= 9` 会做 size-sorted insertion。
   - 这会提高 `free` 的成本，也会增加链表修改次数。
   - 更现实的路线是：
     - 只在极大块 bucket 保留排序
     - 其余 bucket 改为 LIFO

3. 减少热路径中的重复 metadata 更新。
   - 重点审计：
     - `split_block`
     - `free`
     - `realloc`
     - `coalesce_block`
   - 尤其是重复写 footer、重复写 next block prev info 的地方。

4. 用最慢 trace 做定点回归。
   - 每次吞吐优化后都至少看：
     - `syn-array.rep`
     - `syn-array-scaled.rep`
     - `syn-string-scaled.rep`
     - `syn-struct-scaled.rep`

Phase 2 完成标准：

- 本地 `./mdriver -v 0` 的 throughput 明显抬升
- 最低目标：逼近或超过 `2348 Kops/s`
- 中间目标：先把当前约 `1100 Kops/s` 提到 `1800~2200`
- `mdriver-emulate` 不因搜索策略修改而重新退化

### Phase 3: 再做 utilization 精修

这一步不是从零开始优化，而是在 emulate 和吞吐已经站稳后，再处理碎片和复用质量。

具体动作：

1. 重新检查 split 策略。
   - 当前只要 remainder `>= 16` 就会 split。
   - 需要确认这是否在 `syn-array*` 等分布中制造过多 16-byte / 32-byte 小碎片。

2. 继续保留并打磨 in-place `realloc`。
   - 当前已经会尝试吞并 next free block。
   - 还要检查 shrink/grow 时是否在制造没有价值的小 remainder。

3. 检查大块复用是否足够积极。
   - 重点关注大 bucket 搜索是否过早停止，导致明明有可复用块却继续扩堆。
   - 这既伤 utilization，也会重新伤到 emulate。

4. 只在必要时调整扩堆策略。
   - `chunksize` 和大请求扩堆策略可以调，但这属于后置微调。
   - 不要在 giant path 还没稳定前就同时改扩堆策略和 bucket 策略。

Phase 3 完成标准：

- 常规 `./mdriver -V` correctness 仍全过
- utilization 从当前 `67.6%` 提高到至少 `70%+`
- 理想目标是靠近 `74%`
- throughput 不因为 utilization 调优而再次掉回危险区

### Phase 4: 最终验收

最后统一做一次完整检查：

- `make`
- `./mdriver -V`
- `./mdriver-emulate -V`
- `./mdriver-dbg -d 2`
- `./mdriver-uninit`
- `./check-format`
- `perl macro-check.pl mm.c`

验收口径：

- 所有 correctness 通过
- `mdriver-emulate` clean pass
- 无 warning
- `mm_checkheap` 在正常情况下安静返回
- 没有未初始化内存读取
- 正常运行无调试输出
- writable globals 仍不超过 128 bytes
- dense 与 emulate 的 utilization 行为一致

## 推荐执行顺序

严格按下面顺序推进：

1. 先补验证护栏和 64-bit/overflow 审计
2. 再解决 giant emulate fail
3. 再处理 throughput 的明显悬崖
4. 最后做 utilization 精修

原因：

- 先补护栏，后面每次修改才有可靠反馈
- emulate fail 是 final 的硬伤，必须先清掉
- throughput 现在已经低到直接丢分，优先级高于 utilization 精修
- utilization 调优往往会和 free-list 策略耦合，适合放在吞吐大坑处理之后做微调

## 明确不做的事

- 不重写 allocator，不切换到完全不同的数据结构。
- 不在同一轮里同时改：
  - giant block 扩堆路径
  - free-list bucket 策略
  - 扩堆尺寸策略
- 不把“`-d 2` 没报错”当作 free-list 绝对正确的证明。

## 结论

最稳的路线仍然是保留当前框架，只做局部但关键的修复：

- 先补验证护栏与 64-bit 审计
- 再修大块扩堆/direct place 路径，消除 emulate fail
- 再收 `find_fit` / insertion 的吞吐悬崖
- 最后精修碎片与复用，把 utilization 往 `74%` 靠

如果这几步按顺序做，并且每一步都用固定回归集兜住，当前实现是有机会从“checkpoint 可过”推进到“final 可过且分数合理”的。
