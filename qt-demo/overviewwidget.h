#pragma once

#include "types.h"

#include <QWidget>

class LevelOverviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit LevelOverviewWidget(QWidget* parent = nullptr);

    void setStats(
        const QVector<DemoLayerStat>& layers,
        const DemoCacheStat& cache,
        qint64 totalBytes);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<DemoLayerStat> layers_;
    DemoCacheStat cache_;
    qint64 totalBytes_ = 0;
};
