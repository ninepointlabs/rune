#include "AppInfo.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtGlobal>

AppInfo::AppInfo(QObject *parent) : QObject(parent) {}

QString AppInfo::version() const
{
    return QStringLiteral(RUNE_VERSION);
}

QString AppInfo::licenseText() const
{
    QFile f(QStringLiteral(":/rune/LICENSE"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return license() + QStringLiteral(", ") + copyright();
    // One line per paragraph, so it wraps to the dialog's width.
    const QStringList paragraphs = QString::fromUtf8(f.readAll()).split(QStringLiteral("\n\n"), Qt::SkipEmptyParts);
    QStringList out;
    for (const QString &p : paragraphs) {
        const QString joined = p.simplified();
        if (!joined.isEmpty())
            out.append(joined);
    }
    return out.join(QStringLiteral("\n\n"));
}

QString AppInfo::qtVersion() const
{
    return QString::fromLatin1(qVersion());
}

QString AppInfo::details() const
{
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP", QStringLiteral("unknown desktop"));
    return QStringLiteral("%1 %2\nQt %3, LibreOffice: %4\n%5, %6")
        .arg(name(), version(), qtVersion(),
             m_libreOfficeVersion.isEmpty() ? QStringLiteral("not checked") : m_libreOfficeVersion,
             desktop, QGuiApplication::platformName());
}

void AppInfo::checkLibreOffice()
{
    if (m_checking || !m_libreOfficeVersion.isEmpty())
        return;
    const QString soffice = QStandardPaths::findExecutable(QStringLiteral("soffice"));
    if (soffice.isEmpty()) {
        setLibreOfficeVersion(QStringLiteral("not installed"));
        return;
    }
    m_checking = true;
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process] {
        // "LibreOffice 26.8.0.3 680(Build:3)" → "LibreOffice 26.8.0.3"
        static const QRegularExpression re(QStringLiteral(R"(^(.*?\d+(?:\.\d+)+))"));
        const auto match = re.match(QString::fromUtf8(process->readAllStandardOutput()).trimmed());
        m_checking = false;
        setLibreOfficeVersion(match.hasMatch() ? match.captured(1) : QStringLiteral("unknown version"));
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return; // finished() follows
        m_checking = false;
        setLibreOfficeVersion(QStringLiteral("unknown version"));
        process->deleteLater();
    });
    process->start(soffice, {QStringLiteral("--version")});
}

void AppInfo::setLibreOfficeVersion(const QString &version)
{
    if (version == m_libreOfficeVersion)
        return;
    m_libreOfficeVersion = version;
    emit libreOfficeVersionChanged();
}

void AppInfo::copy(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}
