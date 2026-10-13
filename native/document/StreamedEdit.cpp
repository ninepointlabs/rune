#include "StreamedEdit.h"

#include "EditState.h"
#include "Lists.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFrame>

#include <algorithm>

namespace {

// The reply's Markdown, minus a code fence wrapped around all of it (which
// models add despite being asked not to).
QString unfence(const QString &markdown)
{
    const QString trimmed = markdown.trimmed();
    if (!trimmed.startsWith(QLatin1String("```")) || !trimmed.endsWith(QLatin1String("```")) || trimmed.size() < 6)
        return markdown;
    const qsizetype firstNewline = trimmed.indexOf(QLatin1Char('\n'));
    if (firstNewline < 0)
        return markdown;
    return trimmed.mid(firstNewline + 1, trimmed.size() - 3 - firstNewline - 1);
}

// QTextDocument's frame boundaries (tables): never inside a replacement.
bool isFrameBoundary(QChar c)
{
    return c.unicode() == 0xfdd0 || c.unicode() == 0xfdd1; // QTextBeginningOfFrame / QTextEndOfFrame
}

// `find` as a pattern tolerant of what models change when quoting: Markdown
// markup (emphasis, code, list markers, heading hashes) and whitespace.
QRegularExpression loosePattern(const QString &find)
{
    QString text = find;
    static const QRegularExpression lineMarkers(QStringLiteral(R"((^|\n)[ \t]*(?:[-*+]|\d+[.)]|#{1,6})[ \t]+)"));
    text.replace(lineMarkers, QStringLiteral("\\1"));
    static const QRegularExpression emphasis(QStringLiteral(R"(\*\*|__|\*|`)"));
    text.remove(emphasis);
    const QStringList words = text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
    QStringList escaped;
    for (const QString &w : words)
        escaped.append(QRegularExpression::escape(w));
    return QRegularExpression(escaped.join(QStringLiteral(R"(\s+)")));
}

} // namespace

StreamedEdit::MarkdownContext StreamedEdit::markdownContext() const
{
    QTextDocument *body = m_state.body;
    const auto markdown = [body](int start, int end) {
        if (start >= end)
            return QString();
        QTextCursor range(body);
        range.setPosition(start);
        range.setPosition(end, QTextCursor::KeepAnchor);
        return QTextDocumentFragment(range).toMarkdown();
    };
    const QTextCursor cursor = m_state.selectionCursor();
    const int end = body->characterCount() - 1;
    return {markdown(0, cursor.selectionStart()), markdown(cursor.selectionStart(), cursor.selectionEnd()),
            markdown(cursor.selectionEnd(), end)};
}

bool StreamedEdit::begin()
{
    if (m_active)
        return false;
    const QTextCursor cursor = m_state.selectionCursor();
    m_active = true;
    m_selStart = cursor.selectionStart();
    m_selEnd = cursor.selectionEnd();
    m_hadSelection = cursor.hasSelection();
    m_start = m_end = -1;
    m_searchFrom = m_selStart;
    m_hasStep = false;
    return true;
}

void StreamedEdit::openStep(QTextCursor &cursor)
{
    if (m_hasStep)
        cursor.joinPreviousEditBlock();
    else
        cursor.beginEditBlock();
    m_hasStep = true;
}

void StreamedEdit::startText()
{
    if (m_start >= 0)
        return;
    QTextDocument *body = m_state.body;
    QTextCursor cursor(body);
    cursor.setPosition(m_selStart);
    if (m_hadSelection) {
        // Typing over a selection keeps its format; so does this.
        QTextCursor first(body);
        first.setPosition(m_selStart + 1);
        m_format = first.charFormat();
        const auto selfEdit = m_state.selfEditing();
        cursor.setPosition(m_selEnd, QTextCursor::KeepAnchor);
        openStep(cursor);
        cursor.removeSelectedText();
        cursor.endEditBlock();
    } else {
        m_format = cursor.charFormat();
    }
    m_start = m_end = cursor.position();
}

int StreamedEdit::append(const QString &text)
{
    if (!m_active || text.isEmpty())
        return -1;
    startText();
    QTextCursor cursor(m_state.body);
    cursor.setPosition(m_end);
    const auto selfEdit = m_state.selfEditing();
    openStep(cursor);
    cursor.insertText(text, m_format);
    cursor.endEditBlock();
    m_end = cursor.position();
    return m_end;
}

int StreamedEdit::finish(const QString &markdown)
{
    if (!m_active)
        return -1;
    startText();
    QTextDocument *body = m_state.body;
    // Converted in a document of its own, then inserted as a fragment.
    QTextDocument converted;
    converted.setMarkdown(unfence(markdown).trimmed());
    // Markdown has no empty paragraph; the model writes "&nbsp;" for one.
    for (QTextBlock block = converted.begin(); block.isValid(); block = block.next()) {
        if (block.text() == QString(QChar::Nbsp)) {
            QTextCursor nbsp(block);
            nbsp.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            nbsp.removeSelectedText();
        }
    }
    QTextCursor cursor(body);
    {
        const auto selfEdit = m_state.selfEditing();
        cursor.setPosition(m_start);
        openStep(cursor);
        cursor.setPosition(m_end, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        // Several paragraphs (or a heading, list or table) get paragraphs
        // of their own: inserted mid-paragraph, the fragment's first block
        // would merge into the text before it and its last into the text
        // after. A single plain paragraph stays inline (a rewritten phrase).
        const QTextBlock firstConverted = converted.begin();
        bool hasTable = false;
        for (auto it = converted.rootFrame()->begin(); !it.atEnd() && !hasTable; ++it)
            hasTable = it.currentFrame() != nullptr;
        const bool ownBlocks = converted.blockCount() > 1 || hasTable || firstConverted.textList()
            || firstConverted.blockFormat().headingLevel() > 0;
        if (ownBlocks) {
            // The spaces at a split would start or end a paragraph.
            while (!cursor.atBlockEnd() && body->characterAt(cursor.position()).isSpace())
                cursor.deleteChar();
            while (!cursor.atBlockStart() && body->characterAt(cursor.position() - 1).isSpace())
                cursor.deletePreviousChar();
        }
        if (ownBlocks && !cursor.atBlockEnd()) {
            cursor.insertBlock();
            cursor.movePosition(QTextCursor::PreviousCharacter);
        }
        if (ownBlocks && !cursor.atBlockStart())
            cursor.insertBlock();
        const int insertedAt = cursor.position();
        const bool atBlockStart = cursor.atBlockStart();
        if (!converted.isEmpty())
            cursor.insertFragment(QTextDocumentFragment(&converted));
        // A fragment's first block merges into the block it lands in and
        // loses its own block format: a leading "# Title" would arrive as
        // body text. Restore its heading level when it starts a block.
        const int heading = firstConverted.blockFormat().headingLevel();
        if (atBlockStart && heading > 0) {
            QTextCursor first(body->findBlock(insertedAt));
            QTextBlockFormat format;
            format.setHeadingLevel(heading);
            first.mergeBlockFormat(format);
        }
        cursor.endEditBlock();
    }
    m_active = false;
    m_start = m_end = -1;
    m_hasStep = false;
    return cursor.position();
}

int StreamedEdit::applyReplacement(const QString &find, const QString &replace)
{
    if (!m_active || find.isEmpty() || m_start >= 0)
        return -1; // new text was already being written: one kind of edit per reply
    QTextDocument *body = m_state.body;
    // The scope as text, position for position ('\n' for paragraph breaks).
    const int scopeStart = m_hadSelection ? m_selStart : 0;
    const int scopeEnd = m_hadSelection ? m_selEnd : body->characterCount() - 1;
    QString text;
    text.reserve(scopeEnd - scopeStart);
    for (int i = scopeStart; i < scopeEnd; ++i) {
        const QChar c = body->characterAt(i);
        text.append(c == QChar::ParagraphSeparator || c == QChar::LineSeparator ? QChar(u'\n') : c);
    }
    // First match at or after the previous replacement, else the first.
    const int from = std::clamp(m_searchFrom - scopeStart, 0, int(text.size()));
    int at = -1, length = 0;
    const auto exact = [&](int start) {
        const qsizetype i = text.indexOf(find, start);
        if (i >= 0) {
            at = int(i);
            length = int(find.size());
        }
        return i >= 0;
    };
    const QRegularExpression loose = loosePattern(find);
    const auto fuzzy = [&](int start) {
        if (loose.pattern().isEmpty())
            return false;
        const QRegularExpressionMatch m = loose.match(text, start);
        if (m.hasMatch()) {
            at = int(m.capturedStart());
            length = int(m.capturedLength());
        }
        return m.hasMatch();
    };
    if (!exact(from) && !exact(0) && !fuzzy(from) && !fuzzy(0))
        return -1;
    const QString found = text.mid(at, length);
    if (std::any_of(found.begin(), found.end(), isFrameBoundary))
        return -1;

    // Rewrite only what differs: the common prefix and suffix stay as they
    // are, formatting and all.
    int prefix = 0;
    while (prefix < found.size() && prefix < replace.size() && found[prefix] == replace[prefix])
        ++prefix;
    int suffix = 0;
    while (suffix < found.size() - prefix && suffix < replace.size() - prefix
           && found[found.size() - 1 - suffix] == replace[replace.size() - 1 - suffix])
        ++suffix;
    const int start = scopeStart + at + prefix;
    const int end = scopeStart + at + int(found.size()) - suffix;
    const QString middle = replace.mid(prefix, replace.size() - prefix - suffix);

    QTextCursor cursor(body);
    {
        const auto selfEdit = m_state.selfEditing();
        cursor.setPosition(start);
        openStep(cursor);
        // The text it replaces sets the format (else the text before it).
        QTextCursor formatAt(body);
        formatAt.setPosition(start < end ? start + 1 : start);
        const QTextCharFormat format = formatAt.charFormat();
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.insertText(middle, format);
        // Empty paragraphs it made ("leave a line") aren't list items.
        for (QTextBlock block = body->findBlock(start); block.isValid() && block.position() <= cursor.position();
             block = block.next()) {
            if (block.text().isEmpty() && block.textList())
                Lists::removeFromList(block);
        }
        cursor.endEditBlock();
    }
    const int delta = int(middle.size()) - (end - start);
    if (m_hadSelection)
        m_selEnd += delta;
    m_searchFrom = cursor.position() + suffix;
    return cursor.position();
}

int StreamedEdit::abort(bool keepText)
{
    if (!m_active)
        return -1;
    const bool revert = !keepText && m_hasStep;
    const int position = revert ? m_selStart : (m_end >= 0 ? m_end : m_searchFrom);
    m_active = false;
    m_start = m_end = -1;
    if (revert) {
        // Our step is the last one: nothing else could edit meanwhile.
        const auto selfEdit = m_state.selfEditing();
        m_state.body->undo();
        // Undone, it would otherwise be redoable.
        m_state.body->clearUndoRedoStacks(QTextDocument::RedoStack);
    }
    m_hasStep = false;
    return position;
}
