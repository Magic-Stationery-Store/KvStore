#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KVStore LSM Console"));
    QApplication::setOrganizationName(QStringLiteral("KVStore"));

    qRegisterMetaType<DemoSnapshot>("DemoSnapshot");
    qRegisterMetaType<DemoRow>("DemoRow");
    qRegisterMetaType<DemoFileRow>("DemoFileRow");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("KVStore LSM demo console"));
    parser.addHelpOption();

    QCommandLineOption demoOption(
        QStringLiteral("demo"),
        QStringLiteral("Open and seed the demo database."));
    QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QStringLiteral("Save a screenshot to <path> and exit."),
        QStringLiteral("path"));

    parser.addOption(demoOption);
    parser.addOption(screenshotOption);
    parser.process(application);

    MainWindow window;
    window.show();

    if (parser.isSet(screenshotOption)) {
        window.scheduleScreenshot(parser.value(screenshotOption));
    }
    if (parser.isSet(demoOption)) {
        window.openDemoDatabase();
    }

    return application.exec();
}
