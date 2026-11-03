# 从 SSTable 到 MANIFEST：学习单讲解版

对应 `SSTABLE_TO_MANIFEST_STUDY.md`。按你的要求，**第 2 节（独立 SSTable 的排序、索引、文件和 `lower_bound` 二分查找）跳过**，只保留第 3 节查询所需的三态结论。其余单元（0、1、3、4、5、6、7、8、9）逐段讲解，并给出学习单里"先自己预测"部分的答案。

文中结论按当前源码逐条核对；测试状态为 2026-10-18 在 `build-ubuntu` 实测：**12/12 通过，总耗时 4.92 秒**。

---

## 0. 项目全貌

### 0.1 它是什么

一个**单进程、嵌入式、持久化的键值存储引擎**，C++17，CMake 构建，只依赖 POSIX（`unistd.h`、`sys/file.h`），所以要在 Linux/WSL/macOS 上编译运行。它把 LevelDB 的核心结构拆成教学步骤依次实现：

```text
WAL（预写日志） → MemTable（有序内存表）
      → Immutable MemTable（冻结快照） + Immutable WAL
      → 后台线程写 L0 SSTable → 提交 MANIFEST → 回收 Immutable WAL
      → 读时按层归并
      → Compact：合并所有层 → 新 BASE 文件 → 提交 MANIFEST → 回收旧文件
```

代码规模：库约 1900 行（`kv_store.cpp` 1105 行、`sstable.cpp` 302 行、`log_record.cpp` 368 行、`manifest.cpp` 153 行），CLI 116 行，12 个测试。

它有两种运行模式：

- `KVStore store;`（默认构造）：**纯内存模式**，`log_path_` 为空，只操作 `memory_data_`，不碰磁盘。`tests/kv_store_test.cpp` 里那组基础断言用的就是它。
- `KVStore store("kvstore.log");`：持久化模式，下面讲的一切都以这个模式为准。

### 0.2 三条数据流

```text
写：Put/Delete
      → 追加一条记录到 Active WAL（append，不开 fsync）
      → 更新 Active MemTable（map[key] = value 或 nullopt）
      → 若不同 key 数 ≥ 阈值：冻结（Freeze）

冻结：fsync 基础文件 / 当前 WAL
      → WAL 改名为 <db>.wal.L0.<编号>.imm
      → 新建空 Active WAL（8 字节头）并 fsync
      → Active MemTable 整体 swap 成 Immutable 快照
      → 通知后台线程

后台：Immutable 快照 → 写 <db>.L0.<编号>.sst（fsync 文件与目录）
      → 提交 MANIFEST（旧表 + 新表）
      → 删除 .imm
      → 清空 Immutable 槽位并唤醒前台

读：Active MemTable → Immutable MemTable → L0 从新到旧 → BASE 内存索引 + 磁盘读值

Compact：冻结并等后台完成
      → 读 BASE 全部值 + 每个 L0 的全部记录（旧→新覆盖）+ Active MemTable
      → 写新 <db>.base.<编号>.log（.tmp → fsync → rename → fsync 目录）
      → 一次 MANIFEST 提交：BASE=新编号、RETIRED=新编号、L0 清空
      → 换内存索引、清 L0 列表、WAL 重置为 8 字节、回收旧文件
```

### 0.3 磁盘上的文件

| 文件（`<db>` 是构造时传入的路径，如 `kvstore.log`） | 内容 | 有效性由谁决定 |
| --- | --- | --- |
| `<db>.wal` | Active MemTable 的恢复日志 | 始终有效，启动时重放 |
| `<db>.wal.L0.<20位编号>.imm` | 已冻结但尚未发布的 MemTable 日志 | 存在即恢复来源，发布后删除 |
| `<db>.L0.<20位编号>.sst` | 按 key 排序的不可变表 | **只在 MANIFEST 的 L0 列表里才有效** |
| `<db>.manifest` | 完整版本快照（文本 + CRC32） | 唯一的版本真相 |
| `<db>.base.<20位编号>.log` | Compact 生成的基础数据 | 只在 MANIFEST 的 `BASE` 指向它时有效 |
| `<db>` | 旧格式基础文件（`BASE 0` 时选用） | 只在 `BASE 0` 时有效 |
| `<db>.lock` | `flock` 进程锁 | 内容无关紧要 |
| `*.tmp` | 未完成的临时输出 | 启动时一律回收 |

编号统一 20 位零填充，保证目录里按文件名字典序 = 按 generation 数值序。文件位置由**程序启动时的当前工作目录**决定，与可执行文件位置无关。

### 0.4 二进制格式（WAL / BASE / SSTable 共用一套记录格式）

8 字节文件头：`"KVDB"` + 小端 `uint32` 版本号（当前 2；SSTable 是 `"SSTB"` + 版本 1）。

每条记录：

```text
u8   operation       1 = Put, 2 = Delete
u32  key_size
u32  value_size
byte key[key_size]
byte value[value_size]
u32  crc32           覆盖 op + 两个长度字段 + key + value
```

约束：key ≤ 1 MiB，value ≤ 16 MiB（`kMaxKeySize` / `kMaxValueSize`）；`Delete` 记录必须 value 为空。读取时会校验 CRC：**完整但校验错误 → 报错拒绝**；**读到一半 EOF → 视为截断，把文件截到该记录起点并返回**（`IncompleteRecordError` 与 CRC 错误是两种不同处理，别混）。

空 WAL / 空基础文件 = 8 字节。所以学习单里反复出现"WAL 回到 8 字节"，意思就是"内存里的东西已经全部落到别处，WAL 被重置为空"。

### 0.5 内存里的结构

| 成员 | 类型 | 作用 |
| --- | --- | --- |
| `memtable_` | `std::map<string, optional<string>>` | Active，**有序**，`nullopt` 是 tombstone（删除标记） |
| `immutable_memtable_` | `shared_ptr<const SSTable::Entries>` | 冻结快照，后台线程持有一份，前台也能查 |
| `level0_tables_` | `vector<Level0Table>` | **旧 → 新**存放；每个元素含 generation、路径、`unique_ptr<SSTable>` |
| `index_` | `unordered_map<string, IndexEntry>` | BASE 文件的 key → 值偏移/长度，启动时 `replay` 建立 |
| `manifest_` | `ManifestState` | 当前内存里的版本快照 |
| `next_sstable_generation_` | `uint64` | 下一个可用编号（SSTable、BASE、.imm 编号共用这一条编号线） |

`IndexEntry` 只有 `{value_offset, value_size}`，值本身不常驻内存；`get` 命中 BASE 时才 `seekg` + `read`。

### 0.6 线程、锁与生命周期

- 一把 `mutex_` 保护**所有**成员；文件 I/O 也在锁内，所以操作是串行的。
- 一个后台线程 `background_thread_`，一个 `condition_variable background_condition_`。
- 一个 Immutable 槽位 → 天然的**背压**：槽位没被后台清空时，下一次冻结会先等。
- 构造函数：进程锁 → 读/建 MANIFEST → 恢复 `.imm` → 回收垃圾 → 打开 WAL 头 → 重放 WAL → 启动后台线程。
- 析构：置 `stop_background_`、`notify_all`、`join`。**已冻结的任务会做完**；只存在于 Active 的数据留在 Active WAL 里，下次启动重放。
- 后台异常：`catch (...)` → 存 `exception_ptr` → 置停止 → 通知；之后每个公共操作（`put/get/remove/sync/flush/compact`）入口 `rethrow_background_error_locked()` 重新抛出。**注意：如果此后不再调用任何公共操作，异常不会自己冒出来，析构函数也不会抛。**

### 0.7 代码地图

| 关注点 | 位置 |
| --- | --- |
| 写入与冻结 | `KVStore::put` `KVStore::remove` `freeze_memtable_locked` |
| 读路径 | `get` → `get_locked` → `SSTable::get` / `read_value` |
| 后台 | `background_flush_loop`、`wait_for_background_flush_locked` |
| 恢复 | `initialize_manifest`、`recover_immutable_wals`、`replay`、`replay_wal`、`cleanup_obsolete_files` |
| 版本提交 | `publish_manifest`、`manifest.cpp` 的 `write_manifest` / `read_manifest` |
| 全量压缩 | `compact` |
| 记录编解码 | `log_record.cpp` 的 `write_record` / `read_record` |
| 文件命名 | `sstable_path_for_generation`、`immutable_wal_path_for_generation`、`base_file_path` |
| 进程锁 | `KVStore::ProcessLock`（`flock(LOCK_EX｜LOCK_NB)`） |

### 0.8 测试地图（12 项，各自守什么）

| ctest 名称 | 守的规则 |
| --- | --- |
| `kvstore_basic_tests` | 纯内存模式 CRUD、空值、覆盖；WAL 尾部截断修复后仍可写 |
| `kvstore_crc32_tests` | 完整但 CRC 错的记录必须报错，不能当截断尾部 |
| `kvstore_disk_index_tests` | 空值、含 `\0` 的二进制值、覆盖后索引指向最新、删除后重启不复活 |
| `kvstore_compaction_tests` | 100 次覆盖 + 删除 → Compact 后体积变小、值不变、WAL 回 8 字节、重启可读 |
| `kvstore_concurrency_tests` | 4 写线程 + 1 Compact 线程；同进程第二个句柄拒绝、`fork` 子进程拒绝 |
| `kvstore_wal_memtable_tests` | WAL 遮蔽基础文件、Compact 重置 WAL、重启保持 |
| `kvstore_shadow_tests` | 你自己写的四阶段 WAL 遮蔽实验 |
| `kvstore_model_tests` | 1000 步随机操作与 `unordered_map` 对拍，含重启与 Compact |
| `kvstore_sstable_tests` | 单表构建、排序、三态（第 2 节内容） |
| `kvstore_level0_flush_tests` | 阈值触发 L0、WAL 遮蔽旧 L0、新旧顺序、Compact 吸收 L0 |
| `kvstore_background_flush_tests` | 阈值 2 连写 20 次 → 10 张表；`.imm` 恢复 |
| `kvstore_manifest_tests` | 迁移、未提交输出、未提交表重建、已提交表保留、压缩退役、残留不复活、损坏拒绝 |

### 0.9 能力与限制（面试式自检）

**已有**：Put/Get/Delete、Sync、Flush（含等待屏障）、全量 Compact、WAL 重放与截断修复、不可变 SSTable、多 L0 遮蔽、后台 Flush、MANIFEST 版本提交、`.imm` 恢复、进程锁、多线程安全、清单 CRC 与结构校验、启动时孤儿回收。

**没有**：L1 及分层压缩、稀疏块索引/Bloom Filter/Block Cache、迭代器与范围扫描、MVCC/快照读、批量写、写合并/组提交、压缩算法、按字节的 MemTable 上限（现在只按不同 key 数，一个 16 MiB 的 value 也只算 1 项）、真实断电/`EIO`/磁盘满故障注入、`fsync` 失败演练、性能基准。

**已知设计取舍**：

- `put` 每条记录 open/close 一次 WAL，且**不 fsync**；持久性靠 `sync()`、冻结、Flush、Compact 的同步点。所以 "Put 返回" ≠ "已落盘"。
- Compact 全程持锁（等待后台时除外），会阻塞读写；同时要把 BASE 全部值和所有 L0 全部值读进内存，内存占用与数据量同阶。
- MANIFEST 每次全量重写文本，L0 越多写越大；恢复时也全量解析。
- L0 数量无上限，读放大随表数线性增长。

### 0.10 当前实测状态

```text
100% tests passed, 0 tests failed out of 12
Total Test time (real) = 4.92 sec
```

构建命令（WSL/Ubuntu）：

```bash
cd /mnt/e/VsCode/Cmake/kvstore
cmake -S . -B build-ubuntu -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ubuntu -j2
ctest --test-dir build-ubuntu --output-on-failure
```

---

## 1. 随机模型测试：对拍

### 1.1 核心思想

被测系统有 WAL、MemTable、SSTable、MANIFEST、后台线程、压缩——状态复杂到无法手算。所以另建一个**简单到显然正确**的参考实现：`unordered_map<string, string>`。同一串 Put/Delete 同时喂给两边，然后比较 `Get` 和 `remove` 的返回值。

```text
参考模型：只保存"逻辑最终键值"（不含 WAL、文件、索引）
被测系统：WAL + 内存表 + 文件 + 恢复 + 压缩
比较目标：可观察行为相同
```

关键点：**模型不模拟实现细节**。它不知道 WAL、不知道 tombstone、不知道 generation；它只知道"现在这个 key 应该是什么"。这正是对拍的价值：你测的是**契约**，不是实现。

### 1.2 为什么模型不需要 WAL

WAL 是**持久化手段**，不是**逻辑状态**。模型与数据库在同一进程里，模型不会重启、不需要恢复；它只需要在每次操作后给出"正确答案"。重启后比较 `store.get(key)` 与模型，检验的是"数据库把逻辑状态持久化并能重建"——模型本身没有被持久化的必要。如果模型也写 WAL，你就只是在测两份 WAL 是否一致，反而引入新的错误来源。

### 1.3 代码要点

| 代码 | 说明 |
| --- | --- |
| `kSeed = 20261007` | 固定种子，便于复现；`mt19937` 的引擎输出是标准化的 |
| `operation_distribution(0,99)` | 0–44 Put（45%）、45–74 Get（30%）、75–99 Delete（25%），用区间实现概率 |
| `step % 17 == 0` 时空字符串 | 专门验证"空值 ≠ 不存在" |
| `check_key` | 比较 `optional<string>`，失败打印 seed/step/operation/key/expected/actual |
| `check_all_keys` | Compact 后、重开后检查全部 10 个 key |
| `step % 100 == 0` → `compact()` | 覆盖压缩路径 |
| `step % 50 == 0` → `sync()` + `reset()` + 新建 `KVStore` | `unique_ptr::reset()` 先析构旧对象（触发 `join`、释放进程锁）再重建，**这是进程内重开，不是断电** |

分布函数的映射在标准里不保证跨实现完全一致，所以失败时最好同时保留操作序列；当前实现只打印种子和步骤。

### 1.4 这个测试实际覆盖了什么

- 1000 步、10 个 key、阈值默认 64：仅靠随机 Put/Delete **不会**触发自动冻结（10 < 64）。
- 但 `compact()` 内部会先 `freeze_memtable_locked` 再等后台 → **Compact 会走 Flush 路径**，所以后台发布、MANIFEST 提交、WAL 轮换仍被覆盖。
- 每 50 步重开 → 覆盖 WAL 重放 + BASE 索引重建。

### 1.5 练习：阈值改 3

把路径改成 `low-limit-practice-test.log`，所有 `make_unique<KVStore>(path, 3)`（**包括重开**，否则重启后阈值又回 64）。

阈值 3 后，随机写会不断触发 `put → 满 3 个 key → freeze → 后台写表`，覆盖学习单想让你看到的组合：Active 与 Immutable 同时有数据、后台未完成时重开、Compact 与后台交错。

要求补齐：

1. 每次 Flush、Compact、重开后都 `check_all_keys`（要暴露"跨层遮蔽错误"，必须检查所有 key，不能只查刚写的）。
2. 保留空字符串用例。
3. 失败时打印足够历史（至少最近 N 步操作），否则"第 437 步失败"无法复现。

编译：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -I. \
  learning/model_low_limit_test.cpp build-ubuntu/libkvstore.a \
  -pthread -o build-ubuntu/model_low_limit_practice
./build-ubuntu/model_low_limit_practice
```

验收自问：

- 模型为什么不需要 WAL？（它不是数据库，是判分标准）
- 你能否打印一条可复现失败记录？（seed + step + 操作 + 期望/实际）
- 你的测试实际触发了哪些维护操作？改 3 后，`put` 自己就会冻结；重开发生在 `.imm` 尚未发布时，会走 `recover_immutable_wals`。

---

## 3. 三态查询与多个 L0

### 3.1 三态是"一个 key 在一张表里的状态"

| found | value | 含义 | 下一步 |
| --- | --- | --- | --- |
| false | `nullopt` | 这张表没有该 key | 继续查更旧的层 |
| true | `nullopt` | 这张表有 tombstone（删除） | **停止**，返回"不存在" |
| true | 有字符串（可以为空） | 这张表有值 | **停止**，返回值 |

必须分开 `found` 与 `value`：`optional<string>` 一个类型只能表达"有/无"，而"表里没有"和"表里记着删除"是两种完全不同的语义——前者要继续往下找，后者要立刻终止。所以 `SSTableLookupResult` 用 `bool found` + `optional<string> value`。

### 3.2 例子（学习单 3 节）的答案

```text
新 L0（gen 2）：a=new，b=tombstone，d=four
旧 L0（gen 1）：a=old，b=keep，c=stable
```

| key | 查询路径 | 结果 |
| --- | --- | --- |
| a | Active/Immutable 无 → 新 L0 命中 `new` | `new` |
| b | 新 L0 命中 tombstone → **立即停**（不会看旧 L0 的 keep） | 不存在 |
| c | 新 L0 无 → 旧 L0 命中 `stable` | `stable` |
| d | 新 L0 命中 `four` | `four` |
| missing | 所有 L0 都 `found=false` → 查 BASE 索引，无 | 不存在 |

如果顺序错了（从旧到新），b 会返回 `keep`、a 会返回 `old`——这正是 `level0_flush_test` 要抓的 bug。

### 3.3 tombstone 什么时候可以丢

tombstone 存在的唯一理由是**遮蔽更旧层里的值**。所以：

- Put 进 MemTable、Flush 进 L0：**必须保留**（旧 L0 或 BASE 里可能还有旧值）。
- 完整 Compact：把所有层合并进一个新 BASE，旧层全部退役，**任何旧值都不再被读取**，此时 tombstone 可以丢弃（`compact()` 的 `live_values.erase(key)` 就是丢弃）。
- 将来到 L1 部分压缩时，只有当"被删 key 可能存在的所有更旧层"都参与合并，才能丢 tombstone；否则必须保留并下沉。这是下一阶段最容易写错的地方。

tombstone 也占 MemTable 的一项（`memtable_.size()`），所以学习单说"阈值是按不同 key 数，删除也算一项"。

### 3.4 读 `level0_flush_test.cpp` 的正确方式

阈值 3，场景四段：

1. `put a old / b keep / c stable` → 第 3 个 put 触发冻结 → `flush()` 等后台 → 1 张 L0，WAL 8 字节。
2. 同一对象继续 `put a new / remove b`（此时不满足阈值，留在 WAL）。
3. 重开：加载 L0 → 重放 WAL → `a=new`、`b` 不存在、`c=stable`（第 2 节里说的"WAL 遮蔽 L0"）。注意此时重放产生 2 项（a、tombstone b），再 `put d` → 3 项 → 自动冻结 → 后台生成 L0 #2。
4. 再次重开验证 newest→oldest；`compact()` 后 L0 清零；再重开验证值仍在 BASE。

两个容易踩的坑：

- **断言文件数量前必须 `flush()`**：自动 Flush 是异步的，`put` 返回不代表 `.sst` 已存在。
- 文件计数函数把 `.imm` 也算进前缀匹配，所以先 `flush()` 再数，避免竞态。

### 3.5 练习（阈值 2，必须让 key 重叠）

- 第一次：`a=old`、`b=keep`（2 项触发冻结）→ `flush()`。
- 第二次：`a=new`、`remove b` → `flush()`。
- 重开：`a=new`、`b` 不存在。
- 若只测不重叠的 key，层次顺序写反也照样通过——这是本单元最重要的"测试设计"教训。

验收自问：

- 能否不看答案画出两张表并标出每个 key 停在哪层？
- 为什么 `optional<string>` 单独作返回类型不够？（`found=false` 与 `found=true, nullopt` 都表示 `nullopt`，无法区分继续还是停止）
- 空字符串与 tombstone 怎么区分？（`found=true` + `value=""` vs `found=true` + `value=nullopt`）

---

## 4. Immutable MemTable、后台线程与 WAL 轮换

### 4.1 冻结的精确步骤（`freeze_memtable_locked`）

```text
0. 若已有 Immutable：先等后台清空（背压；等待时释放锁）
1. 检查编号未耗尽
2. frozen = make_shared<Entries>()；frozen->swap(memtable_)   ← 快照瞬间完成，memtable_ 变空
3. generation = next_sstable_generation_++；算出 .imm 路径
4. fsync 基础文件；fsync 当前 WAL
5. rename(WAL → <db>.wal.L0.<gen>.imm)；fsync 目录
6. 新建空 WAL（写 8 字节头）；fsync 新 WAL；fsync 目录
   ← 若第 6 步失败：把 .imm 改回 WAL，rethrow
7. immutable_memtable_ = frozen；记录 generation 与 .imm 路径；dirty_=false
8. notify_all() 叫醒后台
```

任何一步抛异常，外层 `catch` 都会把 `frozen` swap 回 `memtable_`，保证"要么整体冻结成功，要么原地不动"。

注意第 4、5 步：**冻结前先把 WAL fsync**，所以只要 `.imm` 出现，它一定是完整可重放的——这是恢复规则成立的前提。

### 4.2 两份 WAL 各保护什么

| 文件 | 保护的对象 | 谁在读它 | 何时删除 |
| --- | --- | --- | --- |
| `<db>.wal`（Active） | 冻结之后的新写入 | 前台（下次启动重放） | Compact 时重置为 8 字节 |
| `<db>.wal.L0.<gen>.imm` | 已冻结、尚未发布成 SSTable 的快照 | 后台写表；崩溃后启动恢复 | MANIFEST 提交该代之后 |

为什么不能共用一份 WAL？因为冻结后前台要立刻继续写，而旧数据还没落成 SSTable。如果共用一份：要么前台写进同一文件、WAL 无法安全截断（截断会丢尚未发布的数据）；要么清空它——那冻结快照就永远丢了。所以必须"改名前缀 + 新建空文件"。

如果后台还没写完：Immutable 里 `name=alice`，Active 里 `name=bob`，`get("name")` 依次查 Active 先命中 → 返回 `bob`。

### 4.3 线程安全与 C++ 概念

| 工具 | 在本项目里解决什么 |
| --- | --- |
| `std::thread` / `join` | 一个 Flush worker；析构必须 join，否则线程可能在对象销毁后访问成员 |
| `mutex` | 保护所有共享成员（memtable、指针、manifest、异常） |
| `unique_lock` | `wait` 需要在睡眠时释放锁、醒来后重新加锁；`lock_guard` 做不到 |
| `condition_variable` | 没有任务时让 worker 休眠，避免忙等 |
| 谓词版 `wait(lock, pred)` | 每次唤醒**重新判断条件**，抵御虚假唤醒和"通知早于等待" |
| `shared_ptr<const Entries>` | 快照生命周期由引用计数管理：前台 reset 后，worker 手里的局部指针仍能安全读 |
| `swap` / `move` | `frozen->swap(memtable_)` 是 O(1) 转移内容，不需要逐元素拷贝 |
| `exception_ptr` | 后台异常跨线程保存，再由前台线程 `rethrow_exception` |

`shared_ptr<const T>` 只保证**通过这个指针**不能改 T，不保证别的别名不改，也不保护指针变量本身的并发赋值——所以"冻结后不再修改快照" + "mutex 保护槽位切换"两者缺一不可。

条件变量的正确心智模型：

```text
检查谓词 → 不满足：释放锁并睡眠 → 被唤醒：重新加锁 → 再检查谓词
```

通知**不保存状态**；保存状态的是锁保护下的变量（这里是 `immutable_memtable_` / `stop_background_` / `background_error_`）。所以 `sleep(1)` 永远不能替代同步：它既不能保证条件成立，也不能保证条件在你检查时没变。

### 4.4 背压

只有一个 Immutable 槽位。如果后台还没处理完、Active 又写满：

```text
put → 冻结检查 → 槽位被占 → wait_for_background_flush_locked → 前台阻塞
```

这是刻意设计的背压：把内存积压限制在"一个 Active + 一个 Immutable"。代价是 `put` 可能被阻塞（WAL 轮换、元数据提交本身也有同步与锁开销），后台线程并不保证前台永不阻塞。测试 `background_flush_test` 阈值 2 连写 20 个 key，实际就是在这条路径上反复阻塞/唤醒，最后得到 10 张表。

### 4.5 析构、停止与异常

- 析构：`stop_background_ = true` → `notify_all` → `join`。
- worker 循环：`wait(stop || immutable)`；若没有任务且要停止 → `return`；若有任务 → **先做完这个任务**（提交 MANIFEST、删 `.imm`、发通知），再回到循环退出。
- 因此"已冻结的数据一定会发布；只存在于 Active 的数据留在 Active WAL，等下次启动重放"。
- 后台异常：保存到 `background_error_`、停止线程；前台下次调用任意公共方法时重抛。错误发生后数据库不再可用，也没有自动回滚——这是"明确失败"而不是"静默降级"。

### 4.6 与 Compact 的关系

`compact()` 开头就是 `freeze` + `wait_for_background_flush_locked`，确保没有在飞的 Flush 与它并发；之后持锁完成合并与提交。这样 Compact 拿到的是一致版本。

### 4.7 两个场景（`background_flush_test`）

1. 阈值 2，连写 `key-0..key-19`，每写一个都 `get` 验证前台不丢值；`flush()` 后必须正好 10 张 `.sst`、0 个 `.imm`、WAL 8 字节。
2. 人为构造"已冻结未发布"：写一个值 → `sync` → 关闭 → 把 `<db>.wal` 手工改名成 `<db>.wal.L0.00000000000000000001.imm` → 重开。预期：`crash-key` 仍可读、`.imm` 消失、出现 1 张 `.sst`、WAL 8 字节。

这是**状态构造**测试，不是真实断电测试：它只验证"看到这种磁盘状态时恢复逻辑正确"，不验证"所有后台交错时序"。

### 4.8 练习

先写一个只有一次任务的 worker：主线程在 mutex 下设 `ready=true; value=42`，通知；后台谓词 wait 到 `ready` 后读 `value`；主线程 `join` 后检查。不要用 `sleep(1)`。

验收自问：

- 两份 WAL 分别保护什么？
- 查询层次顺序是什么？
- 背压何时发生、阻塞的是谁？
- 为什么 `wait` 必须配谓词和 `unique_lock`？
- 析构时为什么已冻结的任务会完成？
- 后台异常如何传到前台？

---

## 5. MANIFEST：从文件集合到一个可提交的版本

### 5.1 "文件存在" ≠ "文件有效"

目录里可能有：

- 已写完、但**没进清单**的 SSTable：未提交输出，启动时不能用来回答查询。
- 已提交、但**还没删**的旧文件：待回收输入，启动时必须忽略。
- 写完但没 rename 的 `.tmp`：垃圾。

当前版本 = MANIFEST 里那份**完整清单**。数据文件和清单是分步骤写的，靠"写入顺序 + 重启规则"得到一致的版本。

### 5.2 清单格式

```text
KV_MANIFEST 1
NEXT 3
BASE 0
RETIRED 0
L0 2
TABLE 1
TABLE 2
END
CRC32 <十进制>
```

| 字段 | 含义 |
| --- | --- |
| `KV_MANIFEST 1` | 格式标识与版本 |
| `NEXT` | 下一个可用编号（SSTable/BASE 共用；WAL 轮换也会占用编号，恢复时补推） |
| `BASE` | 0 = 旧格式 `<db>`；>0 = `<db>.base.<20位>.log` |
| `RETIRED` | 退役截止：编号 ≤ 它的 `.imm` 不再恢复 |
| `L0` + `TABLE` | 有效 L0 编号，按旧→新（严格递增） |
| `END` | 快照结束 |
| `CRC32` | 校验前面的文本载荷 |

`read_manifest` 的校验（任一失败即拒绝打开，**绝不从目录猜状态**）：

- 文件大小上限 32 MiB、必须有 `CRC32 ` 且 CRC 匹配、payload 完整。
- `NEXT != 0`、`BASE < NEXT`、`RETIRED < NEXT`、**`BASE == RETIRED`**（当前实现两者总相等）。
- L0 编号严格递增、大于 `RETIRED`、小于 `NEXT`；数量上限 100 万。
- 逐 token 严格解析（`from_chars` 要求完整消费、不溢出），`END` 后不允许有杂项。

`initialize_manifest` 还会检查 `BASE` 指向的文件**实际存在**，缺失就抛错。

### 5.3 提交顺序（为什么必须是这个顺序）

```text
1. 数据文件完整写入（.tmp）并 fsync
2. rename 成正式名并 fsync 父目录
3. 写 MANIFEST.tmp 并 fsync
4. rename(MANIFEST.tmp → MANIFEST) 并 fsync 父目录   ← 唯一的"提交点"
5. 回收旧 WAL / 旧数据文件
```

- 先有数据、后有元数据：否则清单会指向不存在的文件。
- 先 fsync 再 rename：`rename` 的原子替换只保证"名字切换是原子的"，**不保证内容已持久化**；目录项本身也要 fsync 才可靠。
- 提交之后才删旧文件：清单提交前，旧文件仍是唯一可恢复来源。

`publish_manifest` 每次把 `state.next_generation` 覆盖成内存里的 `next_sstable_generation_`（单调），再写盘；写失败则保存异常并抛出，**不允许继续用可能已经变化的内存版本**（注释里写得很明确：rename 之后的目录 fsync 失败也算失败，必须重开恢复）。

### 5.4 六个中断点的预测

| 中断点 | 重启依据 | 预期 |
| --- | --- | --- |
| `.imm` 存在，SSTable 未生成 | `.imm` 重放 | 重建表、提交清单、删 `.imm` |
| `.sst` 完整但不在清单，`.imm` 还在 | `.imm` 是恢复来源 | **删掉旧 `.sst`**，从 WAL 重建后提交 |
| `.sst` 在清单，`.imm` 未删 | 清单选择该表 | 保留表，删重复 `.imm`（不重复应用） |
| 新 BASE 写完，清单未切换 | 旧清单 | 用旧 BASE + 旧 L0；新 BASE 是孤儿，回收 |
| 新 BASE 已提交，旧文件未删 | 新清单 + `RETIRED` | 忽略旧文件；`RETIRED` 阻止旧 `.imm` 复活删除键 |
| 清单损坏 / 引用文件缺失 | 无法确认版本 | **报错**，不静默从目录猜 |

### 5.5 幂等恢复（`recover_immutable_wals`）

启动时扫描 `<db>.wal.L0.*.imm`，按编号升序处理：

```text
gen <= RETIRED            → 已经合进当前 BASE，直接删
清单里已有该 gen 的 L0    → 已发布，删 .imm（不再重放）
否则                      → 先删同名 .sst（不信未提交输出）
                            → 重放 .imm 建表、提交清单、删 .imm
```

最后按 generation 排序 `level0_tables_`。这段代码体现两个原则：**只信清单**、**恢复可重复执行**。

### 5.6 回收规则（`cleanup_obsolete_files`）

删除：不在清单 L0 里的 `<db>.L0.*.sst`、`generation != BASE` 的 `<db>.base.*.log`、各类 `.tmp`。删除后 fsync 目录。它**不删 `.imm`**——`.imm` 由恢复流程处理（要么重建、要么按 RETIRED 删除）。

### 5.7 Compact 为什么生成新 BASE、为什么先提交再删

如果原地覆盖旧 BASE，再逐个删旧 L0，中途崩溃就会出现"新 BASE + 旧 L0"的混合状态，旧值可能遮蔽新结果。现在：

```text
提交前：旧 BASE + 旧 L0          （随时可恢复）
提交后：新 BASE + 空 L0          （一次 rename 切换）
```

新 BASE 必须用新文件，是因为**旧版本在提交前必须完整可用**；而提交后旧文件只是待回收输入。旧 L0 与旧 BASE 不在新清单里，`RETIRED` 又挡住了旧 `.imm`，所以被删除的 key 不会复活。

Compact 的特征（学习单 6 节的表）：

| 命令 | 目标 | 结果 |
| --- | --- | --- |
| `Sync` | 把脏数据与目录元数据刷到持久存储 | 不生成新 SSTable |
| `Flush` | 让 Active/Immutable 数据变成已提交 SSTable | L0 可能增加；不合并旧数据 |
| `Compact` | 合并所有层的最终逻辑状态 | 发布新 BASE、清空 L0、WAL 归零 |

严格说，`Flush` 的保证是"**调用时刻**已在 Active/Immutable 里的数据全部进入已提交 SSTable"；如果等待期间别的线程又写了新的 Active 数据，那部分不在等待范围内（除非它自己触发冻结并排队）。单线程测试里两者等价，但写文档/做设计时要说准。

### 5.8 当前实现的限制

- 固定文件名的**全量文本快照**，不是 LevelDB 的追加 `VersionEdit` + `CURRENT`。
- 无 L1 元数据；`BASE` + `L0` 就是全部层级。
- 每次 Flush 都重写整份清单，L0 多时写放大。
- 无 checksum 覆盖 SSTable 整体（靠每条记录的 CRC）；无 `CURRENT` 双层保护。

### 5.9 读 `manifest_test.cpp` 的 9 个断言

1. 无清单的旧库 → 迁移发布清单，L0 数据不丢（`key=table`）。
2. 目录里有未提交 `.sst` + `MANIFEST.tmp` → 被忽略且被回收。
3. `.imm` 与未提交 `.sst` 同时存在 + Active WAL 更新 → 从 `.imm` 重建（`frozen`），Active 覆盖（`active`）。
4. 已提交表 + 重复 `.imm` → 保留表、删 `.imm`。
5. Compact → 新 BASE、L0 空、`base_generation > old_id`。
6. 放回旧 `.sst` + 写回旧 `.imm` → 删除键不复活。
7. 删掉清单引用的表 → 拒绝打开。
8. 截断清单 → 拒绝打开，不回退目录扫描。
9. 改坏一个字节 → CRC 检测，拒绝打开。

### 5.10 独立练习

在**临时实验目录**里：保留一份旧 SSTable，Compact 后放回原名，重启确认旧值不复活；再改坏清单一个字节，确认拒绝打开。验收自问：能否预测 5.4 的六行；能否解释"为什么新 BASE 必须新文件"和"为什么提交前不能删旧文件"。

---

## 6. 一条完整 CLI 实验

编译见 0.10。**在临时目录里运行**，让文件生成在那里：

```bash
task_dir=$(mktemp -d /tmp/kv-study.XXXXXX)
cd "$task_dir"
/mnt/e/VsCode/Cmake/kvstore/build-ubuntu/kvstore_cli
```

第一轮：

```text
Put a old
Put b keep
Flush
Exit
```

预期：1 个有效 L0；WAL 8 字节；清单 `NEXT 2, BASE 0, RETIRED 0, L0 1, TABLE 1`。`Flush` 已等待完成，所以看不到 `.imm`。

```bash
cat kvstore.log.manifest
ls -lh kvstore.log*
xxd -g 1 kvstore.log.L0.00000000000000000001.sst | head
```

第二轮（同一目录再启动）：

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

预期：`a=new`、`b` 不存在、`c=stable`；两张 L0，`TABLE 1`、`TABLE 2`；表 2 的 tombstone 遮蔽表 1 的 `b=keep`。

第三轮：

```bash
cp kvstore.log.L0.00000000000000000001.sst saved-old.sst
cat kvstore.log.manifest
```

```text
Compact
Get a
Get b
Get c
Exit
```

预期：逻辑结果不变；清单 `NEXT 4, BASE 3, RETIRED 3, L0 0`；基础文件 `kvstore.log.base.00000000000000000003.log`；`kvstore.log` 此时只是旧格式文件，不再是数据源。

第四轮：把旧表放回原名，模拟"提交后清理未完成"：

```bash
cp saved-old.sst kvstore.log.L0.00000000000000000001.sst
/mnt/e/VsCode/Cmake/kvstore/build-ubuntu/kvstore_cli
```

`Get a / Get b / Exit` 仍应是 `new` 与 `Key not found`；旧表不会被加载，启动时被回收。

注意：CLI 的 `Put` 按空白分词，不能输入空字符串或任意二进制；这些边界用 C++ API 测试（`disk_index_test`）。清单用 `cat`，二进制文件用 `xxd`，本项目没有独立的文本运行日志。

---

## 7. 阅读材料：每篇只带一个问题

| 材料 | 读哪段 | 带着的问题 | 一句话提示 |
| --- | --- | --- | --- |
| LevelDB `doc/impl.md` | Sorted tables、Level 0、Manifest、Current、Recovery | 有序表、WAL 轮换、有效版本各解决什么问题？ | 有序表把随机读变顺序读；WAL 轮换为并发写让路；Manifest/Current 定义"哪组文件是一个版本" |
| Bigtable 5.3 | 存储与内存表段落 | 冻结旧表后前台为何还能写？ | 冻结的是只读快照，新写入进新内存表，两者互不干扰 |
| LSM-tree 原始论文 | 引言、内存/磁盘组件、批量合并 | 批量有序写的收益换来了什么代价？ | 换来了读放大（多层查找）与写放大（反复合并） |
| C++ 草案 `alg.binary.search` | `lower_bound` | 为什么返回迭代器还要比 key？ | 返回的是"第一个不小于 key 的位置"，不保证相等（本节已跳过，留作第 2 节练习） |
| C++ 草案 `thread.condition` | `wait` 与谓词版本 | wait 何时释放/重获锁？ | 进入 wait 释放，被唤醒后重获，然后重新判谓词 |
| `fsync(2)` / `rename(2)` | DESCRIPTION | 文件同步、目录同步、原子替换各保护什么？ | fsync 保内容；目录 fsync 保目录项；rename 保名字切换原子 |

不要照搬 LevelDB 的默认参数；本项目阈值、编号、文件布局都以当前代码为准。

---

## 8. 按顺序读源码

| 顺序 | 文件/函数 | 回答的问题 |
| --- | --- | --- |
| 1 | `tests/model_test.cpp` | 正确答案是什么、何时比较？ |
| 2 | `sstable.h`、`SSTable::get` | 返回值为什么需要三态？（实现细节见第 2 节，已跳过） |
| 3 | `SSTable::build`、构造函数 | 有序文件怎么写、打开时如何重建索引？（同上，可先会用） |
| 4 | `get_locked` | 为什么 Active、Immutable、新 L0 优先？ |
| 5 | `freeze_memtable_locked` | 快照与 WAL 如何成对切换、失败如何回滚？ |
| 6 | `background_flush_loop`、析构 | 如何等待、发布、报错、停止？ |
| 7 | `manifest.h`、读写函数 | 什么构成一个完整有效版本、校验哪些不变量？ |
| 8 | `initialize_manifest`、`recover_immutable_wals` | 未完成任务怎样恢复、如何幂等？ |
| 9 | `compact`、`cleanup_obsolete_files` | 提交与回收为什么必须有顺序？ |

---

## 9. 最终掌握检查（12 问答案）

> 第 1 问是第 2 节（二分查找）内容，按你的要求从略；其余给出答案。

1. **（跳过）** 二分查找与三态的区别：属于 SSTable 内部实现，见第 2 节。
2. **Active、Immutable、两张 L0 都有同一个 key，返回哪层？** Active（最新层）。顺序：Active → Immutable → 新 L0 → 旧 L0 → BASE。
3. **最新层 tombstone，旧层空字符串，返回什么？** 不存在（`nullopt`）。tombstone 终止查询；空字符串是"存在的空值"。
4. **阈值 3，反复覆盖 a、b 两个 key，会自动冻结吗？** 不会。计数是不同 key 数（含 tombstone），2 < 3 永远不触发；只是 WAL 变长、内存里同两项。写入第三个不同 key 才冻结。
5. **后台未完成时前台用哪份 WAL？为什么不能共用一份？** 用新建的 Active WAL；旧 WAL 已改名为 `.imm` 保护冻结快照。共用一份会导致无法安全截断：截断丢未发布数据，不截断则新旧数据混在一个文件、编号与恢复语义无法区分。
6. **为什么 `condition_variable` 要配谓词和 `unique_lock`？** wait 需要在睡眠时释放 mutex、醒来重获；谓词版在每次唤醒后重新判条件，抵御虚假唤醒和"通知早于等待"。`sleep` 不能保证条件成立，也不能与状态变化同步。
7. **Immutable 已存在、Active 又写满怎么办？** 背压：前台在 `freeze_memtable_locked` 里等后台把 Immutable 发布掉（等待时释放锁），内存积压被限制在 Active + 一个 Immutable。
8. **`Flush` 返回后哪些数据已落盘？`Put` 返回说明什么？** Flush 返回：调用时刻 Active/Immutable 中的数据都已变成 fsync 过、且已提交清单的 SSTable，`.imm` 已删；等待期间新写入的 Active 数据不保证。Put 返回：记录已写进 WAL（操作系统的页缓存）并更新 MemTable，**没有 fsync**——进程崩溃不丢，机器断电可能丢。
9. **SSTable 写完但不在清单里，能直接加入查询吗？** 不能。只加载清单里的表；若有 `.imm` 就从 WAL 重建并提交，否则当孤儿回收。
10. **Compact 提交后旧文件残留，删除的 key 会复活吗？** 不会。旧文件不在新清单里，`RETIRED` 阻止旧 `.imm` 重放，启动回收会删掉它们；查询根本不读旧层。新 BASE 里也不含该 key（tombstone 已被合并丢弃）。
11. **当前基础文件怎么确定？旧学习单哪个命令过时？** 看清单 `BASE`：0 → `<db>`；否则 `<db>.base.<20位编号>.log`。Compact 之后 `xxd kvstore.log` 已不是数据源（那是旧格式文件），应改为 `xxd kvstore.log.base.*.log`。
12. **模型对拍、进程重开、构造中断状态各验证什么、哪些没覆盖？** 对拍验证随机操作序列下逻辑结果一致；重开验证 WAL/`.imm`/BASE 恢复与索引重建；构造中断验证提交顺序与幂等规则。未覆盖：真实断电/`fsync` 失败、`EIO`/磁盘满、任意字节部分写、并发交错穷举、性能与写放大、多进程真正并发访问（只测了锁拒绝）。

---

## 10. 建议的推进方式

1. 先按 0.10 跑一遍 12 项测试，确认环境和文档一致。
2. 按 1 → 3 → 4 → 5 → 6 的顺序，每个单元先写"记录模板"里的预测表，再跑测试对照。
3. 每个单元至少写一个自己的测试：阈值 2/3、key 重叠、跨重开验证——**不重叠的 key 测不出层次错误**。
4. 第 5 节的中断表能默写、`manifest_test` 9 个断言能解释后，再进入 L0 → L1：跨表归并、key 范围重叠、部分压缩时 tombstone 保留条件。
5. 进阶方向（按性价比）：稀疏块索引 + Bloom Filter → L1 分层压缩 → 迭代器/范围扫描 → 快照读 → 组提交与写缓冲 → 故障注入测试。
