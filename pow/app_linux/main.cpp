// main.cpp -- Glaciem Miner (Linux Qt6/QML) entry point.
//
// Boots a QGuiApplication, registers MinerEngine as a singleton QML type so
// the UI can bind to its properties, and loads the Main.qml dashboard.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

#include "miner_engine.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("Glaciem Miner");
    app.setOrganizationName("Glaciem");
    app.setOrganizationDomain("glaciem.frostmine.workers.dev");

    MinerEngine engine;

    QQmlApplicationEngine qml;
    qml.rootContext()->setContextProperty("MinerEngine", &engine);
    qml.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (qml.rootObjects().isEmpty()) return -1;

    return app.exec();
}
