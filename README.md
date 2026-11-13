# KVStore：从零实现的 LSM-Tree 单机存储引擎

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Qt](https://img.shields.io/badge/Qt-6.8%2B-green)

一个 C++17 编写的嵌入式持久化键值存储，实现了 WAL、有序 MemTable、不可变
MemTable、后台 Flush、多 L0 SSTable、MANIFEST 版本提交和全量 Compact。
配套一个 Qt Widgets 可视化控制台，用来观察各层数据、MANIFEST 和单 key 的查询路径。

![Qt 控制台截图](docs/screenshot.png)

## 核心特性

- **写入路径**：`Put/Delete` 追加 WAL → 更新有序 MemTable → 达到阈值后冻结。
- **崩溃恢复**：WAL 重放、截断尾部修复、CRC 校验；Active/Immutable 双 WAL 轮换
  保证冻结快照在发布为 SSTable 之前始终可恢复。
- **后台 Flush**：单后台线程把冻结快照写成 SSTable，提交 MANIFEST 后再回收
  Immutable WAL；前台通过条件变量等待发布完成（Flush 屏障）。
- **版本提交**：MANIFEST 是完整文本快照，带 CRC32；`tmp → fsync → rename →
  目录 fsync` 一次原子替换即切换版本。
- **全量 Compact**：合并 BASE、所有 L0 和 Active 数据，生成新 BASE 并一次性
  退役全部 L0；旧文件残留不会让删除的 key 复活。
- **并发安全**：单互斥锁保护全部状态，后台线程只处理冻结快照；进程锁防止
  同一个数据库被多个进程打开。
- **可观测性**：`inspect_entries` / `explain_key` / `inspect_tables` 只读接口，
  Qt 控制台直接展示层次数据和查询路径（含被遮蔽的旧值）。
- **MVCC 快照读**：内部键为 `user_key + 反序 sequence(8B) + type(1B)`；
  `snapshot()` / `get(key, snapshot)` / `new_iterator(snapshot)` 可读取历史版本，
  墓碑在快照中同样可见。Compact 在没有活跃快照时回收旧版本，有快照时保守保留。
- **WriteBatch**：多条 Put/Delete 合并成一条 WAL Batch 记录，共享一个 commit
  sequence 原子提交；恢复时整条记录重放，截断则整批回滚。
- **快照迭代器**：`new_iterator()` 跨 Active / Immutable / L0 / BASE 做多路归并，
  提供 `Seek/Next` 并跳过墓碑；当前是快照物化实现，下一步替换为流式迭代器。
- **跨平台**：核心逻辑 POSIX/Windows 双实现（`src/platform.cpp`），
  Windows 上用 `CreateFile/FlushFileBuffers/MoveFileEx` 替代 `fsync/rename/flock`。

### MVCC / WriteBatch 用法

```cpp
auto snapshot = store.snapshot();
store.get("key", snapshot);          // read at a stable sequence
auto iterator = store.new_iterator(snapshot);

KVStore::WriteBatch batch;
batch.put("a", "1");
batch.erase("b");
store.write(batch);                  // one WAL record, all-or-nothing
```

## 架构

```text
写：Put/Delete
      → 追加 WAL 记录（不 fsync）
      → 更新 Active MemTable（std::map，有序，nullopt 为墓碑）
      → 不同 key 数 ≥ 阈值（默认 64）
      → 冻结：fsync WAL → 改名 .imm → 新建 Active WAL → 快照 swap
      → 后台线程：写 L0 SSTable → 提交 MANIFEST → 删除 .imm

读：Active MemTable
      → Immutable MemTable
      → L0 从新到旧
      → BASE 内存索引 + 磁盘读值

Compact：冻结并等待后台 → 合并 BASE + 全部 L0 + Active
      → 写新 BASE（tmp → fsync → rename）
      → 一次 MANIFEST 提交（BASE/RETIRED 前进、L0 清空）
      → 回收旧文件、重置 WAL
```

## 磁盘文件

| 文件 | 内容 |
| --- | --- |
| `kvstore.log.wal` | Active MemTable 的 WAL |
| `kvstore.log.wal.L0.<20位编号>.imm` | 已冻结、尚未发布的 WAL |
| `kvstore.log.L0.<20位编号>.sst` | 不可变有序表，有效性由 MANIFEST 决定 |
| `kvstore.log.manifest` | 完整版本快照（文本 + CRC32） |
| `kvstore.log.base.<20位编号>.log` | Compact 生成的基础数据 |
| `kvstore.log` | 旧格式基础文件（仅 `BASE 0` 时使用） |
| `kvstore.log.lock` | 进程锁 |

记录格式：8 字节文件头（`KVDB` + 版本号），随后每条记录为
`u8 op | u32 key_len | u32 value_len | key | value | u32 crc32`。

## 构建与测试（Linux / WSL）

```bash
cmake -S . -B build-ubuntu -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ubuntu -j2
ctest --test-dir build-ubuntu --output-on-failure
```

当前 17 项测试全部通过，覆盖模型对拍、WAL 截断修复、L0 遮蔽顺序、后台 Flush、
并发与进程锁、MANIFEST 的六类中断点恢复、SIGKILL 循环恢复、MVCC 快照、
WriteBatch 原子提交，以及可视化控制台使用的只读观测接口。

```bash
# 单测示例
ctest --test-dir build-ubuntu -R '^kvstore_manifest_tests$' -V

# 命令行体验
./build-ubuntu/kvstore_cli
```

CLI 支持 `Put / Get / Delete / Sync / Flush / Compact / Exit`。

其中 `kvstore_crash_recovery_tests` 会 fork 子进程顺序写入、随机延迟后 `SIGKILL`，
重开校验“已写入的 key 必须构成前缀”。默认 100 次，可用
`KVSTORE_CRASH_ITERATIONS` 调整；本机实测 200 次全部通过（存活 key 150–1851，
无空洞、无错值）。

Sanitizer 构建：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DKVSTORE_SANITIZER=address,undefined
cmake --build build-asan -j2
ctest --test-dir build-asan --output-on-failure
```

实测 ASan/UBSan 下 17/17 通过；`-DKVSTORE_SANITIZER=thread` 构建下 16 项通过
（不含依赖 fork 的 crash test），无数据竞争报告。

Windows 下引擎与 CLI 也可以构建（POSIX 调用由 `src/platform.cpp` 的 Win32
实现替代）：

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=<MinGW>/bin/g++.exe
cmake --build build-win
```

## 性能基准

`bench/benchmark.cpp` 提供单线程基准，覆盖 Put/Get、value 大小、Flush/Compact 和 L0 读放大。

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-bench -j2 --target kvstore_bench
./build-bench/kvstore_bench
```

默认使用 `std::filesystem::temp_directory_path()`；Linux 下建议用 `TMPDIR` 指向真实文件系统，
避免 tmpfs 让 `fsync` 变成空操作：

```bash
mkdir -p /home/you/kvstore-bench
TMPDIR=/home/you/kvstore-bench ./build-bench/kvstore_bench
```

本机实测（WSL2 / Ubuntu 26.04 / ext4 / i9-13980HX / 单线程，value 100 B，5 万 key）：

| 指标 | 数值 |
| --- | --- |
| Put 吞吐（不含 fsync） | 33.1 万 ops/s |
| 一次 sync | 12.1 ms |
| Get（MemTable） | 200 万 ops/s，P50 0.3 us / P99 0.8 us |
| Get（L0） | 39.8 万 ops/s，P50 2.1 us / P99 5.7 us |
| Flush | 122.1 ms |
| Compact | 159.2 ms |
| Get（BASE） | 38.5 万 ops/s，P50 2.2 us / P99 3.8 us |

value 大小对吞吐的影响：

| value | 记录数 | Put ops/s | Get ops/s | Get P99 |
| --- | --- | --- | --- | --- |
| 64 B | 20000 | 34.7 万 | 230 万 | 0.7 us |
| 1 KiB | 20000 | 12.8 万 | 164 万 | 1.1 us |
| 4 KiB | 10000 | 4.2 万 | 127 万 | 1.4 us |

L0 读放大（`missing` 需要扫完所有 L0；`victim` 命中最早的表）：

| L0 表数 | missing P50 | victim P50 |
| --- | --- | --- |
| 1 | 1.45 us | 1.47 us |
| 10 | 1.72 us | 1.63 us |
| 25 | 2.29 us | 2.42 us |
| 50 | 12.68 us | 3.40 us |

说明：

- Put 只写 WAL 到页缓存，不含 `fsync`；`sync` 是 5 万条写后的一次 fsync。
- 每个 key 额外带 9 B 的 MVCC 内部后缀，所以 BASE 约 6640 KiB；5 万个 key 都是唯一的，
  没有旧版本可回收，因此 Compact 前后大小不变；覆盖/删除场景由 `kvstore_compaction_tests` 覆盖。
- L0 表数到 50 时 `missing` P50 从 1.45 us 升到 12.68 us，体现读放大；`victim` 命中
  最早的表，稳定在 1.5–3.4 us。
- Put/Get 已包含内部键编码和快照版本选择；数字是本机实测，换机器会变。

## Qt 可视化控制台

`qt-demo/` 是一个 Qt Widgets 程序，链接 `libkvstore.a`，全部引擎调用都在 worker
线程执行，界面通过队列信号刷新。功能：

- 层次数据表：Active / Immutable / L0（新→旧）/ BASE，墓碑标红；
- 查询路径：逐层展示命中/墓碑/未命中，并给出最终结果；
- 文件列表：WAL、`.imm`、`.sst`、`.base.*.log`、MANIFEST 及大小；
- MANIFEST 原文、操作日志、Sync / Flush / Compact 按钮。

构建（Windows + MSYS2 Qt6，或 Linux 安装 `qt6-base-dev`）：

```powershell
# Windows：Qt 装在项目内的 .toolchains/msys64 时，直接运行脚本
.\scripts\build-qt-demo.ps1
.\scripts\run-qt-demo.ps1 -Demo
```

```bash
# Linux / WSL
cmake -S . -B build-qt -G Ninja -DKVSTORE_BUILD_QT_DEMO=ON \
  -DCMAKE_PREFIX_PATH=<Qt6 安装路径>
cmake --build build-qt
./build-qt/qt-demo/kvstore_qt_demo --demo
```

`--demo` 会生成一个包含 BASE + L0 + Active 三层的演示库；在查询框输入 `b`
可以看到 L0 的墓碑遮蔽 BASE 中的旧值。`--screenshot <path>` 可无头导出截图。
详见 [qt-demo/README.md](qt-demo/README.md)。

## 设计取舍

| 决策 | 原因 | 代价 |
| --- | --- | --- |
| 每 key 全量内存索引 | 实现简单，查询 O(log N) | 大库内存占用高，尚无块索引/Bloom |
| 全量文本 MANIFEST | 一次替换提交整个版本，易校验 | 每次 Flush 重写整份清单 |
| 单后台线程 + 单 Immutable 槽位 | 天然背压，状态简单 | 前台写入可能被阻塞 |
| Compact 持锁 | 实现简单、版本一致 | 压缩期间阻塞读写 |
| 记录级 CRC | 能定位损坏记录 | SSTable 整体无校验和 |

## 目录结构

```text
kv_store.h / src/       引擎实现（WAL、MemTable、SSTable、MANIFEST、平台层）
app/main.cpp            命令行程序
bench/                  单线程性能基准
qt-demo/                Qt Widgets 可视化控制台
tests/                  17 个端到端测试
learning/               学习期练习代码
docs/                   截图与文档
```

## Roadmap

- [x] 单线程性能基准（Put/Get、value 大小、L0 读放大、Compact）
- [x] SIGKILL 崩溃恢复循环（200 次）
- [x] ASan/UBSan/TSan 验证
- [ ] 磁盘满 / EIO 故障注入
- [ ] L0 → L1 分层压缩，部分压缩时保留墓碑
- [ ] SSTable 块索引 + Bloom Filter
- [x] 快照迭代器（Seek/Next，跳过墓碑）
- [x] MVCC 快照读（get / iterator）
- [x] WriteBatch 原子提交
- [ ] 快照感知 GC（当前有活跃快照时保留全部版本）
- [ ] 流式迭代器 / 范围扫描 API
- [ ] WAL 组提交、无锁读路径

## License

MIT
