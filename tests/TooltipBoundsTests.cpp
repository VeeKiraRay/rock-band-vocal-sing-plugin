#include "TooltipBounds.h"
#include <catch2/catch_test_macros.hpp>

// rbtip::tooltipBounds — tooltip placement. Pure, so these run without a Graphics context
// or a live TooltipWindow.
//
// Background: tooltips flickered under the cursor. A box that contains the cursor makes
// TooltipWindow::mouseEnter hide the tip, and its 500 ms re-show window puts it straight
// back in the same place. Everything here exists to pin the one invariant that prevents
// that: the box never contains the cursor.

using rbtip::Rect;
using rbtip::tooltipBounds;

namespace
{
    // The plugin's real geometry. kWindowW is both the default and the *minimum* width, so
    // 600 is the worst case rather than an unusual one.
    constexpr int kEditorW = 600;
    constexpr int kEditorH = 400;

    constexpr Rect kEditor { 0, 0, kEditorW, kEditorH };

    // Widest a tooltip can get: the text layout wraps at 400 px and the LookAndFeel adds
    // 14 px of padding.
    constexpr int kMaxTipW = 414;
}

TEST_CASE ("tooltip bounds: the box never contains the cursor", "[tooltip]")
{
    // The regression guard. Sweeps every cursor position over the editor against tooltip
    // sizes from a one-liner up to the widest the layout can produce. Before the vertical
    // above/below flip was restored, the box's top edge sat 6 px above the cursor, so the
    // cursor was always inside it vertically and any horizontal clamp put it fully inside.
    for (const int tipW : { 120, 214, 339, kMaxTipW })
        for (const int tipH : { 26, 44, 60 })
            for (int y = 0; y < kEditorH; ++y)
                for (int x = 0; x < kEditorW; ++x)
                {
                    const auto r = tooltipBounds (tipW, tipH, x, y, kEditor);

                    INFO ("tip " << tipW << "x" << tipH << " at cursor (" << x << ", " << y
                          << ") -> box (" << r.x << ", " << r.y << ", " << r.w << ", " << r.h << ")");
                    REQUIRE_FALSE (r.contains (x, y));
                }
}

TEST_CASE ("tooltip bounds: the box stays inside the parent area", "[tooltip]")
{
    for (const int tipW : { 120, kMaxTipW })
        for (int y = 0; y < kEditorH; ++y)
            for (int x = 0; x < kEditorW; ++x)
            {
                const auto r = tooltipBounds (tipW, 60, x, y, kEditor);

                INFO ("tip " << tipW << " at cursor (" << x << ", " << y << ")");
                REQUIRE (r.x >= kEditor.x);
                REQUIRE (r.y >= kEditor.y);
                REQUIRE (r.right()  <= kEditor.right());
                REQUIRE (r.bottom() <= kEditor.bottom());
            }
}

TEST_CASE ("tooltip bounds: the exact case that used to flicker", "[tooltip]")
{
    // A widest-possible tooltip, cursor mid-window at the minimum editor width. The old
    // rule put the box at x = 600 - 414 = 186 spanning 186..600 with its top 6 px above
    // the cursor — containing the cursor on both axes.
    const auto r = tooltipBounds (kMaxTipW, 44, 300, 200, kEditor);
    REQUIRE_FALSE (r.contains (300, 200));
}

TEST_CASE ("tooltip bounds: flips to the far side past each midpoint", "[tooltip]")
{
    // Left of centre and above it: the box hangs down and to the right, untouched by the
    // clamp because a small tip near the top-left corner fits.
    const auto tl = tooltipBounds (120, 30, 100, 100, kEditor);
    REQUIRE (tl.x == 100 + rbtip::kGapRight);
    REQUIRE (tl.y == 100 + rbtip::kGapBelow);

    // Right of centre and below it: the box flips to hang up and to the left.
    const auto br = tooltipBounds (120, 30, 500, 300, kEditor);
    REQUIRE (br.x == 500 - (120 + rbtip::kGapLeft));
    REQUIRE (br.y == 300 - (30 + rbtip::kGapAbove));
}

TEST_CASE ("tooltip bounds: a control on the bottom row gets its tip above", "[tooltip]")
{
    // Controls row 2 sits at the very bottom of the editor, so this is the placement the
    // Actions / Advanced buttons actually get. It must clear the cursor upward and still
    // fit, rather than being clamped back down over it.
    const int cursorY = kEditorH - 20;
    const auto r = tooltipBounds (kMaxTipW, 60, 300, cursorY, kEditor);

    REQUIRE (r.bottom() <= cursorY);
    REQUIRE (r.y >= kEditor.y);
    REQUIRE_FALSE (r.contains (300, cursorY));
}
