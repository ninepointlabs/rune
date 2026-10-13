#include "InlineObjects.h"

#include "PageParts.h"

namespace InlineObjects {

const QList<Kind> &kinds()
{
    static const QList<Kind> all = {PageParts::fieldKind()};
    return all;
}

const Kind *forFormat(const QTextFormat &format)
{
    if (!format.isCharFormat())
        return nullptr;
    for (const Kind &kind : kinds())
        if (kind.objectType == format.objectType())
            return &kind;
    return nullptr;
}

const Kind *forOdfElement(QStringView namespaceUri, QStringView name, DocumentParts::Kind part)
{
    for (const Kind &kind : kinds()) {
        if (!kind.allowedIn.contains(part))
            continue;
        for (const OdfElement &element : kind.odfElements)
            if (element.namespaceUri == namespaceUri && element.name == name)
                return &kind;
    }
    return nullptr;
}

void installAll(QTextDocument *doc, DocumentParts::Kind part)
{
    for (const Kind &kind : kinds())
        if (kind.allowedIn.contains(part))
            kind.install(doc);
}

} // namespace InlineObjects
