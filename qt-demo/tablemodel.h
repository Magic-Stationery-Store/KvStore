#pragma once

#include "types.h"

#include <QAbstractTableModel>

// Two modes:
//   * layer table: 层 / Key / 值
//   * probe table: 层 / 状态 / 值
class LayerTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit LayerTableModel(bool probeMode, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

    void setRows(const QVector<DemoRow>& rows);

private:
    QVector<DemoRow> rows_;
    bool probe_mode_ = false;
};
