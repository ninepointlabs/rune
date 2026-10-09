#include "DocxBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

namespace {

// Prefix of every directory convertToOdt() hands out; removeConvertedOdt()
// refuses anything else.
const QString kTempPrefix = QStringLiteral("rune-docx-");

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString profileUrl()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/rune/lo-profile");
    return QUrl::fromLocalFile(dir).toString();
}

// Runs `soffice --convert-to <filter> --outdir <outDir> <sourcePath>` to
// completion. True if it exited normally with status 0; the caller still
// has to check the output exists.
bool runSoffice(const QString &filter, const QString &outDir, const QString &sourcePath, QString *error)
{
    const QString soffice = QStandardPaths::findExecutable(QStringLiteral("soffice"));
    if (soffice.isEmpty()) {
        setError(error, QStringLiteral("LibreOffice (soffice) was not found in PATH; it is needed to convert %1")
                            .arg(QFileInfo(sourcePath).fileName()));
        return false;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
    // soffice is a script that execs oosplash, which forks soffice.bin. A
    // session of its own lets a timeout kill the whole group, not just
    // oosplash with soffice.bin orphaned and still converting.
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
    process.start(soffice, {QStringLiteral("-env:UserInstallation=") + profileUrl(), QStringLiteral("--headless"),
                            QStringLiteral("--norestore"), QStringLiteral("--convert-to"), filter,
                            QStringLiteral("--outdir"), outDir, sourcePath});
    if (!process.waitForStarted()) {
        setError(error, QStringLiteral("could not start %1: %2").arg(soffice, process.errorString()));
        return false;
    }
    if (!process.waitForFinished(DocxBridge::kTimeoutMs)) {
#ifdef Q_OS_UNIX
        ::kill(-pid_t(process.processId()), SIGKILL);
#endif
        process.kill();
        process.waitForFinished(5000);
        setError(error, QStringLiteral("LibreOffice timed out after %1 s converting %2")
                            .arg(DocxBridge::kTimeoutMs / 1000).arg(QFileInfo(sourcePath).fileName()));
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString output = QString::fromLocal8Bit(process.readAll()).trimmed();
        setError(error, QStringLiteral("LibreOffice failed converting %1 (exit %2)%3")
                            .arg(QFileInfo(sourcePath).fileName())
                            .arg(process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1)
                            .arg(output.isEmpty() ? QString() : QStringLiteral(": ") + output));
        return false;
    }
    return true;
}

// The one file with `suffix` that soffice wrote into the otherwise empty
// `dir`, or "". Looking rather than predicting the name avoids depending on
// how soffice derives it from the source's.
QString outputIn(const QString &dir, const QString &suffix)
{
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.") + suffix}, QDir::Files);
    return files.size() == 1 && files.first().size() > 0 ? files.first().absoluteFilePath() : QString();
}

} // namespace

namespace DocxBridge {

QString convertToOdt(const QString &sourcePath, QString *error)
{
    const QFileInfo source(sourcePath);
    if (!source.isFile()) {
        setError(error, QStringLiteral("%1: no such file").arg(sourcePath));
        return {};
    }

    QTemporaryDir dir(QDir::tempPath() + '/' + kTempPrefix + QStringLiteral("XXXXXX"));
    if (!dir.isValid()) {
        setError(error, QStringLiteral("cannot create a temporary directory: %1").arg(dir.errorString()));
        return {};
    }
    if (!runSoffice(QStringLiteral("odt:writer8"), dir.path(), source.absoluteFilePath(), error))
        return {}; // dir removes itself
    const QString odt = outputIn(dir.path(), QStringLiteral("odt"));
    if (odt.isEmpty()) {
        setError(error, QStringLiteral("LibreOffice reported success but wrote no .odt for %1 "
                                       "(unreadable or unsupported file?)")
                            .arg(source.fileName()));
        return {};
    }
    dir.setAutoRemove(false); // now the caller's, via removeConvertedOdt()
    return odt;
}

void removeConvertedOdt(const QString &odtPath)
{
    if (odtPath.isEmpty())
        return;
    const QFileInfo odt(odtPath);
    QDir dir = odt.absoluteDir();
    if (!dir.dirName().startsWith(kTempPrefix)
        || QFileInfo(dir.absolutePath()).absolutePath() != QFileInfo(QDir::tempPath()).absoluteFilePath())
        return;
    dir.removeRecursively();
}

bool convertOdtToDocx(const QString &odtPath, const QString &targetPath, QString *error)
{
    const QFileInfo source(odtPath);
    if (!source.isFile()) {
        setError(error, QStringLiteral("%1: no such file").arg(odtPath));
        return false;
    }

    QTemporaryDir dir(QDir::tempPath() + '/' + kTempPrefix + QStringLiteral("XXXXXX"));
    if (!dir.isValid()) {
        setError(error, QStringLiteral("cannot create a temporary directory: %1").arg(dir.errorString()));
        return false;
    }
    if (!runSoffice(QStringLiteral("docx:MS Word 2007 XML"), dir.path(), source.absoluteFilePath(), error))
        return false;
    const QString docx = outputIn(dir.path(), QStringLiteral("docx"));
    if (docx.isEmpty()) {
        setError(error, QStringLiteral("LibreOffice reported success but wrote no .docx for %1")
                            .arg(source.fileName()));
        return false;
    }

    // Copy rather than rename: the temp dir is often on another filesystem.
    QFile in(docx);
    if (!in.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot read converted file %1: %2").arg(docx, in.errorString()));
        return false;
    }
    const QByteArray bytes = in.readAll();
    QSaveFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        setError(error, QStringLiteral("cannot write %1: %2").arg(targetPath, out.errorString()));
        return false;
    }
    return true;
}

} // namespace DocxBridge
