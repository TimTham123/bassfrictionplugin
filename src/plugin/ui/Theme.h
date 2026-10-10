// Friction - visual theme
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace friction::theme
{

inline const juce::Colour background { 0xff111317 };
inline const juce::Colour panel { 0xff181b21 };
inline const juce::Colour panelRaised { 0xff1f232b };
inline const juce::Colour outline { 0xff2b3039 };
inline const juce::Colour grid { 0xff252a32 };
inline const juce::Colour text { 0xffe4e7ec };
inline const juce::Colour textDim { 0xff8b93a1 };
inline const juce::Colour target { 0xffd9dee7 };
inline const juce::Colour reference { 0xff3cc9c4 };
inline const juce::Colour heatLow { 0xffffc163 };
inline const juce::Colour heatHigh { 0xffff4f5e };
inline const juce::Colour gain { 0xffa48bff };
inline const juce::Colour shift { 0xff74e3a6 };
inline const juce::Colour tension { 0xffff8a5c };
inline const juce::Colour accent { 0xffffa94d };

/** Friction value (0 .. ~1.5) to heat colour. */
inline juce::Colour heat (float f, float alpha = 1.0f)
{
    const float t = juce::jlimit (0.0f, 1.0f, f);
    juce::Colour c = heatLow.interpolatedWith (heatHigh, t);
    if (f > 1.0f)
        c = c.interpolatedWith (juce::Colour (0xffff3df2), juce::jlimit (0.0f, 1.0f, (f - 1.0f) * 2.0f));
    return c.withAlpha (alpha);
}

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::String hzString (float hz)
{
    if (hz >= 1000.0f)
        return juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";
    return juce::String (hz, hz < 100.0f ? 2 : 1) + " Hz";
}

} // namespace friction::theme
