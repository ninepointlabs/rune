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
// on its own document on demand. Character formatting (toggles, font,
// size, color) with a collapsed cursor inside a word formats that word;
// elsewhere it arms a pending format that is applied to the next text typed
// at that position. Paragraph formatting (alignment, lists) applies to every
// block the selection touches, or the cursor's block.

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

class QTextList;

class DocumentController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ qmlDocument WRITE setQmlDocument NOTIFY documentChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY currentPathChanged)

public:
    explicit DocumentController(QObject *parent = nullptr);
    ~DocumentController() override;

    QQuickTextDocument *qmlDocument() const { return m_qmlDocument; }
    void setQmlDocument(QQuickTextDocument *qmlDocument);
    QTextDocument *textDocument() const { return m_document; }

    bool isDirty() const { return m_document->isModified(); }
    QString currentPath() const { return m_currentPath; }

    Q_INVOKABLE void newDocument();
    Q_INVOKABLE bool saveToOdf(const QString &path);

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

    // Rewrites `doc` so QTextDocumentWriter's ODF output is well-formed.
    // Every ODF save must go through this; see the .cpp for each rule.
    static void sanitizeForOdfExport(QTextDocument *doc);

signals:
    void documentChanged();
    void dirtyChanged();
    void currentPathChanged();
    // Cursor moved or formatting changed: refresh toolbar state.
    void formatChanged();

private:
    enum class Attr { Bold, Italic, Underline };
    enum class ListKind { None, Bullet, Numbered };

    static bool hasAttr(const QTextCharFormat &f, Attr attr);
    static void setAttr(QTextCharFormat &f, Attr attr, bool on);
    QTextCursor selectionCursor() const;
    bool attrState(Attr attr) const;
    void toggle(Attr attr);
    void applyCharFormat(const QTextCharFormat &change, bool clearForeground = false);
    QTextCharFormat currentCharFormat() const;
    void clearPending();
    void onContentsChange(int position, int removed, int added);

    static ListKind listKind(const QTextList *list);
    static QTextListFormat::Style listStyle(ListKind kind, int indent);
    QList<QTextBlock> selectedBlocks() const;
    ListKind currentListKind() const;
    void toggleList(ListKind kind);
    void changeListLevel(int delta);

    QTextDocument *m_document;
    QPointer<QQuickTextDocument> m_qmlDocument;
    QString m_currentPath;

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
