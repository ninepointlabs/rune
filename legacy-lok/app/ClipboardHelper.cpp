#include "ClipboardHelper.h"

#include <QClipboard>
#include <QGuiApplication>

QString ClipboardHelper::text() const
{
    return QGuiApplication::clipboard()->text(QClipboard::Clipboard);
}

void ClipboardHelper::setText(const QString &text)
{
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
}
