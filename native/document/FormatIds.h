#pragma once

// Every custom id Rune stores in a QTextFormat, allocated here so features
// can't collide. Qt reserves the ranges below UserObject / UserProperty.

#include <QTextFormat>

namespace FormatIds {

// Inline object types (QTextFormat::ObjectType of an object character; see
// InlineObjects).
constexpr int kPageFieldObject = QTextFormat::UserObject + 1;

// Properties.
// The page layout, on the body's root frame (PageLayout::storeIn()): 8
// values, kPageLayout to kPageLayout + 7.
constexpr int kPageLayout = QTextFormat::UserProperty + 100;
// Which page field an object character is (PageParts::Field).
constexpr int kPageFieldKind = QTextFormat::UserProperty + 120;

} // namespace FormatIds
