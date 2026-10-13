#pragma once

// DocumentController: owns Rune's QTextDocument and exposes editing and
// save operations to QML.
//
// The QML-facing facade: each feature's logic lives in document/ (character
// formatting, lists, tables, streamed edits, header and footer, page
// layout) and formats/ (reading and writing files); this class routes QML's
// calls to them, keeps the open file's lifecycle (new, open, save, .docx
// conversion, dirty state, current path) and emits what changed.
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
// Selection: the TextEdit reports its cursor/selection via setSelection()
// (see EditState); when an operation must move the cursor (into a new table
// cell, Tab between cells) the controller emits cursorPositionRequested()
// for the TextEdit to apply. Character formatting: see CharFormatter.
// Paragraph formatting (alignment, lists) applies to every block the
// selection touches, or the cursor's block.

#include "CharFormatter.h"
#include "DocumentData.h"
#include "DocumentParts.h"
#include "EditState.h"
#include "Lists.h"
#include "PageLayout.h"
#include "StreamedEdit.h"

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

class HeaderFooter;
class QPainter;

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
    // The header and footer (see HeaderFooter). `dirty` counts their edits
    // too. Heights are of their content, in pixels; headerYPx / footerYPx
    // are below the page's top edge.
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

    bool isDirty() const;
    QString currentPath() const { return m_currentPath; }
    bool isBusy() const { return m_busy; }
    bool isLoading() const { return m_loading; }
    bool canUndo() const { return m_edit.target->isUndoAvailable(); }
    bool canRedo() const { return m_edit.target->isRedoAvailable(); }
    bool hasSelection() const { return m_edit.hasSelection(); }
    bool isStreamingEdit() const { return m_stream.isActive(); }
    PageLayout pageLayout() const;
    QVariantMap pageLayoutMap() const { return pageLayout().toMap(); }
    QSizeF pageSizePx() const { return pageLayout().sizePx(); }
    int pageCount() const { return m_pageCount; }

    // Page setup: missing values keep the current ones (see
    // PageLayout::fromMap()); the top and bottom margins stay at least
    // tall enough for the header and footer (see fitPageParts()). One undo
    // step; marks the document modified.
    Q_INVOKABLE void setPageLayout(const QVariantMap &layout);

    // A part's document (see DocumentParts): "" the body, "header",
    // "footer"; nullptr for no such part.
    QTextDocument *partDocument(const QString &part) const;
    bool hasHeader() const;
    bool hasFooter() const;
    qreal headerHeightPx() const;
    qreal footerHeightPx() const;
    qreal headerYPx() const;
    qreal footerYPx() const;
    // Installs the header or footer document into a TextEdit's document
    // wrapper, as `document` does for the body.
    Q_INVOKABLE void setPartDocument(const QString &part, QQuickTextDocument *qmlDocument);
    // Inserts a page number (or with `count`, page count) field at
    // `position` in the header or footer, formatted like the text there;
    // returns the position after it.
    Q_INVOKABLE int insertPageField(const QString &part, int position, bool count);
    // Which part (see partDocument()) the selection (setSelection()),
    // character and alignment formatting, undo/redo and canUndo/canRedo act
    // on: the body (""), or the header or footer while one is edited in
    // place. Lists, tables, typeText() and streamed edits act only where the
    // part's kind allows them (DocumentParts::capabilities(); the body
    // only), and do nothing elsewhere. newDocument() and opening go back to
    // the body. An unknown part changes nothing.
    Q_INVOKABLE void setEditTarget(const QString &part);
    // What fields show in the TextEdits: `page`'s numbers, in `color`.
    Q_INVOKABLE void setPartEditing(int page, const QColor &color);
    // Where the header or footer has grown too tall for its margin (less
    // than 3 mm either side of it), grows the margin to make room, as one
    // undo step (a Page setup change). Called when editing them ends.
    Q_INVOKABLE void fitPageParts();
    // Draws the header or footer as on `page` (1-based), text in `color`
    // unless formatted otherwise; for PagePartView.
    void paintPart(QPainter *painter, const QString &part, int page, const QColor &color);
    // What the document carries besides its text: named JSON stores (see
    // DocumentData), saved in the file. Changing one marks the document
    // modified; opening a file replaces them, newDocument() empties them.
    DocumentData *data() const { return m_data; }

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
    using MarkdownContext = StreamedEdit::MarkdownContext;
    MarkdownContext markdownContext() const { return m_stream.markdownContext(); }

    // A streamed edit: an AI reply changing the document as it arrives (see
    // StreamedEdit). Typing, undo and other edits are refused while one is
    // open. beginStreamedEdit() is false, doing nothing, while busy, already
    // streaming, or editing the header or footer.
    bool beginStreamedEdit();
    void appendStreamedText(const QString &text);
    void finishStreamedEdit(const QString &markdown);
    // False if `find` isn't there, changing nothing.
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

    // Lists (see Lists).
    Q_INVOKABLE void toggleBulletList();
    Q_INVOKABLE void toggleNumberedList();
    Q_INVOKABLE bool isInBulletList() const;
    Q_INVOKABLE bool isInNumberedList() const;
    // One nesting level out / in. Promoting a top-level item does nothing.
    Q_INVOKABLE void promoteListItem();
    Q_INVOKABLE void demoteListItem();

    // Font and color (see CharFormatter).
    Q_INVOKABLE void setFontFamily(const QString &family);
    Q_INVOKABLE QString currentFontFamily() const;
    Q_INVOKABLE void setFontSize(qreal pointSize);
    Q_INVOKABLE qreal currentFontSize() const;
    // "#RRGGBB", or "auto" to remove any explicit color.
    Q_INVOKABLE void setTextColor(const QString &hexColor);
    Q_INVOKABLE QString currentTextColor() const;
    // The installed font families (see CharFormatter::installedFamilies()).
    Q_INVOKABLE QVariantList fontFamilies() const;

    // Tables (see Tables). insertTable() clamps rows/columns to 1..50 and puts the
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
    void toggle(CharFormatter::Attr attr);
    // Emits formatChanged(), and cursorPositionRequested() for `position`
    // unless it is -1.
    void moved(int position);
    void onContentsChange(int position, int removed, int added);
    // Emits dirtyChanged() when isDirty() differs from what was last reported.
    void updateDirty();
    // Whether the edit target's kind allows `capability` (see
    // setEditTarget()).
    bool allows(DocumentParts::Capability capability) const;
    void toggleList(Lists::Kind kind);
    void changeListLevel(int delta);
    void editTable(bool rows, bool insert, bool before = false);
    void stepHistory(bool undo);

    // After openOdf()/saveToOdf() of a temporary .odt ran with signals
    // blocked: point currentPath at the real file and emit what changed
    // since `oldPath` (and the dirty state, via updateDirty()).
    void finishConvertedIo(const QString &path, const QString &oldPath);

    // Result of a background soffice run.
    struct Conversion
    {
        QString odt; // convertToOdt()'s output, for an open
        DocumentData::Stores data; // the .docx's data stores, for an open
        QString error; // "" on success
    };
    void setBusy(bool busy, bool loading = false);
    // Writes `layout` into the root frame format (an undo step of its own).
    void writePageLayout(const PageLayout &layout);
    // Every document (body, header, footer) marked modified or not.
    void setAllModified(bool modified);
    // After any change: keeps the document paginated at the page size (the
    // TextEdit resets the page height whenever its width changes) and
    // reports layout and page-count changes.
    void syncPages();
    // Runs `work` on the global thread pool; `done` gets its result back on
    // this thread unless this controller is destroyed first.
    template<typename Work, typename Done>
    QFuture<Conversion> runConversion(Work work, Done done);
    void finishOpen(const QString &path, const Conversion &result);

    DocumentParts *m_parts;
    QTextDocument *m_document; // the body
    HeaderFooter *m_margins;
    DocumentData *m_data;
    EditState m_edit;
    CharFormatter m_format;
    StreamedEdit m_stream;
    QPointer<QQuickTextDocument> m_qmlDocument;
    QString m_currentPath;
    bool m_busy = false;
    bool m_loading = false; // m_busy with an open; see the `loading` property
    // For new documents, and files without a page layout of their own.
    PageLayout m_defaultLayout;
    // What pageLayoutChanged()/pageCountChanged() last reported.
    PageLayout m_reportedLayout;
    int m_pageCount = 1;
    // isDirty() as last reported by dirtyChanged(); see updateDirty().
    bool m_dirty = false;
    // The open conversion in flight, if any, so the destructor can still
    // remove its temporary .odt.
    QFuture<Conversion> m_pendingOpen;
};
