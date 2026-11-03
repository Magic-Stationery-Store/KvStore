#include "mainwindow.h"

#include "engineworker.h"
#include "tablemodel.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

QString formatSize(qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB")
            .arg(QString::number(bytes / 1024.0, 'f', 1));
    }
    return QStringLiteral("%1 MB")
        .arg(QString::number(bytes / 1024.0 / 1024.0, 'f', 1));
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    connectEngine();
}

MainWindow::~MainWindow() {
    engineThread_.quit();
    engineThread_.wait();
    delete worker_;
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("KVStore · LSM 控制台"));
    resize(1280, 820);

    auto* toolbar = addToolBar(QStringLiteral("操作"));
    toolbar->setMovable(false);

    auto* openAction = toolbar->addAction(QStringLiteral("打开数据库…"));
    auto* refreshAction = toolbar->addAction(QStringLiteral("刷新"));
    toolbar->addSeparator();
    auto* syncAction = toolbar->addAction(QStringLiteral("Sync"));
    auto* flushAction = toolbar->addAction(QStringLiteral("Flush"));
    auto* compactAction = toolbar->addAction(QStringLiteral("Compact"));

    connect(openAction, &QAction::triggered,
            this, &MainWindow::openDatabaseDialog);
    connect(refreshAction, &QAction::triggered,
            this, [this] { emit refreshRequested(); });
    connect(syncAction, &QAction::triggered,
            this, [this] { emit syncRequested(); });
    connect(flushAction, &QAction::triggered,
            this, [this] { emit flushRequested(); });
    connect(compactAction, &QAction::triggered,
            this, [this] { emit compactRequested(); });

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* tabs = new QTabWidget(splitter);

    // --- layer table ---------------------------------------------------
    auto* layersPage = new QWidget(tabs);
    auto* layersLayout = new QVBoxLayout(layersPage);
    auto* hint = new QLabel(
        QStringLiteral(
            "读取顺序：Active → Immutable → L0（新→旧）→ BASE。"
            "红色为墓碑，灰色表示该层未命中。"),
        layersPage);
    hint->setWordWrap(true);
    layersLayout->addWidget(hint);

    layerView_ = new QTableView(layersPage);
    layerModel_ = new LayerTableModel(false, this);
    layerView_->setModel(layerModel_);
    layerView_->setAlternatingRowColors(true);
    layerView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layerView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layerView_->verticalHeader()->setVisible(false);
    layerView_->horizontalHeader()->setStretchLastSection(true);
    layerView_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    layersLayout->addWidget(layerView_);
    tabs->addTab(layersPage, QStringLiteral("层次数据"));

    // --- file list -----------------------------------------------------
    auto* filesPage = new QWidget(tabs);
    auto* filesLayout = new QVBoxLayout(filesPage);
    fileView_ = new QTableView(filesPage);
    fileModel_ = new QStandardItemModel(0, 3, this);
    fileModel_->setHorizontalHeaderLabels(
        {QStringLiteral("文件"), QStringLiteral("大小"), QStringLiteral("类型")});
    fileView_->setModel(fileModel_);
    fileView_->setAlternatingRowColors(true);
    fileView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fileView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fileView_->verticalHeader()->setVisible(false);
    fileView_->horizontalHeader()->setStretchLastSection(true);
    filesLayout->addWidget(fileView_);
    tabs->addTab(filesPage, QStringLiteral("文件"));

    // --- manifest ------------------------------------------------------
    auto* manifestPage = new QWidget(tabs);
    auto* manifestLayout = new QVBoxLayout(manifestPage);
    manifestView_ = new QPlainTextEdit(manifestPage);
    manifestView_->setReadOnly(true);
    manifestView_->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    manifestLayout->addWidget(manifestView_);
    tabs->addTab(manifestPage, QStringLiteral("MANIFEST"));

    splitter->addWidget(tabs);

    // --- right panel ---------------------------------------------------
    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);

    auto* queryGroup = new QGroupBox(QStringLiteral("查询路径"), right);
    auto* queryLayout = new QVBoxLayout(queryGroup);

    auto* queryRow = new QHBoxLayout();
    queryEdit_ = new QLineEdit(QStringLiteral("b"), queryGroup);
    queryEdit_->setPlaceholderText(QStringLiteral("输入 key"));
    auto* queryButton = new QPushButton(QStringLiteral("查询"), queryGroup);
    queryButton->setDefault(true);
    queryRow->addWidget(queryEdit_, 1);
    queryRow->addWidget(queryButton);
    queryLayout->addLayout(queryRow);

    queryResult_ = new QLabel(QStringLiteral("尚未查询"), queryGroup);
    queryResult_->setWordWrap(true);
    queryResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    queryResult_->setStyleSheet(QStringLiteral("padding: 6px;"));
    queryLayout->addWidget(queryResult_);

    probeView_ = new QTableView(queryGroup);
    probeModel_ = new LayerTableModel(true, this);
    probeView_->setModel(probeModel_);
    probeView_->setAlternatingRowColors(true);
    probeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    probeView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    probeView_->verticalHeader()->setVisible(false);
    probeView_->horizontalHeader()->setStretchLastSection(true);
    queryLayout->addWidget(probeView_, 1);

    rightLayout->addWidget(queryGroup, 1);

    auto* writeGroup = new QGroupBox(QStringLiteral("写入"), right);
    auto* writeLayout = new QGridLayout(writeGroup);
    keyEdit_ = new QLineEdit(writeGroup);
    keyEdit_->setPlaceholderText(QStringLiteral("key"));
    valueEdit_ = new QLineEdit(writeGroup);
    valueEdit_->setPlaceholderText(QStringLiteral("value"));
    auto* putButton = new QPushButton(QStringLiteral("Put"), writeGroup);
    auto* deleteButton =
        new QPushButton(QStringLiteral("Delete"), writeGroup);

    writeLayout->addWidget(new QLabel(QStringLiteral("Key"), writeGroup), 0, 0);
    writeLayout->addWidget(keyEdit_, 0, 1);
    writeLayout->addWidget(new QLabel(QStringLiteral("Value"), writeGroup), 1, 0);
    writeLayout->addWidget(valueEdit_, 1, 1);
    writeLayout->addWidget(putButton, 2, 0);
    writeLayout->addWidget(deleteButton, 2, 1);

    rightLayout->addWidget(writeGroup);

    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    setCentralWidget(splitter);

    // --- log dock ------------------------------------------------------
    auto* dock = new QDockWidget(QStringLiteral("操作日志"), this);
    logView_ = new QPlainTextEdit(dock);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    dock->setWidget(logView_);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    statusLabel_ = new QLabel(QStringLiteral("未打开数据库"), this);
    statusBar()->addWidget(statusLabel_, 1);

    connect(queryButton, &QPushButton::clicked, this, [this] {
        emit explainRequested(queryEdit_->text());
    });
    connect(queryEdit_, &QLineEdit::returnPressed, this, [this] {
        emit explainRequested(queryEdit_->text());
    });
    connect(putButton, &QPushButton::clicked, this, [this] {
        emit putRequested(keyEdit_->text(), valueEdit_->text());
    });
    connect(deleteButton, &QPushButton::clicked, this, [this] {
        emit removeRequested(keyEdit_->text());
    });
}

void MainWindow::connectEngine() {
    worker_ = new EngineWorker;
    worker_->moveToThread(&engineThread_);

    connect(this, &MainWindow::openRequested,
            worker_, &EngineWorker::openDatabase);
    connect(this, &MainWindow::refreshRequested,
            worker_, &EngineWorker::refresh);
    connect(this, &MainWindow::explainRequested,
            worker_, &EngineWorker::explain);
    connect(this, &MainWindow::putRequested,
            worker_, &EngineWorker::putValue);
    connect(this, &MainWindow::removeRequested,
            worker_, &EngineWorker::removeKey);
    connect(this, &MainWindow::syncRequested,
            worker_, &EngineWorker::syncNow);
    connect(this, &MainWindow::flushRequested,
            worker_, &EngineWorker::flushNow);
    connect(this, &MainWindow::compactRequested,
            worker_, &EngineWorker::compactNow);

    connect(worker_, &EngineWorker::snapshotReady,
            this, &MainWindow::applySnapshot);
    connect(worker_, &EngineWorker::logMessage,
            this, &MainWindow::appendLog);
    connect(worker_, &EngineWorker::failed,
            this, &MainWindow::showError);

    engineThread_.start();
}

void MainWindow::openDatabaseDialog() {
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("打开或新建数据库"),
        QDir::currentPath() + QStringLiteral("/kvstore.log"),
        QStringLiteral("KVStore 数据库 (*.log);;所有文件 (*)"),
        nullptr,
        QFileDialog::DontConfirmOverwrite);

    if (path.isEmpty()) {
        return;
    }

    log(QStringLiteral("打开 %1（阈值 64）").arg(path));
    emit openRequested(path, 64, false);
}

void MainWindow::openDemoDatabase() {
    demoMode_ = true;
    const QString directory =
        QCoreApplication::applicationDirPath() + QStringLiteral("/demo-data");
    const QString path = directory + QStringLiteral("/demo.log");
    log(QStringLiteral("准备演示数据库 %1（阈值 3）").arg(path));
    emit openRequested(path, 3, true);
}

void MainWindow::scheduleScreenshot(const QString& path) {
    screenshotPath_ = path;
    screenshotPending_ = true;
}

void MainWindow::triggerScreenshotIfReady() {
    if (!screenshotPending_) {
        return;
    }
    screenshotPending_ = false;

    QTimer::singleShot(900, this, [this] {
        if (!screenshotPath_.isEmpty()) {
            grab().save(screenshotPath_);
            appendLog(QStringLiteral("已保存截图 %1").arg(screenshotPath_));
        }
        QCoreApplication::quit();
    });
}

void MainWindow::applySnapshot(const DemoSnapshot& snapshot) {
    layerModel_->setRows(snapshot.rows);
    probeModel_->setRows(snapshot.probeRows);

    if (!snapshot.queryResult.isEmpty()) {
        queryResult_->setText(snapshot.queryResult);
    }
    if (!snapshot.queryKey.isEmpty()) {
        queryEdit_->setText(snapshot.queryKey);
    }

    manifestView_->setPlainText(snapshot.manifestText);

    fileModel_->removeRows(0, fileModel_->rowCount());
    for (const auto& file : snapshot.files) {
        QList<QStandardItem*> items;
        items << new QStandardItem(file.name)
              << new QStandardItem(formatSize(file.size))
              << new QStandardItem(file.role);
        for (auto* item : items) {
            item->setEditable(false);
        }
        fileModel_->appendRow(items);
    }

    QString status = snapshot.summary;
    if (!snapshot.dbPath.isEmpty()) {
        status += QStringLiteral("   ·   ") + snapshot.dbPath;
    }
    statusLabel_->setText(status);

    // In demo mode the first snapshot only contains the seeded layers.  Run
    // one explain so the screenshot also shows the per-layer query path.
    if (demoMode_ && !demoExplainIssued_ && !snapshot.dbPath.isEmpty()) {
        demoExplainIssued_ = true;
        emit explainRequested(
            queryEdit_->text().isEmpty() ? QStringLiteral("b")
                                         : queryEdit_->text());
        return;
    }

    triggerScreenshotIfReady();
}

void MainWindow::appendLog(const QString& message) {
    logView_->appendPlainText(message);
}

void MainWindow::showError(const QString& message) {
    appendLog(QStringLiteral("错误：%1").arg(message));
    QMessageBox::critical(this, QStringLiteral("操作失败"), message);
}

void MainWindow::log(const QString& message) {
    logView_->appendPlainText(message);
}
