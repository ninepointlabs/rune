// Typing, character formatting (bold/italic/underline, pending formats), the
// ODF export sanitizer, alignment, lists, font and colour.

#include "TestSupport.h"

namespace TestSupport {

void testFormatting(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;
    auto *boldButton = window->findChild<QQuickItem *>(QStringLiteral("boldButton"));

    // Typing: real key events must land in the controller's document.
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Hello bold world"));
    t.check(doc->toPlainText() == QStringLiteral("Hello bold world"),
            QStringLiteral("typed text reached the document: \"%1\"").arg(doc->toPlainText()));
    t.check(controller->isDirty(), "document is dirty after typing");

    // Bold on a selection, via the same path the toolbar uses.
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 6), Q_ARG(int, 10));
    t.check(!controller->isBold(), "selection \"bold\" reports not bold before toggle");
    controller->toggleBold();
    t.check(controller->isBold(), "isBold() true after toggleBold()");
    t.check(boldButton->property("active").toBool(), "toolbar Bold button highlighted");
    t.check(formatOf(doc, 6, 10) == "B", QStringLiteral("\"bold\" is bold (%1)").arg(formatOf(doc, 6, 10)));
    t.check(formatOf(doc, 0, 6).isEmpty() && formatOf(doc, 10, 16).isEmpty(), "surrounding text unformatted");

    // Collapsed cursor at the end: italic arms a pending format for typing.
    editor->setProperty("cursorPosition", doc->characterCount() - 1);
    t.check(!boldButton->property("active").toBool(), "Bold button off after moving cursor to plain text");
    controller->toggleItalic();
    t.check(controller->isItalic(), "isItalic() true for pending format at collapsed cursor");
    typeText(window, QStringLiteral(" tail"));
    t.check(formatOf(doc, 16, 21) == "I", QStringLiteral("text typed after toggleItalic() is italic (%1)").arg(formatOf(doc, 16, 21)));
    t.check(formatOf(doc, 11, 16).isEmpty(), "\"world\" not italic");

    // Collapsed cursor inside a word: underline applies to the word.
    editor->setProperty("cursorPosition", 2);
    controller->toggleUnderline();
    t.check(formatOf(doc, 0, 5) == "U", QStringLiteral("cursor inside \"Hello\" underlines the word (%1)").arg(formatOf(doc, 0, 5)));
    controller->toggleUnderline();
    t.check(formatOf(doc, 0, 5).isEmpty(), "second toggle removes the underline");

    const bool saved = controller->saveToOdf(kStage1Path);
    t.check(saved, "saveToOdf(" + kStage1Path + ")");
    t.check(!controller->isDirty(), "clean after save");
    t.check(controller->currentPath() == kStage1Path, "currentPath = " + controller->currentPath());
    QTextStream(stdout) << "  final text: \"" << doc->toPlainText() << "\"" << Qt::endl;

    // Export sanitizer: list directly followed by a table (the native-spike
    // bug). Exercised on a scratch controller; Stage 1 has no table UI.
    DocumentController scratch;
    QTextDocument *sdoc = scratch.textDocument();
    sdoc->setHtml(QStringLiteral("<ul><li>Alpha</li><li>Beta</li></ul>"
                                 "<table border=\"1\"><tr><td>Cell A</td><td>Cell B</td></tr></table>"
                                 "<p>After</p>"));
    const QTextBlock before = blockBeforeFirstTable(sdoc);
    t.check(before.isValid() && before.textList(), "scratch doc: list block directly precedes table");
    const int blocksBefore = sdoc->blockCount();
    t.check(scratch.saveToOdf(kSanitizePath), "saveToOdf(" + kSanitizePath + ")");
    t.check(sdoc->blockCount() == blocksBefore, "save left the live document unchanged");

    std::unique_ptr<QTextDocument> copy(sdoc->clone());
    OdfWriter::sanitize(copy.get());
    const QTextBlock separator = blockBeforeFirstTable(copy.get());
    t.check(separator.isValid() && !separator.textList() && separator.text().isEmpty(),
            "sanitized copy: empty non-list paragraph between list and table");
    t.check(separator.isValid() && separator.previous().textList(), "sanitized copy: list still ends right before it");

    // --- Stage 2: alignment, lists, font, color ---
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Centered paragraph"));
    editor->setProperty("cursorPosition", 3);
    controller->setAlignment(Qt::AlignHCenter);
    t.check(controller->currentAlignment() == int(Qt::AlignHCenter), "currentAlignment() reports center after setAlignment");
    // Repeater-generated children aren't reliably found via findChild; check
    // the bound property the toolbar buttons read from instead.
    t.check(window->property("alignment").toInt() == int(Qt::AlignHCenter), "toolbar alignment state reflects center");
    t.check(controller->saveToOdf(kStage2AlignPath), "saveToOdf(" + kStage2AlignPath + ") [alignment]");

    // Bullet list: 3 items, then demote the middle one.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Alpha"));
    controller->toggleBulletList();
    t.check(controller->isInBulletList(), "isInBulletList() true after toggleBulletList()");
    auto *bulletButton = window->findChild<QQuickItem *>(QStringLiteral("bulletButton"));
    t.check(bulletButton && bulletButton->property("active").toBool(), "toolbar bullet button highlighted");
    pressReturn(window);
    typeText(window, QStringLiteral("Beta"));
    pressReturn(window);
    typeText(window, QStringLiteral("Gamma"));
    t.check(doc->toPlainText() == QStringLiteral("Alpha\nBeta\nGamma"),
            QStringLiteral("3-item list text: \"%1\"").arg(doc->toPlainText()));
    t.check(controller->isInBulletList(), "still in bullet list after newlines");

    // Demote "Beta" (middle item) one level.
    const int betaPos = doc->toPlainText().indexOf(QStringLiteral("Beta")) + 1;
    editor->setProperty("cursorPosition", betaPos);
    QTextCursor betaCursor(doc);
    betaCursor.setPosition(betaPos);
    const int indentBefore = betaCursor.block().textList() ? betaCursor.block().textList()->format().indent() : -1;
    controller->demoteListItem();
    QTextCursor betaAfter(doc);
    betaAfter.setPosition(betaPos);
    const int indentAfter = betaAfter.block().textList() ? betaAfter.block().textList()->format().indent() : -1;
    t.check(indentAfter > indentBefore,
            QStringLiteral("demoteListItem() increased indent (%1 -> %2)").arg(indentBefore).arg(indentAfter));
    t.check(controller->saveToOdf(kStage2ListPath), "saveToOdf(" + kStage2ListPath + ") [list]");

    // Font family and size on a word.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Plain word"));
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
    controller->setFontFamily(QStringLiteral("Serif"));
    t.check(controller->currentFontFamily() == QStringLiteral("Serif"),
            QStringLiteral("currentFontFamily() = \"%1\"").arg(controller->currentFontFamily()));
    controller->setFontSize(24);
    t.check(qFuzzyCompare(controller->currentFontSize(), 24.0),
            QStringLiteral("currentFontSize() = %1").arg(controller->currentFontSize()));
    t.check(controller->saveToOdf(kStage2FontPath), "saveToOdf(" + kStage2FontPath + ") [font]");

    // Text color: set, then clear back to auto.
    QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 6), Q_ARG(int, 10));
    controller->setTextColor(QStringLiteral("#cc0000"));
    t.check(controller->currentTextColor() == QStringLiteral("#cc0000"),
            QStringLiteral("currentTextColor() = \"%1\"").arg(controller->currentTextColor()));
    controller->setTextColor(QStringLiteral("auto"));
    t.check(controller->currentTextColor() == QStringLiteral("auto"),
            QStringLiteral("currentTextColor() after clearing = \"%1\"").arg(controller->currentTextColor()));
    // Re-apply color for the saved file so the export check below has something to find.
    controller->setTextColor(QStringLiteral("#cc0000"));
    t.check(controller->saveToOdf(kStage2ColorPath), "saveToOdf(" + kStage2ColorPath + ") [color]");
}

} // namespace TestSupport
