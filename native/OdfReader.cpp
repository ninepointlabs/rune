#include "OdfReader.h"

#include "DocumentController.h"

#include <QColor>
#include <QFileInfo>
#include <QList>
#include <QSet>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>
#include <QXmlStreamReader>

#include <quazip.h>
#include <quazipfile.h>

#include <algorithm>
#include <memory>

namespace OdfReader {

namespace {

QString styleKey(QStringView family, QStringView name)
{
    return family.toString() + u'/' + name.toString();
}

} // namespace

void StyleSheet::insert(const QString &family, const QString &name, const Style &style)
{
    m_styles.insert(styleKey(family, name), style);
}

bool StyleSheet::contains(const QString &family, const QString &name) const
{
    return m_styles.contains(styleKey(family, name));
}

Style StyleSheet::resolve(const QString &family, const QString &name) const
{
    // Nearest first, then applied root first so nearer styles override.
    QList<const Style *> chain;
    QSet<QString> visited;
    for (QString current = name; !current.isEmpty() && !visited.contains(current);) {
        visited.insert(current);
        const auto it = m_styles.constFind(styleKey(family, current));
        if (it == m_styles.cend())
            break;
        chain.append(&*it);
        current = it->parent;
    }
    Style resolved;
    for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
        resolved.charFormat.merge((*it)->charFormat);
        resolved.blockFormat.merge((*it)->blockFormat);
    }
    return resolved;
}

} // namespace OdfReader

namespace {

using OdfReader::Style;
using OdfReader::StyleSheet;

const QString kOfficeNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:office:1.0");
const QString kTextNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:text:1.0");
const QString kStyleNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:style:1.0");
const QString kFoNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
const QString kDrawNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
const QString kTableNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:table:1.0");
const QString kSvgNs = QStringLiteral("urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");

// Cap on table:number-rows-repeated / number-columns-repeated, so a
// hostile or spreadsheet-like file can't ask for millions of cells.
constexpr int kMaxRepeat = 1000;

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
    QTextBlockFormat blockFormat; // from the paragraph style
    // List membership: nesting level (0: not in a list), the top-level
    // <text:list> it belongs to, and whether that level is numbered.
    int listLevel = 0;
    int listId = 0;
    bool listNumbered = false;
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

struct Table;

// A paragraph, or (with `table` set) a table.
struct Block
{
    Paragraph paragraph;
    std::shared_ptr<Table> table;
};

struct Cell
{
    QList<Block> blocks;
    int rowSpan = 1;
    int columnSpan = 1;
    bool covered = false; // <table:covered-table-cell>: a slot under another cell's span
};

struct Table
{
    int columns = 0; // from <table:table-column>s
    QList<QList<Cell>> rows; // one entry per grid slot, covered ones included
};

// A <text:list-style>: whether each level is numbered (else bullet/image).
struct ListStyle
{
    QHash<int, bool> numbered;

    // A level the style doesn't define takes the nearest defined level
    // below it (Qt's writer defines only the level it uses), else any.
    bool numberedAt(int level) const
    {
        for (int l = level; l >= 1; --l)
            if (numbered.contains(l))
                return numbered.value(l);
        return !numbered.isEmpty() && numbered.cbegin().value();
    }
};

struct Context
{
    StyleSheet styles;
    QHash<QString, QStringList> fontFaces; // <style:font-face> name -> families
    QHash<QString, ListStyle> listStyles;
    int lists = 0; // top-level <text:list>s seen, numbering their ids
};

// Where a block sits in a list: nesting level (0: not in a list), the list
// style in effect and the id of the enclosing top-level list.
struct ListContext
{
    int level = 0;
    int id = 0;
    QString style;
};

bool is(const QXmlStreamReader &xml, const QString &ns, QLatin1StringView name)
{
    return xml.namespaceUri() == ns && xml.name() == name;
}

// "'Liberation Sans', serif" -> {"Liberation Sans", "serif"}.
QStringList fontFamilies(QStringView value)
{
    QStringList families;
    for (QStringView family : value.split(u',')) {
        family = family.trimmed();
        if (family.size() >= 2 && (family.front() == u'\'' || family.front() == u'"') && family.back() == family.front())
            family = family.sliced(1, family.size() - 2);
        if (!family.isEmpty())
            families << family.toString();
    }
    return families;
}

// "12pt", "0.5in", "1cm", ... in points; 0 for anything else (including
// percentages, which are relative to the parent style's size).
qreal lengthInPoints(QStringView value)
{
    static const struct { QLatin1StringView unit; qreal points; } units[] = {
        {QLatin1String("pt"), 1.0}, {QLatin1String("in"), 72.0}, {QLatin1String("cm"), 72.0 / 2.54},
        {QLatin1String("mm"), 72.0 / 25.4}, {QLatin1String("pc"), 12.0}, {QLatin1String("px"), 0.75}};
    for (const auto &unit : units) {
        if (!value.endsWith(unit.unit))
            continue;
        bool ok = false;
        const qreal number = value.chopped(unit.unit.size()).toDouble(&ok);
        return ok && number > 0 ? number * unit.points : 0;
    }
    return 0;
}

// A <style:text-properties>, attribute names as written by both Qt's ODF
// writer and LibreOffice. Qt writes fo:font-family; LibreOffice writes
// style:font-name, naming a <style:font-face> declaration, and sometimes
// both (fo:font-family then wins, being the family itself).
void readTextProperties(const QXmlStreamAttributes &attrs, const Context &ctx, QTextCharFormat &format)
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

    // setFontFamilies(), not setFontFamily(): it is what the UI sets and
    // what DocumentController::currentFontFamily() reads.
    QStringList families = fontFamilies(attrs.value(kFoNs, QLatin1String("font-family")));
    if (families.isEmpty()) {
        const QString fontName = attrs.value(kStyleNs, QLatin1String("font-name")).toString();
        if (!fontName.isEmpty())
            families = ctx.fontFaces.value(fontName, {fontName});
    }
    if (!families.isEmpty())
        format.setFontFamilies(families);

    const qreal size = lengthInPoints(attrs.value(kFoNs, QLatin1String("font-size")));
    if (size > 0)
        format.setFontPointSize(size);

    const QColor color = QColor::fromString(attrs.value(kFoNs, QLatin1String("color")));
    if (color.isValid())
        format.setForeground(color);
}

void readParagraphProperties(const QXmlStreamAttributes &attrs, QTextBlockFormat &format)
{
    const QStringView align = attrs.value(kFoNs, QLatin1String("text-align"));
    if (align == u"start" || align == u"left")
        format.setAlignment(Qt::AlignLeft);
    else if (align == u"center")
        format.setAlignment(Qt::AlignHCenter);
    else if (align == u"end" || align == u"right")
        format.setAlignment(Qt::AlignRight);
    else if (align == u"justify")
        format.setAlignment(Qt::AlignJustify);
}

// Called on <office:font-face-decls>; reads up to its end tag.
void readFontFaces(QXmlStreamReader &xml, Context &ctx)
{
    while (xml.readNextStartElement()) {
        if (is(xml, kStyleNs, QLatin1String("font-face"))) {
            const QXmlStreamAttributes attrs = xml.attributes();
            const QStringList families = fontFamilies(attrs.value(kSvgNs, QLatin1String("font-family")));
            if (!families.isEmpty())
                ctx.fontFaces.insert(attrs.value(kStyleNs, QLatin1String("name")).toString(), families);
        }
        xml.skipCurrentElement();
    }
}

// Called on <office:styles> or <office:automatic-styles>; reads up to its
// end tag. A style replaces any earlier one of the same family and name, so
// reading content.xml's automatic styles after styles.xml's named ones lets
// the automatic ones win.
void readStyles(QXmlStreamReader &xml, Context &ctx)
{
    while (xml.readNextStartElement()) {
        const QXmlStreamAttributes attrs = xml.attributes();
        const QString name = attrs.value(kStyleNs, QLatin1String("name")).toString();
        if (is(xml, kStyleNs, QLatin1String("style"))) {
            Style style;
            style.parent = attrs.value(kStyleNs, QLatin1String("parent-style-name")).toString();
            while (xml.readNextStartElement()) {
                if (is(xml, kStyleNs, QLatin1String("text-properties")))
                    readTextProperties(xml.attributes(), ctx, style.charFormat);
                else if (is(xml, kStyleNs, QLatin1String("paragraph-properties")))
                    readParagraphProperties(xml.attributes(), style.blockFormat);
                xml.skipCurrentElement();
            }
            ctx.styles.insert(attrs.value(kStyleNs, QLatin1String("family")).toString(), name, style);
        } else if (is(xml, kTextNs, QLatin1String("list-style"))) {
            ListStyle list;
            while (xml.readNextStartElement()) {
                const bool number = is(xml, kTextNs, QLatin1String("list-level-style-number"));
                if (number || is(xml, kTextNs, QLatin1String("list-level-style-bullet"))
                    || is(xml, kTextNs, QLatin1String("list-level-style-image"))) {
                    bool ok = false;
                    const int level = xml.attributes().value(kTextNs, QLatin1String("level")).toInt(&ok);
                    if (ok && level >= 1)
                        list.numbered.insert(level, number);
                }
                xml.skipCurrentElement();
            }
            ctx.listStyles.insert(name, list);
        } else {
            xml.skipCurrentElement();
        }
    }
}

Style namedStyle(const Context &ctx, const QString &family, const QXmlStreamAttributes &attrs)
{
    return ctx.styles.resolve(family, attrs.value(kTextNs, QLatin1String("style-name")).toString());
}

// Inline content of a paragraph, heading or span; reads up to the current
// element's end tag.
void readInline(QXmlStreamReader &xml, const Context &ctx, Paragraph &paragraph, const QTextCharFormat &format)
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
                spanFormat.merge(namedStyle(ctx, QStringLiteral("text"), xml.attributes()).charFormat);
                readInline(xml, ctx, paragraph, spanFormat);
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
                readInline(xml, ctx, paragraph, format);
            }
            break;
        default:
            break;
        }
    }
}

void readBlocks(QXmlStreamReader &xml, Context &ctx, const ListContext &list, QList<Block> &blocks);

int repeatCount(const QXmlStreamReader &xml, QLatin1StringView attribute)
{
    bool ok = false;
    const int count = xml.attributes().value(kTableNs, attribute).toInt(&ok);
    return ok ? std::clamp(count, 1, kMaxRepeat) : 1;
}

// Called on <table:table> or a row/column group in it; reads up to its end
// tag.
void readTableContent(QXmlStreamReader &xml, Context &ctx, Table &table)
{
    while (xml.readNextStartElement()) {
        if (is(xml, kTableNs, QLatin1String("table-column"))) {
            table.columns += repeatCount(xml, QLatin1String("number-columns-repeated"));
            xml.skipCurrentElement();
        } else if (is(xml, kTableNs, QLatin1String("table-row"))) {
            const int rowRepeat = repeatCount(xml, QLatin1String("number-rows-repeated"));
            QList<Cell> row;
            while (xml.readNextStartElement()) {
                const bool covered = is(xml, kTableNs, QLatin1String("covered-table-cell"));
                if (!covered && !is(xml, kTableNs, QLatin1String("table-cell"))) {
                    xml.skipCurrentElement();
                    continue;
                }
                Cell cell;
                cell.covered = covered;
                cell.rowSpan = repeatCount(xml, QLatin1String("number-rows-spanned"));
                cell.columnSpan = repeatCount(xml, QLatin1String("number-columns-spanned"));
                const int cellRepeat = repeatCount(xml, QLatin1String("number-columns-repeated"));
                if (covered)
                    xml.skipCurrentElement();
                else
                    readBlocks(xml, ctx, ListContext(), cell.blocks);
                for (int i = 0; i < cellRepeat; ++i)
                    row.append(cell);
            }
            for (int i = 0; i < rowRepeat; ++i)
                table.rows.append(row);
        } else if (is(xml, kTableNs, QLatin1String("table-header-rows")) || is(xml, kTableNs, QLatin1String("table-rows"))
                   || is(xml, kTableNs, QLatin1String("table-row-group"))
                   || is(xml, kTableNs, QLatin1String("table-header-columns"))
                   || is(xml, kTableNs, QLatin1String("table-columns"))
                   || is(xml, kTableNs, QLatin1String("table-column-group"))) {
            readTableContent(xml, ctx, table);
        } else {
            xml.skipCurrentElement();
        }
    }
}

// Called on <text:list>; reads up to its end tag. Each item's paragraphs
// join the list one level deeper than `parent`; a nested <text:list>
// without its own style continues the enclosing list's style.
void readList(QXmlStreamReader &xml, Context &ctx, const ListContext &parent, QList<Block> &blocks)
{
    ListContext list;
    list.level = parent.level + 1;
    list.id = parent.level == 0 ? ++ctx.lists : parent.id;
    list.style = xml.attributes().value(kTextNs, QLatin1String("style-name")).toString();
    if (list.style.isEmpty())
        list.style = parent.style;
    while (xml.readNextStartElement()) {
        if (is(xml, kTextNs, QLatin1String("list-item")) || is(xml, kTextNs, QLatin1String("list-header")))
            readBlocks(xml, ctx, list, blocks);
        else
            xml.skipCurrentElement();
    }
}

// Block content of <office:text> or a container in it; reads up to the
// current element's end tag. Containers not modelled (sections, ...) are
// walked so their paragraphs still load, as plain paragraphs.
void readBlocks(QXmlStreamReader &xml, Context &ctx, const ListContext &list, QList<Block> &blocks)
{
    while (xml.readNextStartElement()) {
        const bool heading = is(xml, kTextNs, QLatin1String("h"));
        if (heading || is(xml, kTextNs, QLatin1String("p"))) {
            const QXmlStreamAttributes attrs = xml.attributes();
            const Style style = namedStyle(ctx, QStringLiteral("paragraph"), attrs);
            Block block;
            Paragraph &paragraph = block.paragraph;
            paragraph.format = style.charFormat;
            paragraph.blockFormat = style.blockFormat;
            if (heading) {
                bool ok = false;
                const int level = attrs.value(kTextNs, QLatin1String("outline-level")).toInt(&ok);
                paragraph.headingLevel = ok ? std::clamp(level, 1, 6) : 1;
            }
            if (list.level > 0) {
                paragraph.listLevel = list.level;
                paragraph.listId = list.id;
                paragraph.listNumbered = ctx.listStyles.value(list.style).numberedAt(list.level);
            }
            readInline(xml, ctx, paragraph, paragraph.format);
            blocks.append(block);
        } else if (is(xml, kTextNs, QLatin1String("list"))) {
            readList(xml, ctx, list, blocks);
        } else if (is(xml, kTableNs, QLatin1String("table"))) {
            Block block;
            block.table = std::make_shared<Table>();
            readTableContent(xml, ctx, *block.table);
            blocks.append(block);
        } else if (is(xml, kTextNs, QLatin1String("tracked-changes")) || is(xml, kOfficeNs, QLatin1String("forms"))
                   || xml.namespaceUri() == kDrawNs) {
            // Deleted text, form controls, frames: not document text.
            xml.skipCurrentElement();
        } else {
            readBlocks(xml, ctx, list, blocks);
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

void insertBlocks(QTextCursor &cursor, const QList<Block> &blocks);

// Inserts `table` at `cursor` and leaves the cursor in the block after it.
void insertTable(QTextCursor &cursor, const Table &table)
{
    const int rows = int(table.rows.size());
    int columns = table.columns;
    for (const QList<Cell> &row : table.rows)
        columns = std::max(columns, int(row.size()));
    if (rows == 0 || columns == 0)
        return;

    QTextTable *textTable = cursor.insertTable(rows, columns, DocumentController::tableFormat());
    // Spans first: the cells they swallow must not get content.
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < int(table.rows.at(r).size()); ++c) {
            const Cell &cell = table.rows.at(r).at(c);
            if (!cell.covered && (cell.rowSpan > 1 || cell.columnSpan > 1))
                textTable->mergeCells(r, c, std::min(cell.rowSpan, rows - r), std::min(cell.columnSpan, columns - c));
        }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < int(table.rows.at(r).size()); ++c) {
            const Cell &cell = table.rows.at(r).at(c);
            const QTextTableCell textCell = textTable->cellAt(r, c);
            if (cell.covered || textCell.row() != r || textCell.column() != c)
                continue;
            QTextCursor cellCursor = textCell.firstCursorPosition();
            insertBlocks(cellCursor, cell.blocks);
        }
    cursor.setPosition(textTable->lastPosition() + 1);
}

// Inserts `blocks` starting in the cursor's block, which must be empty (a
// new document, a new table cell). A table can't share a block, so one
// that is first leaves an empty paragraph before it, and every table is
// followed by a block, which the next paragraph reuses.
void insertBlocks(QTextCursor &cursor, const QList<Block> &blocks)
{
    bool fresh = true; // the cursor's block is empty and unused
    // The list each nesting level adds to, index level - 1: a QTextList per
    // run of siblings, as DocumentController's list editing builds them.
    QList<QTextList *> lists;
    int listId = 0;
    for (const Block &block : blocks) {
        if (block.table) {
            insertTable(cursor, *block.table);
            fresh = true;
            lists.clear();
            continue;
        }

        const Paragraph &paragraph = block.paragraph;
        QTextBlockFormat blockFormat = paragraph.blockFormat;
        QTextCharFormat base;
        if (paragraph.headingLevel > 0) {
            blockFormat.setHeadingLevel(paragraph.headingLevel);
            base = headingCharFormat(paragraph.headingLevel);
        }
        base.merge(paragraph.format);
        if (paragraph.format.hasProperty(QTextFormat::FontPointSize))
            base.clearProperty(QTextFormat::FontSizeAdjustment); // the style's size is the size
        if (fresh) {
            cursor.setBlockFormat(blockFormat);
            cursor.setBlockCharFormat(base);
            fresh = false;
        } else {
            cursor.insertBlock(blockFormat, base);
        }
        for (const Run &run : paragraph.runs) {
            QTextCharFormat format = base;
            format.merge(run.format);
            cursor.insertText(run.text, format);
        }

        if (paragraph.listLevel == 0) {
            lists.clear();
            continue;
        }
        if (paragraph.listId != listId) {
            lists.clear();
            listId = paragraph.listId;
        }
        // An item closes any deeper runs: later items there are new lists,
        // as under a new parent item in the UI.
        const int level = paragraph.listLevel;
        lists.resize(std::min(int(lists.size()), level));
        lists.resize(level, nullptr);
        const auto kind = paragraph.listNumbered ? DocumentController::ListKind::Numbered
                                                 : DocumentController::ListKind::Bullet;
        const QTextListFormat::Style style = DocumentController::listStyle(kind, level);
        QTextList *&list = lists[level - 1];
        if (list && list->format().style() == style) {
            list->add(cursor.block());
        } else {
            QTextListFormat format;
            format.setStyle(style);
            format.setIndent(level);
            list = QTextCursor(cursor.block()).createList(format);
        }
    }
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

QString malformed(const QString &path, const QString &part, const QXmlStreamReader &xml)
{
    return QStringLiteral("%1: malformed %2 (line %3, column %4: %5)")
        .arg(path, part).arg(xml.lineNumber()).arg(xml.columnNumber()).arg(xml.errorString());
}

// Reads the archive member `name` into `data`. A missing optional member
// leaves `data` empty and succeeds.
bool extract(QuaZip &zip, const QString &path, const QString &name, bool required, QByteArray &data,
             QString *error)
{
    if (!zip.setCurrentFile(name))
        return !required || fail(error, QStringLiteral("%1: not an ODF document (no %2)").arg(path, name));
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("%1: cannot read %2 (zip error %3)").arg(path, name).arg(file.getZipError()));
    data = file.readAll();
    if (file.getZipError() != UNZ_OK)
        return fail(error, QStringLiteral("%1: cannot read %2 (zip error %3)").arg(path, name).arg(file.getZipError()));
    file.close();
    return true;
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
    QByteArray content, stylesXml;
    // styles.xml is optional: Qt's writer doesn't produce one.
    if (!extract(zip, path, QStringLiteral("content.xml"), true, content, error)
        || !extract(zip, path, QStringLiteral("styles.xml"), false, stylesXml, error))
        return false;
    zip.close();

    Context ctx;
    // styles.xml first, so content.xml's automatic styles replace named
    // styles of the same name. Its own automatic styles serve headers and
    // footers, which aren't read, and may reuse content.xml's names.
    if (!stylesXml.isEmpty()) {
        QXmlStreamReader xml(stylesXml);
        if (xml.readNextStartElement() && is(xml, kOfficeNs, QLatin1String("document-styles"))) {
            while (xml.readNextStartElement()) {
                if (is(xml, kOfficeNs, QLatin1String("font-face-decls")))
                    readFontFaces(xml, ctx);
                else if (is(xml, kOfficeNs, QLatin1String("styles")))
                    readStyles(xml, ctx);
                else
                    xml.skipCurrentElement();
            }
        }
        if (xml.hasError())
            return fail(error, malformed(path, QStringLiteral("styles.xml"), xml));
    }

    // office:font-face-decls and office:automatic-styles precede
    // office:body in the schema, so one pass sees every style before it is
    // used.
    QXmlStreamReader xml(content);
    QList<Block> blocks;
    bool sawText = false;
    if (xml.readNextStartElement() && is(xml, kOfficeNs, QLatin1String("document-content"))) {
        while (xml.readNextStartElement()) {
            if (is(xml, kOfficeNs, QLatin1String("font-face-decls"))) {
                readFontFaces(xml, ctx);
            } else if (is(xml, kOfficeNs, QLatin1String("automatic-styles"))) {
                readStyles(xml, ctx);
            } else if (is(xml, kOfficeNs, QLatin1String("body"))) {
                while (xml.readNextStartElement()) {
                    if (is(xml, kOfficeNs, QLatin1String("text"))) {
                        sawText = true;
                        readBlocks(xml, ctx, ListContext(), blocks);
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
        return fail(error, malformed(path, QStringLiteral("content.xml"), xml));
    if (!sawText)
        return fail(error, QStringLiteral("%1: not an ODF text document (no office:text body)").arg(path));

    doc->clear();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    insertBlocks(cursor, blocks);
    cursor.endEditBlock();
    doc->clearUndoRedoStacks();
    return true;
}
