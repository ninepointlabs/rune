#include "PageLayout.h"

#include <QBuffer>
#include <QHash>
#include <QXmlStreamReader>

#include <quazip.h>
#include <quazipfile.h>
#include <quazipnewinfo.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr qreal kPxPerMm = 96.0 / 25.4;
// Page setup never leaves less text area than this, either way.
constexpr qreal kMinTextMm = 20.0;

const QString kOfficeNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
const QString kStyleNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
const QString kFoNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");

// "21cm", "8.5in", "297mm", "612pt", ... in millimetres; nullopt for
// anything else.
std::optional<qreal> lengthInMm(QStringView value)
{
    static const struct { QLatin1StringView unit; qreal mm; } units[] = {
        {QLatin1String("mm"), 1.0}, {QLatin1String("cm"), 10.0}, {QLatin1String("in"), 25.4},
        {QLatin1String("pt"), 25.4 / 72.0}, {QLatin1String("pc"), 25.4 / 6.0}, {QLatin1String("px"), 25.4 / 96.0}};
    for (const auto &unit : units) {
        if (!value.endsWith(unit.unit))
            continue;
        bool ok = false;
        const qreal number = value.chopped(unit.unit.size()).toDouble(&ok);
        if (ok && number >= 0)
            return number * unit.mm;
        return std::nullopt;
    }
    return std::nullopt;
}

QString mm(qreal value)
{
    return QString::number(value, 'f', 2) + QStringLiteral("mm");
}

} // namespace

bool PageLayout::usesLetter(QLocale::Territory territory)
{
    switch (territory) {
    case QLocale::UnitedStates:
    case QLocale::Canada:
    case QLocale::Mexico:
    case QLocale::Philippines:
    case QLocale::Chile:
    case QLocale::Colombia:
    case QLocale::Venezuela:
    case QLocale::CostaRica:
    case QLocale::Guatemala:
    case QLocale::Panama:
    case QLocale::PuertoRico:
    case QLocale::DominicanRepublic:
        return true;
    default:
        return false;
    }
}

PageLayout PageLayout::defaultFor(QLocale::Territory territory)
{
    PageLayout layout; // Letter
    if (!usesLetter(territory)) {
        layout.widthMm = 210.0;
        layout.heightMm = 297.0;
    }
    return layout;
}

QVariantMap PageLayout::toMap() const
{
    return {{QStringLiteral("width"), widthMm}, {QStringLiteral("height"), heightMm},
            {QStringLiteral("top"), topMm}, {QStringLiteral("bottom"), bottomMm},
            {QStringLiteral("left"), leftMm}, {QStringLiteral("right"), rightMm}};
}

PageLayout PageLayout::fromMap(const QVariantMap &map, const PageLayout &fallback)
{
    const auto value = [&](const char *key, qreal current) {
        bool ok = false;
        const qreal v = map.value(QLatin1String(key)).toDouble(&ok);
        return ok && std::isfinite(v) ? v : current;
    };
    PageLayout layout;
    layout.widthMm = value("width", fallback.widthMm);
    layout.heightMm = value("height", fallback.heightMm);
    if (layout.widthMm <= kMinTextMm)
        layout.widthMm = fallback.widthMm;
    if (layout.heightMm <= kMinTextMm)
        layout.heightMm = fallback.heightMm;
    layout.topMm = std::max<qreal>(0, value("top", fallback.topMm));
    layout.bottomMm = std::max<qreal>(0, value("bottom", fallback.bottomMm));
    layout.leftMm = std::max<qreal>(0, value("left", fallback.leftMm));
    layout.rightMm = std::max<qreal>(0, value("right", fallback.rightMm));
    // Margins that would leave too little text area shrink, each side equally.
    const auto fit = [](qreal size, qreal &a, qreal &b) {
        const qreal excess = a + b - (size - kMinTextMm);
        if (excess > 0) {
            a = std::max<qreal>(0, a - excess / 2);
            b = std::max<qreal>(0, size - kMinTextMm - a);
        }
    };
    fit(layout.widthMm, layout.leftMm, layout.rightMm);
    fit(layout.heightMm, layout.topMm, layout.bottomMm);
    return layout;
}

QSizeF PageLayout::sizePx() const
{
    return {widthMm * kPxPerMm, heightMm * kPxPerMm};
}

QMarginsF PageLayout::marginsPx() const
{
    return {leftMm * kPxPerMm, topMm * kPxPerMm, rightMm * kPxPerMm, bottomMm * kPxPerMm};
}

bool PageLayout::operator==(const PageLayout &other) const
{
    const auto same = [](qreal a, qreal b) { return std::abs(a - b) < 0.01; };
    return same(widthMm, other.widthMm) && same(heightMm, other.heightMm) && same(topMm, other.topMm)
        && same(bottomMm, other.bottomMm) && same(leftMm, other.leftMm) && same(rightMm, other.rightMm);
}

std::optional<PageLayout> PageLayout::fromOdfStyles(const QByteArray &stylesXml)
{
    if (stylesXml.isEmpty())
        return std::nullopt;
    // Every page layout by name, and which one each master page uses.
    QHash<QString, PageLayout> layouts;
    QString standardLayout, firstLayout;
    QString currentLayout;
    QXmlStreamReader xml(stylesXml);
    while (!xml.atEnd()) {
        if (!xml.readNextStartElement())
            continue;
        const QXmlStreamAttributes attrs = xml.attributes();
        if (xml.namespaceUri() == kStyleNs && xml.name() == QLatin1String("page-layout")) {
            currentLayout = attrs.value(kStyleNs, QLatin1String("name")).toString();
        } else if (xml.namespaceUri() == kStyleNs && xml.name() == QLatin1String("page-layout-properties")) {
            PageLayout layout;
            const auto read = [&](const char *name, qreal &field) {
                if (const auto v = lengthInMm(attrs.value(kFoNs, QLatin1String(name))))
                    field = *v;
            };
            // fo:margin sets all four; the sides override it.
            qreal all = -1;
            read("margin", all);
            if (all >= 0)
                layout.topMm = layout.bottomMm = layout.leftMm = layout.rightMm = all;
            read("page-width", layout.widthMm);
            read("page-height", layout.heightMm);
            read("margin-top", layout.topMm);
            read("margin-bottom", layout.bottomMm);
            read("margin-left", layout.leftMm);
            read("margin-right", layout.rightMm);
            layouts.insert(currentLayout, layout);
        } else if (xml.namespaceUri() == kStyleNs && xml.name() == QLatin1String("master-page")) {
            const QString used = attrs.value(kStyleNs, QLatin1String("page-layout-name")).toString();
            if (firstLayout.isEmpty())
                firstLayout = used;
            if (attrs.value(kStyleNs, QLatin1String("name")) == QLatin1String("Standard"))
                standardLayout = used;
        }
    }
    if (xml.hasError())
        return std::nullopt;
    const QString chosen = !standardLayout.isEmpty() ? standardLayout : firstLayout;
    if (!layouts.contains(chosen))
        return std::nullopt;
    return fromMap(layouts.value(chosen).toMap(), PageLayout());
}

std::optional<PageLayout> PageLayout::fromOdtFile(const QString &path)
{
    QuaZip zip(path);
    if (!zip.open(QuaZip::mdUnzip) || !zip.setCurrentFile(QStringLiteral("styles.xml")))
        return std::nullopt;
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return fromOdfStyles(file.readAll());
}

QByteArray PageLayout::odfStyles() const
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<office:document-styles xmlns:office=\"%1\" xmlns:style=\"%2\" xmlns:fo=\"%3\" office:version=\"1.2\">"
               "<office:automatic-styles>"
               "<style:page-layout style:name=\"pm1\">"
               "<style:page-layout-properties fo:page-width=\"%4\" fo:page-height=\"%5\" "
               "style:print-orientation=\"%6\" "
               "fo:margin-top=\"%7\" fo:margin-bottom=\"%8\" fo:margin-left=\"%9\" fo:margin-right=\"%10\"/>"
               "</style:page-layout>"
               "</office:automatic-styles>"
               "<office:master-styles>"
               "<style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\"/>"
               "</office:master-styles>"
               "</office:document-styles>\n")
        .arg(kOfficeNs, kStyleNs, kFoNs, mm(widthMm), mm(heightMm),
             widthMm > heightMm ? QStringLiteral("landscape") : QStringLiteral("portrait"),
             mm(topMm), mm(bottomMm), mm(leftMm))
        .arg(mm(rightMm))
        .toUtf8();
}

QByteArray withOdfPageLayout(const QByteArray &odt, const PageLayout &layout)
{
    // Every member but styles.xml, in order.
    QList<QPair<QString, QByteArray>> members;
    {
        QBuffer in;
        in.setData(odt);
        QuaZip zip(&in);
        if (!zip.open(QuaZip::mdUnzip))
            return {};
        for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
            const QString name = zip.getCurrentFileName();
            QuaZipFile file(&zip);
            if (!file.open(QIODevice::ReadOnly))
                return {};
            const QByteArray data = file.readAll();
            file.close();
            if (name != QLatin1String("styles.xml"))
                members.append({name, data});
        }
        zip.close();
    }

    // The manifest lists every member; add styles.xml to it.
    for (auto &member : members) {
        if (member.first != QLatin1String("META-INF/manifest.xml") || member.second.contains("\"styles.xml\""))
            continue;
        const qsizetype end = member.second.lastIndexOf("</manifest:manifest>");
        if (end < 0)
            return {};
        member.second.insert(end, "<manifest:file-entry manifest:media-type=\"text/xml\" manifest:full-path=\"styles.xml\"/>");
    }
    members.append({QStringLiteral("styles.xml"), layout.odfStyles()});

    QBuffer out; // QuaZip opens it
    QuaZip zip(&out);
    if (!zip.open(QuaZip::mdCreate))
        return {};
    for (const auto &[name, data] : members) {
        QuaZipFile file(&zip);
        // ODF requires the mimetype member first and stored uncompressed;
        // Qt's writer already puts it first.
        const bool stored = name == QLatin1String("mimetype");
        if (!file.open(QIODevice::WriteOnly, QuaZipNewInfo(name), nullptr, 0, stored ? 0 : Z_DEFLATED,
                       stored ? 0 : Z_DEFAULT_COMPRESSION)
            || file.write(data) != data.size())
            return {};
        file.close();
        if (file.getZipError() != ZIP_OK)
            return {};
    }
    zip.close();
    if (zip.getZipError() != ZIP_OK)
        return {};
    return out.data();
}
