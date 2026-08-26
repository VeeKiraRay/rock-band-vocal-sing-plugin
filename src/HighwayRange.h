#pragma once
#include <algorithm>

// Highway pitch axis — turn a chart's raw note min/max into the range actually drawn.
//
// Pure and JUCE-free (header-only, no .cpp) so the unit tests can exercise the axis
// rules without constructing an editor or a Graphics context. drawHighway calls this
// once per section per frame; the rules live here rather than inline because the
// section-transition branch needs the same computation for two sections at once.
//
// Rationale for each rule: _upcoming/CHART_REVIEW_FINDINGS.md §2 (b).

namespace rbhighway
{

// The Rock Band vocal range. Notes outside it are not singable in-game, so a chart that
// contains one is malformed rather than unusual — clamping to this band deliberately
// draws such a note off-screen instead of letting it stretch the axis for the whole
// song. No warning is surfaced: this plugin reviews charts, it does not validate them,
// and dedicated RB authoring tools already check game conventions.
constexpr int kVocalLow  = 36;
constexpr int kVocalHigh = 84;

// Breathing room above and below the content.
constexpr int kPitchPad = 5;

// Smallest axis the highway will draw. A flat placeholder chart (every note the same
// pitch while charting from scratch against a stem) would otherwise collapse to
// 2*kPitchPad + 1 semitones, where the pitch trail folds uselessly. One octave keeps a
// sung contour legible.
constexpr int kMinSpan = 12;

struct Range { float lo, hi; };

// rawLo/rawHi are a section's unpadded note min/max. Order: pad, clamp into the vocal
// band, then widen to kMinSpan — widening last so it can never be undone by the clamp,
// and always shifted back inside the band rather than allowed to escape it.
inline Range visibleRange (int rawLo, int rawHi)
{
    int lo = rawLo - kPitchPad;
    int hi = rawHi + kPitchPad;

    lo = std::max (lo, kVocalLow);
    hi = std::min (hi, kVocalHigh);

    // Content entirely outside the band leaves lo > hi; fall back to the whole band
    // rather than an inverted axis (noteH would go negative and every note would draw
    // mirrored off-screen).
    if (lo > hi)
        return { (float) kVocalLow, (float) kVocalHigh };

    if (const int span = hi - lo + 1; span < kMinSpan)
    {
        const int grow = kMinSpan - span;
        lo -= grow / 2;
        hi += grow - grow / 2;

        // Re-seat inside the band, preserving the span. The band is wider than
        // kMinSpan, so exactly one of these can fire and it always succeeds.
        if (lo < kVocalLow)  { hi += kVocalLow  - lo; lo = kVocalLow; }
        if (hi > kVocalHigh) { lo -= hi - kVocalHigh; hi = kVocalHigh; }
    }

    return { (float) lo, (float) hi };
}

} // namespace rbhighway
