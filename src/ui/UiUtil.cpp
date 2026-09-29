// =============================================================================
// ui/UiUtil.cpp — see UiUtil.h.
// =============================================================================
#include "ui/UiUtil.h"

#include <cstdio>

namespace starmap::ui {

ImU32 to_u32(const glm::vec3& c, float alpha) {
    // ColorConvertFloat4ToU32 packs and clamps; ImGui colours are sRGB-ish
    // "what you see" values, so no gamma conversion is needed for UI use.
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, alpha));
}

std::string with_commas(std::size_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}

namespace {
void replace_all(std::string& text, const std::string& from, const char* to) {
    for (std::size_t p = text.find(from); p != std::string::npos; p = text.find(from, p)) text.replace(p, from.size(), to);
}
}  // namespace

std::string fix_units(std::string text) {
    // IsGlyphInFont asks the font's sources without rasterising anything. The
    // answers cannot change while the app runs (one font), so cache them in
    // function-local statics (initialised once, thread-safe since C++11).
    static const bool has_sun = ImGui::GetFont()->IsGlyphInFont(0x2609);
    static const bool has_earth = ImGui::GetFont()->IsGlyphInFont(0x2295);
    if (!has_sun) {
        replace_all(text, "R\u2609", "Rsun");
        replace_all(text, "M\u2609", "Msun");
        replace_all(text, "L\u2609", "Lsun");
    }
    if (!has_earth) {
        replace_all(text, "R\u2295", "Rearth");
        replace_all(text, "M\u2295", "Mearth");
    }
    return text;
}

// SameLine() puts the marker after the previous item; if that would run past
// the panel's right edge (long label, narrow panel) it wraps to the next line
// instead of being clipped: GetContentRegionAvail() after SameLine() is the
// room left on the current line.
void help_marker(const char* text) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize("(?)").x) ImGui::NewLine();
    ImGui::TextDisabled("(?)");
    // BeginItemTooltip = "if the last item is hovered, open a tooltip".
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void text_na() { ImGui::TextDisabled("n/a"); }

void text_opt(const std::optional<double>& v, int decimals, const char* suffix) {
    if (!v) return text_na();
    // "%.*f": the precision comes from an int argument, so one literal format
    // string serves every field (a runtime-built format string would defeat
    // the compiler's printf argument checking).
    ImGui::Text("%.*f%s", decimals, *v, suffix);
}

void row_label(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

}  // namespace starmap::ui
