# MANIFEST：有效文件清单与版本提交

这一阶段已将 MANIFEST 接入后台 Flush、启动恢复和 Compact。采用固定文件名的完整快照：每次提交重写整个清单。暂不使用追加式 VersionEdit 或 CURRENT 文件，先理解一次版本切换的原理。

## 1. 清单记录什么

例如先执行 Put a one、Flush，清单内容类似：

```text
KV_MANIFEST 1
NEXT 2
BASE 0
RETIRED 0
L0 1
TABLE 1
END
CRC32 <十进制校验值>
```

- `KV_MANIFEST 1`：清单格式及版本号。
- `NEXT`：下一个可用的文件编号。正在进行的 WAL 轮换也可能占用编号，恢复时会补充推进此值。
- `BASE 0`：读取原来的 `kvstore.log`。大于 0 时读取 `kvstore.log.base.<20位编号>.log`。
- `RETIRED`：不再恢复哪些旧 Immutable WAL。当前完整压缩产生的 BASE 编号也是这个截止点。
- `L0`：有效 L0 文件数量。
- `TABLE`：有效 L0 文件编号，按旧到新排列。
- `END`：完整快照结束标记。
- `CRC32`：校验上面的文本载荷，防止把损坏的清单当作有效状态。

路径由数据库名和编号计算，不直接信任清单中的任意文件路径。

## 2. 文件存在与文件有效有什么区别

目录中可能有一个已经写完的 SSTable，但程序在提交 MANIFEST 前退出。这是“未提交输出”。启动后，只加载清单中的 SSTable。

如果未提交文件还有配套 Immutable WAL，就从 WAL 重新生成 SSTable、提交清单，再删除 WAL；如果没有恢复来源，它就是孤儿文件，会被回收。临时 MANIFEST 也不会作为当前版本使用。

已经被 Compact 合并的 SSTable 也可能因为程序退出而暂未删除。只要新清单已提交，旧 SSTable 就无效，不能参与查询。

## 3. 后台 Flush 的提交顺序

```text
生成并同步 SSTable
        ↓
生成 MANIFEST.tmp（旧表编号 + 新表编号）
        ↓
同步 MANIFEST.tmp
        ↓
rename 替换 MANIFEST
        ↓
同步父目录
        ↓
删除对应 Immutable WAL
```

最后删 WAL 的原因：它是清单提交前的恢复来源。清单提交后，即使删除 WAL 的步骤没有完成，启动也能识别“已提交表 + 重复恢复日志”。

## 4. Compact 为什么改为新基础文件

如果先覆盖基础日志、再逐个删除旧 SSTable，程序中途退出时，新基础日志和旧 SSTable 会混在一起；旧值可能遮蔽新结果。

现在先生成独立的 `kvstore.log.base.<编号>.log`，旧版本保持可用。然后提交一份清单，同时指定新 BASE、清空 L0 列表、提高 RETIRED 截止编号。最后重置 Active WAL、回收旧文件。

```text
提交前：旧 BASE + 旧 L0
提交后：新 BASE + 空 L0
```

原来的 `kvstore.log` 暂时保留为旧格式基础文件。当前实际基础数据的位置以 MANIFEST 的 BASE 为准。Compact 仍属于完整压缩，尚未实现 L0 → L1 分层压缩。

## 5. Ubuntu 运行与观察

```bash
cd /mnt/e/VsCode/Cmake/kvstore
cmake -S . -B build-ubuntu -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ubuntu -j2
ctest --test-dir build-ubuntu --output-on-failure
ctest --test-dir build-ubuntu -R kvstore_manifest_tests -V
```

使用一个新的实验目录，在那里运行 CLI：

```bash
task_dir=$(mktemp -d /tmp/kv-manifest-demo.XXXXXX)
cd "$task_dir"
/mnt/e/VsCode/Cmake/kvstore/build-ubuntu/kvstore_cli
```

输入：

```text
Put a one
Put b two
Flush
Exit
```

查看：

```bash
cat kvstore.log.manifest
ls -lh kvstore.log*
```

再次运行 CLI，执行：

```text
Delete b
Compact
Get a
Get b
Exit
```

预期 `a` 返回 `one`、`b` 不存在；清单 `L0` 变为 0，`BASE` 和 `RETIRED` 变为新编号。查看该基础文件：

```bash
ls -lh kvstore.log.base.*.log
xxd -g 1 kvstore.log.base.*.log | head
cat kvstore.log.manifest
```

只有一个基础文件有效时，上面的通配符适用。MANIFEST 是可读文本；数据库日志和 SSTable 是二进制。

## 6. 新知识点与独立练习

1. **数据与元数据**：数据记录键值，元数据决定哪些数据文件组成当前版本。
2. **提交点**：数据文件先写好，通过一次清单替换使新版本生效。`rename` 后还要同步目录。
3. **文件生命周期**：临时文件 → 完整但未提交 → 有效 → 已退役 → 被回收。
4. **幂等恢复**：恢复重复执行，仍得到相同版本；已提交 SSTable 对应的旧 WAL 不重复应用。
5. **严格校验**：格式、CRC、编号排序、上下界都校验；有效文件丢失或清单损坏时直接报错，不静默从目录猜测另一套状态。
6. **版本快照**：先构造新 ManifestState，再发布；共享清单的提交由 mutex 串行保护。

读 `tests/manifest_test.cpp` 时，先预测每个模拟中断场景会采用哪些文件。独立练习：在临时实验目录复制一份旧 SSTable，Compact 后把旧文件放回原名，再重启，验证旧值不会复活；然后把 MANIFEST 中的一个数字改掉，验证程序拒绝打开（只操作实验数据）。

当前测试通过人工构造提交前后磁盘状态验证恢复规则，未做真实断电或完整 I/O 故障注入。
