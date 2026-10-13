#pragma once

// Package: the zip archives .odt and .docx files are, read and written
// whole. Both formats are small enough to hold in memory.

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace Package {

struct Entry
{
    QString name; // path inside the archive
    QByteArray data;
};

// Every entry of the archive in `bytes` (or the file at `path`), in order;
// none if it isn't a readable zip.
std::optional<QList<Entry>> read(const QByteArray &bytes);
std::optional<QList<Entry>> readFile(const QString &path);

// An archive of `entries`, in order. An entry named "mimetype" (ODF's,
// which must come first) is stored uncompressed; the rest are deflated.
// Empty on failure.
QByteArray write(const QList<Entry> &entries);

// The entry named `name`, or nullptr.
Entry *find(QList<Entry> &entries, const QString &name);
const Entry *find(const QList<Entry> &entries, const QString &name);

} // namespace Package
