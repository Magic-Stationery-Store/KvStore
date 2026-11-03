# 从 WAL/MemTable 到 SSTable、后台 Flush 和 MANIFEST：学习单

适用项目：当前 `kvstore`，截至 2026-10-12。衔接 `WAL_MEMTABLE_STUDY.md`，覆盖随机模型测试、独立 SSTable、多 L0 查询、Immutable MemTable、后台 Flush 和 MANIFEST。

学习目标：不用看完整答案，能独立写出一个小模块、预测每层数据的状态，并用测试检验预测。建议每单元分两次完成：第一次读和观察，第二次自己写。完成上一单元再继续，不必按固定天数赶进度。

> 旧学习单保留了当时的项目结构。当前 `Compact()` 已改为生成带编号的基础文件；请以本学习单的文件位置和命令为准。

## 0. 当前项目全貌

```text
写入：Put/Delete → Active WAL → Active MemTable
                                │ 达到阈值
                                ▼
                Immutable WAL + Immutable MemTable
                                │ 后台写入
                                ▼
                  L0 SSTable → 提交 MANIFEST → 回收 Immutable WAL

读取：Active MemTable → Immutable MemTable → L0 从新到旧 → 当前基础文件

完整压缩：合并最终状态 → 新基础文件 → 提交 MANIFEST → 回收旧文件
```

当前阈值是 **不同 key 的数量**，默认 64。一个 key 写 1000 次，Active MemTable 通常仍只有一项。当前只有一个后台 Flush 线程、一个 Immutable MemTable；L0 表允许 key 重叠，暂未实现 L1。

| 文件 | 含义 | 怎么看 |
| --- | --- | --- |
| `kvstore.log.wal` | Active MemTable 的恢复日志 | `xxd -g 1` |
| `kvstore.log.wal.L0.<编号>.imm` | 尚未完成发布的 Immutable MemTable 的恢复日志 | `xxd -g 1`；可能短暂出现 |
| `kvstore.log.L0.<编号>.sst` | 按 key 排序的不可变表；有效性由 MANIFEST 决定 | `xxd -g 1` |
| `kvstore.log.manifest` | 当前基础文件、有效 L0、下一编号、退役截止点和 CRC | `cat` |
| `kvstore.log.base.<编号>.log` | 完整 Compact 生成的基础数据 | `xxd -g 1` |
| `kvstore.log` | 旧格式基础文件；仅当 `BASE 0` 时被选用 | `xxd -g 1` |
| `kvstore.log.lock` | 防止重复打开数据库的进程锁文件 | 无须解析内容 |

文件由**程序启动时的工作目录**决定，和可执行文件所在目录不同。自动测试通常会清理测试数据库；想持续观察文件，使用 CLI 实验。

### 统一编译和验证

下文统一使用 `build-ubuntu`：

```bash
cd /mnt/e/VsCode/Cmake/kvstore
cmake -S . -B build-ubuntu -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ubuntu -j2
ctest --test-dir build-ubuntu --output-on-failure
```

当前注册 12 项测试。查看某项细节用 `-R` 选择并加 `-V`；`--output-on-failure` 主要在失败时输出细节。编译后运行，避免运行旧的可执行文件。

### 每个单元的固定学习方法

1. 先看测试场景，写下自己的预测。
2. 运行测试，比较预测和结果。
3. 只追踪与场景有关的函数，画出数据状态。
4. 在独立的 `learning/` 练习文件里自己写小程序或测试。
5. 对照验收条件，能解释失败原因才算完成。

记录模板：

| 步骤/操作 | Active | Immutable | 有效 L0/BASE | 查询结果 | 恢复来源 |
| --- | --- | --- | --- | --- | --- |
| 在这里先填写预测 | | | | | |

## 1. 随机模型测试：从“看 Passed”到验证状态一致性

**先读**：`tests/model_test.cpp` 的 `get_from_model`、`check_key`、`check_all_keys` 和主循环。

简单的 `unordered_map<string,string>` 保存正确答案，KVStore 保存实际结果；双方收到相同的 Put/Delete，然后比较 Get 和 Delete 的返回值。Compact、Flush、重开数据库都不改变参考模型，因为这些操作不应该改变逻辑数据。

```text
参考模型：保存最终键值
被测系统：WAL、内存表、文件、恢复、压缩
比较目标：可观察的结果相同
```

### 新知识

- `std::mt19937` 与分布：产生操作类型、key 和 value；概率控制通过数值区间实现。
- 固定种子：同一可执行程序环境下便于复现；跨标准库实现时分布映射未必完全一致，失败时最好也保留操作序列。
- `unique_ptr::reset()`：销毁旧对象，再创建新对象验证重开恢复；这不是模拟机器断电。
- 对拍：对照一个简单、独立的参考实现，而非仅检查程序有没有崩溃。
- 失败记录：种子、步骤、操作、key、预期值、实际值。

### 运行

```bash
ctest --test-dir build-ubuntu -R '^kvstore_model_tests$' -V
```

### 自己完成的练习

复制为 `learning/model_low_limit_test.cpp`，把测试路径改为 `low-limit-practice-test.log`。所有创建 KVStore 的位置（包括重开）都使用：

```cpp
std::make_unique<KVStore>(path, 3);
```

原测试只有 10 个 key、阈值 64，随机 Put/Delete 本身不会触发自动 Flush；原测试中的 Compact 会使用 Flush 路径。阈值改为 3 后，能覆盖更多自动冻结与后台提交的组合。

继续补充：每次 Flush、Compact、重开后比较全部 10 个 key；保留空字符串测试；失败时输出足够的操作历史。不要只把操作次数从 1000 改成更大数值。

编译独立练习的通用命令：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -I. \
  learning/model_low_limit_test.cpp build-ubuntu/libkvstore.a \
  -pthread -o build-ubuntu/model_low_limit_practice
./build-ubuntu/model_low_limit_practice
```

**验收**：能解释模型为什么不需要 WAL；能打印一条可复现的失败记录；能说明你的测试实际触发了哪些维护操作。

## 2. 独立 SSTable：排序、不可变和文件偏移

**阅读顺序**：`sstable.h` → `tests/sstable_test.cpp` → `src/sstable.cpp` 的 `build`、构造函数、`get`。

当前 `SSTable::Entries` 是：

```cpp
std::map<std::string, std::optional<std::string>>
```

遍历 `map` 得到排序后的 key，因此即使按 zebra、apple、banana 插入，写入磁盘仍是 apple、banana、zebra。一个 SSTable 中每个 key 只有一个最终状态。

### 当前真实实现

```text
build：8 字节 SSTB/version 文件头 + 有序记录（复用 CRC 记录格式）
打开：顺序读取、验证记录和排序，建立 vector 索引
查询：lower_bound 找索引 → seekg 到 value_offset → read value
```

每个索引项含 key、tombstone、value_offset、value_size。**当前是每个 key 一项的完整内存索引**，还没有稀疏块索引、磁盘索引区、Bloom Filter 或 Block Cache。打开时会扫描和校验整个文件；之后不会长期把全部 value 留在内存。

### 新知识

- `using Entries = ...`：为复杂类型起别名，不会产生新容器。
- `static build()`：构建文件不依赖已经存在的 SSTable 对象；构造函数负责打开已有文件。
- `std::lower_bound`：找第一个“不小于目标 key”的位置，找到后仍要比较是否相等。
- `vector`：保存有序索引，随机访问适合二分；查索引需要 O(log N) 次比较，还要读取磁盘值。
- `seekg/tellg`：定位和记录文件位置。value 可以含 `\0`，不能把它当作以零结尾的 C 字符串。
- 不可变：SSTable 公开接口没有 Put/Delete；新版本使用新文件，原文件不能反复覆盖。

### 运行

```bash
ctest --test-dir build-ubuntu -R '^kvstore_sstable_tests$' -V
```

### 独立小练习（先不碰文件）

自己定义 `vector<Entry>`，每项只有 key、value。写一个 `find_key()`：

```text
输入索引：apple、banana、zebra
查 apple、banana、zebra → 找到
查 aardvark、avocado、zzzz → 没找到
空索引 → 没找到
```

先手写二分，再改成 `lower_bound`。重点测试边界和“插入位置存在但 key 不相等”的情形。最后再用 `SSTable::build` 构建表、销毁对象、重开查询，把算法与文件串起来。

**验收**：能写出文件头和记录字段；能解释 value_offset 的作用；能说明二分是在内存索引中进行，而不是直接对变长磁盘记录做二分。

## 3. 三态查询与多个 L0：删除为什么不能丢

三态描述的是**一个 key 在某个表中的状态**：

| found | value | 下一步 |
| --- | --- | --- |
| false | nullopt | 此表无记录，继续查更旧层 |
| true | nullopt | 此表记录删除，停止并返回不存在 |
| true | 有字符串（可以为空） | 此表有值，停止并返回值 |

例如：

```text
新 L0：a=new，b=tombstone，d=four
旧 L0：a=old，b=keep，c=stable
```

预测 a、b、c、d、missing 的结果，写出每个 key 实际查过哪些层。b 的删除标记在 Flush 时必须保留，因为旧表仍有 b=keep；当前完整 Compact 处理所有旧层后，才可以丢弃这个删除标记和旧值。

**先读**：`get_locked()`、`remove()`、`tests/level0_flush_test.cpp`。

### 运行

```bash
ctest --test-dir build-ubuntu -R '^kvstore_level0_flush_tests$' -V
```

测试阈值为 3；在断言文件数量前调用 `flush()` 等待后台完成。当前自动 Flush 已异步，达到阈值后立即检查 `.sst` 数量会有竞态。

### 新知识与自己写的测试

- `rbegin/rend`：列表按旧到新存储，读取时倒序查找。
- generation：标识文件先后顺序，不是所有 key 的事务版本号。
- 查询优先级：先看最新层；删除标记与实际值都会终止查询。
- 阈值：计数是 map 的不同 key 数量，tombstone 也占一项。

独立写一组测试：阈值 2，第一次写 a=old、b=keep 并 Flush；第二次写 a=new、删除 b 并 Flush；重开后验证 a=new、b 不存在。不要只测不重叠的 key，那样测不出层次顺序错误。

**验收**：不用看答案画出两张表；解释为什么 `optional<string>` 单独作为 SSTable 的结果类型不够；能区分空字符串与 tombstone。

## 4. Immutable MemTable、后台线程与 WAL 轮换

**阅读顺序**：`kv_store.h` 的后台相关成员 → `freeze_memtable_locked()` → `background_flush_loop()` → 析构函数 → `tests/background_flush_test.cpp`。

达到阈值后的过程：

```text
旧 Active WAL 同步并改名为 .imm
创建并同步新 Active WAL
旧 Active MemTable 冻结为 Immutable
新 Active MemTable 接收写入
后台生成 SSTable、提交 MANIFEST、删除旧 .imm
```

如果后台尚未写完：Immutable 中 name=alice，Active 中 name=bob，读取必须返回 bob。

### 必须补齐的 C++ 概念

| 工具/概念 | 作用 | 独立练习 |
| --- | --- | --- |
| `std::thread` / `join` | 执行后台任务，退出前等线程结束 | 开线程计算结果，再 join 后检查 |
| `mutex` | 保护共享状态 | 列出主线程与后台都访问的成员 |
| `unique_lock` | 支持等待时释放锁、醒来后重获锁 | 解释为何 wait 不能用 lock_guard |
| `condition_variable` | 无任务时休眠，有状态变化时通知 | 自己实现一个只有一次任务的 worker |
| 带谓词的 wait | 每次醒来重新判断条件 | 区分“通知发生”与“条件成立” |
| `shared_ptr<const Entries>` | 后台持有只读快照并延长其生命 | 原持有者 reset 后，局部指针仍能读 |
| `swap` / `move` | 转移容器内容和所有权 | 观察交换前后两个 map 的大小 |
| `exception_ptr` | 保存后台异常，前台重新抛出 | 在 worker 中 catch 后保存并由主线程 rethrow |

条件变量等待过程：检查谓词 → 条件不满足时释放 mutex 并休眠 → 唤醒后重获 mutex → 再检查。可能发生虚假唤醒，所以通知不能代替条件检查。通知不保存任务，受锁保护的状态才保存任务。

`shared_ptr<const T>` 保证通过该指针不能修改 T，并不自动保证别的别名绝不修改，也不自动保护成员指针本身的并发赋值。当前项目靠冻结后不再修改快照，以及 mutex 保护共享指针的切换。

### 背压、锁范围和停止

只有一个 Immutable 槽位。若旧槽位仍被后台处理，新 Active 又写满，前台等待槽位释放，以限制内存积压。这是背压，后台线程不保证前台永不阻塞：WAL 轮换及元数据提交仍有同步与锁开销。

析构通知停止并 join；已经冻结的任务会完成，未冻结的 Active 数据留在 Active WAL。后台异常保存后由公开操作报告。`Compact()` 会等待后台完成，再持锁做完整合并。

### 运行和独立练习

```bash
ctest --test-dir build-ubuntu -R '^kvstore_background_flush_tests$' -V
```

该测试连续触发多次 Flush，并人工构造“旧 WAL 已变为 .imm，但 SSTable 尚未生成”的磁盘状态。它验证状态恢复，不是真实机器断电测试，也不证明所有后台交错时序都被覆盖。

先从空文件自己写一个小 worker：主线程在 mutex 保护下设置 `ready=true`、`value=42`，通知后台；后台用谓词 wait 等待，读取 value；主线程 join 后检查结果。不要用 `sleep(1)` 来代替正确同步。能写出这段再回到项目线程循环。

**验收**：能讲清两份 WAL 分别保护什么；列出查询层次；解释背压、谓词等待、析构 join 和后台异常传递。

## 5. MANIFEST：从文件集合到一个可提交的版本

**阅读顺序**：`manifest.h` → `tests/manifest_test.cpp` → `write_manifest/read_manifest` → `publish_manifest` → 当前 `compact()`。

### 文件存在不等于文件有效

新 SSTable 已写完、MANIFEST 尚未提交时，文件只是未提交输出。新 MANIFEST 已提交、旧文件尚未清理时，旧文件只是待回收输入。当前版本由 MANIFEST 的完整清单确定。

示意：

```text
KV_MANIFEST 1
NEXT 3
BASE 0
RETIRED 0
L0 2
TABLE 1
TABLE 2
END
CRC32 <校验值>
```

完整 Compact 可以生成 BASE 3，并提交 NEXT 4、BASE 3、RETIRED 3、L0 0。下一张 L0 使用编号 4；旧 .imm 的编号若不大于 RETIRED，就不能再恢复为新数据。

### 提交顺序

```text
SSTable 或新 BASE 完整写入并 fsync
                  ↓
生成完整 MANIFEST.tmp，带 CRC
                  ↓
fsync 临时清单 → rename 替换正式清单 → fsync 目录
                  ↓
回收旧 WAL / 旧数据文件
```

这里的原子切换针对一个版本的有效文件集合。数据文件与清单是分步骤写入，通过写入顺序和重启规则得到一致版本。`rename` 的原子替换不等于已经完成持久化。

### 逐个预测中断状态

| 中断点 | 重启依据 | 预期 |
| --- | --- | --- |
| Immutable WAL 存在，SSTable 未生成 | .imm 重放 | 生成 SSTable，提交清单 |
| SSTable 完整，但未列入清单；.imm 仍存在 | .imm 是恢复来源 | 从 WAL 重建并提交该表 |
| SSTable 已列入清单，.imm 尚未删 | 清单选择该表 | 保留表，回收重复 WAL |
| 新 BASE 已写完，清单未切换 | 旧清单 | 使用旧 BASE + 旧 L0；新 BASE 是孤儿输出 |
| 新 BASE 已提交，旧 L0 或旧 .imm 尚未删 | 新清单 + RETIRED | 忽略旧文件，删除键不会复活 |
| 清单损坏，或引用文件缺失 | 无法确认有效版本 | 报错；不静默从目录猜测 |

当前只有首次迁移、没有正式 MANIFEST 时，才采用旧版文件发现流程。有清单后，普通恢复按清单加载。

### 新知识

- `ManifestState`：先构造新快照，再提交，提交失败不能继续假定内存版本有效。
- `from_chars`：严格解析编号，检查是否完整消费 token、是否超出范围。
- `istringstream/ostringstream`：解析和生成元数据文本。
- CRC 与格式校验：损坏时明确拒绝；编号排序、上下界、END 都是约束。
- 幂等恢复：重复遇到同一个已提交表和旧 WAL，不重复恢复出旧值。
- 文件回收：只在有效版本已经发布后删除旧文件。

当前采用固定文件名的完整文本快照；不是 LevelDB 那种追加 VersionEdit + CURRENT 的完整实现，也尚无 L1 元数据。

### 运行

```bash
ctest --test-dir build-ubuntu -R '^kvstore_manifest_tests$' -V
```

**独立练习**：先读下面综合实验，保留一份旧 SSTable。Compact 后把它放回原名，重启后确认旧值没有复活；只在临时实验数据中改坏清单一个字节，确认数据库拒绝打开。

**验收**：能预测上表各中断点；能解释为什么新 BASE 必须使用新文件，以及为什么提交前不能回收旧文件。

## 6. 一条完整 CLI 实验：串起全部层次

先按第 0 节编译。创建临时实验目录，后面的启动与查看都在同一个目录进行：

```bash
task_dir=$(mktemp -d /tmp/kv-study.XXXXXX)
cd "$task_dir"
pwd
/mnt/e/VsCode/Cmake/kvstore/build-ubuntu/kvstore_cli
```

第一轮输入：

```text
Put a old
Put b keep
Flush
Exit
```

终端查看：

```bash
cat kvstore.log.manifest
ls -lh kvstore.log*
xxd -g 1 kvstore.log.L0.00000000000000000001.sst | head
```

预期一个有效 L0，WAL 8 字节，清单为 NEXT 2、BASE 0、RETIRED 0、TABLE 1。Flush 已等待完成，所以不要期待还能看到 .imm 文件。

再次启动相同程序，输入：

```text
Put a new
Delete b
Put c stable
Flush
Get a
Get b
Get c
Exit
```

预期 a=new、b 不存在、c=stable；两个有效 L0，清单 TABLE 1 和 TABLE 2。表 2 的 tombstone 遮蔽表 1 中 b=keep。

保存旧输入（仅用于这个实验）：

```bash
cp kvstore.log.L0.00000000000000000001.sst saved-old.sst
cat kvstore.log.manifest
```

再次启动，输入：

```text
Compact
Get a
Get b
Get c
Exit
```

预期逻辑结果不变，清单变为 NEXT 4、BASE 3、RETIRED 3、L0 0。基础文件为 `kvstore.log.base.00000000000000000003.log`，原始 `kvstore.log` 此时不是当前基础数据源。

```bash
cat kvstore.log.manifest
ls -lh kvstore.log*
xxd -g 1 kvstore.log.base.00000000000000000003.log | head
```

把旧 SSTable 放回，模拟提交后清理没有完成：

```bash
cp saved-old.sst kvstore.log.L0.00000000000000000001.sst
/mnt/e/VsCode/Cmake/kvstore/build-ubuntu/kvstore_cli
```

输入 Get a、Get b、Exit。仍应得到 new 和 Key not found。旧表不会被加载，恢复过程会回收它。

`Sync`、`Flush` 和 `Compact` 对比：

| 命令 | 主要目标 | 结果 |
| --- | --- | --- |
| Sync | 同步当前脏数据到持久存储 | 不要求生成新 SSTable |
| Flush | 等待 Active/Immutable 数据成为已提交的 SSTable | 可以增加 L0 数量；不是合并全部旧数据 |
| Compact | 合并当前所有层的逻辑最终状态 | 发布新 BASE，清空有效 L0 |

当前 CLI 的 Put 按空白分词，不能输入空字符串或任意二进制值；这些边界通过 C++ API 测试。`.manifest` 用 cat；二进制文件用 xxd；本项目没有独立文本运行日志。

## 7. 阅读材料：每篇只带一个问题

先完成一个对应的小实验，再读材料，不必一次读完全文。

| 材料 | 读哪一段 | 带着什么问题 |
| --- | --- | --- |
| [LevelDB 官方实现说明](https://github.com/google/leveldb/blob/main/doc/impl.md) | Sorted tables、Level 0、Manifest、Current、Recovery | 有序表、WAL 轮换和有效版本分别解决什么问题？哪些是本项目已经做的，哪些尚未做？ |
| [Bigtable 论文](https://research.google.com/archive/bigtable-osdi06.pdf) | 5.3 节存储与内存表段落 | 为什么冻结旧内存表以后，前台还能使用新内存表继续写？ |
| [LSM-tree 原始论文](https://dsf.berkeley.edu/cs286/papers/lsm-acta1996.pdf) | 引言、内存/磁盘组件和批量合并思路 | 批量有序写的收益由哪些读取/合并代价换来？ |
| [C++ 工作草案：二分查找](https://eel.is/c++draft/alg.binary.search) | lower_bound | 为什么返回迭代器后还要检查 key 相等？ |
| [C++ 工作草案：条件变量](https://eel.is/c++draft/thread.condition) | condition_variable 的 wait 和谓词版本 | wait 在什么时候释放锁、什么时候重新持锁？ |
| [Linux fsync 手册](https://man7.org/linux/man-pages/man2/fsync.2.html) / [rename 手册](https://man7.org/linux/man-pages/man2/rename.2.html) | DESCRIPTION | 文件同步、目录同步和原子替换分别保护什么？ |

C++ 工作草案用作查阅精确定义，不要求初学时通读；它也不是专门针对 C++17 的入门教程。不要照搬 LevelDB 文档中的默认参数作为本项目阈值。本项目各参数以当前代码为准。

## 8. 按顺序读源码，避免从千行文件第一行开始背

| 学习顺序 | 文件/函数 | 要回答的问题 |
| --- | --- | --- |
| 1 | `tests/model_test.cpp` | 什么是正确答案、什么时候比较？ |
| 2 | `sstable.h`、`SSTable::get` | 查询返回什么，为什么需要三态？ |
| 3 | `SSTable::build` 与构造函数 | 如何产生有序文件并恢复索引？ |
| 4 | `get_locked` | 为什么 Active、Immutable、新 L0 要优先？ |
| 5 | `freeze_memtable_locked` | 内存快照与 WAL 如何成对切换？ |
| 6 | `background_flush_loop`、析构 | 怎样等待任务、发布结果、报告失败与停止？ |
| 7 | `manifest.h`、读写函数 | 什么构成一个完整有效版本？ |
| 8 | `initialize_manifest`、`recover_immutable_wals` | 未完成任务怎样恢复？ |
| 9 | `compact`、`cleanup_obsolete_files` | 提交与回收为什么必须有顺序？ |

## 9. 最终掌握检查

不看源码，能回答并用测试验证：

1. SSTable 的二分查找和三态结果有什么区别？为什么插入位置不等于命中？
2. 同一个 key 在 Active、Immutable、两张 L0 都存在，返回哪层？
3. 最新层记录 tombstone，旧层记录空字符串，返回什么？
4. 阈值是 3，反复覆盖 a、b 两个 key，是否自动冻结？
5. 后台未完成时，前台使用哪份 WAL？为什么不能清空共同的一份 WAL？
6. 为什么 condition_variable 要配合谓词与 unique_lock？为什么 sleep 不能代替同步？
7. Immutable 已存在、Active 再写满时，怎样控制内存积压？
8. Flush 返回后，哪些数据已经落入有效 SSTable？普通 Put 返回又说明什么？
9. SSTable 写完但不在 MANIFEST 里时，是否能直接加入查询？
10. Compact 提交后旧文件残留，为什么不会让删除的 key 复活？
11. 当前实际基础文件怎样确定？旧学习单里的哪个观察命令已经过时？
12. 模型对拍、进程重开、构造中断状态分别验证什么，哪些还没有覆盖？

每个单元的独立练习都完成、这些问题能解释后，再进入 L0 → L1 分层压缩。下一阶段会增加跨表合并、key 范围重叠和部分压缩时 tombstone 保留条件。
