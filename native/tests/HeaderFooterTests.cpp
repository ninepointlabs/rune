// Headers and footers: editing in place, page fields, saving and reopening.

#include "TestSupport.h"

#include "OdfStyles.h"
#include "OdfWriter.h"
#include "Package.h"

namespace TestSupport {

void testHeaderFooter(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;

    // --- Headers and footers ---
    {
        const auto partText = [&](bool footer) { return controller->partDocument(footer ? QStringLiteral("footer") : QStringLiteral("header"))->toPlainText(); };
        // How many fields of `kind` the header or footer has.
        const auto fieldCount = [&](bool footer, PageParts::Field kind) {
            QTextDocument *part = controller->partDocument(footer ? QStringLiteral("footer") : QStringLiteral("header"));
            int count = 0;
            for (int pos = 0; pos < part->characterCount() - 1; ++pos) {
                QTextCursor c(part);
                c.setPosition(pos + 1);
                count += part->characterAt(pos) == QChar::ObjectReplacementCharacter && PageParts::isField(c.charFormat())
                    && c.charFormat().intProperty(PageParts::kFieldKind) == int(kind);
            }
            return count;
        };
        const auto layoutOf = [&] { return PageLayout::fromMap(controller->pageLayoutMap(), PageLayout()); };
        // Paper and margins alike (header and footer distances aside).
        const auto sameMargins = [](PageLayout a, const PageLayout &b) {
            a.headerMm = b.headerMm;
            a.footerMm = b.footerMm;
            return a == b;
        };
        const auto partAlignment = [&](bool footer) {
            return controller->partDocument(footer ? QStringLiteral("footer") : QStringLiteral("header"))->begin().blockFormat().alignment() & Qt::AlignHorizontal_Mask;
        };
        const auto readZipMember = [](const QString &path, const QString &name) {
            QuaZip zip(path);
            if (!zip.open(QuaZip::mdUnzip) || !zip.setCurrentFile(name))
                return QByteArray();
            QuaZipFile f(&zip);
            f.open(QIODevice::ReadOnly);
            return f.readAll();
        };

        // PageLayout: with a header, ODF's page margin is the header's distance.
        const PageLayout withParts = PageLayout::fromMap({{QStringLiteral("bottom"), 30.0}, {QStringLiteral("footer"), 10.0}},
                                                         PageLayout());
        const QByteArray partsXml = OdfStyles::write(withParts, {QStringLiteral("<text:p>H</text:p>"), QStringLiteral("<text:p>F</text:p>"), {}});
        const QByteArray plainXml = OdfStyles::write(withParts);
        t.check(partsXml.contains("fo:margin-top=\"12.70mm\"") && partsXml.contains("fo:margin-bottom=\"10.00mm\"")
                    && partsXml.contains("<style:header-style><style:header-footer-properties fo:min-height=\"12.70mm\"")
                    && partsXml.contains("<style:footer-style><style:header-footer-properties fo:min-height=\"20.00mm\"")
                    && OdfStyles::readPageLayout(partsXml) == withParts
                    && plainXml.contains("fo:margin-top=\"25.40mm\"") && !plainXml.contains("header-style"),
                "styles.xml with a header and footer: page margins at their distances, their heights reach the body; reads back the same");
        const QByteArray libreOffice =
            "<?xml version=\"1.0\"?><office:document-styles"
            " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
            " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
            " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
            " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\">"
            "<office:automatic-styles><style:page-layout style:name=\"Mpm1\">"
            "<style:page-layout-properties fo:page-width=\"8.5in\" fo:page-height=\"11in\" fo:margin-top=\"0.5in\""
            " fo:margin-bottom=\"0.5in\" fo:margin-left=\"1in\" fo:margin-right=\"1in\"/>"
            "<style:header-style><style:header-footer-properties fo:min-height=\"0.5in\" fo:margin-bottom=\"0.4602in\"/></style:header-style>"
            "<style:footer-style><style:header-footer-properties fo:min-height=\"0.5in\" fo:margin-top=\"0.4602in\"/></style:footer-style>"
            "</style:page-layout></office:automatic-styles><office:master-styles>"
            "<style:master-page style:name=\"Standard\" style:page-layout-name=\"Mpm1\"><style:header><text:p>H</text:p></style:header></style:master-page>"
            "</office:master-styles></office:document-styles>";
        const std::optional<PageLayout> fromLibreOffice = OdfStyles::readPageLayout(libreOffice);
        t.check(fromLibreOffice && qAbs(fromLibreOffice->topMm - 25.4) < 0.01 && qAbs(fromLibreOffice->headerMm - 12.7) < 0.01
                    && qAbs(fromLibreOffice->bottomMm - 12.7) < 0.01,
                QStringLiteral("LibreOffice's header: 0.5 in from the edge, body at 1 in; its unused footer style changes nothing (top %1, header %2, bottom %3 mm)")
                    .arg(fromLibreOffice ? fromLibreOffice->topMm : -1).arg(fromLibreOffice ? fromLibreOffice->headerMm : -1)
                    .arg(fromLibreOffice ? fromLibreOffice->bottomMm : -1));

        // A footer as OdfWriter writes it into styles.xml.
        {
            QTextDocument part;
            QTextCursor c(&part);
            QTextBlockFormat centred;
            centred.setAlignment(Qt::AlignHCenter);
            c.setBlockFormat(centred);
            c.insertText(QStringLiteral("Page "));
            c.insertText(QString(QChar::ObjectReplacementCharacter), PageParts::fieldFormat({}, PageParts::Field::PageNumber));
            c.insertText(QStringLiteral(" of "));
            c.insertText(QString(QChar::ObjectReplacementCharacter), PageParts::fieldFormat({}, PageParts::Field::PageCount));
            QTextCharFormat bold;
            bold.setFontWeight(QFont::Bold);
            c.insertText(QStringLiteral("\t  <draft>"), bold);
            QTextDocument body, empty;
            const auto stylesOf = [&](const QTextDocument *header, const QTextDocument *footer) {
                OdfWriter::Document document;
                document.body = &body;
                document.header = header;
                document.footer = footer;
                const std::optional<QList<Package::Entry>> entries = Package::read(OdfWriter::write(document));
                const Package::Entry *styles = entries ? Package::find(*entries, QStringLiteral("styles.xml")) : nullptr;
                return styles ? QString::fromUtf8(styles->data) : QString();
            };
            const QString xml = stylesOf(nullptr, &part);
            const QString emptyXml = stylesOf(&empty, nullptr);
            t.check(xml.contains(QStringLiteral("Page <text:page-number text:select-page=\"current\">1</text:page-number> of <text:page-count>1</text:page-count>"))
                        && xml.contains(QStringLiteral("<text:tab/> <text:s/>&lt;draft&gt;"))
                        && xml.contains(QStringLiteral("fo:text-align=\"center\"")) && xml.contains(QStringLiteral("fo:font-weight=\"bold\""))
                        && xml.contains(QStringLiteral("style:position=\"82.55mm\" style:type=\"center\""))
                        && xml.contains(QStringLiteral("style:position=\"165.10mm\" style:type=\"right\""))
                        && xml.contains(QStringLiteral("<style:footer>")) && !emptyXml.isEmpty()
                        && !emptyXml.contains(QStringLiteral("<style:header>")),
                    "a footer in styles.xml: fields, tabs, spaces and escaping, its alignment, bold and tab stops in "
                    "automatic styles; an empty header is left out: " + xml);
        }

        // A header and footer through the controller.
        controller->newDocument();
        QTextCursor(controller->partDocument(QStringLiteral("header"))).insertText(QStringLiteral("Quarterly report"));
        {
            QTextCursor c(controller->partDocument(QStringLiteral("footer")));
            c.insertText(QStringLiteral("Page "));
            int pos = controller->insertPageField(QStringLiteral("footer"), c.position(), false);
            c.setPosition(pos);
            c.insertText(QStringLiteral(" of "));
            controller->insertPageField(QStringLiteral("footer"), c.position(), true);
            QTextBlockFormat centred;
            centred.setAlignment(Qt::AlignHCenter);
            c.mergeBlockFormat(centred);
        }
        QCoreApplication::processEvents();
        t.check(controller->hasHeader() && controller->hasFooter() && controller->isDirty() && !controller->canUndo()
                    && fieldCount(true, PageParts::Field::PageNumber) == 1 && fieldCount(true, PageParts::Field::PageCount) == 1
                    && partAlignment(true) == Qt::AlignHCenter && controller->headerHeightPx() > 10,
                QStringLiteral("a header and a centred \"Page # of #\" footer: the document is dirty, the body's history untouched (header %1 px tall)")
                    .arg(controller->headerHeightPx()));

        // Each page draws its own number.
        {
            const auto render = [&](int page) {
                QImage image(400, 40, QImage::Format_ARGB32);
                image.fill(Qt::white);
                QPainter painter(&image);
                controller->paintPart(&painter, QStringLiteral("footer"), page, Qt::black);
                return image;
            };
            const QImage first = render(1), twelfth = render(12);
            t.check(first != twelfth && first == render(1) && first != QImage(400, 40, QImage::Format_ARGB32),
                    "the footer drawn for page 1 and page 12 differ (their numbers), and page 1 draws the same each time");
        }

        // Saved in styles.xml, read back on open.
        // Centred in its margin, a bigger font growing it both ways.
        {
            const qreal top = controller->pageLayout().marginsPx().top();
            const qreal centre = controller->headerYPx() + controller->headerHeightPx() / 2;
            QTextCursor all(controller->partDocument(QStringLiteral("header")));
            all.select(QTextCursor::Document);
            QTextCharFormat big;
            big.setFontPointSize(24);
            all.mergeCharFormat(big);
            QCoreApplication::processEvents();
            const qreal bigCentre = controller->headerYPx() + controller->headerHeightPx() / 2;
            const qreal bottom = controller->pageLayout().marginsPx().bottom();
            const qreal footerCentre = controller->footerYPx() + controller->footerHeightPx() / 2;
            t.check(qAbs(centre - top / 2) < 0.5 && qAbs(bigCentre - top / 2) < 0.5
                        && qAbs(footerCentre - (controller->pageSizePx().height() - bottom / 2)) < 0.5,
                    QStringLiteral("the header sits centred in the top margin, at 12 and at 24 pt (centre %1 and %2 px of %3), the footer in the bottom one")
                        .arg(centre).arg(bigCentre).arg(top));
            controller->partDocument(QStringLiteral("header"))->undo();
        }

        QFile::remove(kPartsPath);
        const bool partsSaved = controller->saveToOdf(kPartsPath);
        // Where they were drawn: what the file says.
        const qreal savedHeaderMm = controller->headerYPx() * 25.4 / 96;
        const qreal savedFooterMm = (controller->pageSizePx().height() - controller->footerYPx() - controller->footerHeightPx()) * 25.4 / 96;
        const QByteArray savedStyles = readZipMember(kPartsPath, QStringLiteral("styles.xml"));
        t.check(partsSaved && !controller->isDirty() && savedStyles.contains("<style:header><text:p")
                    && savedStyles.contains("Quarterly report") && savedStyles.contains("<text:page-count>"),
                "saveToOdf() writes the header and footer into styles.xml's master page, and the document is clean");
        controller->newDocument();
        const bool clearedByNew = !controller->hasHeader() && !controller->hasFooter() && !controller->isDirty();
        const bool partsOpened = controller->openOdf(kPartsPath);
        QCoreApplication::processEvents();
        t.check(clearedByNew && partsOpened && partText(false) == QStringLiteral("Quarterly report")
                    && fieldCount(true, PageParts::Field::PageNumber) == 1 && fieldCount(true, PageParts::Field::PageCount) == 1
                    && partText(true).startsWith(QStringLiteral("Page ")) && partAlignment(true) == Qt::AlignHCenter
                    && sameMargins(layoutOf(), PageLayout::defaultFor(QLocale::system().territory())) && !controller->isDirty(),
                "New empties them; opening the file brings back the header, the footer's fields and alignment, and the page layout, clean");

        // LibreOffice, independently: Word's header and footer, PAGE and NUMPAGES fields, header distance.
        QFile::remove(kPartsDocxPath);
        QString docxError;
        const bool converted = DocxBridge::convertOdtToDocx(kPartsPath, kPartsDocxPath, &docxError);
        const QString docXml = QString::fromUtf8(readZipMember(kPartsDocxPath, QStringLiteral("word/document.xml")));
        const QString headerXml = QString::fromUtf8(readZipMember(kPartsDocxPath, QStringLiteral("word/header1.xml")));
        const QString footerXml = QString::fromUtf8(readZipMember(kPartsDocxPath, QStringLiteral("word/footer1.xml")));
        const QString pgMar = QRegularExpression(QStringLiteral("<w:pgMar [^>]*>")).match(docXml).captured(0);
        const auto twips = [&](const QString &attribute) {
            return QRegularExpression(QStringLiteral("w:%1=\"(\\d+)\"").arg(attribute)).match(pgMar).captured(1).toInt();
        };
        const PageLayout local = PageLayout::defaultFor(QLocale::system().territory());
        const auto nearTwips = [&](const QString &attribute, qreal mm) { return qAbs(twips(attribute) - mm / 25.4 * 1440) <= 2; };
        t.check(converted && headerXml.contains(QStringLiteral("Quarterly report")) && footerXml.contains(QStringLiteral(" PAGE "))
                    && footerXml.contains(QStringLiteral(" NUMPAGES ")) && nearTwips(QStringLiteral("top"), local.topMm)
                    && nearTwips(QStringLiteral("header"), savedHeaderMm) && nearTwips(QStringLiteral("footer"), savedFooterMm)
                    && nearTwips(QStringLiteral("bottom"), local.bottomMm),
                "soffice: the .docx has the header, PAGE and NUMPAGES in the footer, the same margins, and the header and footer where Rune drew them ("
                    + pgMar + ")" + (docxError.isEmpty() ? QString() : " " + docxError));
        controller->newDocument();
        const IoRun docxOpen = openAndWait(controller, kPartsDocxPath);
        const PageLayout fromDocx = layoutOf();
        QCoreApplication::processEvents();
        // The page view heard about it (the open ran with signals blocked).
        auto *firstHeaderView = findItem(window->contentItem(), QStringLiteral("headerView"));
        t.check(docxOpen.success && firstHeaderView && firstHeaderView->isVisible() && partText(false).trimmed() == QStringLiteral("Quarterly report")
                    && fieldCount(true, PageParts::Field::PageNumber) == 1 && fieldCount(true, PageParts::Field::PageCount) == 1
                    && qAbs(fromDocx.topMm - local.topMm) < 0.5 && qAbs(fromDocx.headerMm - savedHeaderMm) < 0.5
                    && qAbs(fromDocx.bottomMm - local.bottomMm) < 0.5,
                QStringLiteral("opening that .docx brings back the header (shown on the page), both fields and the margins (top %1, header %2, bottom %3 mm)")
                    .arg(fromDocx.topMm).arg(fromDocx.headerMm).arg(fromDocx.bottomMm) + errorNote(docxOpen));

        // On screen: every page shows them; a double-click in a page's top
        // margin edits its header in place.
        controller->openOdf(kPartsPath);
        {
            QTextCursor c(doc);
            c.movePosition(QTextCursor::End);
            for (int i = 0; i < 150; ++i)
                c.insertText(QStringLiteral("Body paragraph %1, long enough to wrap across the line at least once on any page.\n").arg(i + 1));
        }
        QCoreApplication::processEvents();
        QList<QQuickItem *> headerViews, footerViews, headerAreas, footerAreas;
        const std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
            if (item->objectName() == QLatin1String("headerView")) headerViews.append(item);
            if (item->objectName() == QLatin1String("footerView")) footerViews.append(item);
            if (item->objectName() == QLatin1String("headerArea")) headerAreas.append(item);
            if (item->objectName() == QLatin1String("footerArea")) footerAreas.append(item);
            for (QQuickItem *child : item->childItems())
                collect(child);
        };
        collect(window->contentItem());
        const int pages = controller->pageCount();
        // Shown, and drawing from this controller's header and footer.
        const auto shown = [&](QQuickItem *v) {
            return v->isVisible() && v->property("documentController").value<QObject *>() == controller && v->height() > 10;
        };
        const bool allShown = std::all_of(headerViews.cbegin(), headerViews.cend(), shown)
            && std::all_of(footerViews.cbegin(), footerViews.cend(), shown);
        t.check(pages >= 3 && headerViews.size() == pages && footerViews.size() == pages && allShown,
                QStringLiteral("%1 pages, each showing the header and footer (%2 and %3 drawn)").arg(pages).arg(headerViews.size()).arg(footerViews.size()));
        auto *headerEditor = findItem(window->contentItem(), QStringLiteral("headerEditor"));
        auto *footerEditor = findItem(window->contentItem(), QStringLiteral("footerEditor"));
        auto *flick = editor->parentItem()->parentItem()->parentItem(); // pageColumn, contentItem, Flickable
        const auto showY = [&](QQuickItem *item, qreal y) {
            flick->setProperty("contentY", qMax(0.0, item->mapToItem(flick->property("contentItem").value<QQuickItem *>(), QPointF(0, y)).y() - 100));
            QCoreApplication::processEvents();
        };
        if (headerAreas.size() >= 2 && headerEditor && footerEditor) {
            const QString bodyBefore = doc->toPlainText();
            const bool bodyUndo = controller->canUndo();
            QQuickItem *area = headerAreas.at(1);
            showY(area, area->height() / 2);
            const QPoint inMargin = area->mapToScene(QPointF(area->width() / 2, area->height() / 2)).toPoint();
            // One click: just the body's cursor, as before.
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, inMargin);
            QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
            const bool singleIgnored = window->property("editingPart").toString().isEmpty() && editor->hasActiveFocus();
            QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, inMargin);
            QCoreApplication::processEvents();
            t.check(singleIgnored && window->property("editingPart").toString() == QStringLiteral("header")
                        && window->property("editingPage").toInt() == 2 && headerEditor->isVisible() && headerEditor->hasActiveFocus()
                        && !headerViews.at(1)->isVisible() && headerViews.at(0)->isVisible(),
                    "a click in page 2's top margin stays in the body; a double-click there edits page 2's header in place");
            typeText(window, QStringLiteral(" draft"));
            QCoreApplication::processEvents();
            const QString typed = partText(false);
            pressSystemKey(window, Qt::Key_Z, Qt::ControlModifier);
            const QString undone = partText(false);
            t.check(typed == QStringLiteral("Quarterly report draft") && undone != typed && doc->toPlainText() == bodyBefore
                        && doc->isUndoAvailable() == bodyUndo && controller->isDirty(),
                    "typing goes into the header; Ctrl+Z undoes it there, the body and its history untouched (\"" + typed + "\" → \"" + undone + "\")");

            // The toolbar and Ctrl+B format the header now, not the body.
            const QString bodyHtml = doc->toHtml();
            pressSystemKey(window, Qt::Key_A, Qt::ControlModifier);
            pressSystemKey(window, Qt::Key_B, Qt::ControlModifier);
            auto *larger = findItem(window->contentItem(), QStringLiteral("fontLargerButton"));
            const qreal sizeBefore = window->property("fontSize").toDouble();
            if (larger) {
                const QPoint at = larger->mapToScene(QPointF(larger->width() / 2, larger->height() / 2)).toPoint();
                QTest::mouseMove(window, at);
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, at);
            }
            QCoreApplication::processEvents();

            QTextDocument *header = controller->partDocument(QStringLiteral("header"));
            const int headerLength = header->characterCount() - 1;
            QTextCursor last(header);
            last.setPosition(headerLength);
            const qreal headerSize = last.charFormat().fontPointSize();
            t.check(formatOf(header, 0, headerLength) == QStringLiteral("B") && headerSize > sizeBefore && doc->toHtml() == bodyHtml
                        && window->property("boldActive").toBool() && headerEditor->hasActiveFocus(),
                    QStringLiteral("Ctrl+A, Ctrl+B and the toolbar's A+ in the header: all of it bold, %1 → %2 pt, the toolbar shows bold, "
                                   "the body untouched, the header still being edited").arg(sizeBefore).arg(headerSize));

            // A click on the body goes back to it, the cursor where clicked.
            const QPointF bodyPoint(area->width() / 2, area->height() + 150);
            const QPointF inEditor = area->mapToItem(editor, bodyPoint);
            int expected = -1;
            QMetaObject::invokeMethod(editor, "positionAt", Q_RETURN_ARG(int, expected), Q_ARG(qreal, inEditor.x()), Q_ARG(qreal, inEditor.y()));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, area->mapToScene(bodyPoint).toPoint());
            QCoreApplication::processEvents();
            t.check(window->property("editingPart").toString().isEmpty() && editor->hasActiveFocus() && headerViews.at(1)->isVisible()
                        && expected > 0 && editor->property("cursorPosition").toInt() == expected && doc->toHtml() == bodyHtml
                        && !window->property("boldActive").toBool(),
                    QStringLiteral("a click on page 2's text goes back to the body, the cursor where clicked (%1), the toolbar following it")
                        .arg(expected));

            // ... as do Escape and a click beside the page.
            QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, inMargin);
            QCoreApplication::processEvents();
            const bool reopened = window->property("editingPart").toString() == QStringLiteral("header");
            pressKey(window, Qt::Key_Escape, Qt::NoModifier, QString());
            QCoreApplication::processEvents();
            const bool escaped = window->property("editingPart").toString().isEmpty() && editor->hasActiveFocus();
            QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
            QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, inMargin);
            QCoreApplication::processEvents();
            const bool reopenedAgain = window->property("editingPart").toString() == QStringLiteral("header");
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(8, inMargin.y() + 200));
            QCoreApplication::processEvents();
            t.check(reopened && escaped && reopenedAgain && window->property("editingPart").toString().isEmpty() && editor->hasActiveFocus(),
                    "Escape, and a click beside the page, go back to the body too");
        } else {
            t.check(false, "found the header areas and the header and footer editors");
        }

        // Ctrl+K → Page numbers: a centred page number in an empty footer.
        controller->newDocument();
        QCoreApplication::processEvents();
        QMetaObject::invokeMethod(window, "addPageNumbers");
        QCoreApplication::processEvents();
        t.check(window->property("editingPart").toString() == QStringLiteral("footer") && footerEditor && footerEditor->hasActiveFocus()
                    && fieldCount(true, PageParts::Field::PageNumber) == 1 && partAlignment(true) == Qt::AlignHCenter,
                "Page numbers: a centred page number field in the footer, open for editing");
        if (auto *done = findItem(window->contentItem(), QStringLiteral("partDone"))) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, done->mapToScene(QPointF(done->width() / 2, done->height() / 2)).toPoint());
            QCoreApplication::processEvents();
        }
        t.check(window->property("editingPart").toString().isEmpty() && editor->hasActiveFocus() && !controller->canUndo(),
                "Done goes back to the body; a one-line footer fits its margin, so the page layout is unchanged");

        // A header taller than its margin (8 lines, in any font) pushes the
        // body down, as one undo step.
        controller->newDocument();
        QMetaObject::invokeMethod(window, "editPart", Q_ARG(QVariant, QStringLiteral("header")), Q_ARG(QVariant, 1));
        QTextCursor(controller->partDocument(QStringLiteral("header"))).insertText(QStringLiteral("One\nTwo\nThree\nFour\nFive\nSix\nSeven\nEight"));
        QCoreApplication::processEvents();
        const qreal tallHeader = controller->headerHeightPx() * 25.4 / 96;
        const PageLayout before = layoutOf();
        QMetaObject::invokeMethod(window, "finishPartEditing", Q_ARG(QVariant, QVariant()));
        QCoreApplication::processEvents();
        const PageLayout grown = layoutOf();
        controller->setPageLayout({{QStringLiteral("top"), 5.0}});
        const PageLayout squeezed = layoutOf();
        controller->undo();
        controller->undo();
        QCoreApplication::processEvents();
        t.check(grown.topMm > before.topMm && qAbs(grown.topMm - (tallHeader + 6)) < 0.01
                    && qAbs(squeezed.topMm - grown.topMm) < 0.01 && layoutOf().topMm == before.topMm,
                QStringLiteral("an 8-line header (%1 mm) moves the body down to %2 mm, 3 mm clear above and below; Page setup can't "
                               "put the body back under it (%3 mm); undo restores %4 mm")
                    .arg(tallHeader).arg(grown.topMm).arg(squeezed.topMm).arg(before.topMm));
        controller->newDocument();
    }
}

} // namespace TestSupport
