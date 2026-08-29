#pragma once
#include <algorithm>

// Tooltip placement — where the tooltip box goes relative to the mouse cursor.
//
// Pure and JUCE-free (header-only, no .cpp) so the unit tests can exercise the placement
// rule without a Graphics context or a live TooltipWindow. TooltipLAF::getTooltipBounds
// measures the text (which needs JUCE) and delegates the arithmetic here.
//
// This is JUCE's own rule from LookAndFeel_V2::getTooltipBounds, extracted rather than
// invented. It is reproduced instead of inherited because the project's LookAndFeel
// overrides getTooltipBounds purely to enlarge the tooltip font, and the size used to
// place the box has to match the size used to draw it.
//
// THE INVARIANT: the returned box must never contain the cursor. If it does,
// TooltipWindow::mouseEnter fires and hides the tip, and its 500 ms re-show window puts it
// straight back in the same place — a hard flicker. An earlier hand-written version of
// this rule placed the box's top edge 6 px *above* the cursor, so the cursor was always
// inside it vertically; only the horizontal offset kept them apart, and clamping to the
// parent removed even that near the edges. See the tooltip gotcha in CLAUDE.md.

namespace rbtip
{

// Gap between the cursor and the box. Horizontal is asymmetric because the box hangs to
// the right of the cursor but is right-aligned against it on the flipped side.
constexpr int kGapRight  = 24;   // cursor -> left edge, when placed to the right
constexpr int kGapLeft   = 12;   // right edge -> cursor, when placed to the left
constexpr int kGapBelow  =  6;   // cursor -> top edge, when placed below
constexpr int kGapAbove  =  6;   // bottom edge -> cursor, when placed above

struct Rect
{
    int x, y, w, h;

    constexpr int right()  const { return x + w; }
    constexpr int bottom() const { return y + h; }

    // Half-open on both axes, matching juce::Rectangle::contains — a cursor exactly on the
    // right or bottom edge is outside. That is the sense the flicker invariant needs.
    constexpr bool contains (int px, int py) const
    {
        return px >= x && px < right() && py >= y && py < bottom();
    }
};

// juce::Rectangle::constrainedWithin for the case that matters here: the box is never
// larger than the area, so this is a pure translation back inside.
inline Rect constrainedWithin (Rect r, Rect area)
{
    r.x = std::max (area.x, std::min (r.x, area.right()  - r.w));
    r.y = std::max (area.y, std::min (r.y, area.bottom() - r.h));
    return r;
}

// Places a tipW x tipH box near (posX, posY), all in the coordinate space of `area`
// (the tooltip window's parent). Flips to the far side on each axis past the midpoint, so
// the box grows away from the edge it is nearest and clamping has nothing to do.
//
// The vertical flip is what upholds the invariant. Clamping can still slide the box
// horizontally over the cursor's column, but with the box wholly above or wholly below,
// sharing a column is harmless.
inline Rect tooltipBounds (int tipW, int tipH, int posX, int posY, Rect area)
{
    const int x = posX > area.x + area.w / 2 ? posX - (tipW + kGapLeft)
                                             : posX + kGapRight;
    const int y = posY > area.y + area.h / 2 ? posY - (tipH + kGapAbove)
                                             : posY + kGapBelow;

    return constrainedWithin ({ x, y, tipW, tipH }, area);
}

} // namespace rbtip
