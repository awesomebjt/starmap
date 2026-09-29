// =============================================================================
// ui/FilterPanel.cpp — see FilterPanel.h for the role.
// =============================================================================
//
// IMMEDIATE-MODE PATTERN, as used throughout this file:
//     if (ImGui::Checkbox("Hide ...", &filter.hide_missing)) settings.dirty = true;
//   The widget reads the bool, draws itself, and — if the user clicked it —
//   writes the new value back and returns true. "Returns true when changed"
//   is our cue to mark the derived data (the FilteredOut tags) stale. There
//   are no callbacks, listeners or widget objects to keep in sync with the
//   data: the data IS the UI state.
//
// IDs: ImGui identifies widgets by a hash of their label (and the ID stack).
//   "##" hides the rest of a label from display but keeps it in the ID:
//   Checkbox("##G") draws no text but has a unique ID. PushID/PopID scope
//   IDs inside loops so 14 identical-looking checkboxes stay distinct.
// =============================================================================
#include "ui/FilterPanel.h"

#include "render/Colormaps.h"
#include "scene/ColorScheme.h"
#include "scene/Filters.h"
#include "scene/Tags.h"
#include "systems/ColorSystem.h"
#include "ui/RangeSlider.h"
#include "ui/UiUtil.h"

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace starmap::ui {

using scene::ColorScheme;

namespace {

const char* class_description(char c) {
    switch (c) {
        case 'O': return "blue-violet, hottest";
        case 'B': return "blue";
        case 'A': return "white-blue (Sirius, Vega)";
        case 'F': return "yellow-white";
        case 'G': return "yellow (the Sun)";
        case 'K': return "orange";
        case 'M': return "red dwarfs";
        case 'L': return "brown dwarf";
        case 'T': return "brown dwarf (methane)";
        case 'Y': return "brown dwarf (coldest)";
        case 'D': return "white dwarfs";
        case 'C': return "carbon star";
        case 'S': return "S-type giant";
        case 'W': return "Wolf-Rayet";
        default: return "";
    }
}

// ---- legend ---------------------------------------------------------------------------------
// The colormap's own axis (ColorRanges: where the colours actually change),
// which is narrower than the slider's full data domain.
struct LegendSpec {
    SliderScale axis;
    std::vector<float> ticks;
    const char* format;
};

LegendSpec legend_spec(ColorScheme s, const scene::ColorRanges& r) {
    switch (s) {
        case ColorScheme::Temperature: return {{2000.0f, 12000.0f, false, false}, {3000.0f, 5800.0f, 9000.0f, 12000.0f}, "%.0f K"};
        case ColorScheme::Diameter:
            return {{std::pow(10.0f, r.log_radius_lo), std::pow(10.0f, r.log_radius_hi), true, false}, {0.2f, 0.5f, 1.0f, 2.0f}, "%.3g"};
        case ColorScheme::Age: return {{r.age_lo, r.age_hi, false, false}, {1.0f, 5.0f, 10.0f}, "%.0f Gyr"};
        case ColorScheme::Metallicity: return {{r.feh_lo, r.feh_hi, false, false}, {-0.6f, -0.3f, 0.0f, 0.3f}, "%+.1f"};
        case ColorScheme::Planets:
            return {{1.0f, static_cast<float>(std::max(r.planets_max, 2)), true, true}, {1.0f, 2.0f, 4.0f, 8.0f}, "%.0f"};
        default: return {{0.0f, 1.0f, false, false}, {}, "%.2f"};
    }
}

// A gradient bar with tick labels, drawn with the window's ImDrawList.
// ImGui::Dummy reserves the space in the layout (draw lists don't advance
// the cursor by themselves).
void draw_gradient_legend(ColorScheme s, const scene::ColorRanges& ranges) {
    const LegendSpec spec = legend_spec(s, ranges);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float bar_h = 10.0f;
    constexpr int kSegments = 64;
    for (int i = 0; i < kSegments; ++i) {
        const float ta = static_cast<float>(i) / kSegments, tb = static_cast<float>(i + 1) / kSegments;
        const ImU32 ca = to_u32(systems::color_for_value(s, spec.axis.from_t(ta), ranges));
        const ImU32 cb = to_u32(systems::color_for_value(s, spec.axis.from_t(tb), ranges));
        dl->AddRectFilledMultiColor(ImVec2(p.x + ta * w, p.y), ImVec2(p.x + tb * w, p.y + bar_h), ca, cb, cb, ca);
    }
    dl->AddRect(p, ImVec2(p.x + w, p.y + bar_h), ImGui::GetColorU32(ImGuiCol_Border));
    for (float v : spec.ticks) {
        const float x = p.x + spec.axis.to_t(v) * w;
        char buf[32];
        std::snprintf(buf, sizeof buf, spec.format, static_cast<double>(v));
        const std::string text = fix_units(buf);
        const float tw = ImGui::CalcTextSize(text.c_str()).x;
        dl->AddLine(ImVec2(x, p.y + bar_h), ImVec2(x, p.y + bar_h + 3.0f), ImGui::GetColorU32(ImGuiCol_Text));
        dl->AddText(ImVec2(std::clamp(x - tw * 0.5f, p.x, p.x + w - tw), p.y + bar_h + 3.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    }
    ImGui::Dummy(ImVec2(w, bar_h + ImGui::GetTextLineHeight() + 4.0f));
}

void draw_class_legend(const scene::FilterSettings& filters) {
    // One coloured letter per class present: the categorical "legend".
    for (const auto& c : filters.classes) {
        const glm::vec3 rgb = render::spectral_class_rgb(c.letter);
        ImGui::TextColored(ImVec4(rgb.r, rgb.g, rgb.b, 1.0f), "%c", c.letter);
        ImGui::SameLine(0.0f, 7.0f);
    }
    ImGui::NewLine();
}

// ---- filter sections ----------------------------------------------------------------------------
void draw_range_filter(ColorScheme s, scene::FilterSettings& filters, const scene::ColorRanges& ranges) {
    scene::SchemeFilter& f = filters.filter(s);
    const scene::FilterDomain& d = filters.domain(s);
    const scene::FilterFieldInfo& info = scene::filter_field_info(s);
    const std::string unit = fix_units(info.unit);

    char lo_txt[32], hi_txt[32];
    std::snprintf(lo_txt, sizeof lo_txt, info.format, static_cast<double>(f.lo));
    std::snprintf(hi_txt, sizeof hi_txt, info.format, static_cast<double>(f.hi));
    // TextWrapped: long nouns/units ("planet count ... planets (log scale)")
    // wrap at the panel edge instead of being clipped.
    ImGui::TextWrapped("Keep %s %s .. %s %s%s", info.noun, lo_txt, hi_txt, unit.c_str(),
                       d.log_scale ? "  (log scale)" : "");

    RangeSliderConfig cfg;
    cfg.scale = {d.min, d.max, d.log_scale, d.integer};
    cfg.format = info.format;
    cfg.unit = unit.c_str();
    if (!d.integer) cfg.markers = {d.p01, d.p99};
    // Tick labels: both ends, plus the powers of ten in between on a log axis
    // (0.1, 1, 10 R_sun) or 2 and 4 for planet counts.
    cfg.tick_values = {d.min, d.max};
    if (d.integer) {
        for (float v = 2.0f; v < d.max; v *= 2.0f) cfg.tick_values.push_back(v);
    } else if (d.log_scale) {
        for (float v = std::pow(10.0f, std::ceil(std::log10(d.min * 1.5f))); v < d.max / 1.5f; v *= 10.0f)
            cfg.tick_values.push_back(v);
    }
    // The track shows the scheme's colormap across the slider's domain.
    // Captures by reference are fine: the function is only called inside
    // RangeSlider() below, while these locals are alive.
    cfg.track_color = [&](float t) { return to_u32(systems::color_for_value(s, cfg.scale.from_t(t), ranges)); };

    if (RangeSlider("range", f.lo, f.hi, cfg)) filters.dirty = true;

    // Numeric entry for precise values. The filter follows every keystroke
    // (live preview); clamping/ordering happens when the field loses focus
    // (IsItemDeactivatedAfterEdit), so a half-typed "5" on the way to "5000"
    // isn't immediately "corrected" under the user's fingers.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float label_w = ImGui::CalcTextSize("max").x + ImGui::GetStyle().ItemInnerSpacing.x;
    const float field_w = (avail - 2.0f * label_w - ImGui::GetStyle().ItemSpacing.x * 3.0f) * 0.5f;
    const char* input_fmt = d.log_scale && !d.integer ? "%.4g" : info.format;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("min");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::InputFloat("##lo", &f.lo, 0.0f, 0.0f, input_fmt)) filters.dirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        sanitize_range(cfg.scale, true, f.lo, f.hi);
        filters.dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("max");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::InputFloat("##hi", &f.hi, 0.0f, 0.0f, input_fmt)) filters.dirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        sanitize_range(cfg.scale, false, f.lo, f.hi);
        filters.dirty = true;
    }

    if (ImGui::Button("Reset")) {
        f.lo = d.min;
        f.hi = d.max;
        filters.dirty = true;
    }
    // Percentile clamping makes no sense for a handful of integer values
    // (planet counts 1..8), so that button only exists for continuous fields.
    if (!d.integer) {
        ImGui::SameLine();
        if (ImGui::Button("Clamp to 1-99 %")) {
            f.lo = std::max(d.p01, d.min);
            f.hi = std::min(d.p99, d.max);
            filters.dirty = true;
        }
        help_marker("Sets the range to the 1st..99th percentile of the stars that have data "
                    "(the orange marks on the track), trimming the extreme 2 %.");
    }
}

void draw_class_filter(scene::FilterSettings& filters) {
    scene::SchemeFilter& f = filters.filter(ColorScheme::SpectralClass);
    if (ImGui::Button("All")) scene::set_all_classes(filters, true);
    ImGui::SameLine();
    if (ImGui::Button("None")) scene::set_all_classes(filters, false);
    help_marker("Stars without a known spectral class ('?') are controlled by the "
                "'Hide stars of unknown class' checkbox above, not by a checkbox here.");

    // Up to 14 rows: tighten the vertical frame padding and item spacing for
    // this list only, so the whole panel fits a 720-pixel-high window without
    // scrolling. PushStyleVar/PopStyleVar is ImGui's scoped style override;
    // every Push must be matched by a Pop (ImGui asserts otherwise).
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 3.0f));
    for (const auto& c : filters.classes) {
        ImGui::PushID(c.letter);  // unique ID scope per row
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const auto bit = static_cast<unsigned char>(c.letter);
        bool on = f.classes.test(bit);
        char label[64];
        std::snprintf(label, sizeof label, "%c   %s", c.letter, class_description(c.letter));
        if (ImGui::Checkbox(label, &on)) {
            f.classes.set(bit, on);
            filters.dirty = true;
        }
        // Right-aligned: colour swatch + star count.
        const std::string count = with_commas(c.count);
        const float count_w = ImGui::CalcTextSize(count.c_str()).x;
        const float sw = ImGui::GetTextLineHeight();
        ImGui::SameLine(right - count_w - sw - ImGui::GetStyle().ItemSpacing.x);
        const glm::vec3 rgb = render::spectral_class_rgb(c.letter);
        ImGui::ColorButton("##swatch", ImVec4(rgb.r, rgb.g, rgb.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(sw, sw));
        ImGui::SameLine(right - count_w);
        ImGui::TextDisabled("%s", count.c_str());
        ImGui::PopID();
    }
    ImGui::PopStyleVar(2);
}

void draw_combine_section(ColorScheme active, scene::FilterSettings& filters) {
    if (ImGui::Checkbox("Combine filters from all schemes", &filters.combine)) filters.dirty = true;
    help_marker("Off: only the filter of the colour scheme shown applies; the others are remembered. "
                "On: every scheme's filter that is not at its default applies at the same time (AND).");

    int others = 0;
    for (std::size_t i = 0; i < scene::FilterSettings::kSchemes; ++i) {
        const auto s = static_cast<ColorScheme>(i);
        if (s == active || scene::is_default(s, filters)) continue;
        ++others;
        if (!filters.combine) continue;
        ImGui::PushID(static_cast<int>(i));
        ImGui::Bullet();
        ImGui::TextWrapped("%s: %s", scene::scheme_info(s).title, fix_units(scene::describe_filter(s, filters)).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) scene::reset_filter(s, filters);
        ImGui::PopID();
    }
    if (filters.combine && others == 0) ImGui::TextDisabled("No other scheme has a filter set.");
    if (!filters.combine && others > 0)
        ImGui::TextDisabled("%d other scheme%s saved filters (not applied).", others, others == 1 ? " has" : "s have");
}

// Distance is not a colour scheme, so this section is always on screen and
// always ANDed with the scheme filter. The track is a plain blue ramp: there
// is no colormap to preview, and reusing one would imply the stars change
// colour with the handles, which they do not.
void draw_distance_filter(scene::FilterSettings& filters) {
    ImGui::SeparatorText("Distance from Sol");
    ImGui::PushID("distance");
    const scene::FilterDomain& d = filters.distance_domain;
    char lo_txt[32], hi_txt[32];
    std::snprintf(lo_txt, sizeof lo_txt, "%.1f", static_cast<double>(filters.distance_lo));
    std::snprintf(hi_txt, sizeof hi_txt, "%.1f", static_cast<double>(filters.distance_hi));
    ImGui::TextWrapped("Keep distance %s .. %s ly", lo_txt, hi_txt);
    help_marker("Always applied, together with the colour-scheme filter. Both handles are inclusive. "
                "Sol stays visible: it is the origin, at 0 ly.");

    // Whole light years on a 0..200 track: one pixel is already coarser than
    // 0.1 ly, so the drag tooltip shows integers. The min/max fields below
    // keep a decimal for a typed value such as 12.5.
    const float span = d.max - d.min;
    const char* slider_fmt = span > 50.0f ? "%.0f" : (span > 5.0f ? "%.1f" : "%.2f");
    RangeSliderConfig cfg;
    cfg.scale = {d.min, d.max, false, false};
    cfg.format = slider_fmt;
    cfg.unit = "ly";
    if (d.p01 > d.min && d.p99 < d.max && d.p99 > d.p01) cfg.markers = {d.p01, d.p99};
    cfg.tick_values = {d.min};
    for (float v : {25.0f, 50.0f, 100.0f, 150.0f}) {
        if (v > d.min + 1.0f && v < d.max - 1.0f) cfg.tick_values.push_back(v);
    }
    cfg.tick_values.push_back(d.max);
    cfg.track_color = [](float t) {
        const glm::vec3 c(0.20f + 0.40f * t, 0.38f + 0.32f * t, 0.72f + 0.18f * t);
        return to_u32(c);
    };
    if (RangeSlider("range", filters.distance_lo, filters.distance_hi, cfg)) filters.dirty = true;

    const float avail = ImGui::GetContentRegionAvail().x;
    const float label_w = ImGui::CalcTextSize("max").x + ImGui::GetStyle().ItemInnerSpacing.x;
    const float field_w = (avail - 2.0f * label_w - ImGui::GetStyle().ItemSpacing.x * 3.0f) * 0.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("min");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::InputFloat("##lo", &filters.distance_lo, 0.0f, 0.0f, "%.2f")) filters.dirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        sanitize_range(cfg.scale, true, filters.distance_lo, filters.distance_hi);
        filters.dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("max");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::InputFloat("##hi", &filters.distance_hi, 0.0f, 0.0f, "%.2f")) filters.dirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        sanitize_range(cfg.scale, false, filters.distance_lo, filters.distance_hi);
        filters.dirty = true;
    }
    if (ImGui::Button("Reset")) scene::reset_distance(filters);
    ImGui::SameLine();
    if (ImGui::Button("Clamp to 1-99 %")) {
        filters.distance_lo = std::max(d.p01, d.min);
        filters.distance_hi = std::min(d.p99, d.max);
        if (filters.distance_lo > filters.distance_hi) std::swap(filters.distance_lo, filters.distance_hi);
        filters.dirty = true;
    }
    help_marker("Sets the range to the 1st..99th percentile of distances (the orange marks on the track).");
    ImGui::PopID();
}

// The tagged-only view is a visibility switch AND a colour switch. The colour
// switch is ColorSettings::dirty, because ColorSystem does not watch this
// flag by itself (see ColorSystem.h). Both flags are set on the toggle so
// the same frame hides the untagged stars and paints the rest from the palette.
void draw_tag_filter(entt::registry& registry, scene::ColorSettings& colors, scene::FilterSettings& filters) {
    ImGui::SeparatorText("Tags");
    ImGui::PushID("tags");
    if (ImGui::Checkbox("Show only tagged stars", &filters.only_tagged)) {
        filters.dirty = true;
        colors.dirty = true;
    }
    help_marker("Right-click a star and pick one of 16 colours. With this on, untagged stars are hidden "
                "and the tagged ones are drawn in the colour you picked, instead of the colour scheme. "
                "Turn it off to restore the scheme colours; the tags stay. Sol stays visible.");
    // storage() creates the pool the first time, which is free when empty.
    // The count is membership, so it stays correct while the view is off.
    const std::size_t tagged = registry.storage<scene::StarTag>().size();
    ImGui::TextDisabled("%s tagged    right-click a star to tag it", with_commas(tagged).c_str());
    if (filters.only_tagged) {
        ImGui::TextWrapped("Tagged view: stars on screen use their tag colour, not the scheme legend.");
    }
    ImGui::PopID();
}

}  // namespace

void FilterPanel::draw_contents(entt::registry& registry) {
    auto& colors = registry.ctx().get<scene::ColorSettings>();
    auto& filters = registry.ctx().get<scene::FilterSettings>();
    const ColorScheme active = colors.scheme;
    {  // (block kept from M2's `if (ImGui::Begin(...))` so the diff stays readable)
        // ---- scheme selector (same as keys 1-6) ------------------------------------------------
        ImGui::SeparatorText("Colour scheme");
        char preview[48];
        std::snprintf(preview, sizeof preview, "%d  %s", static_cast<int>(active) + 1, scene::scheme_info(active).title);
        ImGui::SetNextItemWidth(-FLT_MIN);  // -FLT_MIN = "stretch to the right edge"
        if (ImGui::BeginCombo("##scheme", preview)) {
            for (int i = 0; i < static_cast<int>(ColorScheme::Count); ++i) {
                const auto s = static_cast<ColorScheme>(i);
                char item[48];
                std::snprintf(item, sizeof item, "%d  %s", i + 1, scene::scheme_info(s).title);
                if (ImGui::Selectable(item, s == active)) colors.set(s);  // marks colours dirty
                if (s == active) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // ---- star count ------------------------------------------------------------------------
        const double frac = filters.total_stars ? static_cast<double>(filters.visible) / static_cast<double>(filters.total_stars) : 0.0;
        ImGui::Text("Showing %s of %s stars", with_commas(filters.visible).c_str(),
                    with_commas(filters.total_stars).c_str());
        help_marker("Sol is always shown, whatever the filters: it is the reference point of the map. "
                    "That includes the distance range and the tagged-only view.");
        // A progress bar makes a nice "fraction shown" meter; the overlay
        // string is drawn centred on it.
        char pct[32];
        std::snprintf(pct, sizeof pct, "%.1f %%", 100.0 * frac);
        ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), pct);

        // ---- legend ----------------------------------------------------------------------------
        ImGui::Spacing();
        if (active == ColorScheme::SpectralClass) draw_class_legend(filters);
        else draw_gradient_legend(active, colors.ranges);
        ImGui::TextDisabled("grey = no data%s", active == ColorScheme::Planets ? " (no known planets)" : "");

        // ---- filter for the active scheme --------------------------------------------------------
        const std::string title = std::string("Filter: ") + scene::scheme_info(active).title;
        ImGui::SeparatorText(title.c_str());
        scene::SchemeFilter& f = filters.filter(active);
        const scene::FilterFieldInfo& info = scene::filter_field_info(active);
        // The generic wording is "Hide stars with no <field> data"; two
        // schemes get a phrasing that says what "no data" means for them.
        std::string toggle = std::string("Hide stars with no ") + info.noun + " data";
        if (active == ColorScheme::Planets) toggle = "Hide stars with no known planets";
        if (active == ColorScheme::SpectralClass) toggle = "Hide stars of unknown class";
        if (ImGui::Checkbox(toggle.c_str(), &f.hide_missing)) filters.dirty = true;
        if (active == ColorScheme::Planets) {
            help_marker("A planet count of 0 means 'no planet detected so far', which is treated as no data: "
                        "the absence of a detection is not a measurement. With this on, only planet hosts remain. "
                        "The range below applies to hosts only.");
        } else {
            help_marker("With this off, stars without a value stay visible (grey) even when the range is "
                        "narrowed: the range only judges stars that have a value.");
        }
        const std::size_t with_data = filters.domain(active).with_data;
        ImGui::TextDisabled("%s of %s stars have data (%.0f %%)", with_commas(with_data).c_str(),
                            with_commas(filters.total_stars).c_str(),
                            filters.total_stars ? 100.0 * static_cast<double>(with_data) / static_cast<double>(filters.total_stars) : 0.0);
        ImGui::Spacing();

        if (scene::is_range_scheme(active)) draw_range_filter(active, filters, colors.ranges);
        else draw_class_filter(filters);

        // Global filters: they do not follow the colour scheme. Drawn after
        // the scheme filter so the legend and its slider stay next to each other.
        draw_distance_filter(filters);
        draw_tag_filter(registry, colors, filters);

        // ---- combine -----------------------------------------------------------------------------
        ImGui::SeparatorText("Combine");
        draw_combine_section(active, filters);

        ImGui::Spacing();
        ImGui::Separator();
        // PushTextWrapPos(0) = wrap at the window's right edge.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("last filter pass %.2f ms  |  P/Tab hides this panel  |  drag the left edge to resize",
                            filters.last_pass_ms);
        ImGui::PopTextWrapPos();
    }
}

}  // namespace starmap::ui
