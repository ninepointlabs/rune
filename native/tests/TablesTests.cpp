// Tables: the toolbar menu, row/column edits, bounds, Tab between cells,
// and the list-then-table export bug through the production path.

#include "TestSupport.h"

namespace TestSupport {

bool testTables(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;

    // --- Stage 3: tables ---

    // The Stage 1 sanitizer bug through the production path: a bullet list
    // typed through the UI, then insertTable() straight after its last item
    // with no paragraph in between.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Alpha"));
    controller->toggleBulletList();
    pressReturn(window);
    typeText(window, QStringLiteral("Beta"));
    t.check(doc->toPlainText() == QStringLiteral("Alpha\nBeta") && controller->isInBulletList(),
            QStringLiteral("2-item bullet list typed: \"%1\"").arg(doc->toPlainText()));
    controller->insertTable(2, 2);
    const QTextBlock listEnd = blockBeforeFirstTable(doc);
    t.check(listEnd.isValid() && listEnd.textList() && listEnd.text() == QStringLiteral("Beta"),
            "live doc: list item \"Beta\" directly precedes the inserted table (the bug pattern)");
    t.check(controller->isInTable() && cellAtCursor(doc, editor) == QStringLiteral("0,0"),
            "insertTable() puts the cursor in cell 0,0: " + cellAtCursor(doc, editor));
    typeText(window, QStringLiteral("Cell A"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell B"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell C"));
    pressTab(window);
    typeText(window, QStringLiteral("Cell D"));
    const int listTableBlocks = doc->blockCount();
    t.check(controller->saveToOdf(kStage3ListTablePath), "saveToOdf(" + kStage3ListTablePath + ") [list then table]");
    t.check(doc->blockCount() == listTableBlocks && blockBeforeFirstTable(doc).textList(),
            "save left the live list-then-table document unchanged");
    const QString listTableHtml = convertWithSoffice(kStage3ListTablePath);
    logConversion(kStage3ListTablePath, listTableHtml);
    t.check(countOf(listTableHtml, QStringLiteral("table")) == 1 && countOf(listTableHtml, QStringLiteral("td")) == 4
                && countOf(listTableHtml, QStringLiteral("li")) == 2,
            "soffice: list-then-table export has the list (2 <li>) and the table (4 <td>)");
    t.check(textOf(listTableHtml).contains(QStringLiteral("Alpha Beta Cell A Cell B Cell C Cell D")),
            "soffice: list items and all four cells present, in order");
    // Control: the same document written without the sanitizer loses the
    // table, so the check above really went through the bug.
    {
        QTextDocumentWriter raw(kStage3ListTableRawPath, QByteArrayLiteral("ODF"));
        raw.write(doc);
    }
    const QString rawHtml = convertWithSoffice(kStage3ListTableRawPath);
    logConversion(kStage3ListTableRawPath, rawHtml);
    t.check(!rawHtml.isEmpty() && countOf(rawHtml, QStringLiteral("table")) == 0
                && !textOf(rawHtml).contains(QStringLiteral("Cell A")),
            "control: the unsanitized export of the same document loses the table in soffice");

    // The toolbar's Table menu, isInTable() and its row/column entries'
    // availability. Entries are clicked as a menu click does (click()).
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Intro"));
    auto *insertTableItem = window->findChild<QObject *>(QStringLiteral("insertTableItem"));
    auto *insertRowItem = window->findChild<QObject *>(QStringLiteral("insertRowItem"));
    auto *deleteRowItem = window->findChild<QObject *>(QStringLiteral("deleteRowItem"));
    auto *insertColumnItem = window->findChild<QObject *>(QStringLiteral("insertColumnItem"));
    auto *deleteColumnItem = window->findChild<QObject *>(QStringLiteral("deleteColumnItem"));
    t.check(insertTableItem && insertRowItem && deleteRowItem && insertColumnItem && deleteColumnItem,
            "found the Table menu's entries");
    if (!t.ok())
        return false;
    const auto rowColumnAvailable = [&] {
        return insertRowItem->property("enabled").toBool() && deleteRowItem->property("enabled").toBool()
            && insertColumnItem->property("enabled").toBool() && deleteColumnItem->property("enabled").toBool();
    };
    t.check(!controller->isInTable() && !insertRowItem->property("enabled").toBool(),
            "outside a table: isInTable() false, row/column entries unavailable");
    QMetaObject::invokeMethod(insertTableItem, "click");
    QTextTable *table = tableAtCursor(doc, editor);
    t.check(table && table->rows() == 3 && table->columns() == 3,
            QStringLiteral("Table → Insert table inserted a 3x3 table (%1x%2)")
                .arg(table ? table->rows() : 0).arg(table ? table->columns() : 0));
    if (!table)
        return false;
    t.check(controller->isInTable() && rowColumnAvailable(), "in a cell: isInTable() true, row/column entries available");
    // Fill row by row ("r1c2" = row 1, column 2) so row/column edits show up by content.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            typeText(window, QStringLiteral("r%1c%2").arg(r).arg(c));
            if (r < 2 || c < 2)
                pressTab(window);
        }
    editor->setProperty("cursorPosition", doc->characterCount() - 1);
    typeText(window, QStringLiteral("Trailing"));
    t.check(!controller->isInTable() && !rowColumnAvailable(),
            "in trailing text after the table: isInTable() false, row/column entries unavailable");

    // +Row / +Col from cell 1,1: one more of each, after the cursor's.
    editor->setProperty("cursorPosition", table->cellAt(1, 1).firstCursorPosition().position());
    QMetaObject::invokeMethod(insertRowItem, "click");
    t.check(table->rows() == 4 && table->cellAt(2, 0).firstCursorPosition().block().text().isEmpty()
                && table->cellAt(3, 0).firstCursorPosition().block().text() == QStringLiteral("r2c0"),
            QStringLiteral("+Row: 3 -> %1 rows, empty row inserted below row 1").arg(table->rows()));
    QMetaObject::invokeMethod(insertColumnItem, "click");
    t.check(table->columns() == 4 && table->cellAt(0, 2).firstCursorPosition().block().text().isEmpty()
                && table->cellAt(0, 3).firstCursorPosition().block().text() == QStringLiteral("r0c2"),
            QStringLiteral("+Col: 3 -> %1 columns, empty column inserted right of column 1").arg(table->columns()));
    t.check(controller->saveToOdf(kStage3TablePath), "saveToOdf(" + kStage3TablePath + ") [4x4]");
    const QString grownHtml = convertWithSoffice(kStage3TablePath);
    logConversion(kStage3TablePath, grownHtml);
    t.check(countOf(grownHtml, QStringLiteral("tr")) == 4 && countOf(grownHtml, QStringLiteral("td")) == 16,
            "soffice: grown table has 4 rows, 16 cells");

    // −Row / −Col at cell 1,1: removes row 1 ("r1c*") and then column 1 ("*c1").
    t.check(cellAtCursor(doc, editor) == QStringLiteral("1,1"), "cursor still in cell 1,1: " + cellAtCursor(doc, editor));
    QMetaObject::invokeMethod(deleteRowItem, "click");
    t.check(table->rows() == 3 && controller->isInTable(),
            QStringLiteral("−Row: 4 -> %1 rows, cursor still in the table").arg(table->rows()));
    QMetaObject::invokeMethod(deleteColumnItem, "click");
    t.check(table->columns() == 3 && controller->isInTable(),
            QStringLiteral("−Col: 4 -> %1 columns, cursor still in the table").arg(table->columns()));
    t.check(controller->saveToOdf(kStage3TablePath), "saveToOdf(" + kStage3TablePath + ") [after deletes]");
    const QString shrunkHtml = convertWithSoffice(kStage3TablePath);
    logConversion(kStage3TablePath, shrunkHtml);
    const QString shrunkText = textOf(shrunkHtml);
    t.check(countOf(shrunkHtml, QStringLiteral("tr")) == 3 && countOf(shrunkHtml, QStringLiteral("td")) == 9,
            "soffice: shrunk table has 3 rows, 9 cells");
    t.check(shrunkText.contains(QStringLiteral("Intro r0c0 r0c2 r2c0 r2c2 Trailing")) && !shrunkText.contains(QStringLiteral("r1c"))
                && !shrunkText.contains(QStringLiteral("c1")),
            "soffice: row 1 and column 1 content gone, the rest intact");

    // Row above / column left, from the "r2c0" cell: inserted before it,
    // with the cursor still in that cell. Then Delete table.
    auto *insertRowAboveItem = window->findChild<QObject *>(QStringLiteral("insertRowAboveItem"));
    auto *insertColumnLeftItem = window->findChild<QObject *>(QStringLiteral("insertColumnLeftItem"));
    auto *deleteTableItem = window->findChild<QObject *>(QStringLiteral("deleteTableItem"));
    t.check(insertRowAboveItem && insertColumnLeftItem && deleteTableItem
                && insertRowAboveItem->property("enabled").toBool() && deleteTableItem->property("enabled").toBool(),
            "found Insert row above, Insert column left and Delete table, available in a cell");
    if (!t.ok())
        return false;
    const auto cellText = [&](int r, int c) { return table->cellAt(r, c).firstCursorPosition().block().text(); };
    editor->setProperty("cursorPosition", table->cellAt(2, 0).firstCursorPosition().position());
    QMetaObject::invokeMethod(insertRowAboveItem, "click");
    t.check(table->rows() == 4 && cellText(2, 0).isEmpty() && cellText(3, 0) == QStringLiteral("r2c0")
                && cellAtCursor(doc, editor) == QStringLiteral("3,0"),
            QStringLiteral("Insert row above: %1 rows, empty row above \"r2c0\", cursor still in it (%2)")
                .arg(table->rows()).arg(cellAtCursor(doc, editor)));
    QMetaObject::invokeMethod(insertColumnLeftItem, "click");
    t.check(table->columns() == 4 && cellText(3, 0).isEmpty() && cellText(3, 1) == QStringLiteral("r2c0")
                && cellAtCursor(doc, editor) == QStringLiteral("3,1"),
            QStringLiteral("Insert column left: %1 columns, empty column left of \"r2c0\", cursor still in it (%2)")
                .arg(table->columns()).arg(cellAtCursor(doc, editor)));
    {
        // The paragraph after the table, centered: it must stay so.
        QTextCursor trailing(doc->lastBlock());
        QTextBlockFormat centered = trailing.blockFormat();
        centered.setAlignment(Qt::AlignHCenter);
        trailing.setBlockFormat(centered);
    }
    QMetaObject::invokeMethod(deleteTableItem, "click");
    table = nullptr; // deleted with the table
    bool anyTable = false;
    for (auto it = doc->rootFrame()->begin(); !it.atEnd(); ++it)
        anyTable = anyTable || qobject_cast<QTextTable *>(it.currentFrame());
    QStringList blocks;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        blocks.append(b.text());
    t.check(!anyTable && !controller->isInTable() && !deleteTableItem->property("enabled").toBool()
                && blocks == QStringList{QStringLiteral("Intro"), QStringLiteral("Trailing")}
                && doc->firstBlock().blockFormat().alignment() != Qt::AlignHCenter
                && doc->lastBlock().blockFormat().alignment() == Qt::AlignHCenter,
            "Delete table removes the table and its contents; the paragraphs around it stay separate, "
            "each with its own alignment: ["
                + blocks.join(QStringLiteral("] [")) + "]");

    // Bounds: sizes clamp to 1..50; the last row/column is never deleted.
    controller->newDocument();
    editor->forceActiveFocus();
    controller->insertTable(0, 99);
    table = tableAtCursor(doc, editor);
    t.check(table && table->rows() == 1 && table->columns() == 50,
            QStringLiteral("insertTable(0, 99) clamps to 1x50 (%1x%2)")
                .arg(table ? table->rows() : 0).arg(table ? table->columns() : 0));
    controller->deleteTableColumn();
    controller->insertTable(1, 1); // nested in the cell, then removed down to nothing
    controller->deleteTableRow();
    controller->deleteTableColumn();
    QTextTable *inner = tableAtCursor(doc, editor);
    t.check(inner && inner != table && inner->rows() == 1 && inner->columns() == 1,
            "deleting the only row/column of a 1x1 table leaves it intact");
    t.check(table && table->columns() == 49, "−Col on a 1x50 table leaves 49 columns");
    editor->setProperty("cursorPosition", 0); // the empty paragraph before the table
    controller->insertTableRow();
    controller->deleteTableRow();
    t.check(!controller->isInTable() && table && table->rows() == 1, "row commands outside a table are no-ops");

    // Tab / Shift+Tab move between cells in reading order.
    controller->newDocument();
    editor->forceActiveFocus();
    controller->insertTable(2, 2);
    table = tableAtCursor(doc, editor);
    QStringList visited{cellAtCursor(doc, editor)};
    const QStringList words{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third"),
                            QStringLiteral("fourth")};
    for (int i = 0; i < words.size(); ++i) {
        typeText(window, words[i]);
        if (i + 1 < words.size()) {
            pressTab(window);
            visited << cellAtCursor(doc, editor);
        }
    }
    t.check(visited == QStringList{"0,0", "0,1", "1,0", "1,1"}, "Tab visits cells " + visited.join(" -> "));
    t.check(!doc->toPlainText().contains('\t'), "Tab in a table inserted no tab characters");
    QStringList back;
    for (int i = 0; i < 4; ++i) {
        pressBacktab(window);
        back << cellAtCursor(doc, editor);
    }
    t.check(back == QStringList{"1,0", "0,1", "0,0", "0,0"},
            "Shift+Tab visits cells " + back.join(" -> ") + " (stops at the first)");
    t.check(controller->saveToOdf(kStage3TabPath), "saveToOdf(" + kStage3TabPath + ") [Tab fill]");
    const QString tabHtml = convertWithSoffice(kStage3TabPath);
    logConversion(kStage3TabPath, tabHtml);
    t.check(countOf(tabHtml, QStringLiteral("td")) == 4 && textOf(tabHtml).contains(QStringLiteral("first second third fourth")),
            "soffice: cells filled via Tab read \"first second third fourth\"");
    editor->setProperty("cursorPosition", table->cellAt(1, 1).lastCursorPosition().position());
    pressTab(window);
    t.check(table->rows() == 3 && cellAtCursor(doc, editor) == QStringLiteral("2,0"),
            "Tab in the last cell appends a row and moves into it: " + cellAtCursor(doc, editor));

    // Tab outside a table keeps its Stage 2 behaviour.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Item"));
    controller->toggleBulletList();
    pressReturn(window);
    typeText(window, QStringLiteral("Sub"));
    pressTab(window);
    QTextCursor subCursor(doc);
    subCursor.setPosition(editor->property("cursorPosition").toInt());
    t.check(listIndent(subCursor.block()) == 2 && !doc->toPlainText().contains('\t'),
            QStringLiteral("Tab in a list (no table) still demotes: indent %1").arg(listIndent(subCursor.block())));
    pressBacktab(window);
    t.check(listIndent(subCursor.block()) == 1, QStringLiteral("Shift+Tab in a list still promotes: indent %1")
                                                    .arg(listIndent(subCursor.block())));
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("x"));
    pressTab(window);
    typeText(window, QStringLiteral("y"));
    t.check(doc->toPlainText() == QStringLiteral("x\ty"),
            QStringLiteral("Tab in plain text still inserts a tab: \"%1\"").arg(doc->toPlainText()));
    return true;
}

} // namespace TestSupport
