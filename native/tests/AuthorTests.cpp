// Author tooling (headings, the outline, moving sections, story notes) and
// citations (sources, styles, the bibliography, saving), through the
// controller, real keys and the panels.

#include "TestSupport.h"

#include "Citations.h"
#include "Package.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace TestSupport {

namespace {

const QString kAuthorPath = QStringLiteral("/tmp/rune-native-author.odt");
const QString kCitePath = QStringLiteral("/tmp/rune-native-cite.odt");
const QString kCiteDocxPath = QStringLiteral("/tmp/rune-native-cite.docx");
const QString kCiteTextDir = QStringLiteral("/tmp/rune-native-cite-txt");

void settle(int ms = 350)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents();
}

QStringList blockTexts(QTextDocument *doc)
{
    QStringList out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        out << b.text();
    return out;
}

// The rendered in-text citations, in order.
QStringList renderedCitations(QTextDocument *doc)
{
    QStringList out;
    const Citations::Engine *engine = Citations::Engine::of(doc);
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (Citations::isCitation(it.fragment().charFormat()))
                for (const QChar ch : it.fragment().text())
                    if (ch == QChar::ObjectReplacementCharacter)
                        out << (engine ? engine->render(it.fragment().charFormat()) : QStringLiteral("?"));
    return out;
}

// The bibliography's paragraphs.
QStringList bibliography(QTextDocument *doc)
{
    QStringList out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.blockFormat().hasProperty(QTextFormat::UserProperty + 140))
            out << b.text();
    return out;
}

QVariantMap person(const QString &family, const QString &given)
{
    return {{QStringLiteral("family"), family}, {QStringLiteral("given"), given}};
}

QVariantMap issued(int year)
{
    return {{QStringLiteral("date-parts"), QVariantList{QVariantList{year}}}};
}

} // namespace

void testAuthorTools(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;

    // A heading from the keyboard, and body text after it.
    controller->newDocument();
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Chapter One"));
    pressSystemKey(window, Qt::Key_1, Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::processEvents();
    const QTextBlock h1 = doc->begin();
    t.check(h1.blockFormat().headingLevel() == 1 && controller->currentHeadingLevel() == 1
                && window->property("headingLevel").toInt() == 1 && controller->isBold(),
            "Ctrl+Alt+1 makes the paragraph a Heading 1, bold, and the toolbar shows it");
    pressReturn(window);
    settle(50);
    typeText(window, QStringLiteral("It began."));
    const QTextBlock body = doc->begin().next();
    t.check(body.text() == QStringLiteral("It began.") && body.blockFormat().headingLevel() == 0 && !controller->isBold()
                && window->property("headingLevel").toInt() == 0,
            "Return at the end of a heading starts body text, not another heading");
    controller->setSelection(2, 2, 2);
    controller->setHeadingLevel(0);
    bool plain = true;
    for (auto it = doc->begin().begin(); !it.atEnd(); ++it)
        plain = plain && !it.fragment().charFormat().hasProperty(QTextFormat::FontWeight)
            && !it.fragment().charFormat().hasProperty(QTextFormat::FontSizeAdjustment);
    t.check(doc->begin().blockFormat().headingLevel() == 0 && plain, "Body text turns a heading back, bold and size gone");
    controller->undo();
    t.check(doc->begin().blockFormat().headingLevel() == 1, "... and undo makes it a heading again (one step)");

    // The outline: chapters with scenes, word counts, moves.
    controller->newDocument();
    {
        QTextCursor c(doc);
        const auto add = [&](const QString &text, int level, bool first = false) {
            if (!first)
                c.insertBlock();
            QTextBlockFormat f;
            f.setHeadingLevel(level);
            c.setBlockFormat(f);
            c.insertText(text);
        };
        add(QStringLiteral("Chapter One"), 1, true);
        add(QStringLiteral("It began."), 0);
        add(QStringLiteral("Scene A"), 2);
        add(QStringLiteral("Two words."), 0);
        add(QStringLiteral("Chapter Two"), 1);
        add(QStringLiteral("Three more words here."), 0);
        add(QStringLiteral("Chapter Three"), 1);
        add(QStringLiteral("End."), 0);
    }
    settle();
    const auto outline = [&] {
        QStringList out;
        for (const QVariant &v : controller->outline()) {
            const QVariantMap m = v.toMap();
            out << QStringLiteral("%1:%2:%3").arg(m.value("level").toInt()).arg(m.value("title").toString()).arg(m.value("words").toInt());
        }
        return out;
    };
    // Headings' own words count too, so the chapters add up to the total.
    t.check(outline() == QStringList({"1:Chapter One:8", "2:Scene A:4", "1:Chapter Two:6", "1:Chapter Three:3"})
                && controller->wordCount() == 17,
            "the outline: chapters and scenes with their words, headings included, a chapter's counting its scenes' "
            "and the chapters adding up to the total; "
            + QString::number(controller->wordCount()) + " words in all: " + outline().join(QStringLiteral(" | ")));
    const QStringList original = blockTexts(doc);
    t.check(controller->moveSection(3, 0), "moveSection(): Chapter Three before Chapter One");
    t.check(blockTexts(doc) == QStringList({"Chapter Three", "End.", "Chapter One", "It began.", "Scene A", "Two words.",
                                            "Chapter Two", "Three more words here."})
                && doc->begin().blockFormat().headingLevel() == 1 && blockWithText(doc, QStringLiteral("Scene A")).blockFormat().headingLevel() == 2
                && blockWithText(doc, QStringLiteral("End.")).blockFormat().headingLevel() == 0,
            "the chapter moved whole, every paragraph keeping its style: " + blockTexts(doc).join(QStringLiteral(" | ")));
    controller->undo();
    t.check(blockTexts(doc) == original, "one undo puts it back exactly");
    t.check(controller->moveSection(0, 4) && blockTexts(doc) == QStringList({"Chapter Two", "Three more words here.", "Chapter Three", "End.",
                                                                              "Chapter One", "It began.", "Scene A", "Two words."}),
            "moving Chapter One to the end takes its scene with it: " + blockTexts(doc).join(QStringLiteral(" | ")));
    t.check(!controller->moveSection(2, 3) && !controller->moveSection(2, 2),
            "a move onto itself or into itself changes nothing");
    controller->undo();
    settle();

    // The outline panel lists them; a click goes there.
    auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebar"));
    auto *outlineList = window->findChild<QQuickItem *>(QStringLiteral("outlineList"));
    callRoot(window, "showSidebar", QStringLiteral("outline"));
    settle(100);
    t.check(sidebar && sidebar->isVisible() && outlineList && outlineList->property("count").toInt() == 4,
            "the sidebar's outline lists the four sections");
    callRoot(window, "goToPosition", controller->outline().at(2).toMap().value("position"));
    t.check(editor->property("cursorPosition").toInt() == blockWithText(doc, QStringLiteral("Chapter Two")).position(),
            "going to a section puts the cursor at its heading");

    // Story notes: in the panel, saved with the document.
    QObject *notesPanel = window->findChild<QObject *>(QStringLiteral("notesPanel"));
    QMetaObject::invokeMethod(notesPanel, "add", Q_ARG(QVariant, QStringLiteral("place")));
    settle(50);
    window->findChild<QObject *>(QStringLiteral("noteName"))->setProperty("text", QStringLiteral("The Harbour"));
    window->findChild<QObject *>(QStringLiteral("noteText"))->setProperty("text", QStringLiteral("Grey stone, gulls."));
    QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("saveNote")), "click");
    settle(50);
    const QString characterId = controller->addNote(QStringLiteral("character"), QStringLiteral("Mara"), QStringLiteral("Narrator."));
    const auto noteSummary = [](DocumentController *c) {
        QStringList out;
        for (const QVariant &n : c->notes())
            out << n.toMap().value("kind").toString() + ":" + n.toMap().value("name").toString() + ":" + n.toMap().value("text").toString();
        return out;
    };
    t.check(noteSummary(controller) == QStringList({"place:The Harbour:Grey stone, gulls.", "character:Mara:Narrator."})
                && controller->isDirty(),
            "notes added in the panel and through the controller: " + noteSummary(controller).join(QStringLiteral(" | ")));
    controller->updateNote(characterId, QStringLiteral("Mara Quill"), QStringLiteral("Narrator, 30."));
    t.check(controller->saveToOdf(kAuthorPath), "saved with headings and notes");
    {
        DocumentController reader;
        const bool read = reader.openOdf(kAuthorPath);
        settle(50);
        t.check(read && noteSummary(&reader) == QStringList({"place:The Harbour:Grey stone, gulls.", "character:Mara Quill:Narrator, 30."})
                    && reader.outline().size() == 4,
                "reopened: the notes and the outline are back");
        t.check(reader.agentData().contains(QStringLiteral("```notes")) && reader.agentData().contains(QStringLiteral("Mara Quill")),
                "agents get the notes as a fenced JSON block");
        const int merged = reader.applyAgentData(QStringLiteral("notes"), QStringLiteral(
            R"([{"id": "%1", "text": "Narrator, 31."}, {"kind": "Monster", "name": "The Fog"},
                {"name": "the harbour", "deleted": true}])").arg(characterId));
        t.check(merged == 3 && noteSummary(&reader) == QStringList({"character:Mara Quill:Narrator, 31.", "research:The Fog:"}),
                "an agent's notes: one updated by id (other fields kept), an unknown kind filed as research, one "
                "deleted by name: " + noteSummary(&reader).join(QStringLiteral(" | ")));
    }
    controller->removeNote(characterId);
    t.check(noteSummary(controller) == QStringList({"place:The Harbour:Grey stone, gulls."}), "removeNote()");
    sidebar->setProperty("visible", false);
    controller->newDocument();
    t.check(controller->notes().isEmpty() && controller->outline().isEmpty() && controller->wordCount() == 0,
            "a new document has no notes and no outline");
}

void testCitations(Env &env)
{
    Checker &t = env.t;
    QQuickWindow *window = env.window;
    DocumentController *controller = env.controller;
    QQuickItem *editor = env.editor;
    QTextDocument *doc = env.doc;

    controller->newDocument();
    const QString smith = controller->addSource(
        {{"id", "smith2020"}, {"type", "book"}, {"title", "Harbours of the North"}, {"author", QVariantList{person("Smith", "Jane")}},
         {"issued", issued(2020)}, {"publisher", "Tide Press"}, {"publisher-place", "Oslo"}});
    const QString doe = controller->addSource(
        {{"type", "article-journal"}, {"title", "Tides and towns"}, {"author", QVariantList{person("Doe", "Alex"), person("Lee", "Ann")}},
         {"issued", issued(2019)}, {"container-title", "Sea Journal"}, {"volume", "12"}, {"issue", "3"}, {"page", "45-67"},
         {"DOI", "10.1000/xyz"}});
    t.check(smith == QStringLiteral("smith2020") && doe == QStringLiteral("doe2019") && controller->sources().size() == 2,
            "sources added, the one without an id given one from its author and year: " + doe);

    editor->forceActiveFocus();
    typeText(window, QStringLiteral("Harbours matter "));
    t.check(controller->insertCitation({smith}, QStringLiteral("12")), "insertCitation() at the cursor");
    typeText(window, QStringLiteral(" and tides too "));
    controller->insertCitation({doe, smith}, QString());
    typeText(window, QStringLiteral("."));
    t.check(renderedCitations(doc) == QStringList({"(Smith, 2020, p. 12)", "(Doe & Lee, 2019; Smith, 2020)"}),
            "APA: in-text citations as drawn: " + renderedCitations(doc).join(QStringLiteral(" | ")));
    t.check(controller->markdownContext().before.contains(QStringLiteral("[@smith2020, 12]"))
                && controller->markdownContext().before.contains(QStringLiteral("[@doe2019; @smith2020]")),
            "the AI sees them as Pandoc citations: " + controller->markdownContext().before.trimmed());

    // The bibliography.
    QTextCursor end(doc);
    end.movePosition(QTextCursor::End);
    controller->setSelection(end.position(), end.position(), end.position());
    t.check(controller->insertBibliography(), "insertBibliography() at the end");
    const QStringList apa = bibliography(doc);
    t.check(apa == QStringList({"References",
                                "Doe, A., & Lee, A. (2019). Tides and towns. Sea Journal, 12(3), 45–67. https://doi.org/10.1000/xyz",
                                "Smith, J. (2020). Harbours of the North. Tide Press."}),
            "APA references, alphabetical: " + apa.join(QStringLiteral(" | ")));
    const QTextBlock smithEntry = doc->lastBlock();
    bool titleItalic = false;
    for (auto it = smithEntry.begin(); !it.atEnd(); ++it)
        if (it.fragment().text() == QStringLiteral("Harbours of the North"))
            titleItalic = it.fragment().charFormat().fontItalic();
    t.check(titleItalic && blockWithText(doc, QStringLiteral("References")).blockFormat().headingLevel() == 1,
            "a book's title in italics; the References heading is a Heading 1");

    // Other styles: citations and the bibliography follow.
    controller->setCitationStyle(QStringLiteral("mla"));
    t.check(renderedCitations(doc) == QStringList({"(Smith 12)", "(Doe and Lee; Smith)"})
                && bibliography(doc).value(0) == QStringLiteral("Works Cited")
                && bibliography(doc).contains(QStringLiteral("Smith, Jane. Harbours of the North. Tide Press, 2020.")),
            "MLA: " + renderedCitations(doc).join(QStringLiteral(" | ")) + " / " + bibliography(doc).join(QStringLiteral(" | ")));
    controller->setCitationStyle(QStringLiteral("chicago"));
    t.check(renderedCitations(doc) == QStringList({"(Smith 2020, 12)", "(Doe and Lee 2019; Smith 2020)"}),
            "Chicago author-date: " + renderedCitations(doc).join(QStringLiteral(" | ")));
    controller->setCitationStyle(QStringLiteral("ieee"));
    t.check(renderedCitations(doc) == QStringList({"[1, p. 12]", "[2], [1]"})
                && bibliography(doc) == QStringList({"References", "[1] J. Smith, Harbours of the North. Oslo: Tide Press, 2020.",
                                                     "[2] A. Doe and A. Lee, “Tides and towns,” Sea Journal, vol. 12, no. 3, "
                                                     "pp. 45–67, 2019, doi: 10.1000/xyz."}),
            "IEEE: numbered by first citation: " + renderedCitations(doc).join(QStringLiteral(" | ")) + " / "
                + bibliography(doc).join(QStringLiteral(" | ")));
    controller->setCitationStyle(QStringLiteral("apa"));

    // Deleting a citation with the keyboard: the bibliography follows, and
    // one undo brings both back.
    const QString before = doc->toPlainText();
    const int second = doc->toPlainText().indexOf(QChar::ObjectReplacementCharacter,
                                                  doc->toPlainText().indexOf(QChar::ObjectReplacementCharacter) + 1);
    editor->setProperty("cursorPosition", second + 1);
    QCoreApplication::processEvents();
    pressKey(window, Qt::Key_Backspace, Qt::NoModifier, QString());
    settle(100);
    t.check(renderedCitations(doc).size() == 1 && bibliography(doc).size() == 2
                && bibliography(doc).value(1).startsWith(QStringLiteral("Smith")),
            "Backspace over a citation: the bibliography drops the source no longer cited: " + bibliography(doc).join(QStringLiteral(" | ")));
    controller->undo();
    settle(100);
    t.check(doc->toPlainText() == before && bibliography(doc).size() == 3, "one undo restores the citation and its entry");

    // Saving: the citations' text between bookmarks, what they cite in the
    // file's data.
    t.check(controller->saveToOdf(kCitePath), "saved with citations");
    const std::optional<QList<Package::Entry>> entries = Package::readFile(kCitePath);
    const Package::Entry *content = entries ? Package::find(*entries, QStringLiteral("content.xml")) : nullptr;
    const Package::Entry *saved = entries ? Package::find(*entries, QStringLiteral("Rune/citations.json")) : nullptr;
    t.check(content && content->data.contains("<text:bookmark-start text:name=\"_RuneCite1\"/>(Smith, 2020, p. 12)<text:bookmark-end text:name=\"_RuneCite1\"/>")
                && saved && QJsonDocument::fromJson(saved->data).object().value("2").toObject().value("ids").toArray().size() == 2,
            "content.xml: each citation's text between _RuneCite bookmarks; Rune/citations.json: what each cites");
    {
        DocumentController reader;
        const bool read = reader.openOdf(kCitePath);
        settle(50);
        t.check(read && renderedCitations(reader.textDocument()) == renderedCitations(doc)
                    && bibliography(reader.textDocument()) == bibliography(doc)
                    && !reader.data()->names().contains(QStringLiteral("citations")) && !reader.isDirty(),
                "reopened: live citations again, the same bibliography, clean: "
                    + renderedCitations(reader.textDocument()).join(QStringLiteral(" | ")) + " / "
                    + bibliography(reader.textDocument()).join(QStringLiteral(" | ")) + " / "
                    + reader.data()->names().join(QStringLiteral(",")) + (reader.isDirty() ? " dirty" : " clean"));
    }
    t.check(sofficeToText(kCitePath, kCiteTextDir).contains(QStringLiteral("(Smith, 2020, p. 12)")),
            "LibreOffice shows the citations' text");
    const IoRun docx = saveAndWait(controller, kCiteDocxPath);
    {
        DocumentController reader;
        const IoRun open = openAndWait(&reader, kCiteDocxPath);
        settle(50);
        t.check(docx.success && open.success && renderedCitations(reader.textDocument()) == renderedCitations(doc)
                    && reader.sources().size() == 2,
                "through .docx and back: the citations are live again (bookmarks kept by Word's format)"
                    + errorNote(docx) + errorNote(open));
    }

    // Sources change: citations follow.
    QVariantMap renamed = controller->sources().at(0).toMap().value("item").toMap();
    renamed.insert("id", "smithj2020");
    t.check(controller->updateSource(smith, renamed) == QStringLiteral("smithj2020")
                && controller->markdownContext().before.contains(QStringLiteral("[@smithj2020, 12]")),
            "a source's id changed: its citations follow");
    controller->removeSource(doe);
    t.check(renderedCitations(doc).value(1).contains(QStringLiteral("[missing source: doe2019]")) == false
                && renderedCitations(doc).value(1) == QStringLiteral("(Smith, 2020)"),
            "a removed source drops out of the citations that cite others too: " + renderedCitations(doc).join(QStringLiteral(" | ")));

    // BibTeX, and [@key] text becoming citations.
    const int imported = controller->importBibTeX(QStringLiteral(
        "@article{lee2018,\n  author = {Lee, Ann and Bo Park},\n  title = {Gulls of {Norway}},\n  journal = \"Bird Notes\",\n"
        "  year = 2018, pages = {1--10}, doi = {10.5/gulls}\n}\n@book{smithj2020, title={Duplicate key}, year={2001}}"));
    const QVariantList sources = controller->sources();
    QVariantMap lee, dup;
    for (const QVariant &s : sources) {
        if (s.toMap().value("id") == "lee2018")
            lee = s.toMap().value("item").toMap();
        if (s.toMap().value("item").toMap().value("title") == "Duplicate key")
            dup = s.toMap();
    }
    t.check(imported == 2 && lee.value("title") == "Gulls of Norway" && lee.value("page") == "1-10"
                && lee.value("author").toList().value(1).toMap().value("family") == "Park" && dup.value("id") != "smithj2020",
            "BibTeX import: braces and quotes, both name orders, pages; a key already taken gets a fresh one ("
                + dup.value("id").toString() + ")");
    QTextCursor tail(doc);
    tail.setPosition(blockWithText(doc, QStringLiteral("References")).position() - 1);
    tail.insertText(QStringLiteral(" Gulls [@lee2018, p. 3] and [@nobody]."));
    t.check(controller->convertMarkdownCitations() == 1 && doc->toPlainText().contains(QStringLiteral("[@nobody]"))
                && renderedCitations(doc).contains(QStringLiteral("(Lee & Park, 2018, p. 3)")),
            "[@key] text becomes a citation when the key is a source; an unknown key stays text");
    t.check(bibliography(doc).contains(QStringLiteral("Lee, A., & Park, B. (2018). Gulls of Norway. Bird Notes, 1–10. https://doi.org/10.5/gulls")),
            "... and the bibliography gains it");

    // The picker, from the keyboard.
    const int citedBefore = renderedCitations(doc).size();
    editor->forceActiveFocus();
    editor->setProperty("cursorPosition", 0);
    QCoreApplication::processEvents();
    pressSystemKey(window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    settle(50);
    QObject *picker = window->findChild<QObject *>(QStringLiteral("citationPicker"));
    const bool pickerOpen = picker && picker->property("visible").toBool();
    typeText(window, QStringLiteral("gulls"));
    pressReturn(window);
    settle(50);
    t.check(pickerOpen && !picker->property("visible").toBool() && renderedCitations(doc).size() == citedBefore + 1
                && renderedCitations(doc).value(0) == QStringLiteral("(Lee & Park, 2018)"),
            "Ctrl+Shift+C opens the picker; typing finds the source, Enter cites it at the cursor");
    controller->newDocument();
    t.check(controller->sources().isEmpty() && controller->citationStyle() == QStringLiteral("apa"),
            "a new document has no sources, APA style");
}

} // namespace TestSupport
