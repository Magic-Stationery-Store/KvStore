#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

// One row shown in the layer table or in the query-path table.
struct DemoRow {
    QString layer;
    QString key;
    QString value;
    // 0 = value, 1 = tombstone, 2 = not found in this layer
    int state = 0;
};

Q_DECLARE_METATYPE(DemoRow)

struct DemoFileRow {
    QString name;
    QString role;
    qint64 size = 0;
};

Q_DECLARE_METATYPE(DemoFileRow)

// Everything the GUI needs after one engine operation.  It is produced on the
// worker thread and copied to the GUI thread through a queued signal.
struct DemoSnapshot {
    QString dbPath;
    QString manifestText;
    QString summary;
    QString queryKey;
    QString queryResult;
    QVector<DemoRow> rows;
    QVector<DemoRow> probeRows;
    QVector<DemoFileRow> files;
};

Q_DECLARE_METATYPE(DemoSnapshot)
