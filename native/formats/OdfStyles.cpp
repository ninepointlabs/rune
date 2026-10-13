#include "OdfStyles.h"

#include "OdfTextWriter.h"

#include <QHash>
#include <QXmlStreamReader>

#include <quazip.h>
#include <quazipfile.h>

#include <algorithm>

namespace OdfStyles {

namespace {

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

std::optional<PageLayout> readPageLayout(const QByteArray &stylesXml)
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
    return PageLayout::fromMap(layout.toMap(), PageLayout());
}

std::optional<PageLayout> readPageLayoutFile(const QString &path)
{
    QuaZip zip(path);
    if (!zip.open(QuaZip::mdUnzip) || !zip.setCurrentFile(QStringLiteral("styles.xml")))
        return std::nullopt;
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return readPageLayout(file.readAll());
}

QByteArray write(const PageLayout &layout, const Parts &parts)
{
    const auto &[widthMm, heightMm, topMm, bottomMm, leftMm, rightMm, headerMm, footerMm] = layout;
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
        "<office:document-styles %1 office:version=\"1.2\">"
        "<office:styles>%2</office:styles>"
        "<office:automatic-styles>")
        .arg(OdfTextWriter::namespaces(), OdfTextWriter::namedStyles());
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

} // namespace OdfStyles
