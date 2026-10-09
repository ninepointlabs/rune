#pragma once

// DocumentController: owns Rune's QTextDocument and exposes editing and
// save operations to QML.
//
//   TextEdit { id: editor; textFormat: TextEdit.RichText }
//   DocumentController { id: controller }
//   Component.onCompleted: controller.document = editor.textDocument
//
// Ownership: the controller creates and owns the QTextDocument and is the
// source of truth. Assigning a TextEdit's textDocument wrapper to `document`
// installs the controller's document into that TextEdit via
// QQuickTextDocument::setTextDocument() (Qt 6.7+); the document Qt created
// for the TextEdit is then unused. Assign from Component.onCompleted rather
// than a binding so the TextEdit's own initialisation (text, textFormat)
// has already run and can't reset the installed document.
//
// Selection: QML can't hand C++ a QTextCursor, so the TextEdit reports its
// cursor/selection via setSelection() and the controller builds a QTextCursor
// on its own document on demand; when an operation must move the cursor
// (into a new table cell, Tab between cells) the controller emits
// cursorPositionRequested() for the TextEdit to apply. Character formatting (toggles, font,
// size, color) with a collapsed cursor inside a word formats that word;
// elsewhere it arms a pending format that is applied to the next text typed
// at that position. Paragraph formatting (alignment, lists) applies to every
// block the selection touches, or the cursor's block.

#include <QFuture>
#include <QObject>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QList>
#include <QPointer>
#include <QString>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextListFormat>
#include <QTextTableFormat>

class QTextList;

class DocumentController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ qmlDocument WRITE setQmlDocument NOTIFY documentChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY currentPathChanged)
    // A .docx open/save is converting in the background.
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged)

public:
    explicit DocumentController(QObject *parent = nullptr);
    ~DocumentController() override;

    QQuickTextDocument *qmlDocument() const { return m_qmlDocument; }
    void setQmlDocument(QQuickTextDocument *qmlDocument);
    QTextDocument *textDocument() const { return m_document; }

    bool isDirty() const { return m_document->isModified(); }
    QString currentPath() const { return m_currentPath; }
    bool isBusy() const { return m_busy; }
    bool canUndo() const { return m_document->isUndoAvailable(); }
    bool canRedo() const { return m_document->isRedoAvailable(); }

    Q_INVOKABLE void newDocument();
    Q_INVOKABLE bool saveToOdf(const QString &path);
    // Replaces the document with the .odt at `path` (see OdfReader for what
    // is read). On failure the current document is left as it was.
    Q_INVOKABLE bool openOdf(const QString &path);
    // Open / save by extension (case-insensitive): .odt goes straight to
    // openOdf() / saveToOdf(); .docx and .doc (open) and .docx (save) go
    // through a temporary .odt converted by LibreOffice (see DocxBridge).
    // Anything else fails.
    //
    // Asynchronous: the result arrives as fileOpened() / fileSaved(). For
    // .odt (and any failure detected up front) that signal is emitted
    // before the call returns and `busy` never changes. For .docx/.doc the
    // soffice run happens on Qt's global thread pool: `busy` is true from
    // the call until just before the signal, which is emitted later from
    // the event loop. While busy, any further openFile()/saveFile() fails
    // at once ("already in progress") without touching the operation in
    // flight.
    //
    // Signals, dirty state and currentPath (the .docx's, never the
    // temporary's) end up as openOdf()/saveToOdf() leave them; on failure
    // the document, its dirty state and currentPath are unchanged. A .docx
    // save snapshots the document when called: edits made while it
    // converts leave the document dirty afterwards. A .docx open replaces
    // the document when its conversion finishes, edits made meanwhile
    // included; the UI shouldn't allow editing while busy.
    Q_INVOKABLE void openFile(const QString &path);
    Q_INVOKABLE void saveFile(const QString &path);

    // One step of the document's history: typing, a formatting change, a
    // list or table edit. Opening a file or newDocument() clears the
    // history. Moves the cursor to the change (cursorPositionRequested())
    // and drops any pending format. The TextEdit's own Ctrl+Z bypasses
    // this, so the UI routes its undo keys here.
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Typing with a pending format armed (see the class comment): inserts
    // `text` at the cursor in that format and returns true, else inserts
    // nothing and returns false, for the TextEdit to handle the key itself.
    // Formatting text already inserted would be a second undo step after
    // the typing; this way it is one. (Input-method commits, which aren't
    // key presses, still get the pending format applied after insertion.)
    Q_INVOKABLE bool typeWithPendingFormat(const QString &text);

    // Mirror of the TextEdit's cursorPosition/selectionStart/selectionEnd.
    Q_INVOKABLE void setSelection(int cursorPosition, int selectionStart, int selectionEnd);

    Q_INVOKABLE void toggleBold();
    Q_INVOKABLE void toggleItalic();
    Q_INVOKABLE void toggleUnderline();
    Q_INVOKABLE bool isBold() const;
    Q_INVOKABLE bool isItalic() const;
    Q_INVOKABLE bool isUnderline() const;

    // Alignment of the selected blocks; currentAlignment() is the cursor
    // block's horizontal alignment (Qt::AlignLeft/HCenter/Right/Justify).
    Q_INVOKABLE void setAlignment(Qt::Alignment alignment);
    Q_INVOKABLE int currentAlignment() const;

    // Lists. Nested bullet levels use disc/circle/square, all of which count
    // as "bullet"; numbered lists are decimal at every level.
    Q_INVOKABLE void toggleBulletList();
    Q_INVOKABLE void toggleNumberedList();
    Q_INVOKABLE bool isInBulletList() const;
    Q_INVOKABLE bool isInNumberedList() const;
    // One nesting level out / in. Promoting a top-level item does nothing.
    Q_INVOKABLE void promoteListItem();
    Q_INVOKABLE void demoteListItem();

    // Font and color: queries report the first selected character, or what
    // typing at the cursor would produce; unset values report the
    // document's default font and "auto".
    Q_INVOKABLE void setFontFamily(const QString &family);
    Q_INVOKABLE QString currentFontFamily() const;
    Q_INVOKABLE void setFontSize(qreal pointSize);
    Q_INVOKABLE qreal currentFontSize() const;
    // "#RRGGBB", or "auto" to remove any explicit color.
    Q_INVOKABLE void setTextColor(const QString &hexColor);
    Q_INVOKABLE QString currentTextColor() const;

    // Tables. insertTable() clamps rows/columns to 1..50 and puts the
    // cursor in the first cell. Row/column edits act on the cursor's cell
    // and do nothing outside a table; deleting never removes the last row
    // or column (Qt would delete the whole table).
    Q_INVOKABLE void insertTable(int rows, int columns);
    Q_INVOKABLE bool isInTable() const;
    Q_INVOKABLE void insertTableRow();
    Q_INVOKABLE void deleteTableRow();
    Q_INVOKABLE void insertTableColumn();
    Q_INVOKABLE void deleteTableColumn();
    // Tab / Shift+Tab: cursor to the next / previous cell in reading order.
    // Next from the last cell appends a row; previous from the first does
    // nothing.
    Q_INVOKABLE void nextTableCell();
    Q_INVOKABLE void previousTableCell();

    // Rewrites `doc` so QTextDocumentWriter's ODF output is well-formed.
    // Every ODF save must go through this; see the .cpp for each rule.
    static void sanitizeForOdfExport(QTextDocument *doc);

    // How lists and tables are represented, shared with OdfReader so an
    // opened document has the structure the UI would have built.
    enum class ListKind { None, Bullet, Numbered };
    // The QTextListFormat style for a list of `kind` at nesting `indent` (1-based).
    static QTextListFormat::Style listStyle(ListKind kind, int indent);
    static QTextTableFormat tableFormat();

signals:
    void documentChanged();
    void dirtyChanged();
    void currentPathChanged();
    void busyChanged();
    void canUndoChanged();
    void canRedoChanged();
    // Completion of openFile() / saveFile(): `path` as passed, `error` ""
    // on success.
    void fileOpened(bool success, const QString &path, const QString &error);
    void fileSaved(bool success, const QString &path, const QString &error);
    // Cursor moved or formatting changed: refresh toolbar state.
    void formatChanged();
    // The controller moved the cursor (e.g. into a new table cell); the
    // TextEdit should set its cursorPosition to `position`.
    void cursorPositionRequested(int position);

private:
    enum class Attr { Bold, Italic, Underline };

    static bool hasAttr(const QTextCharFormat &f, Attr attr);
    static void setAttr(QTextCharFormat &f, Attr attr, bool on);
    QTextCursor selectionCursor() const;
    bool attrState(Attr attr) const;
    void toggle(Attr attr);
    void applyCharFormat(const QTextCharFormat &change, bool clearForeground = false);
    QTextCharFormat currentCharFormat() const;
    void clearPending();
    void onContentsChange(int position, int removed, int added);
    // Emits dirtyChanged() when isDirty() differs from what was last reported.
    void updateDirty();

    static ListKind listKind(const QTextList *list);
    QList<QTextBlock> selectedBlocks() const;
    ListKind currentListKind() const;
    void toggleList(ListKind kind);
    void changeListLevel(int delta);

    void editTable(bool rows, bool insert);
    void stepHistory(bool undo);

    // After openOdf()/saveToOdf() of a temporary .odt ran with signals
    // blocked: point currentPath at the real file and emit what changed
    // since `oldPath` (and the dirty state, via updateDirty()).
    void finishConvertedIo(const QString &path, const QString &oldPath);

    // Result of a background soffice run.
    struct Conversion
    {
        QString odt; // convertToOdt()'s output, for an open
        QString error; // "" on success
    };
    void setBusy(bool busy);
    // Runs `work` on the global thread pool; `done` gets its result back on
    // this thread unless this controller is destroyed first.
    template<typename Work, typename Done>
    QFuture<Conversion> runConversion(Work work, Done done);
    void finishOpen(const QString &path, const Conversion &result);

    QTextDocument *m_document;
    QPointer<QQuickTextDocument> m_qmlDocument;
    QString m_currentPath;
    bool m_busy = false;
    // isDirty() as last reported by dirtyChanged(); see updateDirty().
    bool m_dirty = false;
    // Bumped on every document edit, so a background .docx save can tell
    // whether what it wrote is still what's in the document.
    quint64 m_contentRevision = 0;
    // The open conversion in flight, if any, so the destructor can still
    // remove its temporary .odt.
    QFuture<Conversion> m_pendingOpen;

    int m_cursorPosition = 0;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;

    // Format armed by a toggle with no selection and no word to apply it
    // to; merged onto the next insertion at m_pendingPosition. A pending
    // "auto" color can't be expressed as a merge, hence the separate flag.
    QTextCharFormat m_pendingFormat;
    bool m_pendingClearForeground = false;
    int m_pendingPosition = -1;
    // Set while the controller edits the document itself, so those
    // contentsChange()s aren't taken for typing.
    bool m_selfEdit = false;
};
