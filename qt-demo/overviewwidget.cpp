#include "overviewwidget.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>

#include <algorithm>

namespace {

QString formatBytes(qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KiB")
            .arg(QString::number(bytes / 1024.0, 'f', 1));
    }
    return QStringLiteral("%1 MiB")
        .arg(QString::number(bytes / 1024.0 / 1024.0, 'f', 1));
}

QColor layerColor(const QString& kind) {
    if (kind == QStringLiteral("L0")) {
        return QColor(0xE6, 0x7E, 0x22);
    }
    if (kind == QStringLiteral("L1")) {
        return QColor(0x3B, 0x82, 0xC4);
    }
    if (kind == QStringLiteral("L2")) {
        return QColor(0x8E, 0x5C, 0xC9);
    }
    if (kind == QStringLiteral("L3")) {
        return QColor(0x2A, 0x9D, 0x8F);
    }
    return QColor(0x6B, 0x72, 0x80);
}

} // namespace

LevelOverviewWidget::LevelOverviewWidget(QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(280);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void LevelOverviewWidget::setStats(
    const QVector<DemoLayerStat>& layers,
    const DemoCacheStat& cache,
    qint64 totalBytes
) {
    layers_ = layers;
    cache_ = cache;
    totalBytes_ = totalBytes;
    update();
}

QSize LevelOverviewWidget::sizeHint() const {
    return QSize(520, 340);
}

void LevelOverviewWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const QRectF area = rect().adjusted(18, 16, -18, -16);
    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(
        QRectF(area.left(), area.top(), area.width(), 28),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("LSM 层级分布"));

    QFont smallFont = font();
    smallFont.setPointSizeF(std::max(8.0, smallFont.pointSizeF() - 0.5));
    painter.setFont(smallFont);
    QColor muted = palette().color(QPalette::Text);
    muted.setAlpha(170);
    painter.setPen(muted);
    painter.drawText(
        QRectF(area.left(), area.top() + 28, area.width(), 20),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("总大小 %1  ·  Block Cache 命中率 %2%")
            .arg(formatBytes(totalBytes_))
            .arg(cache_.hits + cache_.misses == 0
                     ? 0.0
                     : 100.0 * static_cast<double>(cache_.hits)
                         / static_cast<double>(cache_.hits + cache_.misses),
                 0, 'f', 1));

    const qint64 maxBytes = std::max<qint64>(1, [&] {
        qint64 maximum = 1;
        for (const auto& layer : layers_) {
            maximum = std::max(maximum, layer.bytes);
        }
        return maximum;
    }());

    const qreal top = area.top() + 58;
    const qreal rowHeight = 38;
    const qreal barHeight = 8;
    const QFontMetrics metrics(font());

    for (int index = 0; index < layers_.size(); ++index) {
        const auto& layer = layers_[index];
        const qreal y = top + index * rowHeight;
        if (y > area.bottom() - 74) {
            break;
        }

        painter.setPen(palette().color(QPalette::Text));
        painter.setFont(font());
        painter.drawText(
            QRectF(area.left(), y, 42, 18),
            Qt::AlignLeft | Qt::AlignVCenter,
            layer.kind);

        const QString detail =
            QStringLiteral("%1 张表 · %2 条记录 · %3")
                .arg(layer.tables)
                .arg(layer.entries)
                .arg(formatBytes(layer.bytes));
        painter.setPen(muted);
        painter.setFont(smallFont);
        painter.drawText(
            QRectF(area.left() + 52, y, area.width() - 52, 18),
            Qt::AlignRight | Qt::AlignVCenter,
            detail);

        const QRectF track(
            area.left() + 52,
            y + 21,
            area.width() - 52,
            barHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::AlternateBase));
        painter.drawRoundedRect(track, 4, 4);

        const qreal ratio =
            static_cast<qreal>(layer.bytes) / static_cast<qreal>(maxBytes);
        QRectF fill = track;
        fill.setWidth(std::max<qreal>(6.0, track.width() * ratio));
        painter.setBrush(layerColor(layer.kind));
        painter.drawRoundedRect(fill, 4, 4);

        painter.setPen(palette().color(QPalette::Text));
        painter.setFont(smallFont);
        painter.drawText(
            QRectF(area.left(), y + 16, 44, 18),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("%1%").arg(
                layer.bytes == 0
                    ? 0
                    : static_cast<int>(
                          100.0 * static_cast<double>(layer.bytes)
                          / static_cast<double>(totalBytes_))));
    }

    const QRectF cacheCard(
        area.left(),
        area.bottom() - 58,
        area.width(),
        58);
    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.setBrush(palette().color(QPalette::AlternateBase));
    painter.drawRoundedRect(cacheCard, 8, 8);

    painter.setPen(palette().color(QPalette::Text));
    painter.setFont(font());
    painter.drawText(
        cacheCard.adjusted(12, 6, -12, -30),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("Block Cache"));

    painter.setFont(smallFont);
    painter.setPen(muted);
    painter.drawText(
        cacheCard.adjusted(12, 28, -12, -6),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("hit %1 · miss %2 · evict %3 · %4 块 / %5")
            .arg(cache_.hits)
            .arg(cache_.misses)
            .arg(cache_.evictions)
            .arg(cache_.entries)
            .arg(formatBytes(cache_.bytes)));

    painter.setPen(palette().color(QPalette::Text));
    painter.setFont(font());
    painter.drawText(
        cacheCard.adjusted(12, 6, -12, -30),
        Qt::AlignRight | Qt::AlignVCenter,
        QStringLiteral("%1%")
            .arg(cache_.hits + cache_.misses == 0
                     ? 0.0
                     : 100.0 * static_cast<double>(cache_.hits)
                         / static_cast<double>(cache_.hits + cache_.misses),
                 0, 'f', 1));

    Q_UNUSED(metrics)
}
