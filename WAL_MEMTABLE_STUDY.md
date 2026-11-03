# WAL、MemTable 与恢复：读物和实操学习单

这份学习单对应项目当前阶段。现在的写入路径是 `Put/Delete -> kvstore.log.wal -> MemTable`；读取时先看 MemTable，再查旧的基础日志 `kvstore.log`。`Compact` 将两层合并后清空 WAL。**目前 `kvstore.log` 不是 SSTable**，所以先别急着写多层 LSM 或 Bloom Filter。

建议用 4 个学习单元完成，每个单元都按“先写下预测 -> 运行 -> 对照源码解释 -> 再独立写测试”的顺序。每个单元约 1～2 小时；论文只读指定部分，不要求一次读完。

## 0. 先知道各层在做什么

| 部件 | 解决的问题 | 当前代码位置 |
| --- | --- | --- |
| WAL（预写日志） | 内存表丢失后可按顺序重放更新 | `src/kv_store.cpp` 的 `put`、`remove`、`replay_wal`；`src/log_record.cpp` |
| MemTable | 保存最近一次 `Compact` 后的最新值；按键有序 | `kv_store.h` 的 `memtable_`；`src/kv_store.cpp` 的 `get` |
| Tombstone（删除标记） | 让“删除”遮蔽基础日志中的旧值 | `std::optional<std::string>` 中的 `std::nullopt` |
| 基础日志与内存索引 | 保存已合并的值；索引记住值在文件中的位置 | `src/kv_store.cpp` 的 `replay`、`read_value` |
| `Sync` | 请求把已写数据与必要的目录元数据刷到持久存储 | `src/kv_store.cpp` 的 `sync`、`sync_file_path`、`sync_parent_directory` |
| `Compact` | 合并最新状态，丢弃被覆盖或删除的旧记录 | `src/kv_store.cpp` 的 `compact` |

当前代码里的普通 `put` 关闭文件后，数据通常已经交给操作系统，但不等于完成了 `fsync`。进程重启实验验证的是“可重放”；它不能代替断电持久性测试。

## 1. 阅读顺序（每篇带着问题读）

1. **Bitcask 设计论文**：<https://riak.com/assets/bitcask-intro.pdf>。先读第 1～3 页的 active file、keydir、tombstone、merge。回答：为什么追加写比原地修改简单？旧记录为什么可以留到合并时再清理？它最接近本项目的“旧基础日志 + 内存索引”。
2. **LevelDB 实现说明**：<https://github.com/google/leveldb/blob/main/doc/impl.md>。先读 `Files`、`Level 0`、`Recovery`，暂时跳过多层压缩细节。画出 `log -> memtable -> sorted table`，再与本项目对比：本项目的 `Compact` 仍写基础日志，还没有独立 SSTable。
3. **Bigtable 论文**：<https://research.google.com/archive/bigtable-osdi06.pdf>。只读 5.3 节中关于持久化更新、内存表和 SSTable 的段落。回答：为什么写日志之后还需要内存表？内存表写满时为什么要转换为只读并写磁盘？分布式系统其他章节可暂缓。
4. **LSM-tree 原始论文**：<https://dsf.berkeley.edu/cs286/papers/lsm-acta1996.pdf>。先读摘要、引言以及内存组件与磁盘组件的示意和基本思路。推导和成本公式留到实现第一个 SSTable 后再读。回答：为什么批量顺序写能减少随机写？为什么读取路径会变复杂？
5. **Linux `fsync(2)` 与 `rename(2)` 手册**：<https://man7.org/linux/man-pages/man2/fsync.2.html>、<https://man7.org/linux/man-pages/man2/rename.2.html>。回答：`rename` 的原子替换和“断电后目录项仍存在”是不是同一件事？为什么新文件 `fsync` 后还要 `fsync` 父目录？

不必逐行翻译论文。每篇只记三件事：**它解决了什么问题、采用什么数据流、代价是什么**。

## 2. 实验 A：亲眼看到 WAL 与基础日志

在 Ubuntu 终端运行，使用独立临时目录，避免改动项目根目录的 `kvstore.log`：

```bash
cd /mnt/e/VsCode/Cmake/kvstore
cmake --build build-fixed -j2
lab_dir=$(mktemp -d /tmp/kv-wal-lab.XXXXXX)
cd "$lab_dir"
/mnt/e/VsCode/Cmake/kvstore/build-fixed/kvstore_cli
```

在 CLI 里依次输入：

```text
Put name alice
Put name bob
Get name
Delete name
Get name
Put empty x
Sync
Exit
```

先预测 `Get` 输出，然后查看：

```bash
ls -l kvstore.log kvstore.log.wal
xxd -g 1 kvstore.log.wal | head
```

预期：`kvstore.log` 只有 8 字节文件头；`.wal` 大于 8 字节；两次 `Put name` 和一次 `Delete name` 都是追加记录。`Get name` 先返回 `bob`，删除后返回 `Key not found`。`Sync` 不会清空 WAL。

再次启动 CLI，运行 `Get name`、`Get empty`，观察重启后结果。然后执行 `Compact`、`Exit`，再查看：

```bash
ls -l kvstore.log kvstore.log.wal
xxd -g 1 kvstore.log | head
```

预期：`.wal` 回到 8 字节；基础日志包含仍有效的 `empty=x`，不包含已删除的 `name`。用纸画出每一步的 `MemTable`：`name=alice -> name=bob -> name=墓碑`，以及压缩后的状态。

## 3. 实验 B：把已有测试当教材读

在项目根目录运行：

```bash
cd /mnt/e/VsCode/Cmake/kvstore
ctest --test-dir build-fixed -R kvstore_wal_memtable_tests -V
ctest --test-dir build-fixed -R 'kvstore_basic_tests|kvstore_crc32_tests|kvstore_compaction_tests' --output-on-failure
```

对照下列源码，不要只看 Passed：

- `tests/wal_memtable_test.cpp`：旧基础日志与新 WAL 的优先级；重启后的重放；压缩后的 WAL 重置。
- `tests/kv_store_test.cpp`：把 WAL 最后一条记录截短，再重开并修复；完整记录仍可读。
- `tests/crc32_test.cpp`：完整但校验错误的记录应报错，不能被当作“正常截断尾部”。
- `tests/compaction_test.cpp`：重复写、删除、压缩后，文件总大小变小且最终数据保持不变。

读每个测试时写出四栏：**前置文件状态 / 执行的操作 / 预期内存状态 / 预期磁盘状态**。这比背源码更重要。

## 4. 实验 C：自己写一个“旧值被 WAL 遮蔽”的测试

先将练习骨架复制为 `tests/my_shadow_test.cpp`：

```bash
cp learning/my_shadow_test.template.cpp tests/my_shadow_test.cpp
```

模板已经写好头文件、检查函数和第一次打开数据库的代码。你补齐标有 `TODO` 的部分；先不要复制已有的 `wal_memtable_test.cpp`。只用 `KVStore` 的公开接口，完成下面的场景：

1. 使用独立文件名 `my-shadow-test.log`；模板已在开头删除旧的 `.log` 和 `.log.wal`。实验结束后也清理它们。
2. 第一次打开：模板已完成 `put("a", "old")`、`put("b", "keep")`、`compact()`，然后关闭对象。先读懂这一步。
3. 第二次打开：`put("a", "new")`、`remove("b")`，检查 `get("a") == "new"`，`get("b") == std::nullopt`。同时检查基础日志文件大小在这两次写入前后没有变化，而 WAL 大于 8 字节。
4. 第三次打开：不调用 `compact()`，再次验证 `a=new`、`b` 不存在。这一步验证 WAL 重放。
5. 第四次打开：调用 `compact()`；验证 WAL 恢复到 8 字节，再关闭并重开一次，结果依旧成立。
6. 任何断言失败时打印说明并返回 1；全部通过时返回 0。

编译、运行（不必修改 CMake）：

```bash
cd /mnt/e/VsCode/Cmake/kvstore
g++ -std=c++17 -Wall -Wextra -Wpedantic -I. \
  tests/my_shadow_test.cpp build-fixed/libkvstore.a \
  -pthread -o build-fixed/my_shadow_test
./build-fixed/my_shadow_test
```

如果卡住，先只完成第 1～3 步。提示：用 `{ KVStore store(path); ... }` 限定对象生命周期；用 `std::filesystem::file_size(path)` 检查文件大小；用 `std::optional::has_value()` 区分“不存在”和空字符串。查 `tests/wal_memtable_test.cpp` 只看 API 用法，不整段复制。

**通过标准**：测试通过后，故意把源码中 `get()` 的 MemTable 优先查找顺序颠倒，测试应该失败；恢复代码后测试重新通过。这个“红 -> 绿”过程能证明测试确实保护了行为。动生产源码前留一个本地副本，验证后立即恢复。

## 5. 实验 D：从确定性测试走向模型测试

用 `std::unordered_map<std::string, std::string>` 当正确答案模型。固定随机种子，在 10 个键上做 1000 次 `put/get/remove`；每 50 次关闭并重开 `KVStore`，每 100 次做一次 `compact()`。每次 `get` 与模型比较。这个实验要亲手写：它检验的是跨多次重启与压缩的状态一致性，比重复读现有断言更有价值。

先写最简单的版本：固定操作序列，不用随机数。确认通过后再使用 `std::mt19937` 和固定种子。失败时打印种子、步骤号、键、预期值和实际值，便于复现。不要用真正的断电测试代替普通进程重启测试：二者保证不同。

## 6. 掌握检查

不用看源码，能在纸上写出：

1. `Put(a,1); Compact(); Put(a,2); Delete(a);` 后，基础日志、WAL 和 MemTable 各有什么？`Get(a)` 为什么是不存在？
2. `put` 已返回但尚未 `sync`，进程退出与机器断电分别能推出什么结论？
3. 为什么截断的末尾记录可以修复，而完整但 CRC 错误的记录不能静默忽略？
4. `Compact` 时为什么先完成并同步新基础日志，再替换/重置 WAL？
5. 现有 `std::map` 为什么适合作为有序 MemTable？它与将来 SSTable 的关系是什么？

能独立完成实验 C，解释实验 B 中每个断言，并回答这些问题，再进入 SSTable 阶段。
