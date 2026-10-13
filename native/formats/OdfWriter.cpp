#include "OdfWriter.h"

#include "OdfStyles.h"
#include "OdfTextWriter.h"
#include "Package.h"
#include "PageParts.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextDocument>

#ifndef RUNE_VERSION
#define RUNE_VERSION "dev"
#endif

namespace OdfWriter {

namespace {

QString mm(qreal value)
{
    return QString::number(value, 'f', 2) + QStringLiteral("mm");
}

// A header or footer's block elements, its styles added to `parts`; "" for
// none. Its paragraphs get word processors' header tab stops: centre, then
// right.
QString pageMargin(const QTextDocument *doc, const QString &prefix, qreal textWidthMm, OdfStyles::Parts &parts)
{
    if (!doc || PageParts::isEmpty(doc))
        return {};
    OdfTextWriter::Options options;
    options.stylePrefix = prefix;
    options.textWidthMm = textWidthMm;
    options.paragraphProperties = QStringLiteral("<style:tab-stops>"
                                                 "<style:tab-stop style:position=\"%1\" style:type=\"center\"/>"
                                                 "<style:tab-stop style:position=\"%2\" style:type=\"right\"/>"
                                                 "</style:tab-stops>")
                                      .arg(mm(textWidthMm / 2), mm(textWidthMm));
    OdfTextWriter writer(options);
    const QString xml = writer.write(doc);
    parts.automaticStyles += writer.automaticStyles();
    return xml;
}

} // namespace

QString dataEntryName(const QString &store)
{
    return QStringLiteral("Rune/") + store + QStringLiteral(".json");
}

QByteArray write(const Document &document)
{
    if (!document.body) {
        qWarning() << "OdfWriter: no document to write";
        return {};
    }
    const PageLayout &layout = document.layout;
    const qreal textWidthMm = layout.widthMm - layout.leftMm - layout.rightMm;

    OdfTextWriter::Options options;
    options.textWidthMm = textWidthMm;
    OdfTextWriter body(options);
    const QString text = body.write(document.body);
    const QString content = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                           "<office:document-content %1 office:version=\"1.2\">"
                                           "<office:automatic-styles>%2</office:automatic-styles>"
                                           "<office:body><office:text>%3</office:text></office:body>"
                                           "</office:document-content>\n")
                                .arg(OdfTextWriter::namespaces(), body.automaticStyles(), text);

    OdfStyles::Parts parts;
    parts.header = pageMargin(document.header, QStringLiteral("Header"), textWidthMm, parts);
    parts.footer = pageMargin(document.footer, QStringLiteral("Footer"), textWidthMm, parts);

    const QByteArray meta = QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                              "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                                              "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\">"
                                              "<office:meta><meta:generator>Rune/" RUNE_VERSION "</meta:generator></office:meta>"
                                              "</office:document-meta>\n");

    QList<Package::Entry> entries = {
        {QStringLiteral("mimetype"), QByteArrayLiteral("application/vnd.oasis.opendocument.text")},
        {QStringLiteral("content.xml"), content.toUtf8()},
        {QStringLiteral("styles.xml"), OdfStyles::write(layout, parts)},
        {QStringLiteral("meta.xml"), meta},
    };
    QByteArray manifest = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" "
        "manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>");
    const auto list = [&manifest](const QString &name, const char *mediaType) {
        manifest += "<manifest:file-entry manifest:full-path=\"" + name.toHtmlEscaped().toUtf8()
                    + "\" manifest:media-type=\"" + mediaType + "\"/>";
    };
    list(QStringLiteral("content.xml"), "text/xml");
    list(QStringLiteral("styles.xml"), "text/xml");
    list(QStringLiteral("meta.xml"), "text/xml");
    for (auto it = document.data.cbegin(); it != document.data.cend(); ++it) {
        const QJsonDocument json = it.value().isArray() ? QJsonDocument(it.value().toArray())
                                                        : QJsonDocument(it.value().toObject());
        entries.append({dataEntryName(it.key()), json.toJson(QJsonDocument::Indented)});
        list(dataEntryName(it.key()), "application/json");
    }
    manifest += "</manifest:manifest>\n";
    entries.append({QStringLiteral("META-INF/manifest.xml"), manifest});

    const QByteArray odt = Package::write(entries);
    if (odt.isEmpty())
        qWarning() << "OdfWriter: cannot write the ODF archive";
    return odt;
}

} // namespace OdfWriter
