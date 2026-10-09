// native-spike: QML TextEdit (QTextDocument) as Rune's editing core.
//
// Usage: rune_native_spike [--auto-export]
//   --auto-export  dump document structure, screenshot, export ODF, then quit
//                  (headless verification; same path as Ctrl+S)

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption autoExportOpt(QStringLiteral("auto-export"),
                                           QStringLiteral("Export to ODF on startup, then quit."));
    parser.addOption(autoExportOpt);
    parser.process(app);

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"autoExport", parser.isSet(autoExportOpt)}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("RuneNativeSpike", "Main");
    return app.exec();
}
