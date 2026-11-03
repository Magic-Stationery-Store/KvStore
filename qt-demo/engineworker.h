#pragma once

#include "types.h"

#include <QObject>

#include <memory>

class KVStore;

// Owns the storage engine and runs every blocking call on a worker thread.
class EngineWorker : public QObject {
    Q_OBJECT

public:
    explicit EngineWorker(QObject* parent = nullptr);
    ~EngineWorker() override;

public slots:
    void openDatabase(const QString& path, int limit, bool seedDemo);
    void refresh();
    void explain(const QString& key);
    void putValue(const QString& key, const QString& value);
    void removeKey(const QString& key);
    void syncNow();
    void flushNow();
    void compactNow();

signals:
    void snapshotReady(const DemoSnapshot& snapshot);
    void logMessage(const QString& message);
    void failed(const QString& message);

private:
    DemoSnapshot buildSnapshot() const;
    void publish(const QString& queryKey = QString(),
                 const QString& queryResult = QString(),
                 const QVector<DemoRow>& probeRows = QVector<DemoRow>());
    void seedDemoData();

    std::unique_ptr<KVStore> store_;
    QString dbPath_;
};
