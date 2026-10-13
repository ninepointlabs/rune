#include "StoryNotes.h"

namespace StoryNotes {

QStringList kinds()
{
    return {QStringLiteral("character"), QStringLiteral("place"), QStringLiteral("research")};
}

qsizetype indexOf(const QJsonArray &notes, const QString &id)
{
    for (qsizetype i = 0; i < notes.size(); ++i)
        if (notes.at(i).toObject().value(QLatin1String("id")).toString() == id)
            return i;
    return -1;
}

QString freshId(const QJsonArray &notes)
{
    int n = 1;
    while (indexOf(notes, QStringLiteral("n%1").arg(n)) >= 0)
        ++n;
    return QStringLiteral("n%1").arg(n);
}

int merge(QJsonArray &notes, const QJsonArray &incoming)
{
    int changed = 0;
    for (const QJsonValue &value : incoming) {
        if (!value.isObject())
            continue;
        QJsonObject note = value.toObject();
        QString kind = note.value(QLatin1String("kind")).toString().toLower();
        if (note.contains(QLatin1String("kind")) && !kinds().contains(kind)) {
            kind = QStringLiteral("research");
            note.insert(QLatin1String("kind"), kind);
        }
        qsizetype at = indexOf(notes, note.value(QLatin1String("id")).toString());
        if (at < 0 && note.contains(QLatin1String("name"))) {
            const QString name = note.value(QLatin1String("name")).toString();
            for (qsizetype i = 0; i < notes.size() && at < 0; ++i) {
                const QJsonObject existing = notes.at(i).toObject();
                if (existing.value(QLatin1String("name")).toString().compare(name, Qt::CaseInsensitive) == 0
                    && (kind.isEmpty() || existing.value(QLatin1String("kind")).toString() == kind))
                    at = i;
            }
        }
        if (note.value(QLatin1String("deleted")).toBool()) {
            if (at >= 0) {
                notes.removeAt(at);
                ++changed;
            }
            continue;
        }
        if (at >= 0) {
            QJsonObject existing = notes.at(at).toObject();
            const QString id = existing.value(QLatin1String("id")).toString();
            for (auto it = note.constBegin(); it != note.constEnd(); ++it)
                existing.insert(it.key(), it.value());
            existing.insert(QLatin1String("id"), id);
            if (existing != notes.at(at).toObject()) {
                notes.replace(at, existing);
                ++changed;
            }
            continue;
        }
        if (note.value(QLatin1String("name")).toString().trimmed().isEmpty())
            continue;
        note.insert(QLatin1String("id"), freshId(notes));
        if (kind.isEmpty())
            note.insert(QLatin1String("kind"), QStringLiteral("research"));
        notes.append(note);
        ++changed;
    }
    return changed;
}

} // namespace StoryNotes
