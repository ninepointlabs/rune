#pragma once

// CharFormatter: character formatting and typing at the selection of an
// EditState's target document.
//
// Formatting (bold, italic, underline, font, size, colour) with a collapsed
// cursor inside a word formats that word; elsewhere it arms a pending
// format that is applied to the next text typed at that position. Queries
// report the first selected character, or what typing at the cursor would
// produce; unset values report the document's default font and "auto".
//
// Typing (typeText()) groups undo steps a word at a time; see there.

#include <QString>
#include <QTextCharFormat>
#include <QVariantList>

struct EditState;

class CharFormatter
{
public:
    explicit CharFormatter(EditState &state) : m_state(state) {}

    enum class Attr { Bold, Italic, Underline };
    // With a selection: true only if every selected character has `attr`.
    // Collapsed: the pending format if armed here, else the character
    // before the cursor (what typing would inherit).
    bool attrState(Attr attr) const;
    void toggle(Attr attr);

    void setFontFamily(const QString &family);
    QString fontFamily() const;
    // Ignores sizes of 0 and less.
    void setFontSize(qreal pointSize);
    qreal fontSize() const;
    // "#RRGGBB", or "auto" to remove any explicit color. False (changing
    // nothing) for anything else.
    bool setTextColor(const QString &hexColor);
    QString textColor() const;
    // The installed font families, as {family, preview}: preview is false
    // when the family can't write its own Latin name (symbol, emoji and
    // non-Latin fonts), so a picker shows that name in the UI font instead.
    static QVariantList installedFamilies();

    // Applies `change` (or, with `clearForeground`, removes the text color)
    // to the selection; with none, to the word around the cursor or,
    // outside a word, to the next text typed at the cursor.
    void apply(const QTextCharFormat &change, bool clearForeground = false);
    // The format the queries report: the first selected character's, or
    // what typing at the collapsed cursor would produce (pending format
    // included).
    QTextCharFormat current() const;

    // The pending format is dropped by this, and when the selection moves
    // away from where it was armed (selectionMoved()).
    void clearPending();
    void selectionMoved();
    // The target's contentsChange(): text inserted at the pending position
    // (typed through the TextEdit itself, or an input-method commit) gets
    // the pending format.
    void onContentsChange(int position, int removed, int added);

    // Inserts `text` at the cursor, replacing any selection, in the format
    // typing there would produce (pending format included); returns the
    // cursor position after it. Qt merges all consecutive same-format
    // typing into one undo step, so Ctrl+Z after a paragraph of typing
    // would remove the whole paragraph. Here a step is a word plus the
    // spaces after it; a new one also starts after the cursor moves, any
    // other edit, replacing a selection, a pending format, or
    // endTypingStep().
    int typeText(const QString &text);
    // The next typeText() starts a new undo step.
    void endTypingStep() { m_typingEnd = -1; }

private:
    EditState &m_state;

    // Format armed by a toggle with no selection and no word to apply it
    // to; merged onto the next insertion at m_pendingPosition. A pending
    // "auto" color can't be expressed as a merge, hence the separate flag.
    QTextCharFormat m_pendingFormat;
    bool m_pendingClearForeground = false;
    int m_pendingPosition = -1;
    // The typing undo step typeText() is extending: where its text ends,
    // whether that text ended in whitespace, and the content revision
    // after it (any other edit since starts a new step). -1: none.
    int m_typingEnd = -1;
    bool m_typingEndsInSpace = false;
    quint64 m_typingRevision = 0;
};
