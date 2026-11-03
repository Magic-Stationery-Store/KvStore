#include "tablemodel.h"

#include <QBrush>
#include <QColor>
#include <QFont>

namespace {

QString statusText(int state) {
    switch (state) {
    case 1:
        return QStringLiteral("墓碑");
    case 2:
        return QStringLiteral("未命中");
    default:
        return QStringLiteral("命中");
    }
}

} // namespace

LayerTableModel::LayerTableModel(bool probeMode, QObject* parent)
    : QAbstractTableModel(parent), probe_mode_(probeMode) {}

int LayerTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

int LayerTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 3;
}

QVariant LayerTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) {
        return {};
    }

    const DemoRow& row = rows_.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0:
            return row.layer;
        case 1:
            return probe_mode_ ? statusText(row.state) : row.key;
        case 2:
            return row.value;
        default:
            return {};
        }
    }

    if (role == Qt::ForegroundRole) {
        if (row.state == 1) {
            return QBrush(QColor(0xc0, 0x39, 0x2b));
        }
        if (row.state == 2) {
            return QBrush(QColor(0x95, 0xa5, 0xa6));
        }
        if (probe_mode_ && row.state == 0) {
            return QBrush(QColor(0x1e, 0x84, 0x4a));
        }
    }

    if (role == Qt::FontRole && index.column() == 0) {
        QFont font;
        font.setBold(true);
        return font;
    }

    if (role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }

    if (role == Qt::ToolTipRole) {
        return QStringLiteral("%1 / %2").arg(row.layer, row.key);
    }

    return {};
}

QVariant LayerTableModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return {};
    }

    if (probe_mode_) {
        switch (section) {
        case 0:
            return QStringLiteral("层");
        case 1:
            return QStringLiteral("状态");
        default:
            return QStringLiteral("值");
        }
    }

    switch (section) {
    case 0:
        return QStringLiteral("层");
    case 1:
        return QStringLiteral("Key");
    default:
        return QStringLiteral("值");
    }
}

void LayerTableModel::setRows(const QVector<DemoRow>& rows) {
    beginResetModel();
    rows_ = rows;
    endResetModel();
}
