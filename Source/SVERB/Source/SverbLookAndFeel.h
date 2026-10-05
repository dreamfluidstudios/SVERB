#pragma once
#include <JuceHeader.h>
#include <array>

class SverbLookAndFeel : public juce::LookAndFeel_V4
{
  public:
    SverbLookAndFeel ()
    {
        setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
        setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4a4a4a));
        setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    }
    void drawRotarySlider (juce::Graphics& g,
                           int x,
                           int y,
                           int w,
                           int h,
                           float pos,
                           float start,
                           float end,
                           juce::Slider& slider) override
    {
        const bool speed = slider.getComponentID () == "speed",
                   wet = slider.getComponentID () == "wet";
        const float diameter = static_cast<float> (juce::jmin (w, h));
        const float cx = x + w * 0.5f, cy = y + h * 0.5f, radius = diameter * 0.375f;
        struct Stop
        {
            float at;
            juce::uint32 colour;
        };
        const std::array<Stop, 5> speedStops{{{0, 0xffff4500},
                                              {0.196f, 0xffff9900},
                                              {0.392f, 0xffffcc00},
                                              {0.784f, 0xff00ff00},
                                              {1, 0xff00ff00}}};
        const std::array<Stop, 5> wetStops{{{0, 0xff7d26cd},
                                            {0.327f, 0xff5d80e5},
                                            {0.654f, 0xff00ccff},
                                            {1, 0xff55c48a},
                                            {1, 0xff55c48a}}};
        const std::array<Stop, 5> gainStops{{{0, 0xff00cc99},
                                             {0.5f, 0xff10b981},
                                             {1, 0xff34d399},
                                             {1, 0xff34d399},
                                             {1, 0xff34d399}}};
        const auto& stops = speed ? speedStops : wet ? wetStops : gainStops;
        auto colourAt = [&stops] (float p)
        {
            for (size_t i = 1; i < stops.size (); ++i)
                if (p <= stops[i].at)
                    return juce::Colour (stops[i - 1].colour)
                        .interpolatedWith (juce::Colour (stops[i].colour),
                                           (p - stops[i - 1].at) / (stops[i].at - stops[i - 1].at));
            return juce::Colour (stops.back ().colour);
        };
        for (int i = 0; i < 64; ++i)
        {
            const float a = i / 64.0f, b = juce::jmin ((i + 1) / 64.0f + 0.002f, pos);
            if (a >= b)
                break;
            juce::Path arc;
            arc.addCentredArc (cx,
                               cy,
                               radius,
                               radius,
                               0,
                               start + a * (end - start),
                               start + b * (end - start),
                               true);
            g.setColour (colourAt (a).withAlpha (0.15f));
            g.strokePath (arc, juce::PathStrokeType (diameter * 0.27f));
            g.setColour (colourAt (a));
            g.strokePath (arc, juce::PathStrokeType (diameter * 0.25f));
        }
        const float body = diameter * (speed ? 0.75f : 0.667f);
        const auto circle = juce::Rectangle<float> (body, body).withCentre ({cx, cy});
        juce::Path shadow;
        shadow.addEllipse (circle);
        juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 5, {0, 3}).drawForPath (g, shadow);
        juce::ColourGradient gradient (juce::Colour (0xff444444),
                                       circle.getX (),
                                       circle.getY (),
                                       juce::Colour (0xff555555),
                                       circle.getRight (),
                                       circle.getBottom (),
                                       false);
        gradient.addColour (0.4, juce::Colour (0xff777777));
        g.setGradientFill (gradient);
        g.fillEllipse (circle);
        juce::Path marker;
        marker.addRoundedRectangle (cx - 2, cy - body * 0.5f + 5, 4, speed ? 20.0f : 10.0f, 2);
        marker.applyTransform (
            juce::AffineTransform::rotation (start + pos * (end - start), cx, cy));
        g.setColour (juce::Colours::black);
        g.fillPath (marker);
    }
    void drawLed (juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour colour, bool on)
    {
        const auto outer = bounds.withSizeKeepingCentre (60, 60).reduced (1);
        g.setColour (juce::Colour (0xff333333));
        g.fillEllipse (outer);
        g.setColour (juce::Colour (0xff555555));
        g.drawEllipse (outer, 2);
        const auto inner = outer.withSizeKeepingCentre (40, 40);
        if (on)
        {
            juce::Path p;
            p.addEllipse (inner);
            juce::DropShadow (colour.withAlpha (0.65f), 15, {}).drawForPath (g, p);
        }
        g.setColour (colour);
        g.fillEllipse (inner);
    }
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool, bool) override
    {
        const bool on = button.getToggleState ();
        if (button.getComponentID () == "hz432")
        {
            drawLed (g,
                     button.getLocalBounds ().toFloat (),
                     juce::Colour (on ? 0xffff0000 : 0xff550000),
                     on);
            return;
        }
        const auto pill = button.getLocalBounds ().toFloat ().withSizeKeepingCentre (58, 30);
        const juce::Colour colour (on ? 0xff5d80e5 : 0xff444444);
        if (on)
        {
            juce::Path p;
            p.addRoundedRectangle (pill, 15.0f);
            juce::DropShadow (colour.withAlpha (0.65f), 11, {}).drawForPath (g, p);
        }
        g.setColour (colour);
        g.fillRoundedRectangle (pill, 15.0f);
    }
    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& base,
                               bool over,
                               bool down) override
    {
        if (button.getComponentID () == "perfection")
        {
            const auto colour = juce::Colour (static_cast<juce::uint32> (
                static_cast<juce::int64> (button.getProperties ()["ledColour"])));
            drawLed (g,
                     button.getLocalBounds ().toFloat (),
                     colour,
                     colour != juce::Colour (0xff550000));
            return;
        }
        auto bounds = button.getLocalBounds ().toFloat ().reduced (1);
        if (button.getComponentID () == "load")
        {
            g.setColour (juce::Colour (0xffffcc00).withAlpha (over ? 0.1f : 0.0f));
            g.fillRoundedRectangle (bounds, 8);
            g.setColour (juce::Colour (0xffffcc00));
            g.drawRoundedRectangle (bounds, 8, 1);
            return;
        }
        auto colour = base.withMultipliedAlpha (button.isEnabled () ? 1.0f : 0.5f);
        if (over)
            colour = colour.brighter (0.12f);
        g.setColour (colour.darker (0.5f));
        g.fillRoundedRectangle (bounds, 8);
        if (down)
            bounds.removeFromTop (3);
        else
            bounds.removeFromBottom (4);
        g.setColour (colour);
        g.fillRoundedRectangle (bounds, 8);
    }
    juce::Font getTextButtonFont (juce::TextButton&, int) override
    {
        return juce::Font (juce::FontOptions (12.0f, juce::Font::bold));
    }
};
