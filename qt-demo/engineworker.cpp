#include "engineworker.h"

#include "kv_store.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTime>

#include <algorithm>
#include <stdexcept>

namespace {

std::string toNativePath(const QString& path) {
    return QFile::encodeName(path).toStdString();
}

QString fromNativePath(const std::string& path) {
    return QFile::decodeName(QByteArray::fromStdString(path));
}

QString displayValue(const std::optional<std::string>& value) {
    if (!value.has_value()) {
        return QStringLiteral("⟨tombstone⟩");
    }
    if (value->empty()) {
        return QStringLiteral("⟨空字符串⟩");
    }
    return QString::fromStdString(*value);
}

QString roleFor(const QString& name, const QString& base) {
    if (name == base) {
        return QStringLiteral("旧格式 BASE（仅 BASE 0 时使用）");
    }
    if (name == base + QStringLiteral(".manifest")) {
        return QStringLiteral("MANIFEST 版本快照");
    }
    if (name == base + QStringLiteral(".wal")) {
        return QStringLiteral("Active WAL");
    }
    if (name == base + QStringLiteral(".lock")) {
        return QStringLiteral("进程锁");
    }
    if (name.startsWith(base + QStringLiteral(".wal.L0."))
        && name.endsWith(QStringLiteral(".imm"))) {
        return QStringLiteral("Immutable WAL（待发布）");
    }
    if (name.startsWith(base + QStringLiteral(".L0."))
        && name.endsWith(QStringLiteral(".sst"))) {
        return QStringLiteral("L0 SSTable");
    }
    if (name.startsWith(base + QStringLiteral(".base."))
        && name.endsWith(QStringLiteral(".log"))) {
        return QStringLiteral("BASE 数据文件");
    }
    if (name.endsWith(QStringLiteral(".tmp"))) {
        return QStringLiteral("临时文件（启动时回收）");
    }
    return QStringLiteral("其他");
}

} // namespace

EngineWorker::EngineWorker(QObject* parent) : QObject(parent) {}

EngineWorker::~EngineWorker() = default;

void EngineWorker::openDatabase(const QString& path, int limit, bool seedDemo) {
    try {
        store_.reset();
        dbPath_ = path;
        QDir().mkpath(QFileInfo(path).absolutePath());

        const bool fresh =
            !QFileInfo::exists(path)
            && !QFileInfo::exists(path + QStringLiteral(".manifest"));

        store_ = std::make_unique<KVStore>(
            toNativePath(path),
            static_cast<std::size_t>(limit));

        emit logMessage(QStringLiteral("[%1] 打开数据库 %2（阈值 %3）")
                            .arg(QTime::currentTime().toString("HH:mm:ss"),
                                 path)
                            .arg(limit));

        if (seedDemo && fresh) {
            seedDemoData();
        }

        publish();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void EngineWorker::seedDemoData() {
    QElapsedTimer timer;
    timer.start();

    // 1. Build a BASE file with three keys.
    store_->put("a", "base-a");
    store_->put("b", "base-b");
    store_->put("c", "base-c");
    store_->flush();
    store_->compact();

    // 2. Create one L0 table that shadows the BASE and deletes "b".
    store_->put("a", "new");
    store_->remove("b");
    store_->put("d", "four");
    store_->flush();

    // 3. Leave one entry in the Active MemTable.
    store_->put("e", "five");

    emit logMessage(QStringLiteral("[%1] 已写入演示数据（%2 ms）：BASE + 1 张 L0 + Active")
                        .arg(QTime::currentTime().toString("HH:mm:ss"))
                        .arg(timer.elapsed()));
}

void EngineWorker::refresh() {
    try {
        publish();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void EngineWorker::explain(const QString& key) {
    try {
        if (!store_) {
            throw std::runtime_error("数据库尚未打开");
        }

        DemoSnapshot snapshot = buildSnapshot();
        const auto probes = store_->explain_key(key.toStdString());

        QVector<DemoRow> probeRows;
        probeRows.reserve(static_cast<int>(probes.size()));

        QString result;
        bool decided = false;

        for (const auto& probe : probes) {
            DemoRow row;
            row.layer = QString::fromStdString(probe.layer);
            row.key = key;

            if (!probe.found) {
                row.state = 2;
                row.value = QStringLiteral("—");
            } else if (!probe.value.has_value()) {
                row.state = 1;
                row.value = QStringLiteral("⟨tombstone⟩");
            } else {
                row.state = 0;
                row.value = QString::fromStdString(*probe.value);
            }

            probeRows.push_back(row);

            if (!decided && probe.found) {
                decided = true;
                if (!probe.value.has_value()) {
                    result = QStringLiteral(
                                 "不存在：被 %1 的墓碑终止（更旧层不再读取）")
                                 .arg(row.layer);
                } else if (probe.value->empty()) {
                    result = QStringLiteral(
                                 "空字符串（存在的空值），来自 %1")
                                 .arg(row.layer);
                } else {
                    result = QStringLiteral("%1（来自 %2）")
                                 .arg(QString::fromStdString(*probe.value),
                                      row.layer);
                }
            }
        }

        if (!decided) {
            result = QStringLiteral("不存在：所有层均未命中");
        }

        snapshot.queryKey = key;
        snapshot.queryResult = result;
        snapshot.probeRows = probeRows;
        emit snapshotReady(snapshot);
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void EngineWorker::putValue(const QString& key, const QString& value) {
    try {
        if (!store_) {
            throw std::runtime_error("数据库尚未打开");
        }
        if (key.isEmpty()) {
            throw std::runtime_error("Key 不能为空");
        }

        store_->put(key.toStdString(), value.toStdString());
        emit logMessage(QStringLiteral("[%1] Put %2 = %3")
                            .arg(QTime::currentTime().toString("HH:mm:ss"),
                                 key, value));
        publish();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void EngineWorker::removeKey(const QString& key) {
    try {
        if (!store_) {
            throw std::runtime_error("数据库尚未打开");
        }

        const bool removed = store_->remove(key.toStdString());
        emit logMessage(QStringLiteral("[%1] Delete %2 → %3")
                            .arg(QTime::currentTime().toString("HH:mm:ss"),
                                 key,
                                 removed ? QStringLiteral("OK（写入墓碑）")
                                         : QStringLiteral("key 不存在，未写入")));
        publish();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void EngineWorker::syncNow() {
    try {
        if (!store_) {
            throw std::runtime_error("数据库尚未打开");
        }
        QElapsedTimer timer;
        timer.start();
        store_->sync();
        emit logMessage(QStringLiteral("[%1] Sync 完成（%2 ms）")
                            .arg(QTime::currentTime().toString("HH:mm:ss"))
                            .arg(timer.elapsed()));
        publish();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void EngineWorker::flushNow() {
    try {
        if (!store_) {
            throw std::runtime_error("数据库尚未打开");
        }
        QElapsedTimer timer;
        timer.start();
        store_->flush();
        emit logMessage(QStringLiteral("[%1] Flush 完成（%2 ms）")
                            .arg(QTime::currentTime().toString("HH:mm:ss"))
                            .arg(timer.elapsed()));
        publish();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void EngineWorker::compactNow() {
    try {
        if (!store_) {
            throw std::runtime_error("数据库尚未打开");
        }
        QElapsedTimer timer;
        timer.start();
        store_->compact();
        emit logMessage(QStringLiteral("[%1] Compact 完成（%2 ms）")
                            .arg(QTime::currentTime().toString("HH:mm:ss"))
                            .arg(timer.elapsed()));
        publish();
    } catch (const std::exception& error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

DemoSnapshot EngineWorker::buildSnapshot() const {
    DemoSnapshot snapshot;
    snapshot.dbPath = dbPath_;

    if (!store_) {
        return snapshot;
    }

    const auto entries = store_->inspect_entries(5000);
    snapshot.rows.reserve(static_cast<int>(entries.size()));

    for (const auto& entry : entries) {
        DemoRow row;
        row.layer = QString::fromStdString(entry.layer);
        row.key = QString::fromStdString(entry.key);
        row.state = entry.value.has_value() ? 0 : 1;
        row.value = displayValue(entry.value);
        snapshot.rows.push_back(row);
    }

    const auto tables = store_->inspect_tables();
    int l0Tables = 0;
    std::size_t baseEntries = 0;
    for (const auto& table : tables) {
        if (table.kind == "L0") {
            ++l0Tables;
        } else if (table.kind == "BASE") {
            baseEntries = table.entries;
        }
    }

    snapshot.summary =
        QStringLiteral("MemTable %1 项  ·  Immutable %2  ·  L0 %3 张  ·  BASE %4 项")
            .arg(store_->memtable_entry_count())
            .arg(store_->has_immutable_memtable() ? QStringLiteral("有")
                                                  : QStringLiteral("无"))
            .arg(l0Tables)
            .arg(baseEntries);

    if (!dbPath_.isEmpty()) {
        const QFileInfo database(dbPath_);
        const QString base = database.fileName();
        QDir directory = database.absoluteDir();
        const QFileInfoList files =
            directory.entryInfoList(QDir::Files, QDir::Name);

        for (const QFileInfo& info : files) {
            const QString name = info.fileName();
            if (!name.startsWith(base)) {
                continue;
            }
            DemoFileRow row;
            row.name = name;
            row.role = roleFor(name, base);
            row.size = info.size();
            snapshot.files.push_back(row);
        }
    }

    const QString manifestPath =
        fromNativePath(store_->manifest_file_path());
    QFile manifest(manifestPath);
    if (manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        snapshot.manifestText =
            QString::fromUtf8(manifest.readAll());
    } else {
        snapshot.manifestText = QStringLiteral("（尚未生成）");
    }

    return snapshot;
}

void EngineWorker::publish(const QString& queryKey,
                           const QString& queryResult,
                           const QVector<DemoRow>& probeRows) {
    DemoSnapshot snapshot = buildSnapshot();
    snapshot.queryKey = queryKey;
    snapshot.queryResult = queryResult;
    snapshot.probeRows = probeRows;
    emit snapshotReady(snapshot);
}
