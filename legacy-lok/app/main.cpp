// Rune: Qt Quick frontend. Talks to rune-engine over its Unix socket.
//
// Usage: rune [--socket-path PATH] [document]

#include "BridgeSocket.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QRegularExpression>
#include <QTextStream>

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

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Rune"));

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption socketOpt(QStringLiteral("socket-path"),
                                       QStringLiteral("rune-engine Unix socket."),
                                       QStringLiteral("path"), BridgeSocket::defaultSocketPath());
    parser.addOption(socketOpt);
    parser.addPositionalArgument(QStringLiteral("document"), QStringLiteral("Document to open."));
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    const QString document = QFileInfo(args.isEmpty() ? QStringLiteral(RUNE_DEFAULT_DOC) : args.first())
                                 .absoluteFilePath();

    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {"theme", omarchyTheme()},
        {"socketPath", parser.value(socketOpt)},
        {"documentPath", document},
    });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Rune", "Main");
    return app.exec();
}
