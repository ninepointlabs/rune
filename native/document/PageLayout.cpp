#include "PageLayout.h"

#include "FormatIds.h"

#include <QTextDocument>
#include <QTextFrame>

#include <algorithm>
#include <cmath>

namespace {

constexpr qreal kPxPerMm = 96.0 / 25.4;
// Page setup never leaves less text area than this, either way.
constexpr qreal kMinTextMm = 20.0;

} // namespace

bool PageLayout::usesLetter(QLocale::Territory territory)
{
    switch (territory) {
    case QLocale::UnitedStates:
    case QLocale::Canada:
    case QLocale::Mexico:
    case QLocale::Philippines:
    case QLocale::Chile:
    case QLocale::Colombia:
    case QLocale::Venezuela:
    case QLocale::CostaRica:
    case QLocale::Guatemala:
    case QLocale::Panama:
    case QLocale::PuertoRico:
    case QLocale::DominicanRepublic:
        return true;
    default:
        return false;
    }
}

PageLayout PageLayout::defaultFor(QLocale::Territory territory)
{
    PageLayout layout; // Letter
    if (!usesLetter(territory)) {
        layout.widthMm = 210.0;
        layout.heightMm = 297.0;
    }
    return layout;
}

QVariantMap PageLayout::toMap() const
{
    return {{QStringLiteral("width"), widthMm}, {QStringLiteral("height"), heightMm},
            {QStringLiteral("top"), topMm}, {QStringLiteral("bottom"), bottomMm},
            {QStringLiteral("left"), leftMm}, {QStringLiteral("right"), rightMm},
            {QStringLiteral("header"), headerMm}, {QStringLiteral("footer"), footerMm}};
}

PageLayout PageLayout::fromMap(const QVariantMap &map, const PageLayout &fallback)
{
    const auto value = [&](const char *key, qreal current) {
        bool ok = false;
        const qreal v = map.value(QLatin1String(key)).toDouble(&ok);
        return ok && std::isfinite(v) ? v : current;
    };
    PageLayout layout;
    layout.widthMm = value("width", fallback.widthMm);
    layout.heightMm = value("height", fallback.heightMm);
    if (layout.widthMm <= kMinTextMm)
        layout.widthMm = fallback.widthMm;
    if (layout.heightMm <= kMinTextMm)
        layout.heightMm = fallback.heightMm;
    layout.topMm = std::max<qreal>(0, value("top", fallback.topMm));
    layout.bottomMm = std::max<qreal>(0, value("bottom", fallback.bottomMm));
    layout.leftMm = std::max<qreal>(0, value("left", fallback.leftMm));
    layout.rightMm = std::max<qreal>(0, value("right", fallback.rightMm));
    // Margins that would leave too little text area shrink, each side equally.
    const auto fit = [](qreal size, qreal &a, qreal &b) {
        const qreal excess = a + b - (size - kMinTextMm);
        if (excess > 0) {
            a = std::max<qreal>(0, a - excess / 2);
            b = std::max<qreal>(0, size - kMinTextMm - a);
        }
    };
    fit(layout.widthMm, layout.leftMm, layout.rightMm);
    fit(layout.heightMm, layout.topMm, layout.bottomMm);
    layout.headerMm = std::clamp<qreal>(value("header", fallback.headerMm), 0, layout.topMm);
    layout.footerMm = std::clamp<qreal>(value("footer", fallback.footerMm), 0, layout.bottomMm);
    return layout;
}

QSizeF PageLayout::sizePx() const
{
    return {widthMm * kPxPerMm, heightMm * kPxPerMm};
}

QMarginsF PageLayout::marginsPx() const
{
    return {leftMm * kPxPerMm, topMm * kPxPerMm, rightMm * kPxPerMm, bottomMm * kPxPerMm};
}

namespace {

enum PageProperty {
    kPageWidthMm = FormatIds::kPageLayout,
    kPageHeightMm,
    kPageTopMm,
    kPageBottomMm,
    kPageLeftMm,
    kPageRightMm,
    kPageHeaderMm,
    kPageFooterMm,
};

} // namespace

std::optional<PageLayout> PageLayout::fromDocument(const QTextDocument *doc)
{
    const QTextFrameFormat format = doc->rootFrame()->frameFormat();
    if (!format.hasProperty(kPageWidthMm))
        return std::nullopt;
    PageLayout layout;
    layout.widthMm = format.doubleProperty(kPageWidthMm);
    layout.heightMm = format.doubleProperty(kPageHeightMm);
    layout.topMm = format.doubleProperty(kPageTopMm);
    layout.bottomMm = format.doubleProperty(kPageBottomMm);
    layout.leftMm = format.doubleProperty(kPageLeftMm);
    layout.rightMm = format.doubleProperty(kPageRightMm);
    layout.headerMm = format.doubleProperty(kPageHeaderMm);
    layout.footerMm = format.doubleProperty(kPageFooterMm);
    return layout;
}

void PageLayout::storeIn(QTextDocument *doc) const
{
    QTextFrameFormat format = doc->rootFrame()->frameFormat();
    const QMarginsF margins = marginsPx();
    format.setTopMargin(margins.top());
    format.setBottomMargin(margins.bottom());
    format.setLeftMargin(margins.left());
    format.setRightMargin(margins.right());
    format.setProperty(kPageWidthMm, widthMm);
    format.setProperty(kPageHeightMm, heightMm);
    format.setProperty(kPageTopMm, topMm);
    format.setProperty(kPageBottomMm, bottomMm);
    format.setProperty(kPageLeftMm, leftMm);
    format.setProperty(kPageRightMm, rightMm);
    format.setProperty(kPageHeaderMm, headerMm);
    format.setProperty(kPageFooterMm, footerMm);
    doc->rootFrame()->setFrameFormat(format);
}

bool PageLayout::operator==(const PageLayout &other) const
{
    const auto same = [](qreal a, qreal b) { return std::abs(a - b) < 0.01; };
    return same(widthMm, other.widthMm) && same(heightMm, other.heightMm) && same(topMm, other.topMm)
        && same(bottomMm, other.bottomMm) && same(leftMm, other.leftMm) && same(rightMm, other.rightMm)
        && same(headerMm, other.headerMm) && same(footerMm, other.footerMm);
}
