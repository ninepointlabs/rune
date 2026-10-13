#include "CitationStyles.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace CitationStyles {

namespace {

struct Person
{
    QString family, given, literal; // literal: an organisation
};

QList<Person> people(const QJsonObject &item, const char *role)
{
    QList<Person> out;
    for (const QJsonValue &value : item.value(QLatin1String(role)).toArray()) {
        const QJsonObject o = value.toObject();
        Person p{o.value(QLatin1String("family")).toString().trimmed(), o.value(QLatin1String("given")).toString().trimmed(),
                 o.value(QLatin1String("literal")).toString().trimmed()};
        if (p.family.isEmpty() && p.literal.isEmpty() && !p.given.isEmpty())
            std::swap(p.family, p.given);
        if (!p.family.isEmpty() || !p.literal.isEmpty())
            out.append(p);
    }
    return out;
}

QString field(const QJsonObject &item, const char *name)
{
    const QJsonValue v = item.value(QLatin1String(name));
    return v.isDouble() ? QString::number(v.toDouble()) : v.toString().trimmed();
}

QString type(const QJsonObject &item)
{
    return field(item, "type");
}

bool isArticle(const QJsonObject &item)
{
    const QString t = type(item);
    if (t.startsWith(QLatin1String("article")) || t == QLatin1String("paper-conference"))
        return t != QLatin1String("article") || !field(item, "container-title").isEmpty();
    return false;
}

bool isChapter(const QJsonObject &item)
{
    const QString t = type(item);
    return t == QLatin1String("chapter") || t == QLatin1String("entry-encyclopedia") || t == QLatin1String("entry-dictionary");
}

bool isWeb(const QJsonObject &item)
{
    const QString t = type(item);
    return t == QLatin1String("webpage") || t == QLatin1String("post") || t == QLatin1String("post-weblog");
}

// The family name, or the organisation.
QString surname(const Person &p)
{
    return p.literal.isEmpty() ? p.family : p.literal;
}

// "Jane Ann" -> "J. A."; "Jean-Paul" -> "J.-P."
QString initials(const QString &given)
{
    QStringList parts;
    for (const QString &word : given.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        QStringList hyphenated;
        for (const QString &piece : word.split(QLatin1Char('-'), Qt::SkipEmptyParts))
            hyphenated << piece.left(1).toUpper() + QLatin1Char('.');
        parts << hyphenated.join(QLatin1Char('-'));
    }
    return parts.join(QLatin1Char(' '));
}

QString familyFirst(const Person &p, bool initialsOnly)
{
    if (!p.literal.isEmpty())
        return p.literal;
    if (p.given.isEmpty())
        return p.family;
    return p.family + QStringLiteral(", ") + (initialsOnly ? initials(p.given) : p.given);
}

QString givenFirst(const Person &p, bool initialsOnly)
{
    if (!p.literal.isEmpty())
        return p.literal;
    if (p.given.isEmpty())
        return p.family;
    return (initialsOnly ? initials(p.given) : p.given) + QLatin1Char(' ') + p.family;
}

// "a, b, and c" with `last` before the last name ("and", "&").
QString joined(const QStringList &names, const QString &last, bool serialComma)
{
    if (names.size() <= 1)
        return names.value(0);
    if (names.size() == 2)
        return names.at(0) + (serialComma && last == QLatin1String("&") ? QStringLiteral(", ") : QStringLiteral(" ")) + last
            + QLatin1Char(' ') + names.at(1);
    return names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + (serialComma ? QStringLiteral(", ") : QStringLiteral(" "))
        + last + QLatin1Char(' ') + names.constLast();
}

QString pages(QString text)
{
    text.replace(QRegularExpression(QStringLiteral("\\s*-+\\s*")), QStringLiteral("–"));
    return text;
}

// "p. 12" / "pp. 12-14" / "12" -> "12" / "12–14".
QString bareLocator(const QString &locator)
{
    QString text = locator.trimmed();
    text.remove(QRegularExpression(QStringLiteral("^(pp?\\.|pages?)\\s*"), QRegularExpression::CaseInsensitiveOption));
    return pages(text);
}

QString pageLabel(const QString &bare)
{
    return (bare.contains(QChar(0x2013)) || bare.contains(u',')) ? QStringLiteral("pp. ") : QStringLiteral("p. ");
}

QString doiUrl(const QJsonObject &item)
{
    QString doi = field(item, "DOI");
    doi.remove(QRegularExpression(QStringLiteral("^(https?://(dx\\.)?doi\\.org/|doi:)"), QRegularExpression::CaseInsensitiveOption));
    return doi.isEmpty() ? QString() : QStringLiteral("https://doi.org/") + doi;
}

// The DOI as a link, else the URL.
QString link(const QJsonObject &item)
{
    const QString doi = doiUrl(item);
    return doi.isEmpty() ? field(item, "URL") : doi;
}

// Ends `text` with `mark` unless it already ends with punctuation.
QString closed(const QString &text, QChar mark = u'.')
{
    if (text.isEmpty())
        return text;
    const QChar last = text.back();
    return last == u'.' || last == u'?' || last == u'!' ? text : text + mark;
}

class Builder
{
public:
    Builder &text(const QString &t)
    {
        if (!t.isEmpty())
            add(t, false);
        return *this;
    }
    Builder &italic(const QString &t)
    {
        if (!t.isEmpty())
            add(t, true);
        return *this;
    }
    Runs runs() const { return m_runs; }

private:
    void add(const QString &t, bool italic)
    {
        if (!m_runs.isEmpty() && m_runs.last().italic == italic)
            m_runs.last().text += t;
        else
            m_runs.append({t, italic});
    }
    Runs m_runs;
};

// --- In-text ---------------------------------------------------------------

QString apaAuthors(const QJsonObject &item)
{
    const QList<Person> authors = people(item, "author");
    if (authors.isEmpty())
        return field(item, "title");
    if (authors.size() == 1)
        return surname(authors.at(0));
    if (authors.size() == 2)
        return surname(authors.at(0)) + QStringLiteral(" & ") + surname(authors.at(1));
    return surname(authors.at(0)) + QStringLiteral(" et al.");
}

QString mlaAuthors(const QJsonObject &item, const QString &and_)
{
    const QList<Person> authors = people(item, "author");
    if (authors.isEmpty())
        return QStringLiteral("“") + field(item, "title") + QStringLiteral("”");
    if (authors.size() == 1)
        return surname(authors.at(0));
    if (authors.size() == 2)
        return surname(authors.at(0)) + QLatin1Char(' ') + and_ + QLatin1Char(' ') + surname(authors.at(1));
    return surname(authors.at(0)) + QStringLiteral(" et al.");
}

QString chicagoAuthors(const QJsonObject &item)
{
    const QList<Person> authors = people(item, "author");
    if (authors.isEmpty())
        return field(item, "title");
    if (authors.size() >= 4)
        return surname(authors.at(0)) + QStringLiteral(" et al.");
    QStringList names;
    for (const Person &p : authors)
        names << surname(p);
    return joined(names, QStringLiteral("and"), authors.size() > 2);
}

} // namespace

QString id(Style style)
{
    switch (style) {
    case Style::Apa: return QStringLiteral("apa");
    case Style::Mla: return QStringLiteral("mla");
    case Style::Chicago: return QStringLiteral("chicago");
    case Style::Ieee: return QStringLiteral("ieee");
    }
    return QStringLiteral("apa");
}

Style fromId(const QString &value)
{
    for (const Style s : {Style::Apa, Style::Mla, Style::Chicago, Style::Ieee})
        if (id(s) == value)
            return s;
    return Style::Apa;
}

QStringList ids()
{
    return {QStringLiteral("apa"), QStringLiteral("mla"), QStringLiteral("chicago"), QStringLiteral("ieee")};
}

QString displayName(Style style)
{
    switch (style) {
    case Style::Apa: return QStringLiteral("APA 7");
    case Style::Mla: return QStringLiteral("MLA 9");
    case Style::Chicago: return QStringLiteral("Chicago (author-date)");
    case Style::Ieee: return QStringLiteral("IEEE");
    }
    return {};
}

QString plain(const Runs &runs)
{
    QString out;
    for (const Run &run : runs)
        out += run.text;
    return out;
}

QString year(const QJsonObject &item)
{
    const QJsonObject issued = item.value(QLatin1String("issued")).toObject();
    const QJsonArray parts = issued.value(QLatin1String("date-parts")).toArray();
    if (!parts.isEmpty() && !parts.at(0).toArray().isEmpty()) {
        const QJsonValue y = parts.at(0).toArray().at(0);
        return y.isDouble() ? QString::number(y.toInt()) : y.toString();
    }
    const QString literal = issued.value(QLatin1String("literal")).toString();
    if (!literal.isEmpty())
        return literal;
    const QJsonValue raw = item.value(QLatin1String("issued"));
    return raw.isString() ? raw.toString() : QString();
}

QString sortKey(const QJsonObject &item)
{
    const QList<Person> authors = people(item, "author");
    const QString first = authors.isEmpty() ? field(item, "title") : surname(authors.at(0)) + QLatin1Char(' ') + authors.at(0).given;
    return first.toLower() + QLatin1Char('\x01') + year(item);
}

bool ordersByCitation(Style style)
{
    return style == Style::Ieee;
}

QString bibliographyTitle(Style style)
{
    return style == Style::Mla ? QStringLiteral("Works Cited") : QStringLiteral("References");
}

QString inText(Style style, const QList<QJsonObject> &items, const QString &locator, const QList<int> &numbers)
{
    const QString bare = bareLocator(locator);
    QStringList parts;
    QList<QJsonObject> ordered = items;
    if (style == Style::Apa)
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const QJsonObject &a, const QJsonObject &b) { return sortKey(a) < sortKey(b); });
    for (qsizetype i = 0; i < ordered.size(); ++i) {
        const QJsonObject &item = ordered.at(i);
        const bool last = i == ordered.size() - 1;
        const QString y = year(item).isEmpty() ? QStringLiteral("n.d.") : year(item);
        switch (style) {
        case Style::Apa: {
            QString part = apaAuthors(item) + QStringLiteral(", ") + y;
            if (last && !bare.isEmpty())
                part += QStringLiteral(", ") + pageLabel(bare) + bare;
            parts << part;
            break;
        }
        case Style::Mla: {
            QString part = mlaAuthors(item, QStringLiteral("and"));
            if (last && !bare.isEmpty())
                part += QLatin1Char(' ') + bare;
            parts << part;
            break;
        }
        case Style::Chicago: {
            QString part = chicagoAuthors(item) + QLatin1Char(' ') + y;
            if (last && !bare.isEmpty())
                part += QStringLiteral(", ") + bare;
            parts << part;
            break;
        }
        case Style::Ieee: {
            QString part = QLatin1Char('[') + QString::number(numbers.value(i));
            if (last && !bare.isEmpty())
                part += QStringLiteral(", ") + pageLabel(bare) + bare;
            parts << part + QLatin1Char(']');
            break;
        }
        }
    }
    if (style == Style::Ieee)
        return parts.join(QStringLiteral(", "));
    return QLatin1Char('(') + parts.join(QStringLiteral("; ")) + QLatin1Char(')');
}

Runs entry(Style style, const QJsonObject &item, int number)
{
    Builder b;
    const QList<Person> authors = people(item, "author");
    const QList<Person> editors = people(item, "editor");
    const QString title = field(item, "title");
    const QString container = field(item, "container-title");
    const QString publisher = field(item, "publisher");
    const QString place = field(item, "publisher-place");
    const QString volume = field(item, "volume");
    const QString issue = field(item, "issue");
    const QString page = pages(field(item, "page"));
    const QString edition = field(item, "edition");
    const QString y = year(item);
    const bool article = isArticle(item), chapter = isChapter(item), web = isWeb(item);
    const bool titleItalic = !article && !chapter && !web;

    switch (style) {
    case Style::Apa: {
        QStringList names;
        for (const Person &p : authors)
            names << familyFirst(p, true);
        if (names.size() > 20)
            names = names.mid(0, 19) << QStringLiteral("…") << names.constLast();
        const QString authorText = joined(names, QStringLiteral("&"), true);
        if (!authorText.isEmpty())
            b.text(closed(authorText) + QLatin1Char(' '));
        else if (titleItalic)
            b.italic(title).text(QStringLiteral(". "));
        else
            b.text(closed(title) + QLatin1Char(' '));
        b.text(QStringLiteral("(") + (y.isEmpty() ? QStringLiteral("n.d.") : y) + QStringLiteral("). "));
        if (!authorText.isEmpty()) {
            if (titleItalic)
                b.italic(title);
            else
                b.text(title);
            if (titleItalic && !edition.isEmpty())
                b.text(QStringLiteral(" (") + edition + QStringLiteral(" ed.)"));
            b.text(QStringLiteral(". "));
        }
        if (article) {
            b.italic(container);
            if (!volume.isEmpty())
                b.text(QStringLiteral(", ")).italic(volume);
            if (!issue.isEmpty())
                b.text(QLatin1Char('(') + issue + QLatin1Char(')'));
            if (!page.isEmpty())
                b.text(QStringLiteral(", ") + page);
            b.text(QStringLiteral(". "));
        } else if (chapter) {
            QStringList eds;
            for (const Person &p : editors)
                eds << givenFirst(p, true);
            b.text(QStringLiteral("In "));
            if (!eds.isEmpty())
                b.text(joined(eds, QStringLiteral("&"), true) + (eds.size() > 1 ? QStringLiteral(" (Eds.), ") : QStringLiteral(" (Ed.), ")));
            b.italic(container);
            if (!page.isEmpty())
                b.text(QStringLiteral(" (") + pageLabel(page) + page + QLatin1Char(')'));
            b.text(QStringLiteral(". "));
            if (!publisher.isEmpty())
                b.text(publisher + QStringLiteral(". "));
        } else if (web) {
            if (!container.isEmpty())
                b.text(container + QStringLiteral(". "));
        } else if (!publisher.isEmpty()) {
            b.text(publisher + QStringLiteral(". "));
        }
        b.text(link(item));
        break;
    }
    case Style::Mla: {
        QString authorText;
        if (authors.size() == 1)
            authorText = familyFirst(authors.at(0), false);
        else if (authors.size() == 2)
            authorText = familyFirst(authors.at(0), false) + QStringLiteral(", and ") + givenFirst(authors.at(1), false);
        else if (authors.size() > 2)
            authorText = familyFirst(authors.at(0), false) + QStringLiteral(", et al");
        if (!authorText.isEmpty())
            b.text(closed(authorText) + QLatin1Char(' '));
        if (titleItalic)
            b.italic(title).text(QStringLiteral(". "));
        else
            b.text(QStringLiteral("“") + closed(title) + QStringLiteral("” "));
        QStringList rest;
        if (!titleItalic && !container.isEmpty())
            b.italic(container).text(QStringLiteral(", "));
        if (chapter && !editors.isEmpty()) {
            QStringList eds;
            for (const Person &p : editors)
                eds << givenFirst(p, false);
            rest << QStringLiteral("edited by ") + joined(eds, QStringLiteral("and"), eds.size() > 2);
        }
        if (!volume.isEmpty())
            rest << QStringLiteral("vol. ") + volume;
        if (!issue.isEmpty())
            rest << QStringLiteral("no. ") + issue;
        if (!publisher.isEmpty() && !article)
            rest << publisher;
        if (!y.isEmpty())
            rest << y;
        if (!page.isEmpty())
            rest << pageLabel(page) + page;
        const QString url = link(item);
        if (!url.isEmpty())
            rest << url;
        if (!rest.isEmpty())
            b.text(rest.join(QStringLiteral(", ")) + QLatin1Char('.'));
        break;
    }
    case Style::Chicago: {
        QStringList names;
        for (qsizetype i = 0; i < authors.size(); ++i)
            names << (i == 0 ? familyFirst(authors.at(i), false) : givenFirst(authors.at(i), false));
        if (names.size() > 10)
            names = names.mid(0, 7) << QStringLiteral("et al.");
        const QString authorText = names.size() > 10 ? names.join(QStringLiteral(", ")) : joined(names, QStringLiteral("and"), names.size() > 2);
        if (!authorText.isEmpty())
            b.text(closed(authorText) + QLatin1Char(' '));
        b.text((y.isEmpty() ? QStringLiteral("n.d.") : y) + QStringLiteral(". "));
        if (titleItalic)
            b.italic(title).text(QStringLiteral(". "));
        else
            b.text(QStringLiteral("“") + closed(title) + QStringLiteral("” "));
        if (article) {
            b.italic(container);
            if (!volume.isEmpty())
                b.text(QLatin1Char(' ') + volume);
            if (!issue.isEmpty())
                b.text(QStringLiteral(" (") + issue + QLatin1Char(')'));
            if (!page.isEmpty())
                b.text(QStringLiteral(": ") + page);
            b.text(QStringLiteral(". "));
        } else if (chapter) {
            b.text(QStringLiteral("In ")).italic(container);
            if (!editors.isEmpty()) {
                QStringList eds;
                for (const Person &p : editors)
                    eds << givenFirst(p, false);
                b.text(QStringLiteral(", edited by ") + joined(eds, QStringLiteral("and"), eds.size() > 2));
            }
            if (!page.isEmpty())
                b.text(QStringLiteral(", ") + page);
            b.text(QStringLiteral(". "));
        } else if (web) {
            if (!container.isEmpty())
                b.text(container + QStringLiteral(". "));
        }
        if (!article && !web && (!publisher.isEmpty() || !place.isEmpty()))
            b.text((place.isEmpty() ? QString() : place + QStringLiteral(": ")) + publisher + QStringLiteral(". "));
        const QString url = link(item);
        if (!url.isEmpty())
            b.text(closed(url));
        break;
    }
    case Style::Ieee: {
        b.text(QLatin1Char('[') + QString::number(number) + QStringLiteral("] "));
        QStringList names;
        for (const Person &p : authors)
            names << givenFirst(p, true);
        if (names.size() > 6)
            names = {names.at(0) + QStringLiteral(" et al.")};
        if (!names.isEmpty())
            b.text(joined(names, QStringLiteral("and"), names.size() > 2) + QStringLiteral(", "));
        if (titleItalic) {
            b.italic(title);
            if (!edition.isEmpty())
                b.text(QStringLiteral(", ") + edition + QStringLiteral(" ed"));
            b.text(QStringLiteral(". "));
            if (!place.isEmpty() || !publisher.isEmpty())
                b.text((place.isEmpty() ? QString() : place + QStringLiteral(": ")) + publisher
                       + (y.isEmpty() ? QString() : QStringLiteral(", ")));
            b.text(y.isEmpty() ? QString() : closed(y));
        } else {
            b.text(QStringLiteral("“") + title + QStringLiteral(",” "));
            QStringList rest;
            if (chapter)
                b.text(QStringLiteral("in ")).italic(container);
            else if (web)
                b.text(container.isEmpty() ? QString() : container);
            else
                b.italic(container);
            if (!volume.isEmpty())
                rest << QStringLiteral("vol. ") + volume;
            if (!issue.isEmpty())
                rest << QStringLiteral("no. ") + issue;
            if (!page.isEmpty())
                rest << pageLabel(page) + page;
            if (!y.isEmpty())
                rest << y;
            if (!rest.isEmpty())
                b.text(QStringLiteral(", ") + rest.join(QStringLiteral(", ")));
            const QString doi = doiUrl(item);
            if (!doi.isEmpty())
                b.text(QStringLiteral(", doi: ") + doi.mid(QStringLiteral("https://doi.org/").size()) + QLatin1Char('.'));
            else if (!field(item, "URL").isEmpty())
                b.text(QStringLiteral(". [Online]. Available: ") + field(item, "URL"));
            else
                b.text(QStringLiteral("."));
        }
        break;
    }
    }
    Runs runs = b.runs();
    // No trailing space.
    while (!runs.isEmpty() && runs.last().text.endsWith(u' '))
        runs.last().text.chop(1);
    return runs;
}

} // namespace CitationStyles
