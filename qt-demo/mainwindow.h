#pragma once

#include "types.h"

#include <QMainWindow>
#include <QModelIndex>
#include <QThread>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QSplitter;
class QStandardItemModel;
class QTableView;
class LayerTableModel;
class LevelOverviewWidget;
class EngineWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openDemoDatabase();
    void scheduleScreenshot(const QString& path);

signals:
    void openRequested(const QString& path, int limit, bool seedDemo);
    void refreshRequested();
    void explainRequested(const QString& key);
    void putRequested(const QString& key, const QString& value);
    void removeRequested(const QString& key);
    void syncRequested();
    void flushRequested();
    void compactRequested();
    void compactLevelsRequested();
    void compactL1ToL2Requested();
    void compactL2ToL3Requested();

private slots:
    void applySnapshot(const DemoSnapshot& snapshot);
    void appendLog(const QString& message);
    void showError(const QString& message);
    void openDatabaseDialog();
    void triggerScreenshotIfReady();

private:
    void buildUi();
    void connectEngine();
    void log(const QString& message);
    void restoreUiState();
    void saveUiState();
    void updateDetail(const QModelIndex& current);

    EngineWorker* worker_ = nullptr;
    QThread engineThread_;

    LayerTableModel* layerModel_ = nullptr;
    QSortFilterProxyModel* layerProxy_ = nullptr;
    LayerTableModel* probeModel_ = nullptr;
    QTableView* layerView_ = nullptr;
    QTableView* probeView_ = nullptr;
    QStandardItemModel* fileModel_ = nullptr;
    QTableView* fileView_ = nullptr;
    QPlainTextEdit* manifestView_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QPlainTextEdit* detailView_ = nullptr;
    QLineEdit* queryEdit_ = nullptr;
    QLabel* queryResult_ = nullptr;
    QLineEdit* keyEdit_ = nullptr;
    QLineEdit* valueEdit_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    LevelOverviewWidget* overview_ = nullptr;
    QSplitter* splitter_ = nullptr;

    QString screenshotPath_;
    bool screenshotPending_ = false;
    bool demoMode_ = false;
    bool demoExplainIssued_ = false;
};
