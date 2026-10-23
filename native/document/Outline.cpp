#include "Outline.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>

#include <algorithm>

namespace Outline {

namespace {

// The position just past the end of section `index`'s text: the next
// section of the same or a higher level's heading block, or the document's
// end (characterCount() - 1).
int sectionEnd(const QTextDocument *doc, const QList<Section> &all, int index)
{
    for (qsizetype i = index + 1; i < all.size(); ++i)
        if (all.at(i).level <= all.at(index).level)
            return all.at(i).position;
    return doc->characterCount() - 1;
}

} // namespace

int wordCount(const QString &text)
{
    int words = 0;
    bool inWord = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        const bool letter = ch.isLetterOrNumber() && ch != QChar::ObjectReplacementCharacter;
        const bool joiner = (ch == u'\'' || ch == QChar(0x2019) || ch == u'-') && inWord && i + 1 < text.size()
            && text.at(i + 1).isLetterOrNumber();
        if (letter && !inWord)
            ++words;
        inWord = letter || joiner;
    }
    return words;
}

int wordCount(const QTextDocument *doc)
{
    int words = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        words += wordCount(b.text());
    return words;
}

QList<Section> sections(const QTextDocument *doc)
{
    QList<Section> out;
    QList<int> blockWords; // per section, its own heading and text only
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const int level = b.blockFormat().headingLevel();
        if (level > 0) {
            out.append({b.text().trimmed(), level, b.position(), 0});
            // Counted as wordCount(doc) counts it, so the sections add up to the total.
            blockWords.append(wordCount(b.text()));
            continue;
        }
        if (!blockWords.isEmpty())
            blockWords.last() += wordCount(b.text());
    }
    // A section's words: its own and its subsections'.
    for (qsizetype i = 0; i < out.size(); ++i) {
        int words = blockWords.at(i);
        for (qsizetype j = i + 1; j < out.size() && out.at(j).level > out.at(i).level; ++j)
            words += blockWords.at(j);
        out[i].words = words;
    }
    return out;
}

bool moveSection(QTextDocument *doc, int from, int to)
{
    const QList<Section> all = sections(doc);
    if (from < 0 || from >= all.size() || to < 0 || to > all.size())
        return false;
    const int start = all.at(from).position;
    const int end = sectionEnd(doc, all, from);
    // Onto itself, just after itself, or into itself.
    const int target = to == all.size() ? doc->characterCount() - 1 : all.at(to).position;
    if (target >= start && target <= end)
        return false;

    const QTextBlock firstBlock = doc->findBlock(start);
    const QTextBlockFormat firstFormat = firstBlock.blockFormat();
    const QTextCharFormat firstChar = firstBlock.charFormat();
    // The section's text without its final paragraph break.
    const QTextBlock lastBlock = doc->findBlock(std::max(start, end - 1));
    const int textEnd = to == all.size() || end == doc->characterCount() - 1 ? lastBlock.position() + lastBlock.length() - 1
                                                                             : end - 1;
    QTextCursor source(doc);
    source.setPosition(start);
    source.setPosition(textEnd, QTextCursor::KeepAnchor);
    const QTextDocumentFragment fragment = source.selection();

    QTextCursor edit(doc);
    edit.beginEditBlock();
    QTextCursor dest(doc);
    if (to == all.size()) {
        // After the last section: a paragraph of its own at the end.
        dest.movePosition(QTextCursor::End);
        dest.insertBlock(firstFormat, firstChar);
        dest.insertFragment(fragment);
    } else {
        // Before the target heading, which keeps its formats.
        dest.setPosition(target);
        const QTextBlockFormat targetFormat = dest.blockFormat();
        const QTextCharFormat targetChar = dest.block().charFormat();
        dest.insertFragment(fragment);
        dest.insertBlock(targetFormat, targetChar);
    }
    // The fragment's first block took the format of the block it went into.
    QTextCursor(doc->findBlock(to == all.size() ? doc->characterCount() - 1 - (textEnd - start) : target))
        .setBlockFormat(firstFormat);
    // Remove the original, with one paragraph break.
    const int sourceStart = source.selectionStart();
    const int sourceEnd = source.selectionEnd();
    QTextCursor remove(doc);
    if (sourceEnd < doc->characterCount() - 1) {
        remove.setPosition(sourceStart);
        remove.setPosition(sourceEnd + 1, QTextCursor::KeepAnchor);
    } else {
        remove.setPosition(std::max(0, sourceStart - 1));
        remove.setPosition(sourceEnd, QTextCursor::KeepAnchor);
    }
    remove.removeSelectedText();
    edit.endEditBlock();
    return true;
}

QTextCharFormat headingLook(int level)
{
    QTextCharFormat format;
    if (level <= 0)
        return format;
    format.setFontWeight(QFont::Bold);
    format.setProperty(QTextFormat::FontSizeAdjustment, std::max(0, 4 - level));
    return format;
}

} // namespace Outline
