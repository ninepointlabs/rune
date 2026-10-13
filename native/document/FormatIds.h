#pragma once

// Every custom id Rune stores in a QTextFormat, allocated here so features
// can't collide. Qt reserves the ranges below UserObject / UserProperty.

#include <QTextFormat>

namespace FormatIds {

// Inline object types (QTextFormat::ObjectType of an object character; see
// InlineObjects).
constexpr int kPageFieldObject = QTextFormat::UserObject + 1;
constexpr int kCitationObject = QTextFormat::UserObject + 2;

// Properties.
// The page layout, on the body's root frame (PageLayout::storeIn()): 8
// values, kPageLayout to kPageLayout + 7.
constexpr int kPageLayout = QTextFormat::UserProperty + 100;
// Which page field an object character is (PageParts::Field).
constexpr int kPageFieldKind = QTextFormat::UserProperty + 120;
// A citation (Citations): the source ids it cites (QStringList), its
// locator, and while a file is being read, the number of the bookmark it was
// saved between and the text it had there.
constexpr int kCitationIds = QTextFormat::UserProperty + 130;
constexpr int kCitationLocator = QTextFormat::UserProperty + 131;
constexpr int kCitationSaved = QTextFormat::UserProperty + 132;
constexpr int kCitationSavedText = QTextFormat::UserProperty + 133;
// On a block: part of the generated bibliography (Citations): 1 its
// heading, 2 an entry.
constexpr int kBibliography = QTextFormat::UserProperty + 140;

} // namespace FormatIds
