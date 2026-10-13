#include "CharFormatter.h"

#include "EditState.h"
#include "InlineObjects.h"

#include <QColor>
#include <QDebug>
#include <QFontDatabase>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace {

bool hasAttr(const QTextCharFormat &f, CharFormatter::Attr attr)
{
    switch (attr) {
    case CharFormatter::Attr::Bold: return f.fontWeight() >= QFont::Bold;
    case CharFormatter::Attr::Italic: return f.fontItalic();
    case CharFormatter::Attr::Underline: return f.fontUnderline();
    }
    return false;
}

void setAttr(QTextCharFormat &f, CharFormatter::Attr attr, bool on)
{
    switch (attr) {
    case CharFormatter::Attr::Bold: f.setFontWeight(on ? QFont::Bold : QFont::Normal); break;
    case CharFormatter::Attr::Italic: f.setFontItalic(on); break;
    case CharFormatter::Attr::Underline: f.setFontUnderline(on); break;
    }
}

// Removes any explicit text color from [start, end). mergeCharFormat() can't
// do this: a format with the property cleared merges as "no change", and an
// invalid QBrush is stored as a real property that the ODF writer exports
// as fo:color="#000000". So rewrite each fragment's format without it.
void clearForegroundIn(QTextDocument *doc, int start, int end)
{
    struct Range { int start, end; QTextCharFormat format; };
    QList<Range> ranges;
    for (QTextBlock block = doc->findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int from = std::max(start, fragment.position());
            const int to = std::min(end, fragment.position() + fragment.length());
            if (from < to && fragment.charFormat().hasProperty(QTextFormat::ForegroundBrush))
                ranges.append({from, to, fragment.charFormat()});
        }
    }
    // Collected first: setCharFormat() splits and merges fragments.
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (Range &range : ranges) {
        range.format.clearForeground();
        cursor.setPosition(range.start);
        cursor.setPosition(range.end, QTextCursor::KeepAnchor);
        cursor.setCharFormat(range.format);
    }
    cursor.endEditBlock();
}

} // namespace

bool CharFormatter::attrState(Attr attr) const
{
    const QTextCursor cursor = m_state.selectionCursor();
    if (!cursor.hasSelection()) {
        if (m_pendingPosition == cursor.position()) {
            const int property = attr == Attr::Bold ? QTextFormat::FontWeight
                               : attr == Attr::Italic ? QTextFormat::FontItalic
                                                      : QTextFormat::TextUnderlineStyle;
            if (m_pendingFormat.hasProperty(property))
                return hasAttr(m_pendingFormat, attr);
        }
        return hasAttr(cursor.charFormat(), attr);
    }

    const int start = cursor.selectionStart(), end = cursor.selectionEnd();
    for (QTextBlock block = m_state.target->findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.position() + fragment.length() <= start || fragment.position() >= end)
                continue;
            if (!hasAttr(fragment.charFormat(), attr))
                return false;
        }
    }
    return true;
}

void CharFormatter::toggle(Attr attr)
{
    QTextCharFormat change;
    setAttr(change, attr, !attrState(attr));
    apply(change);
}

void CharFormatter::setFontFamily(const QString &family)
{
    QTextCharFormat change;
    change.setFontFamilies({family});
    apply(change);
}

QString CharFormatter::fontFamily() const
{
    const QStringList families = current().fontFamilies().toStringList();
    return families.isEmpty() ? m_state.target->defaultFont().family() : families.constFirst();
}

void CharFormatter::setFontSize(qreal pointSize)
{
    if (pointSize <= 0)
        return;
    QTextCharFormat change;
    change.setFontPointSize(pointSize);
    apply(change);
}

qreal CharFormatter::fontSize() const
{
    const QTextCharFormat format = current();
    return format.hasProperty(QTextFormat::FontPointSize) ? format.fontPointSize()
                                                          : m_state.target->defaultFont().pointSizeF();
}

bool CharFormatter::setTextColor(const QString &hexColor)
{
    if (hexColor.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        apply(QTextCharFormat(), true);
        return true;
    }
    const QColor color = QColor::fromString(hexColor);
    if (!color.isValid())
        return false;
    QTextCharFormat change;
    change.setForeground(color);
    apply(change);
    return true;
}

QString CharFormatter::textColor() const
{
    const QTextCharFormat format = current();
    if (!format.hasProperty(QTextFormat::ForegroundBrush) || format.foreground().style() == Qt::NoBrush)
        return QStringLiteral("auto");
    return format.foreground().color().name();
}

QVariantList CharFormatter::installedFamilies()
{
    QVariantList families;
    for (const QString &family : QFontDatabase::families()) {
        const QList<QFontDatabase::WritingSystem> systems = QFontDatabase::writingSystems(family);
        const bool preview = systems.contains(QFontDatabase::Latin) && !systems.contains(QFontDatabase::Symbol);
        families.append(QVariantMap{{QStringLiteral("family"), family}, {QStringLiteral("preview"), preview}});
    }
    return families;
}

void CharFormatter::apply(const QTextCharFormat &change, bool clearForeground)
{
    QTextCursor cursor = m_state.selectionCursor();
    if (!cursor.hasSelection()) {
        // Strictly inside a word: format the whole word, as word processors do.
        const int position = cursor.position();
        QTextCursor word = cursor;
        word.select(QTextCursor::WordUnderCursor);
        if (word.selectionStart() < position && position < word.selectionEnd()) {
            cursor = word;
        } else {
            // Nothing to format yet: arm it for the next insertion here.
            m_pendingFormat.merge(change);
            if (clearForeground) {
                m_pendingFormat.clearForeground();
                m_pendingClearForeground = true;
            } else if (change.hasProperty(QTextFormat::ForegroundBrush)) {
                m_pendingClearForeground = false;
            }
            m_pendingPosition = position;
            return;
        }
    }
    if (clearForeground)
        clearForegroundIn(m_state.target, cursor.selectionStart(), cursor.selectionEnd());
    else
        cursor.mergeCharFormat(change);
}

QTextCharFormat CharFormatter::current() const
{
    QTextCursor cursor = m_state.selectionCursor();
    if (cursor.hasSelection()) {
        cursor.setPosition(cursor.selectionStart() + 1);
        return cursor.charFormat();
    }
    QTextCharFormat format = cursor.charFormat();
    if (m_pendingPosition == cursor.position()) {
        format.merge(m_pendingFormat);
        if (m_pendingClearForeground)
            format.clearForeground();
    }
    return format;
}

void CharFormatter::clearPending()
{
    m_pendingPosition = -1;
    m_pendingFormat = QTextCharFormat();
    m_pendingClearForeground = false;
}

void CharFormatter::selectionMoved()
{
    if (m_pendingPosition >= 0 && (m_state.hasSelection() || m_state.cursorPosition != m_pendingPosition))
        clearPending();
}

void CharFormatter::onContentsChange(int position, int removed, int added)
{
    // Text inserted at the pending position. An input-method commit reports
    // its whole paragraph as replaced (e.g. 0, 7, 8 for one character typed
    // at 6), so take the net growth, and require the range to cover it.
    const int inserted = added - removed;
    if (m_state.selfEdit || m_pendingPosition < 0 || inserted <= 0 || position > m_pendingPosition
        || position + added < m_pendingPosition + inserted)
        return;
    const int start = m_pendingPosition, end = m_pendingPosition + inserted;
    QTextCursor cursor(m_state.target);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    {
        const auto selfEdit = m_state.selfEditing();
        cursor.mergeCharFormat(m_pendingFormat);
        if (m_pendingClearForeground)
            clearForegroundIn(m_state.target, start, end);
    }
    clearPending();
}

int CharFormatter::typeText(const QString &text)
{
    QTextCursor cursor = m_state.selectionCursor();
    const bool pending = m_pendingPosition >= 0 && m_pendingPosition == cursor.position() && !cursor.hasSelection();
    // Right after a citation or field: its look, not the object itself.
    QTextCharFormat format = InlineObjects::withoutObject(cursor.charFormat());
    if (pending) {
        format.merge(m_pendingFormat);
        if (m_pendingClearForeground)
            format.clearForeground();
    }
    // Extend the current step only when nothing else has happened since
    // and this doesn't start a new word.
    const bool startsWord = m_typingEndsInSpace && !text.front().isSpace();
    const bool extend = m_typingEnd >= 0 && m_typingEnd == cursor.position() && !cursor.hasSelection()
        && m_typingRevision == m_state.contentRevision && !pending && !startsWord;
    {
        const auto selfEdit = m_state.selfEditing();
        // Separate edit blocks are never merged by Qt; joining the previous
        // one makes this part of its undo step.
        if (extend)
            cursor.joinPreviousEditBlock();
        else
            cursor.beginEditBlock();
        cursor.insertText(text, format);
        cursor.endEditBlock();
    }
    m_typingEnd = cursor.position();
    m_typingEndsInSpace = text.back().isSpace();
    m_typingRevision = m_state.contentRevision;
    if (pending)
        clearPending();
    return cursor.position();
}
