// Rune (native): single-process editor on QML TextEdit + QTextDocument.
//
// Usage: rune [--auto-test]
//   --auto-test  type, format and save a document headlessly, report
//                PASS/FAIL, exit (0 = pass)

#include "AutoTest.h"
#include "AiManager.h"

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

namespace {

// The Omarchy palette in use, if this is Omarchy (where it is now, then where
// older Omarchy kept it). Empty elsewhere.
QVariantMap omarchyColors()
{
    static const QRegularExpression re(R"(^\s*(\w+)\s*=\s*"(#[0-9A-Fa-f]{6,8})\")");
    for (const char *dir : {"/.local/state/omarchy/current/theme", "/.config/omarchy/current/theme"}) {
        QFile f(QDir::homePath() + QLatin1String(dir) + QStringLiteral("/colors.toml"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QVariantMap m;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const auto match = re.match(in.readLine());
            if (match.hasMatch())
                m.insert(match.captured(1), match.captured(2));
        }
        return m;
    }
    return {};
}

// Off Omarchy: whether the desktop asks for light or dark, from the
// freedesktop Settings portal (GNOME, KDE and most others), then from Qt.
bool desktopPrefersLight()
{
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("ReadOne"));
    call << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("color-scheme");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 500);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        // 0 no preference, 1 dark, 2 light.
        const uint scheme = qvariant_cast<QDBusVariant>(reply.arguments().constFirst()).variant().toUInt();
        if (scheme != 0)
            return scheme == 2;
    }
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light;
}

// The active Omarchy palette, else a neutral dark (or light) one.
QVariantMap theme()
{
    const QVariantMap dark{
        {"background", "#1a1b26"}, {"foreground", "#c0caf5"},
        {"accent", "#7aa2f7"},     {"muted", "#414868"},
        {"lighter_background", "#24283b"}, {"red", "#f7768e"},
        {"green", "#9ece6a"},      {"yellow", "#e0af68"},
    };
    const QVariantMap colors = omarchyColors();
    QVariantMap m = colors.isEmpty() && desktopPrefersLight()
        ? QVariantMap{
              {"background", "#e1e2e7"}, {"foreground", "#3760bf"},
              {"accent", "#2e7de9"},     {"muted", "#a8aecb"},
              {"lighter_background", "#d0d5e3"}, {"red", "#f52a65"},
              {"green", "#587539"},      {"yellow", "#8c6c3e"},
          }
        : dark;
    for (auto it = colors.cbegin(); it != colors.cend(); ++it)
        m.insert(it.key(), it.value());
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
    parser.addVersionOption();
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

// Rune 0.1 set no organization name, so its settings (AI sign-in state,
// the chosen model) went to ~/.config/Unknown Organization/Rune.conf.
// Copied, not moved, the first time the current file doesn't exist yet.
void migrateSettings()
{
    const QString current = QSettings().fileName();
    const QString old = QSettings(QStringLiteral("Unknown Organization"), QStringLiteral("Rune")).fileName();
    if (QFileInfo::exists(current) || !QFileInfo::exists(old))
        return;
    QDir().mkpath(QFileInfo(current).absolutePath());
    QFile::copy(old, current);
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Rune"));
    // QML's Settings refuses to work without these (Qt 6.8-6.10 at least).
    // Settings live in ~/.config/Rune/Rune.conf; see migrateSettings().
    QGuiApplication::setOrganizationName(QStringLiteral("Rune"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("ninepointlabs.com"));
    QGuiApplication::setApplicationVersion(QStringLiteral(RUNE_VERSION));
    // Matches packaging/rune.desktop, so the window gets its icon and name.
    QGuiApplication::setDesktopFileName(QStringLiteral("rune"));
    // For desktops that take the window's own icon (X11), also when Rune runs
    // from an AppImage and its icon isn't installed anywhere.
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/rune/rune.png")));
    const Options options = parseOptions(app);

    // A test run keeps its settings (e.g. File → Autosave) out of the
    // user's. QSettings() and QML's Settings use NativeFormat.
    QTemporaryDir testSettings;
    if (options.autoTest) {
        AiManager::useTestDefaults();
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, testSettings.path());
    } else {
        migrateSettings();
    }

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"theme", theme()}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Rune", "Main");

    auto *window = engine.rootObjects().isEmpty()
        ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());

    // Same path as File → Open, so the status bar reports progress and errors.
    if (!options.documentPath.isEmpty() && window)
        QMetaObject::invokeMethod(window, "openPath",
                                  Q_ARG(QVariant, QFileInfo(options.documentPath).absoluteFilePath()));

    if (options.autoTest) {
        if (!window)
            return 1;
        // Let the window expose and the scene settle first.
        QTimer::singleShot(300, window, [window] { QCoreApplication::exit(runAutoTest(window) ? 0 : 2); });
    }
    return app.exec();
}
