#pragma once

#include "types.h"

#include <QMainWindow>
#include <QThread>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStandardItemModel;
class QTableView;
class LayerTableModel;
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

    EngineWorker* worker_ = nullptr;
    QThread engineThread_;

    LayerTableModel* layerModel_ = nullptr;
    LayerTableModel* probeModel_ = nullptr;
    QTableView* layerView_ = nullptr;
    QTableView* probeView_ = nullptr;
    QStandardItemModel* fileModel_ = nullptr;
    QTableView* fileView_ = nullptr;
    QPlainTextEdit* manifestView_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QLineEdit* queryEdit_ = nullptr;
    QLabel* queryResult_ = nullptr;
    QLineEdit* keyEdit_ = nullptr;
    QLineEdit* valueEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QString screenshotPath_;
    bool screenshotPending_ = false;
    bool demoMode_ = false;
    bool demoExplainIssued_ = false;
};
