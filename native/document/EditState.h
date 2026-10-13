#pragma once

// EditState: what every editing feature (CharFormatter, StreamedEdit, list
// and table edits) shares, owned by DocumentController.
//
// Selection: QML can't hand C++ a QTextCursor, so the TextEdit reports its
// cursor and selection (DocumentController::setSelection()) and features
// build a QTextCursor on the edited document on demand.

#include <QList>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextCursor>

class QTextDocument;

struct EditState
{
    explicit EditState(QTextDocument *body) : body(body), target(body) {}

    QTextDocument *const body;
    // The document being edited: the body, or the header or footer while
    // one is edited in place (see DocumentController::setEditTarget()).
    QTextDocument *target;

    // Mirror of the TextEdit's cursorPosition/selectionStart/selectionEnd.
    int cursorPosition = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
    bool hasSelection() const { return selectionStart != selectionEnd; }
    // The selection as a cursor on `target`, clamped in case the TextEdit
    // reported positions from before an edit.
    QTextCursor selectionCursor() const;
    // Every block the selection touches, or the cursor's block.
    QList<QTextBlock> selectedBlocks() const;

    // Set while Rune edits a document itself, so those contentsChange()s
    // aren't taken for typing (see CharFormatter::onContentsChange()).
    bool selfEdit = false;
    [[nodiscard]] QScopedValueRollback<bool> selfEditing() { return QScopedValueRollback<bool>(selfEdit, true); }

    // Bumped on every edit of any document, so a background .docx save and
    // typing's undo steps can tell whether anything changed since.
    quint64 contentRevision = 0;
};
