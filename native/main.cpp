// Rune (native): single-process editor on QML TextEdit + QTextDocument.
//
// Usage: rune [--auto-test]
//   --auto-test  type, format and save a document headlessly, report
//                PASS/FAIL, exit (0 = pass)

#include "AutoTest.h"
#include "DocumentController.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>

namespace {

// Pull the active Omarchy palette; fall back to a neutral dark palette.
QVariantMap omarchyTheme()
{
    QVariantMap m{
        {"background", "#1a1b26"}, {"foreground", "#c0caf5"},
        {"accent", "#7aa2f7"},     {"muted", "#414868"},
        {"lighter_background", "#24283b"}, {"red", "#f7768e"},
        {"green", "#9ece6a"},      {"yellow", "#e0af68"},
    };
    QFile f(QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return m;
    static const QRegularExpression re(R"(^\s*(\w+)\s*=\s*"(#[0-9A-Fa-f]{6,8})\")");
    QTextStream in(&f);
    while (!in.atEnd()) {
        const auto match = re.match(in.readLine());
        if (match.hasMatch())
            m.insert(match.captured(1), match.captured(2));
    }
    return m;
}

struct Options
{
    bool autoTest = false;
    QString documentPath; // positional argument: a document to open at startup
};

Options parseOptions(const QCoreApplication &app)
{
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption autoTestOpt(QStringLiteral("auto-test"),
                                         QStringLiteral("Run the headless self-test, then quit."));
    parser.addOption(autoTestOpt);
    parser.addPositionalArgument(QStringLiteral("document"),
                                 QStringLiteral("A .odt, .docx, or .doc file to open at startup."));
    parser.process(app);

    Options options;
    options.autoTest = parser.isSet(autoTestOpt);
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty())
        options.documentPath = positional.constFirst();
    return options;
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Rune"));
    const Options options = parseOptions(app);

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"theme", omarchyTheme()}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Rune", "Main");

    auto *window = engine.rootObjects().isEmpty()
        ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());

    if (!options.documentPath.isEmpty() && window) {
        auto *controller = window->findChild<DocumentController *>(QStringLiteral("controller"));
        if (controller)
            controller->openFile(options.documentPath);
    }

    if (options.autoTest) {
        if (!window)
            return 1;
        // Let the window expose and the scene settle first.
        QTimer::singleShot(300, window, [window] { QCoreApplication::exit(runAutoTest(window) ? 0 : 2); });
    }
    return app.exec();
}
