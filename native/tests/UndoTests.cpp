// Undo / redo: every controller operation is exactly one undo step.

#include "TestSupport.h"

namespace TestSupport {

void testUndo(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;
    auto *boldButton = window->findChild<QQuickItem *>(QStringLiteral("boldButton"));

    // Undo / redo. Every controller operation must be exactly one undo
    // step: one undo() restores the document as it was, one redo()
    // reapplies it.
    {
        auto *undoButton = window->findChild<QQuickItem *>(QStringLiteral("undoButton"));
        auto *redoButton = window->findChild<QQuickItem *>(QStringLiteral("redoButton"));
        const auto available = [](QQuickItem *button) { return button && button->property("available").toBool(); };
        const auto select = [&](int start, int end) {
            QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, end));
        };
        const auto oneStep = [&](const QString &what, const std::function<void()> &op) {
            const QString before = doc->toHtml();
            op();
            const QString after = doc->toHtml();
            controller->undo();
            const bool restored = doc->toHtml() == before;
            controller->redo();
            const bool reapplied = doc->toHtml() == after;
            t.check(after != before && restored && reapplied,
                    QStringLiteral("one undo step: %1 (changed %2, undo restores %3, redo reapplies %4)")
                        .arg(what).arg(after != before).arg(restored).arg(reapplied));
        };

        controller->newDocument();
        editor->forceActiveFocus();
        t.check(!controller->canUndo() && !controller->canRedo() && !available(undoButton) && !available(redoButton),
                "new document: nothing to undo or redo, both buttons unavailable");

        typeText(window, QStringLiteral("One"));
        pressReturn(window);
        typeText(window, QStringLiteral("Two"));
        pressReturn(window);
        typeText(window, QStringLiteral("Three"));
        t.check(controller->canUndo() && available(undoButton) && !available(redoButton),
                "after typing: Undo available, Redo not");

        // Undo/redo move the cursor (collapsing any selection), so each
        // operation selects what it acts on.
        oneStep(QStringLiteral("bold on a selection"), [&] { select(0, 3); controller->toggleBold(); });
        controller->undo();
        t.check(formatOf(doc, 0, 3).isEmpty() && !boldButton->property("active").toBool()
                    && controller->canRedo() && available(redoButton),
                "undoing bold: text plain again, toolbar Bold off, Redo available");
        controller->redo();
        t.check(formatOf(doc, 0, 3) == QStringLiteral("B"), "redo reapplies bold");

        const auto selectAll = [&] { select(0, doc->characterCount() - 1); };
        oneStep(QStringLiteral("alignment over three paragraphs"), [&] { selectAll(); controller->setAlignment(Qt::AlignHCenter); });
        oneStep(QStringLiteral("text color"), [&] { selectAll(); controller->setTextColor(QStringLiteral("#cc0000")); });
        oneStep(QStringLiteral("text color back to automatic"), [&] { selectAll(); controller->setTextColor(QStringLiteral("auto")); });
        oneStep(QStringLiteral("bullet list over three paragraphs"), [&] { selectAll(); controller->toggleBulletList(); });
        oneStep(QStringLiteral("nesting a list item"), [&] { select(4, 7); controller->demoteListItem(); });
        oneStep(QStringLiteral("un-nesting it"), [&] { select(4, 7); controller->promoteListItem(); });
        oneStep(QStringLiteral("removing the list"), [&] { selectAll(); controller->toggleBulletList(); });

        const auto inLastCell = [&] {
            QTextTable *table = nullptr;
            for (auto it = doc->rootFrame()->begin(); !it.atEnd() && !table; ++it)
                table = qobject_cast<QTextTable *>(it.currentFrame());
            if (!table)
                return;
            const int last = table->cellAt(table->rows() - 1, table->columns() - 1).firstCursorPosition().position();
            select(last, last);
        };
        const int end = doc->characterCount() - 1;
        oneStep(QStringLiteral("inserting a table"), [&] { select(end, end); controller->insertTable(2, 2); });
        oneStep(QStringLiteral("inserting a table row"), [&] { inLastCell(); controller->insertTableRow(); });
        oneStep(QStringLiteral("deleting a table column"), [&] { inLastCell(); controller->deleteTableColumn(); });
        oneStep(QStringLiteral("inserting a table row above"), [&] { inLastCell(); controller->insertTableRowAbove(); });
        oneStep(QStringLiteral("inserting a table column left"), [&] { inLastCell(); controller->insertTableColumnLeft(); });
        oneStep(QStringLiteral("Tab off the last cell (appends a row)"), [&] { inLastCell(); controller->nextTableCell(); });
        oneStep(QStringLiteral("deleting the table"), [&] { inLastCell(); controller->deleteTable(); });

        // A pending format is applied to the typed text in a second edit;
        // undoing the typing must take it in the same step.
        controller->newDocument();
        typeText(window, QStringLiteral("Plain "));
        controller->toggleBold();
        const QString beforePending = doc->toHtml();
        typeText(window, QStringLiteral("b"));
        const bool pendingApplied = formatOf(doc, 6, 7) == QStringLiteral("B");
        controller->undo();
        t.check(pendingApplied && doc->toHtml() == beforePending,
                QStringLiteral("one undo removes a character typed with a pending bold, format and all (applied %1, text now \"%2\")")
                    .arg(pendingApplied).arg(doc->toPlainText()));
        controller->redo();
        t.check(doc->toPlainText() == QStringLiteral("Plain b") && formatOf(doc, 6, 7) == QStringLiteral("B"),
                "... and one redo brings it back bold");
        controller->newDocument();
        typeText(window, QStringLiteral("Plain "));
        controller->toggleBold();
        const QString beforeBc = doc->toHtml();
        typeText(window, QStringLiteral("bc"));
        const bool continued = doc->toPlainText() == QStringLiteral("Plain bc") && formatOf(doc, 6, 8) == QStringLiteral("B");
        controller->undo();
        t.check(continued && doc->toHtml() == beforeBc,
                "typing on in bold extends the same step: one undo removes \"bc\"");

        // Input-method commits aren't key presses: the pending format is
        // still applied, after insertion (a second undo step there). Qt
        // reports such a commit as its whole paragraph replaced.
        controller->newDocument();
        typeText(window, QStringLiteral("Plain "));
        controller->toggleBold();
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("q"));
        QCoreApplication::sendEvent(editor, &commit);
        t.check(doc->toPlainText() == QStringLiteral("Plain q") && formatOf(doc, 6, 7) == QStringLiteral("B"),
                "an input-method commit with a pending bold is bold: \"" + doc->toPlainText() + "\" " + formatOf(doc, 6, 7));

        // Typing undoes a word at a time (a word plus the spaces after it),
        // not a whole paragraph: Qt alone merges all of it into one step.
        {
            const auto undoAll = [&](int max) {
                QStringList states{doc->toPlainText()};
                for (int i = 0; i < max && controller->canUndo(); ++i) {
                    controller->undo();
                    states.append(doc->toPlainText());
                }
                return states;
            };
            controller->newDocument();
            editor->forceActiveFocus();
            typeText(window, QStringLiteral("The quick  brown fox."));
            const QStringList steps = undoAll(10);
            const QStringList expected{QStringLiteral("The quick  brown fox."), QStringLiteral("The quick  brown "),
                                       QStringLiteral("The quick  "), QStringLiteral("The "), QString()};
            t.check(steps == expected, "typing undoes word by word: " + steps.join(QStringLiteral(" | ")));
            while (controller->canRedo())
                controller->redo();
            t.check(doc->toPlainText() == QStringLiteral("The quick  brown fox."), "... and redoes back to the full text");

            // A second paragraph: Return is its own step, then words again.
            pressReturn(window);
            typeText(window, QStringLiteral("Next line"));
            controller->undo();
            const bool lineWord = doc->toPlainText() == QStringLiteral("The quick  brown fox.\nNext ");
            controller->undo();
            controller->undo();
            t.check(lineWord && doc->toPlainText() == QStringLiteral("The quick  brown fox."),
                    "after Return: words undo one at a time, then the paragraph break: \"" + doc->toPlainText() + "\"");

            // Moving the cursor starts a new step, even mid-word.
            controller->newDocument();
            typeText(window, QStringLiteral("abcdef"));
            select(3, 3);
            typeText(window, QStringLiteral("XY"));
            controller->undo();
            t.check(doc->toPlainText() == QStringLiteral("abcdef"),
                    "text typed after moving the cursor is its own step: \"" + doc->toPlainText() + "\"");
            controller->undo();
            t.check(doc->toPlainText().isEmpty(), "... and the earlier word is another");

            // Typing over a selection: replacing and the new word are one step.
            controller->newDocument();
            typeText(window, QStringLiteral("a quick fox"));
            select(2, 7);
            typeText(window, QStringLiteral("slow"));
            const bool replaced = doc->toPlainText() == QStringLiteral("a slow fox");
            controller->undo();
            t.check(replaced && doc->toPlainText() == QStringLiteral("a quick fox"),
                    "one undo after typing over a selection restores the selected text: \"" + doc->toPlainText() + "\"");

            // Backspace (the TextEdit's own edit) splits typing too.
            controller->newDocument();
            typeText(window, QStringLiteral("wordx"));
            pressKey(window, Qt::Key_Backspace, Qt::NoModifier, QString());
            typeText(window, QStringLiteral("s"));
            controller->undo();
            const bool sGone = doc->toPlainText() == QStringLiteral("word");
            controller->undo();
            t.check(sGone && doc->toPlainText() == QStringLiteral("wordx"),
                    "a Backspace between keystrokes splits the step: \"" + doc->toPlainText() + "\"");

            // Holding a key down: one long word is still one step.
            controller->newDocument();
            typeText(window, QString(200, QLatin1Char('z')));
            controller->undo();
            t.check(doc->isEmpty(), "200 repeated keystrokes in one word: one undo removes them");
        }

        // Undo back to the saved state is clean again, title included.
        // Mid-word ("Plain bc|"), so the save must end the typing step.
        controller->newDocument();
        typeText(window, QStringLiteral("Plain bc"));
        const IoRun saveRun = saveAndWait(controller, kUndoPath);
        typeText(window, QStringLiteral("x"));
        const bool dirtyAfterEdit = controller->isDirty();
        controller->undo();
        t.check(saveRun.success && dirtyAfterEdit && !controller->isDirty()
                    && !window->property("title").toString().contains(QChar(0x25CF)),
                "undoing back to the last save leaves the document clean (title too)" + errorNote(saveRun));
        controller->redo();
        t.check(controller->isDirty(), "redoing past the save makes it dirty again");

        // History doesn't survive New or Open: undo mustn't unbuild a load.
        controller->newDocument();
        t.check(!controller->canUndo() && !controller->canRedo() && !available(undoButton) && !available(redoButton),
                QStringLiteral("New clears the history and both buttons (canUndo %1, canRedo %2, buttons %3/%4)")
                    .arg(controller->canUndo()).arg(controller->canRedo())
                    .arg(available(undoButton)).arg(available(redoButton)));
        typeText(window, QStringLiteral("y"));
        const IoRun reopen = openAndWait(controller, kUndoPath);
        t.check(reopen.success && !controller->canUndo() && !controller->canRedo() && !available(undoButton),
                "opening a file clears the history" + errorNote(reopen));

        // Real Ctrl+Z / Ctrl+Shift+Z through the platform input path: one
        // step per press, whether the editor or something else has focus.
        controller->newDocument();
        editor->forceActiveFocus();
        typeText(window, QStringLiteral("abc"));
        select(0, 3);
        controller->toggleItalic();
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier);
        const bool keyUndo = doc->toPlainText() == QStringLiteral("abc") && formatOf(doc, 0, 3).isEmpty();
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        t.check(keyUndo && formatOf(doc, 0, 3) == QStringLiteral("I"),
                "Ctrl+Z / Ctrl+Shift+Z in the editor undo and redo exactly one step");
        window->contentItem()->forceActiveFocus();
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier);
        const bool unfocusedUndo = formatOf(doc, 0, 3).isEmpty() && doc->toPlainText() == QStringLiteral("abc");
        pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        t.check(unfocusedUndo && formatOf(doc, 0, 3) == QStringLiteral("I"),
                "... and with focus outside the editor (window shortcuts)");
        editor->forceActiveFocus();

        // Busy: a .docx open is about to replace the document; undo waits.
        const QString beforeBusy = doc->toHtml();
        bool undoneWhileBusy = true;
        const IoRun busyOpen = runIo(controller, &DocumentController::fileOpened, [&] {
            controller->openFile(kSampleDocx);
            controller->undo();
            undoneWhileBusy = doc->toHtml() != beforeBusy;
        });
        t.check(busyOpen.success && busyOpen.busyAfterCall && !undoneWhileBusy,
                "undo does nothing while a .docx open converts" + errorNote(busyOpen));
    }
}

} // namespace TestSupport
