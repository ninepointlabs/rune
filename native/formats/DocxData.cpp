#include "DocxData.h"

#include "Package.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace DocxData {

namespace {

const QString kRuneNs = QStringLiteral("urn:ninepointlabs:rune:data:1");
const QString kRelsNs = QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships");
const QString kOfficeDocumentRel =
    QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument");

// Where the main document part is ("word/document.xml" in practice), from
// the package relationships.
QString mainPart(const QList<Package::Entry> &entries)
{
    if (const Package::Entry *rels = Package::find(entries, QStringLiteral("_rels/.rels"))) {
        QXmlStreamReader xml(rels->data);
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("Relationship")
                && xml.attributes().value(QLatin1String("Type")) == kOfficeDocumentRel) {
                QString target = xml.attributes().value(QLatin1String("Target")).toString();
                if (target.startsWith(u'/'))
                    target.remove(0, 1);
                return target;
            }
            if (xml.name() != QLatin1String("Relationships"))
                xml.skipCurrentElement();
        }
    }
    return QStringLiteral("word/document.xml");
}

// "word/document.xml" -> "word/_rels/document.xml.rels".
QString relsFor(const QString &part)
{
    const qsizetype slash = part.lastIndexOf(u'/');
    return part.left(slash + 1) + QStringLiteral("_rels/") + part.mid(slash + 1) + QStringLiteral(".rels");
}

// Inserts `xml` before `closing` (the root's end tag) in `entry`.
bool insertBefore(Package::Entry *entry, const QByteArray &closing, const QByteArray &xml)
{
    if (!entry)
        return false;
    const qsizetype end = entry->data.lastIndexOf(closing);
    if (end < 0)
        return false;
    entry->data.insert(end, xml);
    return true;
}

} // namespace

QByteArray add(const QByteArray &docx, const DocumentData::Stores &stores)
{
    if (stores.isEmpty())
        return docx;
    std::optional<QList<Package::Entry>> entries = Package::read(docx);
    if (!entries)
        return {};

    int n = 1;
    while (Package::find(*entries, QStringLiteral("customXml/item%1.xml").arg(n)))
        ++n;
    const QString item = QStringLiteral("customXml/item%1.xml").arg(n);
    const QString props = QStringLiteral("customXml/itemProps%1.xml").arg(n);

    QByteArray data;
    {
        QXmlStreamWriter xml(&data);
        xml.writeStartDocument(QStringLiteral("1.0"), true);
        xml.writeNamespace(kRuneNs, QStringLiteral("rune"));
        xml.writeStartElement(kRuneNs, QStringLiteral("data"));
        for (auto it = stores.cbegin(); it != stores.cend(); ++it) {
            const QJsonDocument json = it.value().isArray() ? QJsonDocument(it.value().toArray())
                                                            : QJsonDocument(it.value().toObject());
            xml.writeStartElement(kRuneNs, QStringLiteral("store"));
            xml.writeAttribute(QStringLiteral("name"), it.key());
            xml.writeCharacters(QString::fromUtf8(json.toJson(QJsonDocument::Compact)));
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeEndDocument();
    }
    const QByteArray itemProps =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<ds:datastoreItem ds:itemID=\"" + QUuid::createUuid().toString(QUuid::WithBraces).toUpper().toUtf8()
        + "\" xmlns:ds=\"http://schemas.openxmlformats.org/officeDocument/2006/customXml\">"
          "<ds:schemaRefs><ds:schemaRef ds:uri=\"" + kRuneNs.toUtf8() + "\"/></ds:schemaRefs></ds:datastoreItem>";
    const QByteArray itemRels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"" + kRelsNs.toUtf8() + "\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXmlProps\""
        " Target=\"itemProps" + QByteArray::number(n) + ".xml\"/></Relationships>";

    // Related from the main document part, its properties part typed.
    const QString main = mainPart(*entries);
    const QByteArray relation = "<Relationship Id=\"rIdRuneData\" Type=\"http://schemas.openxmlformats.org/"
                                "officeDocument/2006/relationships/customXml\" Target=\"../"
                                + item.toUtf8() + "\"/>";
    if (!insertBefore(Package::find(*entries, relsFor(main)), "</Relationships>", relation))
        return {};
    const QByteArray types = "<Override PartName=\"/" + props.toUtf8()
                             + "\" ContentType=\"application/vnd.openxmlformats-officedocument.customXmlProperties+xml\"/>"
                             + "<Override PartName=\"/" + item.toUtf8() + "\" ContentType=\"application/xml\"/>";
    if (!insertBefore(Package::find(*entries, QStringLiteral("[Content_Types].xml")), "</Types>", types))
        return {};

    entries->append({item, data});
    entries->append({props, itemProps});
    entries->append({QStringLiteral("customXml/_rels/item%1.xml.rels").arg(n), itemRels});
    return Package::write(*entries);
}

DocumentData::Stores read(const QString &path)
{
    DocumentData::Stores stores;
    const std::optional<QList<Package::Entry>> entries = Package::readFile(path);
    if (!entries)
        return stores;
    static const QRegularExpression itemName(QStringLiteral("^customXml/item\\d+\\.xml$"));
    for (const Package::Entry &entry : *entries) {
        if (!itemName.match(entry.name).hasMatch())
            continue;
        QXmlStreamReader xml(entry.data);
        if (!xml.readNextStartElement() || xml.namespaceUri() != kRuneNs || xml.name() != QLatin1String("data"))
            continue;
        while (xml.readNextStartElement()) {
            if (xml.namespaceUri() != kRuneNs || xml.name() != QLatin1String("store")) {
                xml.skipCurrentElement();
                continue;
            }
            const QString name = xml.attributes().value(QLatin1String("name")).toString();
            QJsonParseError parseError;
            const QJsonDocument json = QJsonDocument::fromJson(xml.readElementText().toUtf8(), &parseError);
            if (!DocumentData::isValidName(name) || json.isNull()) {
                qWarning().noquote() << "DocxData: skipping data store" << name << "in" << path << "-"
                                     << (json.isNull() ? parseError.errorString() : QStringLiteral("invalid name"));
                continue;
            }
            stores.insert(name, json.isArray() ? QJsonValue(json.array()) : QJsonValue(json.object()));
        }
    }
    return stores;
}

} // namespace DocxData
