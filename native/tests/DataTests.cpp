// The document's registries: parts (DocumentParts) and what editing each
// kind allows, inline object kinds (InlineObjects), and data stores
// (DocumentData) saved in .odt and .docx files and read back.

#include "TestSupport.h"

#include "DocumentData.h"
#include "FormatIds.h"
#include "InlineObjects.h"
#include "Package.h"

#include <QJsonArray>
#include <QJsonObject>

namespace TestSupport {

namespace {

const QString kDataOdtPath = QStringLiteral("/tmp/rune-native-data.odt");
const QString kDataBadOdtPath = QStringLiteral("/tmp/rune-native-data-bad.odt");
const QString kDataDocxPath = QStringLiteral("/tmp/rune-native-data.docx");
const QString kDataTextDir = QStringLiteral("/tmp/rune-native-data-txt");
const QString kTextNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0");

QByteArray entryOf(const QString &path, const QString &name)
{
    const std::optional<QList<Package::Entry>> entries = Package::readFile(path);
    const Package::Entry *entry = entries ? Package::find(*entries, name) : nullptr;
    return entry ? entry->data : QByteArray();
}

} // namespace

void testRegistries(Env &env)
{
    Checker &t = env.t;

    // Parts: the edit target is a part id, and its kind decides what acts.
    {
        DocumentController parts;
        bool partsClean = true;
        for (const QString &part : {QString(), QStringLiteral("header"), QStringLiteral("footer")})
            partsClean = partsClean && !parts.partDocument(part)->isModified()
                && !parts.partDocument(part)->isUndoAvailable();
        t.check(partsClean && !parts.isDirty(), "a new controller is clean, every part unmodified with no history");
        QTextDocument *body = parts.textDocument();
        t.check(parts.partDocument(QString()) == body && parts.partDocument(QStringLiteral("header"))
                    && parts.partDocument(QStringLiteral("footer"))
                    && parts.partDocument(QStringLiteral("header")) != parts.partDocument(QStringLiteral("footer"))
                    && !parts.partDocument(QStringLiteral("footnote:1")),
                "partDocument(): \"\" is the body, the header and footer have their own, an unknown part none");

        parts.setEditTarget(QStringLiteral("footnote:1"));
        t.check(parts.typeText(QStringLiteral("a")) && body->toPlainText() == QStringLiteral("a"),
                "setEditTarget(unknown part) changes nothing: typing still reaches the body");

        parts.setEditTarget(QStringLiteral("header"));
        QTextDocument *header = parts.partDocument(QStringLiteral("header"));
        QTextCursor(header).insertText(QStringLiteral("Head"));
        parts.setSelection(2, 2, 2);
        parts.toggleBulletList();
        parts.insertTable(2, 2);
        const bool typed = parts.typeText(QStringLiteral("x"));
        const bool streamed = parts.beginStreamedEdit();
        bool hasTable = false;
        for (auto it = header->rootFrame()->begin(); !it.atEnd(); ++it)
            hasTable = hasTable || it.currentFrame();
        t.check(!header->begin().textList() && !hasTable && !typed && !streamed
                    && header->toPlainText() == QStringLiteral("Head") && body->toPlainText() == QStringLiteral("a"),
                "in the header (kind PageMargin): lists, tables, typeText() and streamed edits do nothing");
        parts.toggleBold();
        t.check(parts.isBold(), "... while character formatting acts on it");

        parts.setEditTarget(QString());
        parts.setSelection(1, 1, 1);
        t.check(parts.typeText(QStringLiteral("b")) && body->toPlainText() == QStringLiteral("ab"),
                "setEditTarget(\"\") is the body again");
    }

    // Inline objects: one registry for object types, drawing and ODF.
    {
        const QList<InlineObjects::Kind> &kinds = InlineObjects::kinds();
        QSet<int> types;
        bool inRange = true;
        for (const InlineObjects::Kind &kind : kinds) {
            types.insert(kind.objectType);
            inRange = inRange && kind.objectType >= QTextFormat::UserObject;
        }
        t.check(!kinds.isEmpty() && types.size() == kinds.size() && inRange,
                QStringLiteral("%1 inline object kind(s), each its own object type above QTextFormat::UserObject")
                    .arg(kinds.size()));

        const InlineObjects::Kind *field = InlineObjects::forOdfElement(kTextNs, QStringLiteral("page-number"),
                                                                        DocumentParts::Kind::PageMargin);
        t.check(field && field->objectType == FormatIds::kPageFieldObject
                    && !InlineObjects::forOdfElement(kTextNs, QStringLiteral("page-number"), DocumentParts::Kind::Body),
                "<text:page-number> is a page field in a header or footer, and not an inline object in the body");
        const QTextCharFormat fieldFormat = field->fromOdf(QStringLiteral("page-count"), QTextCharFormat());
        t.check(InlineObjects::forFormat(fieldFormat) == field && !InlineObjects::forFormat(QTextCharFormat())
                    && field->toOdf(fieldFormat).startsWith(QStringLiteral("<text:page-count")),
                "a field read from <text:page-count> is found by its format and writes <text:page-count> back");
    }
}

void testDocumentData(Env &env)
{
    Checker &t = env.t;

    const QJsonArray characters{QJsonObject{{QStringLiteral("name"), QStringLiteral("Mara Quill")},
                                            {QStringLiteral("role"), QStringLiteral("narrator")}},
                                QJsonObject{{QStringLiteral("name"), QStringLiteral("Ödön <&> \"Vale\"")}}};
    const QJsonObject settings{{QStringLiteral("style"), QStringLiteral("apa")}};

    DocumentController writer;
    DocumentData *data = writer.data();
    t.check(!data->setValue(QStringLiteral("Notes"), characters) && !data->setValue(QStringLiteral("a/b"), characters)
                && !data->setValue(QStringLiteral("notes"), QJsonValue(3)) && data->names().isEmpty() && !writer.isDirty(),
            "data stores: invalid names and non-object/array values are refused, changing nothing");
    t.check(data->setValue(QStringLiteral("notes"), characters) && data->setValue(QStringLiteral("cite"), settings)
                && data->names() == QStringList({QStringLiteral("cite"), QStringLiteral("notes")}) && writer.isDirty(),
            "setting two stores marks the document modified");
    writer.typeText(QStringLiteral("Chapter one"));

    // .odt: package entries listed in the manifest.
    t.check(writer.saveToOdf(kDataOdtPath) && !writer.isDirty(), "saved with its data stores as .odt, now clean");
    const QStringList odtEntries = zipEntries(kDataOdtPath);
    const QByteArray manifest = entryOf(kDataOdtPath, QStringLiteral("META-INF/manifest.xml"));
    t.check(odtEntries.contains(QStringLiteral("Rune/notes.json")) && odtEntries.contains(QStringLiteral("Rune/cite.json"))
                && manifest.contains("manifest:full-path=\"Rune/notes.json\"")
                && manifest.contains("manifest:full-path=\"styles.xml\""),
            "the .odt holds Rune/notes.json and Rune/cite.json, listed in the manifest with styles.xml");
    {
        DocumentController reader;
        reader.data()->setValue(QStringLiteral("stale"), QJsonArray{1});
        t.check(reader.openOdf(kDataOdtPath) && reader.data()->value(QStringLiteral("notes")) == characters
                    && reader.data()->value(QStringLiteral("cite")) == settings
                    && !reader.data()->names().contains(QStringLiteral("stale")) && !reader.isDirty(),
                "reopening the .odt gives back exactly those stores (and only those), clean");
    }
    t.check(wordsOf(sofficeToText(kDataOdtPath, kDataTextDir)) == QStringLiteral("Chapter one"),
            "LibreOffice still opens the .odt with the extra entries");

    // A store that isn't JSON is skipped; the rest of the file opens.
    {
        std::optional<QList<Package::Entry>> entries = Package::readFile(kDataOdtPath);
        bool written = false;
        if (entries) {
            entries->append({QStringLiteral("Rune/broken.json"), QByteArrayLiteral("{not json")});
            QFile out(kDataBadOdtPath);
            const QByteArray bytes = Package::write(*entries);
            written = out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
        }
        DocumentController reader;
        t.check(written && reader.openOdf(kDataBadOdtPath) && reader.data()->value(QStringLiteral("notes")) == characters
                    && !reader.data()->names().contains(QStringLiteral("broken")),
                "a data store that isn't JSON is left out; the document and the other stores open");
    }

    // .docx: a custom XML part, added to LibreOffice's output.
    data->setValue(QStringLiteral("notes"), characters);
    data->setValue(QStringLiteral("cite"), QJsonValue());
    t.check(data->names() == QStringList{QStringLiteral("notes")} && writer.isDirty(),
            "setting a store to null removes it");
    const IoRun docxSave = saveAndWait(&writer, kDataDocxPath);
    t.check(docxSave.success && !writer.isDirty(), "saved with its data stores as .docx, now clean" + errorNote(docxSave));
    QString item;
    for (const QString &name : zipEntries(kDataDocxPath))
        if (name.startsWith(QStringLiteral("customXml/item")) && !name.contains(QStringLiteral("Props"))
            && entryOf(kDataDocxPath, name).contains("urn:ninepointlabs:rune:data:1"))
            item = name;
    const QByteArray types = entryOf(kDataDocxPath, QStringLiteral("[Content_Types].xml"));
    const QByteArray rels = entryOf(kDataDocxPath, QStringLiteral("word/_rels/document.xml.rels"));
    t.check(!item.isEmpty() && rels.contains("Target=\"../" + item.toUtf8() + '"')
                && types.contains("customXmlProperties+xml"),
            "the .docx holds the stores as a custom XML part (" + item
                + "), related from the document, its properties part typed");
    {
        DocumentController reader;
        const IoRun open = openAndWait(&reader, kDataDocxPath);
        t.check(open.success && reader.data()->value(QStringLiteral("notes")) == characters
                    && reader.data()->names() == QStringList{QStringLiteral("notes")} && !reader.isDirty(),
                "reopening the .docx gives back the stores, clean" + errorNote(open));
    }
    t.check(wordsOf(sofficeToText(kDataDocxPath, kDataTextDir)) == QStringLiteral("Chapter one"),
            "LibreOffice still opens the .docx with the custom XML part");

    writer.newDocument();
    t.check(data->names().isEmpty() && !writer.isDirty(), "newDocument() empties the data stores");
}

} // namespace TestSupport
