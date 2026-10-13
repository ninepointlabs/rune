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

#include "PageLayout.h"

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

class QPainter;
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
    // ... and it is an open, which replaces the document when it finishes:
    // the UI should keep the user from editing meanwhile. A .docx save
    // works from a snapshot, so editing carries on while it converts.
    Q_PROPERTY(bool loading READ isLoading NOTIFY busyChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged)
    // Some text is selected (the TextEdit's selection, see setSelection()).
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY formatChanged)
    // A streamed edit (beginStreamedEdit()) is writing into the document;
    // the UI should keep the user from editing meanwhile.
    Q_PROPERTY(bool streamingEdit READ isStreamingEdit NOTIFY streamingEditChanged)
    // Paper size and margins, in millimetres: {width, height, top, bottom,
    // left, right} (PageLayout). Kept in the document's root frame format,
    // so a Page setup change is one undo step like any edit. New documents
    // get the locale's default; opening a file takes the file's own.
    Q_PROPERTY(QVariantMap pageLayout READ pageLayoutMap NOTIFY pageLayoutChanged)
    // The page in pixels (at 96 per inch, the editor's scale), and how many
    // pages the document fills: the editor's page view.
    Q_PROPERTY(QSizeF pageSizePx READ pageSizePx NOTIFY pageLayoutChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pageCountChanged)
    // The header and footer: documents of their own, the same on every page
    // but for their page number fields (PageParts). PagePartView draws them
    // on each page; a TextEdit each edits them in place (setPartDocument()),
    // with its own undo history (the TextEdit's own Ctrl+Z). `dirty` counts
    // their edits too. Heights are of their content, in pixels; each sits
    // centred in its margin, headerYPx / footerYPx below the page's top
    // edge (so a bigger font grows it both ways). Saving writes those
    // distances, so other word processors place them the same.
    Q_PROPERTY(bool hasHeader READ hasHeader NOTIFY pagePartsChanged)
    Q_PROPERTY(bool hasFooter READ hasFooter NOTIFY pagePartsChanged)
    Q_PROPERTY(qreal headerHeightPx READ headerHeightPx NOTIFY pagePartsChanged)
    Q_PROPERTY(qreal footerHeightPx READ footerHeightPx NOTIFY pagePartsChanged)
    Q_PROPERTY(qreal headerYPx READ headerYPx NOTIFY pagePartsChanged)
    Q_PROPERTY(qreal footerYPx READ footerYPx NOTIFY pagePartsChanged)

public:
    explicit DocumentController(QObject *parent = nullptr);

    // Unformatted text, in the body, header and footer: the system's font
    // at this size, the size LibreOffice and Word default to. Saved files
    // name both (Qt's ODF writer writes the default font into every
    // unformatted run; PageParts::toOdf() into every header and footer
    // paragraph), so the text looks the same elsewhere and when reopened.
    // A TextEdit's own font doesn't reach an installed document; this is
    // the size.
    static constexpr qreal kDefaultPointSize = 12;
    ~DocumentController() override;

    QQuickTextDocument *qmlDocument() const { return m_qmlDocument; }
    void setQmlDocument(QQuickTextDocument *qmlDocument);
    QTextDocument *textDocument() const { return m_document; }

    bool isDirty() const
    {
        return m_document->isModified() || m_header->isModified() || m_footer->isModified();
    }
    QString currentPath() const { return m_currentPath; }
    bool isBusy() const { return m_busy; }
    bool isLoading() const { return m_loading; }
    bool canUndo() const { return m_target->isUndoAvailable(); }
    bool canRedo() const { return m_target->isRedoAvailable(); }
    bool hasSelection() const { return m_selectionStart != m_selectionEnd; }
    bool isStreamingEdit() const { return m_streaming; }
    PageLayout pageLayout() const;
    QVariantMap pageLayoutMap() const { return pageLayout().toMap(); }
    QSizeF pageSizePx() const { return pageLayout().sizePx(); }
    int pageCount() const { return m_pageCount; }

    // Page setup: missing values keep the current ones (see
    // PageLayout::fromMap()); the top and bottom margins stay at least
    // tall enough for the header and footer (see fitPageParts()). One undo
    // step; marks the document modified.
    Q_INVOKABLE void setPageLayout(const QVariantMap &layout);

    // The header (`footer` false) or footer document.
    QTextDocument *partDocument(bool footer) const { return footer ? m_footer : m_header; }
    bool hasHeader() const;
    bool hasFooter() const;
    qreal headerHeightPx() const;
    qreal footerHeightPx() const;
    qreal headerYPx() const;
    qreal footerYPx() const;
    // Installs the header or footer document into a TextEdit's document
    // wrapper, as `document` does for the body.
    Q_INVOKABLE void setPartDocument(bool footer, QQuickTextDocument *qmlDocument);
    // Inserts a page number (or with `count`, page count) field at
    // `position` in the header or footer, formatted like the text there;
    // returns the position after it.
    Q_INVOKABLE int insertPageField(bool footer, int position, bool count);
    // Which document the selection (setSelection()), character and
    // alignment formatting, undo/redo and canUndo/canRedo act on: the body
    // (Body), or the header or footer while one is edited in place. Lists,
    // tables, typeText() and streamed edits are the body's only, and do
    // nothing meanwhile. newDocument() and opening go back to the body.
    enum EditTarget { Body, Header, Footer };
    Q_ENUM(EditTarget)
    Q_INVOKABLE void setEditTarget(int target);
    // What fields show in the TextEdits: `page`'s numbers, in `color`.
    Q_INVOKABLE void setPartEditing(int page, const QColor &color);
    // Where the header or footer has grown too tall for its margin (less
    // than 3 mm either side of it), grows the margin to make room, as one
    // undo step (a Page setup change). Called when editing them ends.
    Q_INVOKABLE void fitPageParts();
    // Draws the header or footer as on `page` (1-based), text in `color`
    // unless formatted otherwise; for PagePartView.
    void paintPart(QPainter *painter, bool footer, int page, const QColor &color);
    // Whether this locale uses Letter paper (Page setup shows inches then).
    Q_INVOKABLE bool localeUsesLetter() const;

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
    // included; the UI shouldn't allow editing while `loading`.
    Q_INVOKABLE void openFile(const QString &path);
    Q_INVOKABLE void saveFile(const QString &path);

    // One step of the document's history: typing, a formatting change, a
    // list or table edit. Opening a file or newDocument() clears the
    // history. Moves the cursor to the change (cursorPositionRequested())
    // and drops any pending format. Does nothing while loading or
    // streaming. The TextEdit's own Ctrl+Z bypasses this, so the UI routes
    // its undo keys here.
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Typed text (a printable key press): inserts `text` at the cursor,
    // replacing any selection, in the format typing there would produce
    // (pending format included). Returns false, inserting nothing, while
    // loading or streaming. The UI routes printable keys here instead of letting the
    // TextEdit insert them, to control undo grouping: Qt merges all
    // consecutive same-format typing into one undo step, so Ctrl+Z after
    // a paragraph of typing would remove the whole paragraph. Here a step
    // is a word plus the spaces after it; a new one also starts after the
    // cursor moves, any other edit, replacing a selection, a pending
    // format, or a save. (Input-method commits don't come through here;
    // Qt makes each commit its own step.)
    Q_INVOKABLE bool typeText(const QString &text);

    // The document as Markdown around the selection (or the cursor, where
    // `selection` is ""): what an AI request sends as context.
    struct MarkdownContext
    {
        QString before, selection, after;
    };
    MarkdownContext markdownContext() const;

    // A streamed edit: an AI reply changing the document as it arrives, all
    // of it ONE undo step. Typing, undo and other edits are refused while
    // one is open; nothing changes until the reply's first piece.
    //
    // beginStreamedEdit() opens it on the current selection or cursor
    // (false, doing nothing, while busy or already streaming). Then either:
    //  - New text: appendStreamedText() replaces the selection (first call)
    //    or writes at the cursor, as plain text as it arrives; then
    //    finishStreamedEdit() replaces all of it with `markdown` converted
    //    to formatting (headings, lists, emphasis, tables; a paragraph that
    //    is only a non-breaking space becomes an empty one).
    //  - Changes to existing text: applyStreamedReplacement() for each
    //    (find, replace) pair, within the selection if there was one, else
    //    the whole document; see there.
    // abortStreamedEdit() closes it, keeping what was done, or reverting the
    // document to exactly how it was.
    bool beginStreamedEdit();
    void appendStreamedText(const QString &text);
    void finishStreamedEdit(const QString &markdown);
    // Replaces `find` with `replace` (plain text; "\n" is a paragraph
    // break). `find` is matched exactly, else ignoring Markdown markup and
    // differences in whitespace; the first match at or after the previous
    // replacement wins, else the first in scope. Only the characters that
    // differ are rewritten, so the formatting of the rest is kept; new empty
    // paragraphs are left out of any list. False if `find` isn't there (or
    // spans a table boundary), changing nothing.
    bool applyStreamedReplacement(const QString &find, const QString &replace);
    void abortStreamedEdit(bool keepText);

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
    // The installed font families, as {family, preview}: preview is false
    // when the family can't write its own Latin name (symbol, emoji and
    // non-Latin fonts), so a picker shows that name in the UI font instead.
    Q_INVOKABLE QVariantList fontFamilies() const;

    // Tables. insertTable() clamps rows/columns to 1..50 and puts the
    // cursor in the first cell. Row/column edits act on the cursor's cell
    // and do nothing outside a table; inserting keeps the cursor in its
    // cell; deleting never removes the last row or column (that's
    // deleteTable()). Each is one undo step.
    Q_INVOKABLE void insertTable(int rows, int columns);
    Q_INVOKABLE bool isInTable() const;
    Q_INVOKABLE void insertTableRow();        // below
    Q_INVOKABLE void insertTableRowAbove();
    Q_INVOKABLE void deleteTableRow();
    Q_INVOKABLE void insertTableColumn();     // to the right
    Q_INVOKABLE void insertTableColumnLeft();
    Q_INVOKABLE void deleteTableColumn();
    // The table the cursor is in, contents and all; the cursor goes to
    // where it was.
    Q_INVOKABLE void deleteTable();
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
    void streamingEditChanged();
    void canUndoChanged();
    void canRedoChanged();
    // Completion of openFile() / saveFile(): `path` as passed, `error` ""
    // on success.
    void fileOpened(bool success, const QString &path, const QString &error);
    void fileSaved(bool success, const QString &path, const QString &error);
    // Cursor moved or formatting changed: refresh toolbar state.
    void formatChanged();
    void pageLayoutChanged();
    void pageCountChanged();
    // The header or footer changed: content, height or emptiness.
    void pagePartsChanged();
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

    void editTable(bool rows, bool insert, bool before = false);
    void stepHistory(bool undo);
    // The next typeText() starts a new undo step.
    void endTypingStep() { m_typingEnd = -1; }

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
    void setBusy(bool busy, bool loading = false);
    // Writes `layout` into the root frame format (an undo step of its own).
    void writePageLayout(const PageLayout &layout);
    // The header and footer laid out at `layout`'s text width, with tab
    // stops at its centre and right edge.
    void syncPartWidths(const PageLayout &layout);
    // `layout` with its top and bottom margins grown where the header or
    // footer needs more room.
    PageLayout withRoomForParts(PageLayout layout) const;
    // Empties the header and footer, unmodified with no history.
    void clearParts();
    // Every document (body, header, footer) marked modified or not.
    void setAllModified(bool modified);
    // The styles.xml saveToOdf() writes: page layout, header and footer.
    QByteArray odfStyles() const;
    // After any change: keeps the document paginated at the page size (the
    // TextEdit resets the page height whenever its width changes) and
    // reports layout and page-count changes.
    void syncPages();
    // Runs `work` on the global thread pool; `done` gets its result back on
    // this thread unless this controller is destroyed first.
    template<typename Work, typename Done>
    QFuture<Conversion> runConversion(Work work, Done done);
    void finishOpen(const QString &path, const Conversion &result);

    QTextDocument *m_document;
    QTextDocument *m_header;
    QTextDocument *m_footer;
    QTextDocument *m_target; // see setEditTarget()
    // The page the header and footer TextEdits show fields for, and their
    // color (see setPartEditing()).
    int m_editPage = 1;
    QColor m_editColor = Qt::black;
    QPointer<QQuickTextDocument> m_qmlDocument;
    QString m_currentPath;
    bool m_busy = false;
    bool m_loading = false; // m_busy with an open; see the `loading` property
    // For new documents, and files without a page layout of their own.
    PageLayout m_defaultLayout;
    // What pageLayoutChanged()/pageCountChanged() last reported.
    PageLayout m_reportedLayout;
    int m_pageCount = 1;
    // The streamed edit in progress (see beginStreamedEdit()): its
    // selection when it began, the text it has written so far (start -1:
    // none yet), where the next replacement search starts, the format new
    // text gets, and whether its undo step exists yet.
    bool m_streaming = false;
    int m_streamSelStart = 0;
    int m_streamSelEnd = 0;
    bool m_streamHadSelection = false;
    int m_streamStart = -1;
    int m_streamEnd = -1;
    int m_streamSearchFrom = 0;
    QTextCharFormat m_streamFormat;
    bool m_streamHasStep = false;
    // Opens (or joins) the edit's undo step on `cursor`.
    void openStreamStep(QTextCursor &cursor);
    // The first appendStreamedText()/finishStreamedEdit(): removes the
    // selection and sets where new text goes.
    void startStreamedText();
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
    // The typing undo step typeText() is extending: where its text ends,
    // whether that text ended in whitespace, and m_contentRevision after it
    // (any other edit since starts a new step). -1: none.
    int m_typingEnd = -1;
    bool m_typingEndsInSpace = false;
    quint64 m_typingRevision = 0;

    // Set while the controller edits the document itself, so those
    // contentsChange()s aren't taken for typing.
    bool m_selfEdit = false;
};
