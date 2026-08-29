#include <JuceHeader.h>
#include <catch2/catch_test_macros.hpp>

// A juce::Slider is not one component. setSliderStyle and setTextBoxStyle each recreate
// the value box — and, for IncDecButtons, the +/- buttons — as child components, copying
// the slider's tooltip *as it is at that moment*
// (juce::Slider::Pimpl::lookAndFeelChanged). On a stepper those children cover the whole
// control, so a tooltip set afterwards is one the mouse can never reach: it showed only on
// the row's Label. Every stepper in Settings and Advanced shipped that way until
// 2026-08-29.
//
// PanelBase::setupStepper fixes the ordering in one place. The panels are file-local to
// PluginEditor.cpp so they cannot be constructed here; what these tests pin is the JUCE
// behaviour that ordering exists for, so a JUCE upgrade that changed it would fail here
// instead of silently costing the tooltips again.

namespace
{
    struct TipCount { int clients = 0, withTip = 0; };

    // Counts the slider's tooltip-capable children — the parts the mouse actually lands on.
    TipCount countChildTooltips (const juce::Slider& s, const juce::String& tip)
    {
        TipCount c;

        for (auto* child : s.getChildren())
        {
            if (auto* ttc = dynamic_cast<juce::SettableTooltipClient*> (child))
            {
                ++c.clients;
                if (ttc->getTooltip() == tip)
                    ++c.withTip;
            }
        }

        return c;
    }

    const juce::String kTip = "Explains what this control does.";
}

TEST_CASE ("slider tooltip: set before the style calls, every child inherits it",
           "[tooltip]")
{
    // The order PanelBase::setupStepper uses, and the shape of every stepper row in
    // Settings and Advanced.
    juce::Slider s;
    s.setTooltip (kTip);
    s.setSliderStyle (juce::Slider::IncDecButtons);
    s.setRange (0, 100, 5);
    s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 65, 28);

    const auto c = countChildTooltips (s, kTip);

    REQUIRE (c.clients >= 3);            // value box + increment + decrement
    REQUIRE (c.withTip == c.clients);    // ...and the mouse finds the tip on all of them
}

TEST_CASE ("slider tooltip: set after the style calls, no child inherits it", "[tooltip]")
{
    // The bug, pinned so the ordering above reads as load-bearing rather than incidental.
    juce::Slider s;
    s.setSliderStyle (juce::Slider::IncDecButtons);
    s.setRange (0, 100, 5);
    s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 65, 28);
    s.setTooltip (kTip);

    const auto c = countChildTooltips (s, kTip);

    REQUIRE (s.getTooltip() == kTip);    // the slider body knows the tooltip...
    REQUIRE (c.clients >= 3);
    REQUIRE (c.withTip == 0);            // ...and none of the children the mouse hits do
}

TEST_CASE ("slider tooltip: a plain slider's value box needs the same ordering",
           "[tooltip]")
{
    // The Mic Gate row: LinearHorizontal, so no +/- buttons, but still a value box child.
    // It cannot use setupStepper, which is why its ordering is fixed at the call site.
    juce::Slider s;
    s.setTooltip (kTip);
    s.setRange (0.001, 0.05, 0.0);
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 55, 28);

    const auto c = countChildTooltips (s, kTip);

    REQUIRE (c.clients >= 1);
    REQUIRE (c.withTip == c.clients);
}
