// Main.qml: the file flow (dialogs, Save, the unsaved-changes guard),
// autosave, the command palette, the toolbar and closing the window.

#include "TestSupport.h"

namespace TestSupport {

void testWindow(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;

    // File flow in Main.qml: Open/Save As dialogs, Save to the current
    // path, the unsaved-changes guard on New/Open/close. File dialogs can't
    // be driven headlessly, so those checks call what their accepted
    // handlers call (acceptSave, openPath); the unsaved-changes dialog gets
    // its real buttonClicked() signal (clickMessageButton()).
    {
        const QStringList dialogs{QStringLiteral("openDialog"), QStringLiteral("saveDialog"),
                                  QStringLiteral("discardDialog")};
        t.check(std::all_of(dialogs.begin(), dialogs.end(),
                            [&](const QString &n) { return window->findChild<QObject *>(n) != nullptr; }),
                "found openDialog, saveDialog and discardDialog");
        // Qt 6.11's compiled QML resolved these enums to undefined, leaving
        // only an OK button (see UnsavedChangesDialog.qml).
        const int discardButtons = window->findChild<QObject *>(QStringLiteral("discardDialog"))->property("buttons").toInt();
        t.check(discardButtons == (kSaveButton | kDiscardButton | kCancelButton),
                QStringLiteral("unsaved-changes dialog has Save, Discard and Cancel buttons (0x%1)").arg(discardButtons, 0, 16));
        const auto anyDialog = [&] {
            return std::any_of(dialogs.begin(), dialogs.end(), [&](const QString &n) { return dialogVisible(window, n); });
        };
        const auto waitSaved = [&](const std::function<void()> &start) {
            return runIo(controller, &DocumentController::fileSaved, start);
        };
        // What QML sees: the title binding only re-reads `dirty` on dirtyChanged().
        const auto titleDirty = [&] { return window->property("title").toString().contains(QChar(0x25CF)); };

        // QTextDocument::clear() resets the modified flag without emitting
        // modificationChanged(); New must still clear the title's marker.
        controller->newDocument();
        typeText(window, QStringLiteral("x"));
        const bool markedAfterTyping = titleDirty();
        controller->newDocument();
        t.check(markedAfterTyping && !titleDirty(),
                "title marks unsaved changes, and New clears the mark: \"" + window->property("title").toString() + "\"");

        controller->newDocument();
        doc->setPlainText(QStringLiteral("Dialog text"));
        doc->setModified(true);
        callRoot(window, "save");
        t.check(dialogVisible(window, QStringLiteral("saveDialog")) && !controller->isBusy(),
                "save() on an untitled document opens Save As instead of writing anywhere");
        closeDialog(window, QStringLiteral("saveDialog"));

        QFile::remove(kDialogOdtPath);
        const IoRun asRun = waitSaved([&] { callRoot(window, "acceptSave", kDialogBasePath); });
        t.check(asRun.success && QFileInfo::exists(kDialogOdtPath) && controller->currentPath() == kDialogOdtPath
                    && !controller->isDirty(),
                "Save As with no extension writes .odt (the first filter) and becomes the current path"
                    + errorNote(asRun));

        QTextCursor(doc).insertText(QStringLiteral("More "));
        const QDateTime before = QFileInfo(kDialogOdtPath).lastModified();
        const IoRun quick = waitSaved([&] { callRoot(window, "save"); });
        t.check(quick.success && !anyDialog() && quick.path == kDialogOdtPath && !controller->isDirty()
                    && QFileInfo(kDialogOdtPath).lastModified() >= before,
                "save() with a current .odt writes it directly, no dialog" + errorNote(quick));

        QFile::remove(kDialogDocxPath);
        const IoRun docxRun = waitSaved([&] { callRoot(window, "acceptSave", kDialogDocxPath); });
        t.check(docxRun.success && controller->currentPath() == kDialogDocxPath && !controller->isDirty(),
                "Save As .docx goes through the bridge and becomes the current path" + errorNote(docxRun));

        // The toolbar's File menu: a real click opens it; its items run the
        // same functions as the shortcuts, and are off while a file loads.
        {
            auto *menuButton = window->findChild<QQuickItem *>(QStringLiteral("fileMenuButton"));
            QObject *menu = window->findChild<QObject *>(QStringLiteral("fileMenu"));
            const QStringList itemNames{QStringLiteral("newMenuItem"), QStringLiteral("openMenuItem"),
                                        QStringLiteral("saveMenuItem"), QStringLiteral("saveAsMenuItem")};
            QList<QObject *> items;
            for (const QString &n : itemNames)
                items.append(window->findChild<QObject *>(n));
            t.check(menuButton && menu && !items.contains(nullptr), "found the File menu and New/Open/Save/Save As");
            if (menuButton && menu && !items.contains(nullptr)) {
                const auto item = [&](const QString &n) { return items.at(itemNames.indexOf(n)); };
                const auto trigger = [&](const QString &n) {
                    QMetaObject::invokeMethod(item(n), "click");
                    QCoreApplication::processEvents();
                };
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  menuButton->mapToScene(QPointF(menuButton->width() / 2, menuButton->height() / 2)).toPoint());
                QCoreApplication::processEvents();
                t.check(menu->property("visible").toBool(), "clicking the Rune/File button opens the File menu");

                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                QCoreApplication::processEvents();
                const bool escClosed = !menu->property("visible").toBool();
                pressSystemKey(window, Qt::Key_F, Qt::AltModifier);
                t.check(escClosed && menu->property("visible").toBool() && item(QStringLiteral("newMenuItem"))->property("highlighted").toBool(),
                        "Escape closes the File menu; Alt+F opens it with New highlighted");
                auto *saveButton = window->findChild<QQuickItem *>(QStringLiteral("saveButton"));
                t.check(saveButton && saveButton->isVisible(), "the toolbar keeps a one-click Save button next to the File menu");

                trigger(QStringLiteral("saveAsMenuItem"));
                t.check(!menu->property("visible").toBool() && dialogVisible(window, QStringLiteral("saveDialog")),
                        "File → Save As… closes the menu and opens Save As");
                closeDialog(window, QStringLiteral("saveDialog"));

                trigger(QStringLiteral("openMenuItem"));
                t.check(dialogVisible(window, QStringLiteral("openDialog")), "File → Open… opens the Open dialog");
                closeDialog(window, QStringLiteral("openDialog"));

                bool enabledWhileBusy = true;
                const IoRun busyRun = runIo(controller, &DocumentController::fileOpened, [&] {
                    controller->openFile(kDialogDocxPath);
                    enabledWhileBusy = std::any_of(items.begin(), items.end(),
                                                   [](QObject *i) { return i->property("enabled").toBool(); });
                });
                t.check(busyRun.success && busyRun.busyAfterCall && !enabledWhileBusy
                            && std::all_of(items.begin(), items.end(), [](QObject *i) { return i->property("enabled").toBool(); }),
                        "File menu items are disabled while a .docx opens, enabled after" + errorNote(busyRun));

                trigger(QStringLiteral("newMenuItem"));
                t.check(!anyDialog() && doc->isEmpty() && controller->currentPath().isEmpty() && !controller->isDirty(),
                        "File → New on a saved document: empty and untitled, no prompt");
            }
        }

        // Autosave, and the Save button showing where the document stands.
        // The timer isn't waited out: autosaveNow() is what it calls.
        {
            auto *saveButton = window->findChild<QQuickItem *>(QStringLiteral("saveButton"));
            QObject *timer = window->findChild<QObject *>(QStringLiteral("autosaveTimer"));
            QObject *autosaveItem = window->findChild<QObject *>(QStringLiteral("autosaveMenuItem"));
            t.check(saveButton && timer && autosaveItem, "found the Save button, autosave timer and File → Autosave");
            if (saveButton && timer && autosaveItem) {
                const auto label = [&] { return saveButton->property("label").toString(); };
                const auto timerRunning = [&] { return timer->property("running").toBool(); };
                const auto flash = [&] { return window->property("statusFlash").toString(); };
                const auto edit = [&](const QString &text) {
                    editor->forceActiveFocus();
                    typeText(window, text);
                    QCoreApplication::processEvents();
                };
                const QString saved = QStringLiteral("✓ Saved");
                QDir(kAutosaveDir).removeRecursively();
                QDir().mkpath(kAutosaveDir);
                window->setProperty("autosaveEnabled", true);

                controller->newDocument();
                edit(QStringLiteral("untitled "));
                t.check(controller->isDirty() && !timerRunning() && label() == QLatin1String("Save"),
                        "an untitled document doesn't autosave; the button reads Save (\"" + label() + "\")");

                const IoRun first = waitSaved([&] { callRoot(window, "acceptSave", kAutosaveOdtPath); });
                t.check(first.success && label() == saved,
                        "saved as .odt with autosave on: the button reads ✓ Saved (\"" + label() + "\")" + errorNote(first));

                edit(QStringLiteral("more "));
                t.check(controller->isDirty() && timerRunning() && timer->property("interval").toInt() == 5000
                            && label() == QLatin1String("Save"),
                        "an edit starts the 5 s autosave timer; the button reads Save");
                QFile::remove(kAutosaveOdtPath);
                window->setProperty("statusFlash", QString());
                const IoRun odtRun = waitSaved([&] { callRoot(window, "autosaveNow"); });
                t.check(odtRun.success && odtRun.path == kAutosaveOdtPath && QFileInfo::exists(kAutosaveOdtPath)
                            && !controller->isDirty() && label() == saved && flash().isEmpty() && !timerRunning(),
                        "autosave writes the .odt: ✓ Saved, no status-bar message" + errorNote(odtRun));

                // A failed autosave shows on the button, and the next save clears it.
                QDir(kAutosaveDir).removeRecursively();
                edit(QStringLiteral("lost "));
                const IoRun failRun = waitSaved([&] { callRoot(window, "autosaveNow"); });
                t.check(!failRun.success && controller->isDirty() && label() == QLatin1String("Save failed")
                            && flash().startsWith(QLatin1String("Autosave failed")),
                        "a failed autosave: the button reads Save failed, the status bar says why (\"" + flash() + "\")");
                QDir().mkpath(kAutosaveDir);
                const IoRun retry = waitSaved([&] { QMetaObject::invokeMethod(saveButton, "clicked"); });
                t.check(retry.success && !controller->isDirty() && label() == saved,
                        "clicking Save failed retries; success clears it" + errorNote(retry));

                // .docx: a longer pause, and typing carries on while it converts.
                const IoRun toDocx = waitSaved([&] { callRoot(window, "acceptSave", kAutosaveDocxPath); });
                edit(QStringLiteral("docx "));
                t.check(toDocx.success && timerRunning() && timer->property("interval").toInt() == 30000,
                        "a .docx document autosaves after a 30 s pause" + errorNote(toDocx));
                QString labelWhileSaving;
                bool editableWhileSaving = false, typedWhileSaving = false;
                const IoRun docxRun = waitSaved([&] {
                    callRoot(window, "autosaveNow");
                    labelWhileSaving = label();
                    editableWhileSaving = !editor->property("readOnly").toBool();
                    typedWhileSaving = controller->typeText(QStringLiteral("during "));
                });
                t.check(docxRun.success && docxRun.busyAfterCall && labelWhileSaving == QStringLiteral("Saving…"),
                        "autosaving a .docx: the button reads Saving… (\"" + labelWhileSaving + "\")" + errorNote(docxRun));
                t.check(editableWhileSaving && typedWhileSaving && controller->isDirty()
                            && doc->toPlainText().contains(QStringLiteral("during")) && label() == QLatin1String("Save"),
                        "typing works while a .docx autosaves, and leaves the document unsaved afterwards");

                // File → Autosave turns it off and on.
                QMetaObject::invokeMethod(autosaveItem, "click");
                QCoreApplication::processEvents();
                edit(QStringLiteral("off "));
                t.check(!window->property("autosaveEnabled").toBool() && !timerRunning() && label() == QLatin1String("Save"),
                        "File → Autosave off: edits start no timer; the button reads Save");
                QMetaObject::invokeMethod(autosaveItem, "click");
                QCoreApplication::processEvents();
                t.check(window->property("autosaveEnabled").toBool() && timerRunning(),
                        "File → Autosave back on with unsaved changes starts the timer");

                window->setProperty("autosaveEnabled", false);
                timer->setProperty("running", false);
                controller->newDocument();
            }
        }

        // The toolbar's font and color pickers, driven with real clicks and keys.
        {
            auto *fontButton = window->findChild<QQuickItem *>(QStringLiteral("fontButton"));
            auto *colorButton = window->findChild<QQuickItem *>(QStringLiteral("colorButton"));
            QObject *fontPopup = window->findChild<QObject *>(QStringLiteral("fontPopup"));
            QObject *colorPopup = window->findChild<QObject *>(QStringLiteral("colorPopup"));
            auto *fontList = window->findChild<QQuickItem *>(QStringLiteral("fontList"));
            QObject *customField = window->findChild<QObject *>(QStringLiteral("colorCustomField"));
            t.check(fontButton && colorButton && fontPopup && colorPopup && fontList && customField,
                    "found the font and color pickers");
            if (fontButton && colorButton && fontPopup && colorPopup && fontList && customField) {
                const auto clickItem = [&](QQuickItem *item) {
                    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                      item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
                    QCoreApplication::processEvents();
                };
                const auto shown = [](QObject *popup) { return popup->property("visible").toBool(); };
                const auto listed = [&] {
                    QStringList families;
                    for (const QVariant &f : fontList->property("model").toList())
                        families.append(f.toMap().value(QStringLiteral("family")).toString());
                    return families;
                };

                controller->newDocument();
                editor->forceActiveFocus();
                typeText(window, QStringLiteral("Picked words"));
                QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 6));

                // Fonts: every installed family, each name in its own font.
                const QStringList installed = QFontDatabase::families();
                clickItem(fontButton);
                const QStringList all = listed();
                t.check(shown(fontPopup) && !installed.isEmpty() && all == installed,
                        QStringLiteral("the font button lists the system's %1 font families").arg(all.size()));
                auto *current = fontList->property("currentItem").value<QQuickItem *>();
                const QQuickItem *name = current && !current->childItems().isEmpty() ? current->childItems().first() : nullptr;
                t.check(name && name->property("text").toString() == name->property("font").value<QFont>().family(),
                        "each font's name is written in that font (\"" + (name ? name->property("text").toString() : QString()) + "\")");
                // Symbol, emoji and non-Latin fonts would draw their own name
                // as symbols or tofu: those are shown in the UI font.
                int previews = 0, plain = 0;
                bool flagsRight = true;
                for (const QVariant &v : controller->fontFamilies()) {
                    const QVariantMap f = v.toMap();
                    const auto systems = QFontDatabase::writingSystems(f.value(QStringLiteral("family")).toString());
                    const bool latin = systems.contains(QFontDatabase::Latin) && !systems.contains(QFontDatabase::Symbol);
                    flagsRight = flagsRight && f.value(QStringLiteral("preview")).toBool() == latin;
                    ++(latin ? previews : plain);
                }
                t.check(flagsRight && previews > 0,
                        QStringLiteral("fonts that can't write their Latin name are listed in the UI font (%1 previewed, %2 not)")
                            .arg(previews).arg(plain));

                typeText(window, QStringLiteral("mono"));
                const QStringList monos = listed();
                const bool allMatch = std::all_of(monos.begin(), monos.end(),
                                                  [](const QString &f) { return f.contains(QStringLiteral("mono"), Qt::CaseInsensitive); });
                t.check(!monos.isEmpty() && monos.size() < all.size() && allMatch,
                        QStringLiteral("typing filters the list: \"mono\" leaves %1").arg(monos.size()));
                pressKey(window, Qt::Key_Down, Qt::NoModifier, QString());
                const QString picked = monos.value(qMin(1, int(monos.size()) - 1));
                pressReturn(window);
                t.check(!shown(fontPopup) && controller->currentFontFamily() == picked && editor->hasActiveFocus(),
                        "Down then Enter picks the second match for the selection and returns to the document (\""
                            + controller->currentFontFamily() + "\")");

                clickItem(fontButton);
                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                QCoreApplication::processEvents();
                t.check(!shown(fontPopup) && controller->currentFontFamily() == picked,
                        "Escape closes the font list without changing the font");

                // Colors.
                clickItem(colorButton);
                auto *paletteItem = colorPopup->property("contentItem").value<QQuickItem *>();
                auto *red = findItem(paletteItem, QStringLiteral("swatch#cc0000"));
                t.check(shown(colorPopup) && red, "the color button opens a palette");
                if (red)
                    clickItem(red);
                t.check(!shown(colorPopup) && controller->currentTextColor() == QStringLiteral("#cc0000")
                            && editor->hasActiveFocus(),
                        "clicking a swatch colors the selection and closes the palette (\"" + controller->currentTextColor() + "\")");

                clickItem(colorButton);
                customField->setProperty("text", QStringLiteral("12zz56"));
                QMetaObject::invokeMethod(customField, "forceActiveFocus");
                pressReturn(window);
                t.check(shown(colorPopup) && controller->currentTextColor() == QStringLiteral("#cc0000"),
                        "an invalid custom color is ignored, the palette stays open");
                customField->setProperty("text", QStringLiteral("1A2b3C"));
                pressReturn(window);
                t.check(!shown(colorPopup) && controller->currentTextColor() == QStringLiteral("#1a2b3c"),
                        "a custom #RRGGBB (with or without #) colors the selection (\"" + controller->currentTextColor() + "\")");

                clickItem(colorButton);
                auto *automatic = findItem(paletteItem, QStringLiteral("colorAutomatic"));
                if (automatic)
                    clickItem(automatic);
                t.check(automatic && !shown(colorPopup) && controller->currentTextColor() == QStringLiteral("auto"),
                        "Automatic removes the color");
                controller->newDocument();
            }
        }

        // The toolbar at different window widths: groups move into the ⋯
        // menu (and back) so nothing runs under the AI button.
        {
            auto *aiButton = window->findChild<QQuickItem *>(QStringLiteral("aiButton"));
            auto *historyGroup = window->findChild<QQuickItem *>(QStringLiteral("historyGroup"));
            auto *toolRow = historyGroup ? historyGroup->parentItem() : nullptr;
            auto *overflowButton = window->findChild<QQuickItem *>(QStringLiteral("overflowButton"));
            QObject *overflowMenu = window->findChild<QObject *>(QStringLiteral("overflowMenu"));
            QObject *alignCenter = window->findChild<QObject *>(QStringLiteral("alignCenterItem"));
            QObject *colorPopup = window->findChild<QObject *>(QStringLiteral("colorPopup"));
            t.check(aiButton && toolRow && overflowButton && overflowMenu && alignCenter && colorPopup,
                    "found the toolbar groups, ⋯ menu and alignment menu");
            if (aiButton && toolRow && overflowButton && overflowMenu && alignCenter && colorPopup) {
                const auto collapsed = [&] { return window->property("collapsedGroups").toStringList(); };
                const auto rightEdge = [&] {
                    qreal right = 0;
                    for (QQuickItem *group : toolRow->childItems())
                        if (group->isVisible())
                            right = qMax(right, group->mapToScene(QPointF(group->width(), 0)).x());
                    return right;
                };
                const auto fits = [&] { return rightEdge() <= aiButton->mapToScene(QPointF(0, 0)).x(); };
                // Whether a ⋯ entry is listed (an entry only counts as visible
                // while its menu is open).
                const auto shown = [&](const char *name) {
                    QMetaObject::invokeMethod(overflowButton, "openMenu");
                    QCoreApplication::processEvents();
                    QObject *o = window->findChild<QObject *>(QLatin1String(name));
                    const bool visible = o && o->property("visible").toBool();
                    QMetaObject::invokeMethod(overflowMenu, "close");
                    QCoreApplication::processEvents();
                    return visible;
                };
                const auto resize = [&](int width) {
                    window->setWidth(width);
                    QElapsedTimer settle;
                    settle.start();
                    while (settle.elapsed() < 100)
                        QCoreApplication::processEvents();
                };
                const auto edges = [&] {
                    return QStringLiteral("toolbar ends at %1, AI starts at %2")
                        .arg(rightEdge()).arg(aiButton->mapToScene(QPointF(0, 0)).x());
                };
                const int originalWidth = window->width();

                controller->newDocument();
                editor->forceActiveFocus();
                typeText(window, QStringLiteral("Aligned"));
                QMetaObject::invokeMethod(alignCenter, "click");
                t.check(controller->currentAlignment() == int(Qt::AlignHCenter)
                            && window->property("alignment").toInt() == int(Qt::AlignHCenter)
                            && alignCenter->property("hint").toString() == QStringLiteral("✓"),
                        "the alignment menu centers the paragraph and ticks Align center");

                // How much fits depends on the fonts installed; 1400 px fits
                // everything with any of them.
                resize(1400);
                t.check(collapsed().isEmpty() && !overflowButton->isVisible() && fits(),
                        "at 1400 px every group fits and there's no ⋯ button (" + edges() + ")");
                resize(1000);
                t.check(fits() && overflowButton->isVisible() == !collapsed().isEmpty(),
                        "at 1000 px nothing runs under the AI button (in ⋯: " + collapsed().join(QStringLiteral(", "))
                            + "; " + edges() + ")");

                resize(760);
                const QStringList at760 = collapsed();
                t.check(!at760.isEmpty() && at760.first() == QStringLiteral("table") && overflowButton->isVisible() && fits()
                            && shown("overflowInsertTable") && shown("overflowUndo") == at760.contains(QStringLiteral("history")),
                        "at 760 px the lowest-priority groups move into ⋯ (" + at760.join(QStringLiteral(", ")) + "; " + edges() + ")");

                resize(window->minimumWidth());
                const QStringList atMin = collapsed();
                t.check(atMin.size() == 6 && fits() && window->findChild<QQuickItem *>(QStringLiteral("boldButton"))->isVisible()
                            && window->findChild<QQuickItem *>(QStringLiteral("fontButton"))->isVisible()
                            && window->findChild<QQuickItem *>(QStringLiteral("saveButton"))->isVisible(),
                        QStringLiteral("at the minimum width (%1 px) all six groups are in ⋯; File, Save, B/I/U and font stay (%2)")
                            .arg(window->minimumWidth()).arg(edges()));

                QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("overflowNumbered")), "click");
                t.check(controller->isInNumberedList() && shown("overflowUndo") && shown("overflowAlignRight"),
                        "⋯ → Numbered list works, and ⋯ lists every collapsed group's actions");
                QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("overflowColor")), "click");
                QCoreApplication::processEvents();
                t.check(colorPopup->property("visible").toBool(), "⋯ → Text color… opens the palette");
                QMetaObject::invokeMethod(colorPopup, "close");
                QCoreApplication::processEvents();

                resize(1400);
                t.check(collapsed().isEmpty() && !overflowButton->isVisible() && fits(),
                        "widening the window again puts every group back on the toolbar (" + edges() + ")");
                resize(originalWidth);
                controller->newDocument();
            }
        }

        // Ctrl+K: the command palette, driven with real keys from the document.
        {
            QObject *palette = window->findChild<QObject *>(QStringLiteral("commandPalette"));
            auto *paletteList = window->findChild<QQuickItem *>(QStringLiteral("paletteList"));
            QObject *fontPopup = window->findChild<QObject *>(QStringLiteral("fontPopup"));
            t.check(palette && paletteList && fontPopup, "found the command palette");
            if (palette && paletteList && fontPopup) {
                const auto open = [&] { return palette->property("visible").toBool(); };
                const auto names = [&] {
                    QStringList found;
                    for (const QVariant &r : palette->property("results").toList())
                        found.append(r.toMap().value(QStringLiteral("command")).toMap().value(QStringLiteral("name")).toString());
                    return found;
                };
                const auto firstAvailable = [&] {
                    const QVariantList results = palette->property("results").toList();
                    return !results.isEmpty() && results.first().toMap().value(QStringLiteral("available")).toBool();
                };
                const auto settle = [&] {
                    QElapsedTimer w;
                    w.start();
                    while (w.elapsed() < 50)
                        QCoreApplication::processEvents();
                };

                controller->newDocument();
                editor->forceActiveFocus();
                typeText(window, QStringLiteral("Palette text"));
                QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 7));
                pressSystemKey(window, Qt::Key_K, Qt::ControlModifier);
                const int all = names().size();
                t.check(open() && all >= 30 && doc->toPlainText() == QStringLiteral("Palette text"),
                        QStringLiteral("Ctrl+K in the document opens the palette with every command (%1), deleting nothing").arg(all));

                typeText(window, QStringLiteral("bold"));
                t.check(names().value(0) == QStringLiteral("Bold") && firstAvailable(),
                        "typing \"bold\" puts Bold first (" + names().join(QStringLiteral(", ")) + ")");
                pressReturn(window);
                settle();
                t.check(!open() && controller->isBold() && editor->hasActiveFocus(),
                        "Enter runs it: the selection is bold, and typing goes back to the document");

                pressSystemKey(window, Qt::Key_K, Qt::ControlModifier);
                typeText(window, QStringLiteral("row above"));
                t.check(names() == QStringList{QStringLiteral("Insert row above")} && !firstAvailable(),
                        "outside a table, \"row above\" finds Insert row above, unavailable");
                pressReturn(window);
                settle();
                t.check(open() && !controller->isInTable(), "Enter on an unavailable command does nothing; the palette stays open");
                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                settle();
                t.check(!open() && editor->hasActiveFocus(), "Escape closes the palette");

                pressSystemKey(window, Qt::Key_K, Qt::ControlModifier);
                typeText(window, QStringLiteral("tab ins"));
                const QStringList tableMatches = names();
                pressKey(window, Qt::Key_Down, Qt::NoModifier, QString());
                pressReturn(window);
                settle();
                t.check(tableMatches.value(0) == QStringLiteral("Insert table") && !controller->isInTable(),
                        "\"tab ins\" matches across group and name, and Down moves past Insert table to an unavailable command ("
                            + tableMatches.join(QStringLiteral(", ")) + ")");
                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                settle();

                pressSystemKey(window, Qt::Key_K, Qt::ControlModifier);
                typeText(window, QStringLiteral("font"));
                pressReturn(window);
                settle();
                t.check(!open() && fontPopup->property("visible").toBool(), "Font… opens the font list");
                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                settle();

                const bool autosaveBefore = window->property("autosaveEnabled").toBool();
                pressSystemKey(window, Qt::Key_K, Qt::ControlModifier);
                typeText(window, QStringLiteral("autosave"));
                pressReturn(window);
                settle();
                t.check(window->property("autosaveEnabled").toBool() != autosaveBefore,
                        "\"autosave\" toggles autosave");
                window->setProperty("autosaveEnabled", autosaveBefore);

                pressSystemKey(window, Qt::Key_K, Qt::ControlModifier);
                typeText(window, QStringLiteral("zzqx"));
                t.check(names().isEmpty(), "no command matches \"zzqx\"");
                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                settle();

                QObject *fileEntry = window->findChild<QObject *>(QStringLiteral("commandPaletteMenuItem"));
                if (fileEntry)
                    QMetaObject::invokeMethod(fileEntry, "click");
                settle();
                t.check(fileEntry && open() && fileEntry->property("hint").toString() == QStringLiteral("Ctrl+K"),
                        "File → Command palette… (Ctrl+K) opens the palette");
                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                settle();
                controller->newDocument();
            }
        }

        // About Rune: from the command palette and File, with the real
        // license, the version, the support address and the privacy note.
        {
            QObject *about = window->findChild<QObject *>(QStringLiteral("aboutDialog"));
            QObject *palette = window->findChild<QObject *>(QStringLiteral("commandPalette"));
            t.check(about && palette, "found the About dialog");
            if (about && palette) {
                const auto settle = [] {
                    QElapsedTimer w;
                    w.start();
                    while (w.elapsed() < 50)
                        QCoreApplication::processEvents();
                };
                const auto shownAbout = [&] { return about->property("visible").toBool(); };
                auto *contentItem = about->property("contentItem").value<QQuickItem *>();
                const auto text = [&](const char *name) {
                    QQuickItem *item = contentItem ? findItem(contentItem, QString::fromLatin1(name)) : nullptr;
                    return item ? item->property("text").toString() : QString();
                };

                editor->forceActiveFocus();
                pressSystemKey(window, Qt::Key_K, Qt::ControlModifier);
                typeText(window, QStringLiteral("about"));
                pressReturn(window);
                settle();
                t.check(shownAbout(), "Ctrl+K → \"about\" opens About Rune");
                t.check(text("aboutVersion") == QStringLiteral("Version " RUNE_VERSION),
                        "About shows the version (" + text("aboutVersion") + ")");

                QFile license(QStringLiteral(RUNE_SAMPLES_DIR "/../LICENSE"));
                const QString repoLicense = license.open(QIODevice::ReadOnly | QIODevice::Text)
                    ? QString::fromUtf8(license.readAll()).simplified() : QStringLiteral("(LICENSE not found)");
                QQuickItem *licenseText = contentItem ? findItem(contentItem, QStringLiteral("aboutLicenseText")) : nullptr;
                const bool hiddenFirst = licenseText && !licenseText->isVisible();
                about->setProperty("showLicense", true);
                settle();
                t.check(hiddenFirst && licenseText->isVisible()
                            && text("aboutLicenseText").startsWith(QStringLiteral("MIT License"))
                            && text("aboutLicenseText").simplified() == repoLicense,
                        "Read it shows the license, the repo's LICENSE word for word");

                const QString privacy = text("aboutPrivacy");
                t.check(privacy.contains(QStringLiteral("no telemetry")) && privacy.contains(QStringLiteral("keyring"))
                            && privacy.contains(QStringLiteral("OpenRouter")),
                        "About states what leaves the machine");
                const bool checked = waitUntil([&] { return !text("aboutBuiltWith").contains(QStringLiteral("checking")); }, 30000);
                const QString builtWith = text("aboutBuiltWith");
                t.check(checked && builtWith.startsWith(QStringLiteral("Qt ") + QString::fromLatin1(qVersion()))
                            && builtWith.contains(QStringLiteral("LibreOffice")),
                        "About lists what Rune is built with (" + builtWith + ")");

                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                settle();
                t.check(!shownAbout() && editor->hasActiveFocus(), "Escape closes About, back to the document");

                QObject *fileEntry = window->findChild<QObject *>(QStringLiteral("aboutMenuItem"));
                if (fileEntry)
                    QMetaObject::invokeMethod(fileEntry, "click");
                settle();
                t.check(fileEntry && shownAbout() && !about->property("showLicense").toBool(),
                        "File → About Rune opens it again, license folded");
                pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
                settle();
            }
        }

        // The unsaved-changes guard.
        QTextCursor(doc).insertText(QStringLiteral("Unsaved "));
        callRoot(window, "requestOpen");
        t.check(dialogVisible(window, QStringLiteral("discardDialog")) && !dialogVisible(window, QStringLiteral("openDialog")),
                "Open with unsaved changes asks first");
        t.check(clickMessageButton(window, QStringLiteral("discardDialog"), kCancelButton, kRejectRole), "clicked Cancel");
        t.check(!anyDialog() && controller->isDirty() && doc->toPlainText().startsWith(QStringLiteral("Unsaved")),
                "Cancel keeps the document and opens nothing");

        callRoot(window, "requestNew");
        clickMessageButton(window, QStringLiteral("discardDialog"), kDiscardButton, kDestructiveRole);
        t.check(doc->isEmpty() && !controller->isDirty() && !titleDirty() && controller->currentPath().isEmpty(),
                "New → Discard: empty, clean (title too), untitled");

        doc->setPlainText(QStringLiteral("Save then new"));
        doc->setModified(true);
        callRoot(window, "requestNew");
        clickMessageButton(window, QStringLiteral("discardDialog"), kSaveButton, kAcceptRole);
        t.check(dialogVisible(window, QStringLiteral("saveDialog")) && !doc->isEmpty(),
                "New → Save on an untitled document goes through Save As first");
        QFile::remove(kDialogOdtPath);
        const IoRun thenNew = waitSaved([&] { callRoot(window, "acceptSave", kDialogOdtPath); });
        closeDialog(window, QStringLiteral("saveDialog"));
        t.check(thenNew.success && doc->isEmpty() && controller->currentPath().isEmpty(),
                "... and the new document follows the successful save" + errorNote(thenNew));

        doc->setPlainText(QStringLiteral("Dropped"));
        doc->setModified(true);
        callRoot(window, "requestNew");
        clickMessageButton(window, QStringLiteral("discardDialog"), kSaveButton, kAcceptRole);
        closeDialog(window, QStringLiteral("saveDialog"));
        QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("saveDialog")), "reject");
        t.check(window->property("pendingAction").isNull() && doc->toPlainText() == QStringLiteral("Dropped"),
                "cancelling that Save As drops the pending New");

        // Open: .odt directly, .doc via the bridge, then Save must not try
        // to write a .doc.
        controller->newDocument();
        const IoRun openOdt = runIo(controller, &DocumentController::fileOpened,
                                    [&] { callRoot(window, "openPath", kDialogOdtPath); });
        t.check(openOdt.success && doc->toPlainText() == QStringLiteral("Save then new")
                    && controller->currentPath() == kDialogOdtPath && !titleDirty(),
                "openPath() opens the .odt Save As wrote" + errorNote(openOdt));
        bool readOnlyWhileBusy = false;
        const IoRun openDoc = runIo(controller, &DocumentController::fileOpened, [&] {
            callRoot(window, "openPath", kSampleDoc);
            readOnlyWhileBusy = editor->property("readOnly").toBool();
        });
        t.check(openDoc.success && openDoc.busyAfterCall && !titleDirty()
                    && controller->currentPath() == QFileInfo(kSampleDoc).absoluteFilePath(),
                "openPath() opens a .doc through the bridge" + errorNote(openDoc));
        t.check(readOnlyWhileBusy && !editor->property("readOnly").toBool(),
                "editor is read-only while the conversion runs, editable once it finishes");
        callRoot(window, "save");
        t.check(dialogVisible(window, QStringLiteral("saveDialog")) && !controller->isBusy(),
                "save() on a .doc opens Save As (.doc can't be written)");
        const QString suggested = window->findChild<QObject *>(QStringLiteral("saveDialog"))
                                      ->property("selectedFile").toUrl().toLocalFile();
        t.check(suggested == QFileInfo(kSampleDoc).absolutePath() + QStringLiteral("/test.odt"),
                "... suggesting the same name as .odt beside it: " + suggested);
        closeDialog(window, QStringLiteral("saveDialog"));

        // Close with unsaved changes is refused until the user decides.
        // Keep the app alive if the guard fails, so the run still reports.
        const bool quitOnClose = QGuiApplication::quitOnLastWindowClosed();
        QGuiApplication::setQuitOnLastWindowClosed(false);
        QTextCursor(doc).insertText(QStringLiteral("x"));
        window->close();
        QCoreApplication::processEvents();
        t.check(window->isVisible() && dialogVisible(window, QStringLiteral("discardDialog")),
                "closing with unsaved changes asks first and keeps the window open");
        clickMessageButton(window, QStringLiteral("discardDialog"), kCancelButton, kRejectRole);
        t.check(window->isVisible() && controller->isDirty() && window->property("pendingAction").isNull(),
                "Cancel keeps the window and the edits");

        // Escape / the dialog's own close button: rejected() with no button.
        window->close();
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("discardDialog")), "reject");
        QCoreApplication::processEvents();
        t.check(window->isVisible() && window->property("pendingAction").isNull(),
                "dismissing the dialog without a button cancels: window open, nothing left pending");
        // Left pending, the close would have run after the next save.
        const IoRun laterSave = saveAndWait(controller, kDialogOdtPath);
        t.check(laterSave.success && window->isVisible(), "a later save doesn't close the window");

        // Discard really closes it (what failed on the desktop).
        QTextCursor(doc).insertText(QStringLiteral("y"));
        window->close();
        QCoreApplication::processEvents();
        clickMessageButton(window, QStringLiteral("discardDialog"), kDiscardButton, kDestructiveRole);
        t.check(!window->isVisible(), "Discard closes the window without saving");
        QGuiApplication::setQuitOnLastWindowClosed(quitOnClose);
    }
}

} // namespace TestSupport
