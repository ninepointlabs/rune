#pragma once

// StreamedEdit: an AI reply changing the body as it arrives, all of it ONE
// undo step. DocumentController refuses typing, undo and other edits while
// one is open, and only begins one on the body; nothing changes until the
// reply's first piece.
//
// begin() opens it on the current selection or cursor. Then either:
//  - New text: append() replaces the selection (first call) or writes at
//    the cursor, as plain text as it arrives; then finish() replaces all of
//    it with `markdown` converted to formatting (headings, lists, emphasis,
//    tables; a paragraph that is only a non-breaking space becomes an empty
//    one).
//  - Changes to existing text: applyReplacement() for each (find, replace)
//    pair, within the selection if there was one, else the whole document;
//    see there.
// abort() closes it, keeping what was done, or reverting the document to
// exactly how it was.
//
// Each edit returns where the TextEdit's cursor should go (-1: nowhere).

#include <QString>
#include <QTextCharFormat>

class QTextCursor;
struct EditState;

class StreamedEdit
{
public:
    explicit StreamedEdit(EditState &state) : m_state(state) {}

    bool isActive() const { return m_active; }

    // The body as Markdown around the selection (or the cursor, where
    // `selection` is ""): what an AI request sends as context. One line a
    // paragraph: unlike Qt's own Markdown, never wrapped.
    struct MarkdownContext
    {
        QString before, selection, after;
    };
    MarkdownContext markdownContext() const;

    // False, doing nothing, if one is already open.
    bool begin();
    int append(const QString &text);
    int finish(const QString &markdown);
    // Replaces `find` with `replacement` (plain text; "\n" is a paragraph
    // break, except inside one paragraph, where a line break that wraps a
    // line is a space). `find` is matched exactly, else ignoring Markdown
    // markup and differences in whitespace; the first match at or after the
    // previous replacement wins, else the first in scope. Failing those, a
    // quote of 12 or more characters found exactly once, compared without
    // whitespace and with curly quotes as straight ones, matches. Only the
    // characters that differ are rewritten, so the formatting of the rest
    // is kept; new empty paragraphs are left out of any list. -1 if `find`
    // isn't there (or spans a table boundary), changing nothing.
    int applyReplacement(const QString &find, const QString &replacement);
    int abort(bool keepText);

private:
    // Opens (or joins) the edit's undo step on `cursor`.
    void openStep(QTextCursor &cursor);
    // The first append()/finish(): removes the selection and sets where new
    // text goes.
    void startText();

    EditState &m_state;
    // The edit in progress: its selection when it began, the text it has
    // written so far (start -1: none yet), where the next replacement
    // search starts, the format new text gets, and whether its undo step
    // exists yet.
    bool m_active = false;
    int m_selStart = 0;
    int m_selEnd = 0;
    bool m_hadSelection = false;
    int m_start = -1;
    int m_end = -1;
    int m_searchFrom = 0;
    QTextCharFormat m_format;
    bool m_hasStep = false;
};
