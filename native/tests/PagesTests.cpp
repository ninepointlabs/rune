// Pages (paper size, margins, pagination, Page setup) and the default font
// size.

#include "TestSupport.h"

namespace TestSupport {

void testPages(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;

    // --- Pages: paper size, margins, pagination, Page setup ---
    {
        // PageLayout on its own.
        const PageLayout us = PageLayout::defaultFor(QLocale::UnitedStates);
        const PageLayout de = PageLayout::defaultFor(QLocale::Germany);
        t.check(qFuzzyCompare(us.widthMm, 215.9) && qFuzzyCompare(us.heightMm, 279.4) && qFuzzyCompare(de.widthMm, 210.0)
                    && qFuzzyCompare(de.heightMm, 297.0) && qFuzzyCompare(us.topMm, 25.4) && qFuzzyCompare(de.leftMm, 25.4)
                    && PageLayout::usesLetter(QLocale::Canada) && !PageLayout::usesLetter(QLocale::UnitedKingdom),
                "default paper: Letter in the US and Canada, A4 in Germany and the UK; 1 inch margins");
        const PageLayout squeezed = PageLayout::fromMap({{QStringLiteral("left"), 150.0}, {QStringLiteral("right"), 150.0},
                                                         {QStringLiteral("top"), -5.0}}, us);
        t.check(squeezed.widthMm - squeezed.leftMm - squeezed.rightMm >= 19.99 && squeezed.topMm == 0
                    && squeezed.heightMm == us.heightMm,
                QStringLiteral("margins too wide for the page shrink to leave 20 mm of text; negative ones are 0 (%1 + %2 on %3)")
                    .arg(squeezed.leftMm).arg(squeezed.rightMm).arg(squeezed.widthMm));
        const QByteArray libreOffice =
            "<?xml version=\"1.0\"?><office:document-styles"
            " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
            " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
            " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\">"
            "<office:automatic-styles>"
            "<style:page-layout style:name=\"pm2\"><style:page-layout-properties fo:page-width=\"11in\" fo:page-height=\"8.5in\" fo:margin=\"2cm\" fo:margin-left=\"0.5in\"/></style:page-layout>"
            "<style:page-layout style:name=\"pm1\"><style:page-layout-properties fo:page-width=\"100mm\" fo:page-height=\"100mm\"/></style:page-layout>"
            "</office:automatic-styles><office:master-styles>"
            "<style:master-page style:name=\"Endnote\" style:page-layout-name=\"pm1\"/>"
            "<style:master-page style:name=\"Standard\" style:page-layout-name=\"pm2\"/>"
            "</office:master-styles></office:document-styles>";
        const std::optional<PageLayout> read = PageLayout::fromOdfStyles(libreOffice);
        t.check(read && qAbs(read->widthMm - 279.4) < 0.01 && qAbs(read->heightMm - 215.9) < 0.01 && qAbs(read->topMm - 20) < 0.01
                    && qAbs(read->leftMm - 12.7) < 0.01 && qAbs(read->rightMm - 20) < 0.01,
                "styles.xml: the Standard master page's layout, in any unit, fo:margin overridden by a side");
        const PageLayout custom = PageLayout::fromMap({{QStringLiteral("width"), 297.0}, {QStringLiteral("height"), 210.0},
                                                       {QStringLiteral("top"), 10.0}, {QStringLiteral("bottom"), 15.0},
                                                       {QStringLiteral("left"), 20.0}, {QStringLiteral("right"), 30.0}}, us);
        t.check(PageLayout::fromOdfStyles(custom.odfStyles()) == custom && !PageLayout::fromOdfStyles("<not xml")
                    && !PageLayout::fromOdfStyles(QByteArray()),
                "a written styles.xml reads back the same; unreadable or missing ones give no layout");

        // A new document: the locale's paper, paginated at it, no history.
        const PageLayout localDefault = PageLayout::defaultFor(QLocale::system().territory());
        const auto layoutOf = [&] { return PageLayout::fromMap(controller->pageLayoutMap(), PageLayout()); };
        const auto paginated = [&] {
            return doc->pageSize() == controller->pageLayout().sizePx();
        };
        controller->newDocument();
        QCoreApplication::processEvents();
        const QTextFrameFormat root = doc->rootFrame()->frameFormat();
        t.check(layoutOf() == localDefault && paginated() && qAbs(root.topMargin() - localDefault.marginsPx().top()) < 0.01
                    && !controller->isDirty() && !controller->canUndo() && controller->pageCount() == 1,
                QStringLiteral("a new document gets the locale's paper (%1 × %2 mm), paginated at it, clean and with nothing to undo")
                    .arg(localDefault.widthMm).arg(localDefault.heightMm));

        // Lines never start in a page's top or bottom margin.
        {
            QTextCursor c(doc);
            for (int i = 0; i < 60; ++i) {
                c.insertText(QStringLiteral("Paragraph %1: the quick brown fox jumps over the lazy dog, then runs on "
                                            "across the page until this line wraps.").arg(i + 1));
                c.insertBlock();
            }
        }
        QCoreApplication::processEvents();
        const QSizeF page = controller->pageLayout().sizePx();
        const QMarginsF margins = controller->pageLayout().marginsPx();
        int lines = 0, misplaced = 0;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            const qreal blockTop = doc->documentLayout()->blockBoundingRect(b).top();
            for (int i = 0; i < b.layout()->lineCount(); ++i) {
                const qreal top = blockTop + b.layout()->lineAt(i).y();
                const qreal inPage = std::fmod(top, page.height());
                ++lines;
                if (inPage < margins.top() - 1 || inPage > page.height() - margins.bottom())
                    ++misplaced;
            }
        }
        auto *sheets = window->findChild<QQuickItem *>(QStringLiteral("pageColumn"));
        int sheetCount = 0;
        if (sheets)
            for (QQuickItem *child : sheets->childItems())
                sheetCount += child->objectName() == QLatin1String("pageSheet");
        t.check(controller->pageCount() >= 2 && misplaced == 0 && sheetCount == controller->pageCount(),
                QStringLiteral("%1 lines over %2 pages (as many sheets drawn), none starting in a margin (%3 do)")
                    .arg(lines).arg(controller->pageCount()).arg(misplaced));
        // A click on the last page, below the text, puts the cursor at the end.
        if (auto *below = window->findChild<QQuickItem *>(QStringLiteral("belowText"))) {
            editor->setProperty("cursorPosition", 0);
            const bool onScreen = below->height() > 60;
            auto *flick = sheets->parentItem()->parentItem();
            flick->setProperty("contentY", qMax(0.0, flick->property("contentHeight").toDouble() - flick->height()));
            QCoreApplication::processEvents();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              below->mapToScene(QPointF(below->width() / 2, below->height() - 40)).toPoint());
            t.check(onScreen && editor->property("cursorPosition").toInt() == doc->characterCount() - 1,
                    "a click below the text on the last page puts the cursor at the end");
            flick->setProperty("contentY", 0);
        }
        auto *indicator = window->findChild<QObject *>(QStringLiteral("pageIndicator"));
        editor->setProperty("cursorPosition", 0);
        QCoreApplication::processEvents();
        const QString atStart = indicator ? indicator->property("text").toString() : QString();
        editor->setProperty("cursorPosition", doc->characterCount() - 1);
        QCoreApplication::processEvents();
        const QString atEnd = indicator ? indicator->property("text").toString() : QString();
        t.check(atStart == QStringLiteral("Page 1 of %1").arg(controller->pageCount())
                    && atEnd == QStringLiteral("Page %1 of %1").arg(controller->pageCount()),
                "the status bar shows the cursor's page: \"" + atStart + "\" at the start, \"" + atEnd + "\" at the end");

        // Page setup: one undo step; pagination survives the page width change.
        controller->setPageLayout(custom.toMap());
        QCoreApplication::processEvents();
        const QSizeF landscape = custom.sizePx();
        t.check(layoutOf() == custom && paginated() && qAbs(editor->width() - landscape.width()) < 0.5 && controller->isDirty(),
                QStringLiteral("setPageLayout(A4 landscape, 1/1.5/2/3 cm margins): the editor is %1 px wide, still paginated (%2 × %3)")
                    .arg(editor->width()).arg(doc->pageSize().width()).arg(doc->pageSize().height()));
        controller->undo();
        QCoreApplication::processEvents();
        const bool undone = layoutOf() == localDefault && paginated();
        controller->redo();
        QCoreApplication::processEvents();
        t.check(undone && layoutOf() == custom && paginated(), "undo puts the previous page layout back, redo the new one");

        // Saved in the .odt, read back on open.
        QFile::remove(kPagePath);
        t.check(controller->saveToOdf(kPagePath) && PageLayout::fromOdtFile(kPagePath) == custom,
                "saveToOdf() stores the page layout in styles.xml");
        {
            QuaZip zip(kPagePath);
            QByteArray manifest, mimetype;
            QString first;
            if (zip.open(QuaZip::mdUnzip)) {
                zip.goToFirstFile();
                first = zip.getCurrentFileName();
                if (zip.setCurrentFile(QStringLiteral("META-INF/manifest.xml"))) {
                    QuaZipFile f(&zip);
                    f.open(QIODevice::ReadOnly);
                    manifest = f.readAll();
                }
            }
            t.check(first == QStringLiteral("mimetype") && manifest.contains("manifest:full-path=\"styles.xml\""),
                    "the archive still starts with mimetype, and the manifest lists styles.xml");
        }
        controller->newDocument();
        t.check(controller->openOdf(kPagePath) && layoutOf() == custom && paginated() && !controller->isDirty()
                    && !controller->canUndo(),
                "opening it brings the page layout back, clean and with nothing to undo");
        {
            QTextDocument bare;
            QTextCursor(&bare).insertText(QStringLiteral("No page layout here"));
            QTextDocumentWriter(kPageBarePath, QByteArrayLiteral("ODF")).write(&bare);
        }
        t.check(controller->openOdf(kPageBarePath) && layoutOf() == localDefault && paginated(),
                "a file with no page layout of its own opens on the locale's paper");

        // LibreOffice, independently: the saved page size and margins in the .docx it makes.
        controller->openOdf(kPagePath);
        QFile::remove(kPageDocxPath);
        QString docxError;
        const bool converted = DocxBridge::convertOdtToDocx(kPagePath, kPageDocxPath, &docxError);
        QByteArray wordXml;
        {
            QuaZip zip(kPageDocxPath);
            if (converted && zip.open(QuaZip::mdUnzip) && zip.setCurrentFile(QStringLiteral("word/document.xml"))) {
                QuaZipFile f(&zip);
                f.open(QIODevice::ReadOnly);
                wordXml = f.readAll();
            }
        }
        // <w:pgSz> and <w:pgMar>, whatever order their attributes come in.
        const QString word = QString::fromUtf8(wordXml);
        const QString pgSz = QRegularExpression(QStringLiteral("<w:pgSz [^>]*>")).match(word).captured(0);
        const QString pgMar = QRegularExpression(QStringLiteral("<w:pgMar [^>]*>")).match(word).captured(0);
        // Twips: 1/1440 inch. LibreOffice rounds; allow 2 twips.
        const auto near = [](const QString &element, const QString &attribute, qreal mm) {
            const QRegularExpressionMatch m =
                QRegularExpression(QStringLiteral("w:%1=\"(\\d+)\"").arg(attribute)).match(element);
            return m.hasMatch() && qAbs(m.captured(1).toDouble() - mm / 25.4 * 1440) <= 2;
        };
        t.check(near(pgSz, QStringLiteral("w"), 297) && near(pgSz, QStringLiteral("h"), 210)
                    && near(pgMar, QStringLiteral("top"), 10) && near(pgMar, QStringLiteral("bottom"), 15)
                    && near(pgMar, QStringLiteral("left"), 20) && near(pgMar, QStringLiteral("right"), 30),
                "soffice: the .docx made from it has the same page size and margins (" + pgSz + " " + pgMar + ")"
                    + (docxError.isEmpty() ? QString() : " " + docxError));
        controller->newDocument();
        const IoRun docxOpen = openAndWait(controller, kPageDocxPath);
        const PageLayout fromDocx = layoutOf();
        t.check(docxOpen.success && qAbs(fromDocx.widthMm - 297) < 0.2 && qAbs(fromDocx.heightMm - 210) < 0.2
                    && qAbs(fromDocx.leftMm - 20) < 0.2 && qAbs(fromDocx.rightMm - 30) < 0.2 && paginated(),
                QStringLiteral("opening that .docx brings the page layout back (%1 × %2 mm, left %3, right %4)")
                    .arg(fromDocx.widthMm).arg(fromDocx.heightMm).arg(fromDocx.leftMm).arg(fromDocx.rightMm)
                    + errorNote(docxOpen));

        // The Page setup dialog, from the File menu, with real clicks and keys.
        controller->newDocument();
        QObject *setupItem = window->findChild<QObject *>(QStringLiteral("pageSetupMenuItem"));
        QObject *setup = window->findChild<QObject *>(QStringLiteral("pageSetup"));
        auto *setupContent = setup ? setup->property("contentItem").value<QQuickItem *>() : nullptr;
        auto *landscapeButton = findItem(setupContent, QStringLiteral("landscapeButton"));
        auto *a5Button = findItem(setupContent, QStringLiteral("paperA5"));
        auto *applyButton = findItem(setupContent, QStringLiteral("pageSetupApply"));
        auto *topField = findItem(setupContent, QStringLiteral("topMarginField"));
        t.check(setupItem && setup && landscapeButton && a5Button && applyButton && topField,
                "found File → Page setup… and the dialog's paper, orientation, margin and Apply controls");
        if (setupItem && setup && landscapeButton && a5Button && applyButton && topField) {
            const auto click = [&](QQuickItem *item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
                QCoreApplication::processEvents();
            };
            const bool inches = controller->localeUsesLetter();
            QMetaObject::invokeMethod(setupItem, "click");
            QCoreApplication::processEvents();
            const QString shownTop = topField->property("text").toString();
            t.check(setup->property("visible").toBool()
                        && shownTop == (inches ? QStringLiteral("1") : QStringLiteral("2.54")),
                    "Page setup opens showing the current margins in " + QString(inches ? "inches" : "centimetres")
                        + " (top: " + shownTop + ")");
            click(a5Button);
            click(landscapeButton);
            topField->setProperty("text", QStringLiteral("abc"));
            QCoreApplication::processEvents();
            const bool blocked = !applyButton->property("available").toBool();
            pressReturn(window);
            t.check(blocked && setup->property("visible").toBool() && layoutOf() == localDefault,
                    "a margin that isn't a number blocks Apply, and Enter does nothing");
            topField->setProperty("text", inches ? QStringLiteral("0.5") : QStringLiteral("1.5"));
            QMetaObject::invokeMethod(topField, "forceActiveFocus");
            pressReturn(window);
            QCoreApplication::processEvents();
            const PageLayout applied = layoutOf();
            t.check(!setup->property("visible").toBool() && qAbs(applied.widthMm - 210) < 0.01 && qAbs(applied.heightMm - 148) < 0.01
                        && qAbs(applied.topMm - (inches ? 12.7 : 15)) < 0.01 && qAbs(applied.leftMm - localDefault.leftMm) < 0.01
                        && paginated() && editor->hasActiveFocus(),
                    QStringLiteral("A5, Landscape, a new top margin and Enter: %1 × %2 mm, top %3 mm, back in the document")
                        .arg(applied.widthMm).arg(applied.heightMm).arg(applied.topMm));
            QMetaObject::invokeMethod(setupItem, "click");
            QCoreApplication::processEvents();
            click(findItem(setupContent, QStringLiteral("paperLetter")));
            pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
            QCoreApplication::processEvents();
            t.check(!setup->property("visible").toBool() && layoutOf() == applied, "Escape closes Page setup without changing anything");
        }
        controller->newDocument();
    }

    // --- Default font size ---
    {
        // What the toolbar says unformatted text is, it is: laid out at 12
        // pt in the body and header, after New and after opening a file.
        const auto laidOutAt = [](QTextDocument *d, qreal points) {
            QFont font = d->defaultFont();
            font.setPointSizeF(points);
            const QTextLayout *layout = d->begin().layout();
            return layout && layout->lineCount() > 0 && qAbs(layout->lineAt(0).ascent() - QFontMetricsF(font).ascent()) < 0.01;
        };
        controller->newDocument();
        QTextCursor(doc).insertText(QStringLiteral("Unformatted"));
        QTextCursor(controller->partDocument(QStringLiteral("header"))).insertText(QStringLiteral("Header"));
        QCoreApplication::processEvents();
        const bool fresh = laidOutAt(doc, 12) && laidOutAt(controller->partDocument(QStringLiteral("header")), 12)
            && controller->currentFontSize() == 12 && window->property("fontSize").toDouble() == 12;
        QFile::remove(kFontSizePath);
        controller->saveToOdf(kFontSizePath);
        // The file names the font by its real family, at that size, for
        // the body and the header: other programs show the same.
        const QString family = doc->defaultFont().family();
        QString content, styles;
        {
            QuaZip zip(kFontSizePath);
            for (const QString &name : {QStringLiteral("content.xml"), QStringLiteral("styles.xml")}) {
                if (zip.isOpen() || zip.open(QuaZip::mdUnzip)) {
                    zip.setCurrentFile(name);
                    QuaZipFile f(&zip);
                    f.open(QIODevice::ReadOnly);
                    (name == QLatin1String("content.xml") ? content : styles) = QString::fromUtf8(f.readAll());
                }
            }
        }
        const bool named = family != QLatin1String("Sans")
            && content.contains(QStringLiteral("fo:font-family=\"%1\" fo:font-size=\"12pt\"").arg(family))
            && styles.contains(QStringLiteral("fo:font-family=\"'%1'\" fo:font-size=\"12pt\"").arg(family));
        controller->newDocument();
        controller->openOdf(kFontSizePath);
        QCoreApplication::processEvents();
        t.check(fresh && named && laidOutAt(doc, 12) && laidOutAt(controller->partDocument(QStringLiteral("header")), 12) && controller->currentFontSize() == 12,
                "unformatted text is 12 pt " + family + ", as the toolbar says, in the body and the header; the file says so, and reopens the same");
        controller->newDocument();
    }
}

} // namespace TestSupport
