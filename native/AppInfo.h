#pragma once

// AppInfo: what File → About Rune shows (version, license, support
// address, the toolkit versions), as a QML singleton:
//
//   import Rune
//   Text { text: "Version " + AppInfo.version }
//
// The license text is the repo's LICENSE, compiled in as a resource
// (native/CMakeLists.txt), so it can't drift from the real one.

#include <QObject>
#include <QQmlEngine>
#include <QString>

class AppInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString tagline READ tagline CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString organization READ organization CONSTANT)
    Q_PROPERTY(QString created READ created CONSTANT)
    Q_PROPERTY(QString license READ license CONSTANT)
    Q_PROPERTY(QString copyright READ copyright CONSTANT)
    Q_PROPERTY(QString supportEmail READ supportEmail CONSTANT)
    Q_PROPERTY(QString licenseText READ licenseText CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    // "LibreOffice 26.8.0.3" (soffice --version, for .docx/.doc), "" until
    // checkLibreOffice() has run, "not installed" without soffice in PATH.
    Q_PROPERTY(QString libreOfficeVersion READ libreOfficeVersion NOTIFY libreOfficeVersionChanged)
    // Version and system details to paste into a support email.
    Q_PROPERTY(QString details READ details NOTIFY libreOfficeVersionChanged)

public:
    explicit AppInfo(QObject *parent = nullptr);

    QString name() const { return QStringLiteral("Rune"); }
    QString tagline() const { return QStringLiteral("A word processor that writes with you."); }
    QString version() const;
    QString organization() const { return QStringLiteral("Nine Point Labs"); }
    QString created() const { return QStringLiteral("October 2026"); } // first commit: 2026-10-08
    QString license() const { return QStringLiteral("MIT License"); }
    QString copyright() const { return QStringLiteral("© 2026 Nine Point Labs"); }
    QString supportEmail() const { return QStringLiteral("help@ninepointlabs.com"); }
    QString licenseText() const;
    QString qtVersion() const;
    QString libreOfficeVersion() const { return m_libreOfficeVersion; }
    QString details() const;

    // Runs `soffice --version` in the background, once; the About dialog
    // calls it when it opens (LibreOffice is slow to start).
    Q_INVOKABLE void checkLibreOffice();
    Q_INVOKABLE void copy(const QString &text) const;

signals:
    void libreOfficeVersionChanged();

private:
    void setLibreOfficeVersion(const QString &version);

    QString m_libreOfficeVersion;
    bool m_checking = false;
};
