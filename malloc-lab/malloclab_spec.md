# Malloc Lab 技术要求文档

## 1. 文档目的

本文档用于整理 `Malloc Lab: Writing a Dynamic Storage Allocator` 的**明确技术要求、约束、评分规则与提交流程**。  
本文档**不包含实现方案、优化设计或代码结构建议**，仅作为开发与验收的规格说明。

---

## 2. 作业概述

本实验要求实现一个动态内存分配器，行为对应以下标准接口：

- `malloc`
- `free`
- `realloc`
- `calloc`

目标要求：

- **Correct**：行为正确，不出现非法重叠、数据破坏、错误返回等问题
- **Efficient**：空间利用率高
- **Fast**：吞吐量高

---

## 3. 提交版本与权重

本作业需要提交两个版本：

| 版本 | 权重 |
|---|---:|
| Checkpoint | 4% |
| Final | 7% |

> 说明：原 PDF 封面写的是 Spring 2026，但首页 due date 表格中年份写成了 2025。本文档仅保留评分与提交结构，不对该年份冲突做额外解释。

---

## 4. 交付物

唯一需要提交的文件：

- `mm.c`

要求：

- 所有分配器相关代码必须放在 `mm.c`
- 其他源文件和头文件不能作为提交内容的一部分被 Autolab 采纳
- 即使本地修改了其他文件，Autolab 也**只处理 `mm.c`**

---

## 5. 必须实现的函数

必须在 `mm.c` 中实现以下函数，且不得修改 `mm.h` 中接口定义：

```c
bool mm_init(void);
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
bool mm_checkheap(int line);
```

---

## 6. 每个函数的行为要求

### 6.1 `bool mm_init(void)`

职责：

- 执行初始化工作，例如建立初始 heap 区域、初始化全局状态等

返回值：

- 初始化成功返回 `true`
- 初始化失败返回 `false`

强制要求：

- 每次 driver 开始新 trace 时都会调用 `mm_init`
- 因此，`mm_init` **必须重新初始化所有数据结构**
- 不允许依赖前一个 trace 的残留状态

---

### 6.2 `void *malloc(size_t size)`

职责：

- 返回一个至少可容纳 `size` 字节 payload 的已分配块指针

强制要求：

- 返回的整个块必须位于 heap 区域之内
- 返回块不得与其他已分配块重叠
- 返回指针必须指向 **payload 起始位置**
- 返回的指针必须始终满足 **16-byte alignment**
  - 即使 `size < 16` 也必须 16 字节对齐

---

### 6.3 `void free(void *ptr)`

行为要求：

- 如果 `ptr == NULL`，则什么都不做
- 否则，`ptr` 必须满足以下条件：
  - 指向先前由 `malloc` / `calloc` / `realloc` 返回的块的 payload 开头
  - 且该块尚未被释放

效果：

- 释放该块
- 无返回值

---

### 6.4 `void *realloc(void *ptr, size_t size)`

行为要求：

#### 情况 A：`size != 0` 且 `ptr != NULL`

必须：

- 分配一个新的、至少包含 `size` 字节 payload 的块
- 将旧块中的数据拷贝到新块
- 拷贝字节数为以下两者较小值：
  - `size`
  - 原块 payload 大小
- 释放旧块
- 返回新块指针

#### 情况 B：`size != 0` 且 `ptr == NULL`

行为必须等同于：

```c
malloc(size)
```

#### 情况 C：`size == 0`

行为必须等同于：

```c
free(ptr);
return NULL;
```

说明：

- `realloc` 对吞吐量和利用率影响较小
- 重点是**行为正确**

---

### 6.5 `void *calloc(size_t nmemb, size_t size)`

职责：

- 分配 `nmemb * size` 字节的数组空间
- 将分配到的所有字节初始化为 `0`
- 返回分配内存的指针

说明：

- `calloc` 对吞吐量和利用率影响较小
- 重点是**行为正确**

---

### 6.6 `bool mm_checkheap(int line)`

职责：

- 扫描整个 heap，并检查数据结构是否一致、是否存在错误

要求：

- 这是 heap consistency checker
- 会被用于调试和评分
- driver 可能会调用该函数；若由 driver 调用，参数始终为 `0`
- 也可以在代码中用 `__LINE__` 作为参数辅助定位问题，例如：

```c
mm_checkheap(__LINE__);
```

输出行为要求：

- 没发现错误时，应当**安静运行**并返回 `true`
- 只有在发现错误时，才打印信息并返回 `false`
- 不允许在正常情况下产生大量输出

---

## 7. 提供的基线代码说明

提供了两个版本的内存分配器：

### 7.1 `mm.c`

- 一个**可工作的 implicit-list allocator**
- 推荐作为起点
- 但**未实现 block coalescing**

影响：

- 缺少 coalescing 会导致较高的 external fragmentation
- 会显著影响性能与空间利用率表现

### 7.2 `mm-naive.c`

- 一个可以运行得很快的实现
- 但由于几乎不复用已释放内存，utilization 很差

---

## 8. 64-bit 地址空间支持要求

这是强制要求。

分配器必须：

- 在 **64-bit machine** 上正确运行
- 支持**完整 64-bit address space**
- 不能只依赖当前 x86-64 常见的 48-bit 地址空间假设

影响：

- 类型选择必须正确
- 不能错误使用 `int` 代替 `size_t`、指针大小字段等
- 表达式中不能无意引入 32-bit 算术，例如不正确的位移常量写法

Autolab 会通过 `mdriver-emulate` 专门测试这一点。

---

## 9. 可用支持函数与允许调用的外部函数

### 9.1 `memlib.c / memlib.h` 提供的 primitive

#### `void *mem_sbrk(intptr_t incr)`

作用：

- 将 heap 扩展 `incr` 字节
- 返回新扩展区域的起始地址

返回值：

- 成功：返回新区域起始地址
- 失败：返回 `(void *) -1`

注意：

- `mem_sbrk` **不能缩小 heap**
- 若 `incr < 0` 会失败
- 每次 `mm_init` 被调用时，heap 都会被重置为 0 字节

---

### 9.2 其他辅助函数

```c
void *mem_heap_lo(void);
void *mem_heap_hi(void);
size_t mem_heapsize(void);
```

含义：

- `mem_heap_lo()`：heap 第一个有效字节
- `mem_heap_hi()`：heap 最后一个有效字节
- `mem_heapsize()`：当前 heap 总大小（字节）

注意：

- `mem_heap_hi()` 返回的是**最后一个有效字节地址**
- 不一定是对齐地址

---

### 9.3 允许调用的标准库函数

仅允许使用以下标准 C 库函数：

- `memcpy`
- `memset`
- `printf`
- `fprintf`
- `sprintf`

除本节列出的函数外，`mm.c` 必须保持**自包含**

---

## 10. 编程规则

### 10.1 不得识别当前 trace

禁止：

- 编写代码判断当前运行的是哪一个 trace，并针对 trace 特判

处罚：

- 若检测到 allocator 试图识别 trace，扣 **20 分**

允许：

- 编写根据运行时一般特征进行自适应调整的 allocator

---

### 10.2 不得修改接口与其他文件

禁止修改：

- `mm.h`
- 其他 `.c` / `.h` 文件
- `Makefile`

允许：

- 在 `mm.c` 内定义 `static` 辅助函数

---

### 10.3 必须无 warning 编译通过

要求：

- 使用课程提供的 warning flags 编译时，代码必须**无警告**

---

### 10.4 可写全局变量总量限制：128 字节

禁止：

- 在 `mm.c` 中声明大型全局数据结构，例如大数组、树、链表等

允许：

- 小型全局数组、结构体、标量变量
- 任意数量的 `const` 常量数据

硬性限制：

- **所有可写全局变量总量不得超过 128 bytes**

原因：

- 全局变量不会被计入 utilization
- 若需要大结构，应当放在 heap 内部

违规后果：

- Autolab 会自动检查，并可能扣分

---

### 10.5 需要尽量减少未定义行为

由于 allocator 操作 heap 原始字节，不可避免会涉及某些 C 标准意义下的未定义行为。要求：

- 尽量减少此类行为
- 尽量通过 `struct` / `union` 建模
- 尽量把指针运算封装到少量短小辅助函数中

---

### 10.6 关于 zero-length array

基线代码中使用了 zero-length array 表示 payload 字段。要求理解：

- 这是非标准扩展
- 本实验中允许这种写法
- 它不同于 C99 flexible array member
- 在本实验背景下，这是推荐方式之一

---

### 10.7 宏使用限制

允许：

- 仅用于定义**常量**
- 或定义**调试宏**

调试宏要求：

- 名字必须以 `dbg_` 开头
- 当 `DEBUG` 未定义时，必须**没有任何效果**

允许示例：

```c
#define DEBUG 1
#define CHUNKSIZE (1<<12)
#define WSIZE sizeof(uint64_t)
#define dbg_printf(...) printf(__VA_ARGS__)
```

禁止示例：

```c
#define GET(p) (*(unsigned int *)(p))
#define PACK(size, alloc) ((size)|(alloc))
```

说明：

- 自动检查器会检查非法宏
- 该检查器较严格，代码必须在其检查下无警告

---

### 10.8 不可使用网上 malloc 实现代码

允许：

- 阅读高层算法描述
- 参考教材或课程提供实现
- 借鉴通用数据结构/算法代码（如链表、哈希表、树、优先队列等），前提是：
  - 这些代码本身不是 malloc 实现的一部分
  - 且必须在注释中注明来源

禁止：

- 查看、复制、改写网上的 malloc allocator 代码
- 使用除教材、K&R、课程 handout 代码外的现成内存分配器代码

---

### 10.9 对齐要求

强制要求：

- allocator 返回的所有指针必须为 **16-byte aligned**

driver 会检查这一点。

---

## 11. Driver 程序

运行 `make` 后会生成 4 个程序：

| 程序 | 用途 |
|---|---|
| `mdriver` | 正式 correctness / utilization / throughput 测试 |
| `mdriver-emulate` | 测试完整 64-bit 地址空间支持 |
| `mdriver-dbg` | 调试版本，带 DEBUG、`-O0`、AddressSanitizer |
| `mdriver-uninit` | 调试版本，检测未初始化内存使用（MemorySanitizer） |

---

## 12. 各 driver 的具体作用

### 12.1 `mdriver`

用于测试：

- 正确性
- 空间利用率
- 吞吐量

Autolab 使用它计算性能分数基础值。

---

### 12.2 `mdriver-emulate`

用于测试：

- 是否支持完整 64-bit 地址空间
- 是否能处理巨大的分配请求

说明：

- 这是模拟，不会真的分配 exabytes 内存
- 但会验证你的 allocator 是否**逻辑上**能支持那样的地址范围

若失败，会触发自动扣分。

---

### 12.3 `mdriver-dbg`

特征：

1. 定义了 `DEBUG`，因此 `dbg_` 宏会生效
2. 编译时使用 `-O0`
3. 启用了 AddressSanitizer

作用：

- 便于调试越界、非法访问等问题

---

### 12.4 `mdriver-uninit`

作用：

- 使用 MemorySanitizer 检测未初始化内存读取

---

## 13. Trace 文件

driver 由 `traces/` 目录下的 trace 文件驱动。  
每个 trace 文件包含一系列操作命令，用来按顺序调用你的：

- `malloc`
- `realloc`
- `free`

说明：

- Autolab 使用相同 trace 文件评分
- 每个 trace 会被运行多次：
  - 一次 correctness
  - 一次 utilization
  - 3 到 20 次 throughput
- 有些短 trace 仅用于调试和查错
- **只有带 `*` 的 trace 才计入成绩**

---

## 14. Driver 常用命令行参数

### `-C`

- 使用 Checkpoint 的评分标准

### `-f tracefile`

- 只运行指定 trace
- 测 correctness / utilization / performance

### `-c tracefile`

- 只运行指定 trace 的 correctness 测试
- 仍会运行两次，以验证 `mm_init` 是否能正确重置 heap

### `-v level`

设置输出详细程度：

- `0`
- `1`（默认）
- `2`

### `-d level`

设置 driver 进行的有效性检查程度：

- `0`：几乎不检查，适合只测性能
- `1`：检查 payload 是否被无关操作破坏（默认）
- `2`：每次操作后都调用 `mm_checkheap`

---

## 15. 评分结构

## 15.1 作业总分构成

### Checkpoint

| 项目 | 分值 |
|---|---:|
| Autograded score | 100 |
| Heap checker | 10 |
| 总计 | 110 |

### Final

| 项目 | 分值 |
|---|---:|
| Autograded score | 100 |
| Code style | 4 |
| 总计 | 104 |

---

## 15.2 Autograded score 计算流程

`driver.pl` 先运行 `mdriver` 获得性能指数 `P`：

- 若任何 trace correctness 失败，则 `P = 0`
- 否则根据 utilization 与 throughput 计算 `P`

之后再运行 `mdriver-emulate`：

- 检测 `mdriver` 无法发现的问题
- 如有错误，将从 `P` 中扣分

最终自动评分：

```text
Autograded Score = P - deductions
```

---

## 16. Performance Index 权重

### Checkpoint

- Utilization：20%
- Throughput：80%

### Final

- Utilization：60%
- Throughput：40%

---

## 17. Utilization 评分标准

定义：

- 单个 trace 的 utilization =  
  某一时刻 driver 已分配且尚未释放的总内存 / allocator 使用的 heap 大小 的峰值比率

总利用率 `U`：

- 所有 trace utilization 的平均值

评分区间：

### Checkpoint

| 指标 | 数值 |
|---|---:|
| `Umin` | 55% |
| `Umax` | 58% |

### Final

| 指标 | 数值 |
|---|---:|
| `Umin` | 55% |
| `Umax` | 74% |

规则：

- `U <= Umin`：utilization 得分为 0
- `Umin < U < Umax`：线性计分
- `U >= Umax`：utilization 满分

---

## 18. Throughput 评分标准

定义：

- 单个 trace 的 throughput = 每秒完成的操作数（KOPS）

总吞吐量 `T`：

- 所有 trace throughput 的平均值

评分依赖参考吞吐量 `Tref`：

### Checkpoint

- `Tmin = 0.1 * Tref`
- `Tmax = 0.8 * Tref`

### Final

- `Tmin = 0.5 * Tref`
- `Tmax = 0.9 * Tref`

规则：

- `T <= Tmin`：throughput 得分为 0
- `Tmin < T < Tmax`：线性计分
- `T >= Tmax`：throughput 满分

说明：

- 本地 `mdriver` 输出的 throughput 仅供参考
- **Autolab 服务器上的结果才是最终成绩依据**

---

## 19. 自动扣分项

`mdriver-emulate` 可能导致以下扣分：

### 19.1 运行失败

若 `mdriver-emulate` 无法成功运行：

- 扣 **30 分**

典型原因：

- 未正确支持完整 64-bit 地址空间
- 错误使用 `int` 等不合适类型

### 19.2 utilization 与普通 driver 不一致

若某个 trace 的 utilization 在：

- `mdriver`
- `mdriver-emulate`

之间不一致：

- 扣 **30 分**

### 19.3 可写全局变量超限

若使用了超过 128 bytes 的 writable globals：

- 最多扣 **20 分**

---

## 20. `mm_checkheap` 评分要求

`mm_checkheap` 在 **Checkpoint** 中手动评分，共 **10 分**。  
Final 不再单独评分 heap checker。

需要检查的内容取决于你的数据结构设计，但 handout 明确给出以下检查项示例。

### 20.1 检查整个 heap

适用于 implicit / explicit / segregated list：

- prologue block 是否存在
- epilogue block 是否存在
- 每个 block 地址是否正确对齐
- 每个 block 是否位于 heap 边界内
- header / footer 是否一致
- size 是否合法（例如最小块大小）
- 前驱/后继已分配或空闲状态位是否一致
- 是否存在未合并的连续 free blocks

### 20.2 检查 free list

适用于 explicit / segregated list：

- next / previous 指针是否互相一致
- free list 指针是否位于 `mem_heap_lo()` 与 `mem_heap_high()` 之间
- 通过遍历 heap 与遍历 free list 统计得到的 free block 数量是否一致
- 若使用 segregated lists，各 bucket 中 block 是否落在正确大小范围内

### 20.3 输出要求

- 正常情况下必须**静默**
- 只有发现错误时才打印信息并返回 `false`
- 无错误时返回 `true`

---

## 21. 代码风格要求

Final 中会有 **4 分** code style 分。

明确要求包括：

- 使用版本控制（Git）
- 避免 magic numbers
- 用 `#define` 常量或 `static const` 替代散落常数
- 优先使用 `sizeof`、`offsetof`
- `mm.c` 文件开头必须有总览注释，说明：
  - allocated block / free block 的结构
  - free list 的组织方式
  - allocator 如何操作 free list
- 每个函数必须有函数头注释，至少说明：
  - purpose
  - arguments
  - return value
  - relevant preconditions / postconditions
- 复杂逻辑需要有 inline comments
- 代码需模块化、稳健、易扩展
- 必须使用 `clang-format`
  - 运行方式：`make format`

---

## 22. 提交流程

提交前要求：

- 正常运行时不得打印任何调试信息
- 所有 debugging macros 必须禁用
- 必须保证代码已提交并推送到 GitHub 最新版本

提交方式：

- 运行 `make submit`
- 或直接上传 `mm.c` 到 Autolab

规则：

- **只有最后一次提交会被评分**

---

## 23. 调试与使用说明相关要求

虽然以下内容不直接构成评分点，但 handout 明确强调其使用方式：

- 使用 `-c` 和 `-f` 运行单个 trace 进行调试
- 使用 `-V`/verbose 模式辅助定位出错 trace
- 使用 `gdb`
- 调试时优先使用 `mdriver-dbg`
- 可以使用 `gdb watch`
- 使用结构体、联合体和辅助函数减少裸指针运算复杂度
- 必须确保你的 allocator 适用于 64-bit address space

---

## 24. GDB 辅助函数

PDF 附录提供了用于调试 heap 的函数：

```c
void hprobe(void *ptr, int offset, size_t count);
```

作用：

- 打印从 `ptr + offset` 开始的 `count` 个字节内容

用途：

- 在 `mdriver` 和 `mdriver-emulate` 下都可用于检查 heap
- 特别适用于 emulated 大地址空间下，普通指针解引用无法直接工作的问题

---

## 25. 验收清单

在提交前，应确认以下项目全部满足：

### 基础接口
- [ ] `mm_init` / `malloc` / `free` / `realloc` / `calloc` / `mm_checkheap` 已实现
- [ ] 未修改 `mm.h`

### 行为正确性
- [ ] `malloc` 返回 payload 指针
- [ ] 返回指针始终 16-byte aligned
- [ ] 分配块位于 heap 内且不重叠
- [ ] `free(NULL)` 无副作用
- [ ] `realloc` 三种语义正确
- [ ] `calloc` 分配并清零

### 64-bit 支持
- [ ] 可通过 `mdriver-emulate`
- [ ] 不依赖 32-bit 假设
- [ ] size / pointer 相关类型使用正确

### 约束遵守
- [ ] 仅修改 `mm.c`
- [ ] 无非法宏
- [ ] 无大型 writable globals
- [ ] writable globals 总量不超过 128 bytes
- [ ] 编译无 warning

### 调试与检查
- [ ] `mm_checkheap` 能静默通过正常情况
- [ ] 出错时返回 `false` 并打印定位信息
- [ ] 在 `-d 2` 模式下可辅助定位错误

### 提交前
- [ ] 正常运行无调试输出
- [ ] debug 宏已禁用
- [ ] 已执行最终提交

---

## 26. 评分参数速查表

### 26.1 Checkpoint

| 项目 | 数值 |
|---|---:|
| Utilization 权重 | 20% |
| Throughput 权重 | 80% |
| `Umin` | 55% |
| `Umax` | 58% |
| `Tmin` | `0.1 * Tref` |
| `Tmax` | `0.8 * Tref` |

### 26.2 Final

| 项目 | 数值 |
|---|---:|
| Utilization 权重 | 60% |
| Throughput 权重 | 40% |
| `Umin` | 55% |
| `Umax` | 74% |
| `Tmin` | `0.5 * Tref` |
| `Tmax` | `0.9 * Tref` |

---

## 27. 结论

本实验的核心不是仅“写出可运行代码”，而是在严格约束下实现一个：

- 接口语义正确
- 支持完整 64-bit 地址空间
- 满足 16-byte 对齐
- 遵守代码与全局变量限制
- 能通过 driver correctness / utilization / throughput / emulate 检查
- 并提供高质量 heap checker 的动态内存分配器

本文档可直接作为 `malloclab_spec.md` 使用。