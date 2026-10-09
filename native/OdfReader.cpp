#include "OdfReader.h"

#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QTextCursor>
#include <QTextDocument>
#include <QXmlStreamReader>

#include <quazip.h>
#include <quazipfile.h>

#include <algorithm>

namespace {

const QString kOfficeNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
const QString kTextNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
const QString kStyleNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
const QString kFoNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
const QString kDrawNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");

struct Run
{
    QString text;
    QTextCharFormat format;
};

// Read into this first and only touch the document once the whole file has
// parsed, so a failure leaves the document as it was.
struct Paragraph
{
    int headingLevel = 0; // 0: body text
    QTextCharFormat format; // paragraph-wide character format
    QList<Run> runs;
    // ODF whitespace (ODF 1.2 part 1, 6.1.2): runs of space/tab/CR/LF in the
    // XML collapse to one space and leading ones are dropped; real spaces,
    // tabs and breaks are <text:s>, <text:tab>, <text:line-break>. Qt's
    // writer relies on this: it indents inside spans before a <text:s>.
    bool afterSpace = true;

    void append(const QString &text, const QTextCharFormat &format)
    {
        if (runs.isEmpty() || runs.constLast().format != format)
            runs.append({QString(), format});
        runs.last().text += text;
    }
    void appendXmlText(QStringView text, const QTextCharFormat &format)
    {
        QString collapsed;
        for (const QChar ch : text) {
            const bool space = ch == u' ' || ch == u'\t' || ch == u'\r' || ch == u'\n';
            if (space && afterSpace)
                continue;
            collapsed += space ? QChar(u' ') : ch;
            afterSpace = space;
        }
        if (!collapsed.isEmpty())
            append(collapsed, format);
    }
    void appendLiteral(const QString &text, const QTextCharFormat &format)
    {
        append(text, format);
        afterSpace = false;
    }
};

// Automatic styles by family ("text", "paragraph") and name. Each format
// holds only the properties the style sets, so merging one onto another
// overrides just those.
using StyleMap = QHash<QString, QTextCharFormat>;

QString styleKey(QStringView family, QStringView name)
{
    return family.toString() + u'/' + name.toString();
}

bool is(const QXmlStreamReader &xml, const QString &ns, QLatin1StringView name)
{
    return xml.namespaceUri() == ns && xml.name() == name;
}

// The B/I/U part of a <style:text-properties>, attribute names as written by
// both Qt's ODF writer and LibreOffice.
void readTextProperties(const QXmlStreamAttributes &attrs, QTextCharFormat &format)
{
    const QStringView weight = attrs.value(kFoNs, QLatin1String("font-weight"));
    if (weight == u"bold") {
        format.setFontWeight(QFont::Bold);
    } else if (weight == u"normal") {
        format.setFontWeight(QFont::Normal);
    } else if (!weight.isEmpty()) {
        bool ok = false;
        const int numeric = weight.toInt(&ok); // "100".."900", same scale as QFont::Weight
        if (ok)
            format.setFontWeight(std::clamp(numeric, 1, 1000));
    }

    const QStringView style = attrs.value(kFoNs, QLatin1String("font-style"));
    if (!style.isEmpty())
        format.setFontItalic(style == u"italic" || style == u"oblique");

    const QStringView underline = attrs.value(kStyleNs, QLatin1String("text-underline-style"));
    if (!underline.isEmpty())
        format.setFontUnderline(underline != u"none");
}

// Called on <office:automatic-styles>; reads up to its end tag.
void readAutomaticStyles(QXmlStreamReader &xml, StyleMap &styles)
{
    while (xml.readNextStartElement()) {
        if (!is(xml, kStyleNs, QLatin1String("style"))) {
            xml.skipCurrentElement();
            continue;
        }
        const QXmlStreamAttributes attrs = xml.attributes();
        const QString key = styleKey(attrs.value(kStyleNs, QLatin1String("family")),
                                     attrs.value(kStyleNs, QLatin1String("name")));
        QTextCharFormat format;
        while (xml.readNextStartElement()) {
            if (is(xml, kStyleNs, QLatin1String("text-properties")))
                readTextProperties(xml.attributes(), format);
            xml.skipCurrentElement();
        }
        styles.insert(key, format);
    }
}

QTextCharFormat namedStyle(const StyleMap &styles, QStringView family, const QXmlStreamAttributes &attrs)
{
    return styles.value(styleKey(family, attrs.value(kTextNs, QLatin1String("style-name"))));
}

// Inline content of a paragraph, heading or span; reads up to the current
// element's end tag.
void readInline(QXmlStreamReader &xml, const StyleMap &styles, Paragraph &paragraph, const QTextCharFormat &format)
{
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::Characters:
            paragraph.appendXmlText(xml.text(), format);
            break;
        case QXmlStreamReader::EndElement:
            return;
        case QXmlStreamReader::StartElement:
            if (is(xml, kTextNs, QLatin1String("span"))) {
                QTextCharFormat spanFormat = format;
                spanFormat.merge(namedStyle(styles, u"text", xml.attributes()));
                readInline(xml, styles, paragraph, spanFormat);
            } else if (is(xml, kTextNs, QLatin1String("s"))) {
                bool ok = false;
                const int count = xml.attributes().value(kTextNs, QLatin1String("c")).toInt(&ok);
                paragraph.appendLiteral(QString(ok ? std::clamp(count, 1, 10000) : 1, u' '), format);
                xml.skipCurrentElement();
            } else if (is(xml, kTextNs, QLatin1String("tab"))) {
                paragraph.appendLiteral(QStringLiteral("\t"), format);
                xml.skipCurrentElement();
            } else if (is(xml, kTextNs, QLatin1String("line-break"))) {
                paragraph.appendLiteral(QString(QChar::LineSeparator), format);
                xml.skipCurrentElement();
            } else if (is(xml, kTextNs, QLatin1String("note")) || is(xml, kOfficeNs, QLatin1String("annotation"))
                       || xml.namespaceUri() == kDrawNs) {
                // Footnotes, comments and frames hold their own paragraphs,
                // which must not run into this one.
                xml.skipCurrentElement();
            } else {
                // Links, fields, bookmarks, ...: keep their text.
                readInline(xml, styles, paragraph, format);
            }
            break;
        default:
            break;
        }
    }
}

// Block content of <office:text> or a container in it; reads up to the
// current element's end tag. Containers this stage doesn't model (lists,
// tables, sections) are walked so their paragraphs still load, as plain
// paragraphs.
void readBlocks(QXmlStreamReader &xml, const StyleMap &styles, QList<Paragraph> &paragraphs)
{
    while (xml.readNextStartElement()) {
        const bool heading = is(xml, kTextNs, QLatin1String("h"));
        if (heading || is(xml, kTextNs, QLatin1String("p"))) {
            const QXmlStreamAttributes attrs = xml.attributes();
            Paragraph paragraph;
            paragraph.format = namedStyle(styles, u"paragraph", attrs);
            if (heading) {
                bool ok = false;
                const int level = attrs.value(kTextNs, QLatin1String("outline-level")).toInt(&ok);
                paragraph.headingLevel = ok ? std::clamp(level, 1, 6) : 1;
            }
            readInline(xml, styles, paragraph, paragraph.format);
            paragraphs.append(paragraph);
        } else if (is(xml, kTextNs, QLatin1String("tracked-changes")) || is(xml, kOfficeNs, QLatin1String("forms"))
                   || xml.namespaceUri() == kDrawNs) {
            // Deleted text, form controls, frames: not document text.
            xml.skipCurrentElement();
        } else {
            readBlocks(xml, styles, paragraphs);
        }
    }
}

// Heading look, matching what QTextDocument::setHtml() gives <h1>..<h6>.
QTextCharFormat headingCharFormat(int level)
{
    QTextCharFormat format;
    format.setFontWeight(QFont::Bold);
    format.setProperty(QTextFormat::FontSizeAdjustment, 4 - level);
    return format;
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

bool OdfReader::readInto(QTextDocument *doc, const QString &path, QString *error)
{
    if (!doc)
        return fail(error, QStringLiteral("no document to read into"));
    if (!QFileInfo(path).isFile())
        return fail(error, QStringLiteral("%1: no such file").arg(path));

    QuaZip zip(path);
    if (!zip.open(QuaZip::mdUnzip))
        return fail(error, QStringLiteral("%1: not an ODF document (not a zip archive)").arg(path));
    if (!zip.setCurrentFile(QStringLiteral("content.xml")))
        return fail(error, QStringLiteral("%1: not an ODF document (no content.xml)").arg(path));
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("%1: cannot read content.xml (zip error %2)").arg(path).arg(file.getZipError()));
    const QByteArray content = file.readAll();
    if (file.getZipError() != UNZ_OK)
        return fail(error, QStringLiteral("%1: cannot read content.xml (zip error %2)").arg(path).arg(file.getZipError()));
    file.close();
    zip.close();

    // office:automatic-styles precedes office:body in the schema, so one
    // pass sees every style before it is used.
    QXmlStreamReader xml(content);
    StyleMap styles;
    QList<Paragraph> paragraphs;
    bool sawText = false;
    if (xml.readNextStartElement() && is(xml, kOfficeNs, QLatin1String("document-content"))) {
        while (xml.readNextStartElement()) {
            if (is(xml, kOfficeNs, QLatin1String("automatic-styles"))) {
                readAutomaticStyles(xml, styles);
            } else if (is(xml, kOfficeNs, QLatin1String("body"))) {
                while (xml.readNextStartElement()) {
                    if (is(xml, kOfficeNs, QLatin1String("text"))) {
                        sawText = true;
                        readBlocks(xml, styles, paragraphs);
                    } else {
                        xml.skipCurrentElement();
                    }
                }
            } else {
                xml.skipCurrentElement();
            }
        }
    }
    if (xml.hasError())
        return fail(error, QStringLiteral("%1: malformed content.xml (line %2, column %3: %4)")
                               .arg(path).arg(xml.lineNumber()).arg(xml.columnNumber()).arg(xml.errorString()));
    if (!sawText)
        return fail(error, QStringLiteral("%1: not an ODF text document (no office:text body)").arg(path));

    doc->clear();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (qsizetype i = 0; i < paragraphs.size(); ++i) {
        const Paragraph &paragraph = paragraphs.at(i);
        QTextBlockFormat blockFormat;
        QTextCharFormat base;
        if (paragraph.headingLevel > 0) {
            blockFormat.setHeadingLevel(paragraph.headingLevel);
            base = headingCharFormat(paragraph.headingLevel);
        }
        base.merge(paragraph.format);
        if (i == 0) {
            cursor.setBlockFormat(blockFormat);
            cursor.setBlockCharFormat(base);
        } else {
            cursor.insertBlock(blockFormat, base);
        }
        for (const Run &run : paragraph.runs) {
            QTextCharFormat format = base;
            format.merge(run.format);
            cursor.insertText(run.text, format);
        }
    }
    cursor.endEditBlock();
    doc->clearUndoRedoStacks();
    return true;
}
