#include "Sources.h"

#include "CitationStyles.h"

#include <QRegularExpression>

namespace Sources {

namespace {

QString firstFamily(const QJsonObject &item)
{
    for (const char *role : {"author", "editor"}) {
        const QJsonArray people = item.value(QLatin1String(role)).toArray();
        if (people.isEmpty())
            continue;
        const QJsonObject p = people.at(0).toObject();
        const QString name = p.value(QLatin1String("family")).toString();
        return name.isEmpty() ? p.value(QLatin1String("literal")).toString() : name;
    }
    return item.value(QLatin1String("title")).toString().section(QLatin1Char(' '), 0, 0);
}

// ASCII letters of `text`, lower case: "Ödön" -> "odon".
QString keyPart(const QString &text)
{
    QString out;
    for (const QChar ch : text.normalized(QString::NormalizationForm_KD))
        if (ch.isLetterOrNumber() && ch.unicode() < 0x80)
            out += ch.toLower();
    return out;
}

bool taken(const QJsonArray &existing, const QString &id, const QString &replacing)
{
    if (id == replacing)
        return false;
    return indexOf(existing, id) >= 0;
}

} // namespace

qsizetype indexOf(const QJsonArray &sources, const QString &id)
{
    for (qsizetype i = 0; i < sources.size(); ++i)
        if (sources.at(i).toObject().value(QLatin1String("id")).toString() == id)
            return i;
    return -1;
}

QJsonObject find(const QJsonArray &sources, const QString &id)
{
    const qsizetype i = indexOf(sources, id);
    return i < 0 ? QJsonObject() : sources.at(i).toObject();
}

QJsonObject normalized(QJsonObject item, const QJsonArray &existing, const QString &replacing)
{
    if (item.value(QLatin1String("type")).toString().isEmpty())
        item.insert(QLatin1String("type"), QStringLiteral("book"));
    static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_:.#$%&+?<>~/-]*$"));
    const QString own = item.value(QLatin1String("id")).toString();
    if (validId.match(own).hasMatch() && !taken(existing, own, replacing))
        return item;
    QString base = keyPart(firstFamily(item));
    if (base.isEmpty())
        base = QStringLiteral("source");
    base += CitationStyles::year(item).left(4);
    QString id = base;
    for (int n = 0; taken(existing, id, replacing); ++n)
        id = base + (n < 26 ? QString(QChar(u'a' + n)) : QString::number(n));
    item.insert(QLatin1String("id"), id);
    return item;
}

QString label(const QJsonObject &item)
{
    QStringList names;
    for (const QJsonValue &v : item.value(QLatin1String("author")).toArray()) {
        const QJsonObject p = v.toObject();
        const QString n = p.value(QLatin1String("family")).toString();
        names << (n.isEmpty() ? p.value(QLatin1String("literal")).toString() : n);
    }
    QString who = names.isEmpty() ? QString() : names.size() > 2 ? names.at(0) + QStringLiteral(" et al.") : names.join(QStringLiteral(" & "));
    const QString year = CitationStyles::year(item);
    QString head = (who + QLatin1Char(' ') + year).trimmed();
    const QString title = item.value(QLatin1String("title")).toString();
    return head.isEmpty() ? title : title.isEmpty() ? head : head + QStringLiteral(" · ") + title;
}

bool isUnreviewed(const QJsonObject &item)
{
    return item.value(QLatin1String("custom")).toObject().value(QLatin1String("unreviewed")).toBool();
}

QJsonObject withUnreviewed(QJsonObject item, bool unreviewed)
{
    QJsonObject custom = item.value(QLatin1String("custom")).toObject();
    if (unreviewed)
        custom.insert(QLatin1String("unreviewed"), true);
    else
        custom.remove(QLatin1String("unreviewed"));
    if (custom.isEmpty())
        item.remove(QLatin1String("custom"));
    else
        item.insert(QLatin1String("custom"), custom);
    return item;
}

QString linkState(const QJsonObject &item)
{
    return item.value(QLatin1String("custom")).toObject().value(QLatin1String("link")).toString();
}

QJsonObject withLinkState(QJsonObject item, const QString &state)
{
    QJsonObject custom = item.value(QLatin1String("custom")).toObject();
    if (state.isEmpty())
        custom.remove(QLatin1String("link"));
    else
        custom.insert(QLatin1String("link"), state);
    if (custom.isEmpty())
        item.remove(QLatin1String("custom"));
    else
        item.insert(QLatin1String("custom"), custom);
    return item;
}

QStringList merge(QJsonArray &sources, const QJsonArray &incoming, bool unreviewed)
{
    QStringList ids;
    for (const QJsonValue &value : incoming) {
        if (!value.isObject())
            continue;
        QJsonObject item = value.toObject();
        if (unreviewed)
            item = withUnreviewed(item, true);
        const QString id = item.value(QLatin1String("id")).toString();
        const qsizetype existing = id.isEmpty() ? -1 : indexOf(sources, id);
        if (existing >= 0) {
            item = normalized(item, sources, id);
            sources.replace(existing, item);
        } else {
            item = normalized(item, sources);
            sources.append(item);
        }
        ids << item.value(QLatin1String("id")).toString();
    }
    return ids;
}

namespace {

// The value starting at `pos` (after "="): {braced}, "quoted" or a bare
// word/number; `pos` moves past it.
QString bibValue(const QString &text, qsizetype &pos)
{
    while (pos < text.size() && text.at(pos).isSpace())
        ++pos;
    QString out;
    if (pos < text.size() && (text.at(pos) == u'{' || text.at(pos) == u'"')) {
        const QChar close = text.at(pos) == u'{' ? u'}' : u'"';
        int depth = 0;
        ++pos;
        for (; pos < text.size(); ++pos) {
            const QChar ch = text.at(pos);
            if (ch == u'{') {
                ++depth;
                continue;
            }
            if (ch == u'}' && depth > 0) {
                --depth;
                continue;
            }
            if (ch == close && depth == 0) {
                ++pos;
                break;
            }
            out += ch;
        }
    } else {
        while (pos < text.size() && !QStringView(u",}").contains(text.at(pos)))
            out += text.at(pos++);
    }
    return out.simplified();
}

QJsonArray bibNames(const QString &value)
{
    QJsonArray out;
    for (QString name : value.split(QRegularExpression(QStringLiteral("\\s+and\\s+")), Qt::SkipEmptyParts)) {
        name = name.trimmed();
        if (name.compare(QLatin1String("others"), Qt::CaseInsensitive) == 0)
            continue;
        QJsonObject person;
        if (name.contains(u',')) {
            person.insert(QLatin1String("family"), name.section(u',', 0, 0).trimmed());
            person.insert(QLatin1String("given"), name.section(u',', 1).trimmed());
        } else if (name.contains(u' ')) {
            person.insert(QLatin1String("family"), name.section(u' ', -1));
            person.insert(QLatin1String("given"), name.section(u' ', 0, -2));
        } else {
            person.insert(QLatin1String("family"), name);
        }
        out.append(person);
    }
    return out;
}

} // namespace

QJsonArray fromBibTeX(const QString &bibtex)
{
    static const QHash<QString, QString> types = {
        {QStringLiteral("article"), QStringLiteral("article-journal")},
        {QStringLiteral("book"), QStringLiteral("book")},
        {QStringLiteral("inbook"), QStringLiteral("chapter")},
        {QStringLiteral("incollection"), QStringLiteral("chapter")},
        {QStringLiteral("inproceedings"), QStringLiteral("paper-conference")},
        {QStringLiteral("conference"), QStringLiteral("paper-conference")},
        {QStringLiteral("phdthesis"), QStringLiteral("thesis")},
        {QStringLiteral("mastersthesis"), QStringLiteral("thesis")},
        {QStringLiteral("techreport"), QStringLiteral("report")},
        {QStringLiteral("online"), QStringLiteral("webpage")},
        {QStringLiteral("misc"), QStringLiteral("document")},
    };
    static const QRegularExpression entryStart(QStringLiteral("@\\s*(\\w+)\\s*\\{\\s*([^,\\s]*)\\s*,"));
    QJsonArray out;
    qsizetype pos = 0;
    for (QRegularExpressionMatch m = entryStart.match(bibtex, pos); m.hasMatch(); m = entryStart.match(bibtex, pos)) {
        const QString bibType = m.captured(1).toLower();
        pos = m.capturedEnd();
        if (bibType == QLatin1String("comment") || bibType == QLatin1String("string") || bibType == QLatin1String("preamble"))
            continue;
        QJsonObject item;
        item.insert(QLatin1String("type"), types.value(bibType, QStringLiteral("document")));
        if (!m.captured(2).isEmpty())
            item.insert(QLatin1String("id"), m.captured(2));
        // field = value, ... }
        static const QRegularExpression fieldName(QStringLiteral("\\G\\s*,?\\s*(\\w[\\w-]*)\\s*="));
        for (;;) {
            const QRegularExpressionMatch f = fieldName.match(bibtex, pos, QRegularExpression::NormalMatch,
                                                              QRegularExpression::AnchorAtOffsetMatchOption);
            if (!f.hasMatch())
                break;
            pos = f.capturedEnd();
            const QString name = f.captured(1).toLower();
            const QString value = bibValue(bibtex, pos);
            if (name == QLatin1String("author") || name == QLatin1String("editor"))
                item.insert(name, bibNames(value));
            else if (name == QLatin1String("title"))
                item.insert(QLatin1String("title"), value);
            else if (name == QLatin1String("journal") || name == QLatin1String("booktitle") || name == QLatin1String("journaltitle"))
                item.insert(QLatin1String("container-title"), value);
            else if (name == QLatin1String("year"))
                item.insert(QLatin1String("issued"), QJsonObject{{QLatin1String("date-parts"), QJsonArray{QJsonArray{value.toInt()}}}});
            else if (name == QLatin1String("volume") || name == QLatin1String("edition"))
                item.insert(name, value);
            else if (name == QLatin1String("number"))
                item.insert(QLatin1String("issue"), value);
            else if (name == QLatin1String("pages"))
                item.insert(QLatin1String("page"), QString(value).replace(QStringLiteral("--"), QStringLiteral("-")));
            else if (name == QLatin1String("publisher") || name == QLatin1String("institution") || name == QLatin1String("school"))
                item.insert(QLatin1String("publisher"), value);
            else if (name == QLatin1String("address") || name == QLatin1String("location"))
                item.insert(QLatin1String("publisher-place"), value);
            else if (name == QLatin1String("url"))
                item.insert(QLatin1String("URL"), value);
            else if (name == QLatin1String("doi"))
                item.insert(QLatin1String("DOI"), value);
        }
        if (!item.value(QLatin1String("title")).toString().isEmpty())
            out.append(item);
    }
    return out;
}

} // namespace Sources
