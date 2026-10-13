#include "PageLayout.h"

#include <QBuffer>
#include <QHash>
#include <QTextDocument>
#include <QTextFrame>
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
            {QStringLiteral("left"), leftMm}, {QStringLiteral("right"), rightMm},
            {QStringLiteral("header"), headerMm}, {QStringLiteral("footer"), footerMm}};
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
    layout.headerMm = std::clamp<qreal>(value("header", fallback.headerMm), 0, layout.topMm);
    layout.footerMm = std::clamp<qreal>(value("footer", fallback.footerMm), 0, layout.bottomMm);
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

namespace {

enum PageProperty {
    kPageWidthMm = QTextFormat::UserProperty + 100,
    kPageHeightMm,
    kPageTopMm,
    kPageBottomMm,
    kPageLeftMm,
    kPageRightMm,
    kPageHeaderMm,
    kPageFooterMm,
};

} // namespace

std::optional<PageLayout> PageLayout::fromDocument(const QTextDocument *doc)
{
    const QTextFrameFormat format = doc->rootFrame()->frameFormat();
    if (!format.hasProperty(kPageWidthMm))
        return std::nullopt;
    PageLayout layout;
    layout.widthMm = format.doubleProperty(kPageWidthMm);
    layout.heightMm = format.doubleProperty(kPageHeightMm);
    layout.topMm = format.doubleProperty(kPageTopMm);
    layout.bottomMm = format.doubleProperty(kPageBottomMm);
    layout.leftMm = format.doubleProperty(kPageLeftMm);
    layout.rightMm = format.doubleProperty(kPageRightMm);
    layout.headerMm = format.doubleProperty(kPageHeaderMm);
    layout.footerMm = format.doubleProperty(kPageFooterMm);
    return layout;
}

void PageLayout::storeIn(QTextDocument *doc) const
{
    QTextFrameFormat format = doc->rootFrame()->frameFormat();
    const QMarginsF margins = marginsPx();
    format.setTopMargin(margins.top());
    format.setBottomMargin(margins.bottom());
    format.setLeftMargin(margins.left());
    format.setRightMargin(margins.right());
    format.setProperty(kPageWidthMm, widthMm);
    format.setProperty(kPageHeightMm, heightMm);
    format.setProperty(kPageTopMm, topMm);
    format.setProperty(kPageBottomMm, bottomMm);
    format.setProperty(kPageLeftMm, leftMm);
    format.setProperty(kPageRightMm, rightMm);
    format.setProperty(kPageHeaderMm, headerMm);
    format.setProperty(kPageFooterMm, footerMm);
    doc->rootFrame()->setFrameFormat(format);
}

bool PageLayout::operator==(const PageLayout &other) const
{
    const auto same = [](qreal a, qreal b) { return std::abs(a - b) < 0.01; };
    return same(widthMm, other.widthMm) && same(heightMm, other.heightMm) && same(topMm, other.topMm)
        && same(bottomMm, other.bottomMm) && same(leftMm, other.leftMm) && same(rightMm, other.rightMm)
        && same(headerMm, other.headerMm) && same(footerMm, other.footerMm);
}

std::optional<PageLayout> PageLayout::fromOdfStyles(const QByteArray &stylesXml)
{
    if (stylesXml.isEmpty())
        return std::nullopt;
    // A <style:page-layout>: the page, and the height its header and footer
    // styles give them (spacing included), if they give one.
    struct OdfPageLayout
    {
        PageLayout page;
        std::optional<qreal> headerHeight, footerHeight;
    };
    // A master page: its layout, and whether it shows a header / footer.
    struct Master
    {
        QString layout;
        bool header = false, footer = false;
    };
    QHash<QString, OdfPageLayout> layouts;
    QHash<QString, Master> masters;
    QString firstMaster;
    QString currentLayout, currentMaster;
    enum { Page, HeaderStyle, FooterStyle } part = Page;
    QXmlStreamReader xml(stylesXml);
    while (!xml.atEnd()) {
        if (!xml.readNextStartElement())
            continue;
        const QXmlStreamAttributes attrs = xml.attributes();
        const auto length = [&](const QString &ns, const char *name) {
            return lengthInMm(attrs.value(ns, QLatin1String(name)));
        };
        if (xml.namespaceUri() != kStyleNs)
            continue;
        if (xml.name() == QLatin1String("page-layout")) {
            currentLayout = attrs.value(kStyleNs, QLatin1String("name")).toString();
            layouts.insert(currentLayout, {});
            part = Page;
        } else if (xml.name() == QLatin1String("header-style")) {
            part = HeaderStyle;
        } else if (xml.name() == QLatin1String("footer-style")) {
            part = FooterStyle;
        } else if (xml.name() == QLatin1String("page-layout-properties")) {
            PageLayout &layout = layouts[currentLayout].page;
            const auto read = [&](const char *name, qreal &field) {
                if (const auto v = length(kFoNs, name))
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
        } else if (xml.name() == QLatin1String("header-footer-properties") && part != Page) {
            // A fixed svg:height, else fo:min-height, which LibreOffice
            // takes to include the spacing to the body; at least the spacing.
            const std::optional<qreal> spacing =
                length(kFoNs, part == HeaderStyle ? "margin-bottom" : "margin-top");
            std::optional<qreal> height = length(QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0"), "height");
            if (!height)
                height = length(kFoNs, "min-height");
            const qreal value = std::max(height.value_or(0), spacing.value_or(0));
            (part == HeaderStyle ? layouts[currentLayout].headerHeight : layouts[currentLayout].footerHeight) = value;
        } else if (xml.name() == QLatin1String("master-page")) {
            currentMaster = attrs.value(kStyleNs, QLatin1String("name")).toString();
            if (firstMaster.isEmpty())
                firstMaster = currentMaster;
            masters[currentMaster].layout = attrs.value(kStyleNs, QLatin1String("page-layout-name")).toString();
        } else if ((xml.name() == QLatin1String("header") || xml.name() == QLatin1String("footer"))
                   && !currentMaster.isEmpty() && attrs.value(kStyleNs, QLatin1String("display")) != QLatin1String("false")) {
            (xml.name() == QLatin1String("header") ? masters[currentMaster].header : masters[currentMaster].footer) = true;
        }
    }
    if (xml.hasError())
        return std::nullopt;
    const Master master = masters.value(masters.contains(QStringLiteral("Standard")) ? QStringLiteral("Standard") : firstMaster);
    if (!layouts.contains(master.layout))
        return std::nullopt;
    const OdfPageLayout found = layouts.value(master.layout);
    PageLayout layout = found.page;
    // With a header, the page margin is the header's distance and the body
    // starts below the header's height.
    if (master.header) {
        layout.headerMm = layout.topMm;
        layout.topMm += found.headerHeight.value_or(0);
    }
    if (master.footer) {
        layout.footerMm = layout.bottomMm;
        layout.bottomMm += found.footerHeight.value_or(0);
    }
    return fromMap(layout.toMap(), PageLayout());
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

QByteArray PageLayout::odfStyles(const OdfParts &parts) const
{
    const bool header = !parts.header.isEmpty();
    const bool footer = !parts.footer.isEmpty();
    // See the header: with a header, the page margin is its distance and
    // its height reaches to the body. LibreOffice grows a header taller
    // than that and moves the body down, as Rune does (fitPageParts()).
    const auto partStyle = [](const char *element, const char *spacing, qreal heightMm) {
        return QStringLiteral("<style:%1><style:header-footer-properties fo:min-height=\"%2\" fo:%3=\"0mm\" "
                              "fo:margin-left=\"0mm\" fo:margin-right=\"0mm\" style:dynamic-spacing=\"false\"/></style:%1>")
            .arg(QLatin1String(element), mm(std::max<qreal>(0, heightMm)), QLatin1String(spacing));
    };
    QString xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-styles xmlns:office=\"%1\" xmlns:style=\"%2\" xmlns:fo=\"%3\" xmlns:text=\"%4\" office:version=\"1.2\">"
        "<office:automatic-styles>")
        .arg(kOfficeNs, kStyleNs, kFoNs, QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0"));
    xml += QStringLiteral("<style:page-layout style:name=\"pm1\">"
                          "<style:page-layout-properties fo:page-width=\"%1\" fo:page-height=\"%2\" "
                          "style:print-orientation=\"%3\" "
                          "fo:margin-top=\"%4\" fo:margin-bottom=\"%5\" fo:margin-left=\"%6\" fo:margin-right=\"%7\"/>")
               .arg(mm(widthMm), mm(heightMm),
                    widthMm > heightMm ? QStringLiteral("landscape") : QStringLiteral("portrait"),
                    mm(header ? headerMm : topMm), mm(footer ? footerMm : bottomMm), mm(leftMm), mm(rightMm));
    if (header)
        xml += partStyle("header-style", "margin-bottom", topMm - headerMm);
    if (footer)
        xml += partStyle("footer-style", "margin-top", bottomMm - footerMm);
    xml += QStringLiteral("</style:page-layout>");
    xml += parts.automaticStyles;
    xml += QStringLiteral("</office:automatic-styles>"
                          "<office:master-styles>"
                          "<style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\">");
    if (header)
        xml += QStringLiteral("<style:header>") + parts.header + QStringLiteral("</style:header>");
    if (footer)
        xml += QStringLiteral("<style:footer>") + parts.footer + QStringLiteral("</style:footer>");
    xml += QStringLiteral("</style:master-page>"
                          "</office:master-styles>"
                          "</office:document-styles>\n");
    return xml.toUtf8();
}

QByteArray withOdfStyles(const QByteArray &odt, const QByteArray &stylesXml)
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
    members.append({QStringLiteral("styles.xml"), stylesXml});

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
