#include "Package.h"

#include <QBuffer>

#include <quazip.h>
#include <quazipfile.h>
#include <quazipnewinfo.h>

namespace Package {

namespace {

std::optional<QList<Entry>> readFrom(QuaZip &zip)
{
    if (!zip.open(QuaZip::mdUnzip))
        return std::nullopt;
    QList<Entry> entries;
    for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
        const QString name = zip.getCurrentFileName();
        QuaZipFile file(&zip);
        if (!file.open(QIODevice::ReadOnly))
            return std::nullopt;
        entries.append({name, file.readAll()});
        file.close();
    }
    zip.close();
    return entries;
}

} // namespace

std::optional<QList<Entry>> read(const QByteArray &bytes)
{
    QBuffer in;
    in.setData(bytes);
    QuaZip zip(&in);
    return readFrom(zip);
}

std::optional<QList<Entry>> readFile(const QString &path)
{
    QuaZip zip(path);
    return readFrom(zip);
}

QByteArray write(const QList<Entry> &entries)
{
    QBuffer out; // QuaZip opens it
    QuaZip zip(&out);
    if (!zip.open(QuaZip::mdCreate))
        return {};
    for (const Entry &entry : entries) {
        QuaZipFile file(&zip);
        const bool stored = entry.name == QLatin1String("mimetype");
        if (!file.open(QIODevice::WriteOnly, QuaZipNewInfo(entry.name), nullptr, 0, stored ? 0 : Z_DEFLATED,
                       stored ? 0 : Z_DEFAULT_COMPRESSION)
            || file.write(entry.data) != entry.data.size())
            return {};
        file.close();
        if (file.getZipError() != ZIP_OK)
            return {};
    }
    zip.close();
    if (zip.getZipError() != ZIP_OK)
        return {};
    return out.data();
}

Entry *find(QList<Entry> &entries, const QString &name)
{
    for (Entry &entry : entries)
        if (entry.name == name)
            return &entry;
    return nullptr;
}

const Entry *find(const QList<Entry> &entries, const QString &name)
{
    for (const Entry &entry : entries)
        if (entry.name == name)
            return &entry;
    return nullptr;
}

} // namespace Package
