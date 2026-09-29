// =============================================================================
// ui/StarDetailsView.cpp — see StarDetailsView.h.
//
// IMGUI TABLES, as used below:
//   BeginTable(id, columns, flags) ... EndTable() (EndTable ONLY if
//   BeginTable returned true: it returns false when the table is clipped).
//   TableSetupColumn() declares each column once (name, sizing policy);
//   TableNextRow() starts a row and TableSetColumnIndex(i) / TableNextColumn()
//   moves between cells. Everything drawn in a cell is ordinary ImGui text.
//   SizingStretchProp lets columns share the width proportionally, which
//   suits a resizable side panel.
// =============================================================================
#include "ui/StarDetailsView.h"

#include "catalog/Indexes.h"
#include "catalog/StarDetails.h"
#include "ecs/Components.h"
#include "scene/ColorScheme.h"
#include "scene/Filters.h"
#include "scene/Selection.h"
#include "scene/Tags.h"
#include "systems/InteractionSystem.h"
#include "systems/SelectionSystem.h"
#include "systems/TagSystem.h"
#include "ui/UiUtil.h"

#include <entt/entity/registry.hpp>
#include <glm/geometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace starmap::ui {

namespace {

constexpr ImVec4 kWarn{1.0f, 0.65f, 0.3f, 1.0f};

// Short, human labels for the *_source provenance codes written by the
// Python catalogue builder (short because they share a 380-point panel with
// two other columns; the raw code is in the cell's tooltip). Unknown codes
// are shown verbatim, so a new source never disappears silently.
std::string source_label(std::string_view code) {
    if (code.empty()) return {};
    static const std::map<std::string, std::string, std::less<>> labels = {
        {"gaia_parallax", "Gaia plx"}, {"gaia_dr3", "Gaia DR3"}, {"hyg", "HYG"},
        {"exoplanet_archive", "NASA Exo"}, {"sol", "IAU"},
        {"gaia_esphs", "Gaia ESP-HS"}, {"gaia_gspspec", "GSP-Spec"},
        {"gaia_gspphot", "GSP-Phot"}, {"gaia_flame", "Gaia FLAME"},
        {"hypatia", "Hypatia"}, {"estimated_from_hyg_bv", "HYG B-V est."},
        {"hyg_visual", "HYG V mag"},
    };
    const auto it = labels.find(code);
    return it != labels.end() ? it->second : std::string(code);
}

// Metallicity source codes carry the measured quantity in brackets
// ('hypatia[Fe/H]', 'gaia_gspspec[M/H]'): split into (source, quantity).
std::pair<std::string, std::string> split_quantity(const std::string& code) {
    const auto b = code.find('[');
    if (b == std::string::npos) return {code, {}};
    return {code.substr(0, b), code.substr(b)};
}

// Catalogue display order and labels for the names table. star_names has
// ~20 catalog codes; the obvious ones first, the machine ids last.
struct CatalogLabel {
    const char* code;
    const char* label;
};
constexpr CatalogLabel kCatalogOrder[] = {
    {"IAU", "IAU name"}, {"proper", "Proper name"}, {"bayer", "Bayer"}, {"bayer_unicode", "Bayer (Greek)"},
    {"bayer_greek", "Bayer (letter)"}, {"bayer_abbrev", "Bayer (abbr.)"}, {"bayer_plain", "Bayer (plain)"},
    {"bayer_component", "Bayer comp."}, {"flamsteed", "Flamsteed"}, {"gliese", "Gliese/GJ"},
    {"HR", "HR (BSC)"}, {"HD", "HD"}, {"HIP", "Hipparcos"}, {"Gaia DR3", "Gaia DR3"},
    {"exoplanet_host", "Exoplanet host"},
};

int catalog_rank(std::string_view code) {
    for (int i = 0; i < static_cast<int>(std::size(kCatalogOrder)); ++i)
        if (code == kCatalogOrder[i].code) return i;
    return 1000;  // aliases and unknown codes go last
}
std::string catalog_label(std::string_view code) {
    for (const auto& c : kCatalogOrder)
        if (code == c.code) return c.label;
    return std::string(code);  // e.g. 'bayer_alias'
}

// Right ascension in hours/minutes/seconds (24 h = 360°, so 1 h = 15°), the
// way star charts print it; declination in degrees/arcminutes/arcseconds.
std::string format_ra(double ra_deg) {
    const double h = std::fmod(ra_deg / 15.0 + 24.0, 24.0);
    const int hh = static_cast<int>(h);
    const int mm = static_cast<int>((h - hh) * 60.0);
    const double ss = ((h - hh) * 60.0 - mm) * 60.0;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%02dh %02dm %05.2fs", hh, mm, ss);
    return buf;
}
std::string format_dec(double dec_deg) {
    const char sign = dec_deg < 0 ? '-' : '+';
    const double a = std::fabs(dec_deg);
    const int dd = static_cast<int>(a);
    const int mm = static_cast<int>((a - dd) * 60.0);
    const double ss = ((a - dd) * 60.0 - mm) * 60.0;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%c%02d\u00b0 %02d' %04.1f\"", sign, dd, mm, ss);
    return buf;
}

// Discovery methods are long ("Transit Timing Variations"); a 5-column table
// in a 380-point panel needs short forms. The full text goes in a tooltip.
const char* method_short(std::string_view m) {
    if (m == "Radial Velocity") return "RV";
    if (m == "Transit") return "Transit";
    if (m == "Imaging") return "Imaging";
    if (m == "Microlensing") return "Microlens";
    if (m == "Astrometry") return "Astrometry";
    if (m == "Transit Timing Variations") return "TTV";
    if (m == "Eclipse Timing Variations") return "ETV";
    if (m == "Orbital Brightness Modulation") return "OBM";
    if (m == "Pulsar Timing") return "Pulsar";
    if (m == "Disk Kinematics") return "Disk kin.";
    return nullptr;
}

std::optional<double> opt(const std::optional<float>& f) {
    return f ? std::optional<double>(static_cast<double>(*f)) : std::nullopt;
}

// Three-column row: label | value | source (dim). The value is drawn by the
// caller-supplied lambda so each row can format itself.
template <typename F>
void sourced_row(const char* label, const std::string& source, F&& draw_value) {
    row_label(label);
    draw_value();
    ImGui::TableSetColumnIndex(2);
    if (source.empty()) return;
    ImGui::TextDisabled("%s", source_label(source).c_str());
    // Provenance detail on hover: the exact code stored in stars.db.
    ImGui::SetItemTooltip("source: %s", source.c_str());
}
void value_row(const char* label, const catalog::SourcedValue& v, int decimals, const char* suffix = "") {
    sourced_row(label, v.value ? v.source : std::string(), [&] { text_opt(v.value, decimals, suffix); });
}

bool begin_props_table(const char* id) {
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return false;
    // Label column sized to the longest label (fixed), the other two share
    // what is left (stretch), so widening the panel widens values/sources.
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Spectral type ").x);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    return true;
}

void draw_filtered_notice(entt::registry& registry, scene::FilterSettings& filters) {
    // Policy (see scene/Filters.h): a selected star is exempt from filtering,
    // so a search hit the filters would hide is still drawn and selected. We
    // SAY so rather than silently bending the filter, and offer a one-click
    // way back to "show everything".
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kWarn, "Hidden by the current filters; shown because it is selected.");
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Clear all filters")) {
        const bool was_tagged_view = filters.only_tagged;
        for (std::size_t i = 0; i < scene::FilterSettings::kSchemes; ++i)
            scene::reset_filter(static_cast<scene::ColorScheme>(i), filters);
        scene::reset_distance(filters);
        filters.only_tagged = false;
        filters.combine = false;
        filters.dirty = true;
        // The tagged view also replaces scheme colours. Turning it off here
        // has to mark those stale, or the stars would stay in tag colours
        // until the next scheme change.
        if (was_tagged_view) {
            if (auto* colors = registry.ctx().find<scene::ColorSettings>()) colors->dirty = true;
        }
    }
}

void draw_names(const catalog::NameIndex& names, entt::entity e) {
    // Group the star's names by catalogue: collect (rank, code) -> names.
    std::map<std::pair<int, std::string>, std::vector<const catalog::NameEntry*>> groups;
    const auto list = names.names_of(e);
    for (const auto& n : list) groups[{catalog_rank(n.catalog), n.catalog}].push_back(&n);
    char header[48];
    std::snprintf(header, sizeof header, "Names (%zu)###names", list.size());
    // "###names": text after ### is the ID only, so the header keeps its
    // open/closed state even though the visible count changes per star.
    if (!ImGui::CollapsingHeader(header)) return;
    if (!ImGui::BeginTable("##names", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("catalog", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Bayer (letter) ").x);
    ImGui::TableSetupColumn("names", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& [key, entries] : groups) {
        row_label(catalog_label(key.second).c_str());
        std::string joined;
        for (const auto* n : entries) {
            if (!joined.empty()) joined += ", ";
            joined += n->name;
            if (n->is_primary) joined += " *";
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(joined.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("* = display name");
}

void draw_planets(const catalog::StarDetails& d) {
    char header[48];
    std::snprintf(header, sizeof header, "Planets (%zu)###planets", d.planets.size());
    // ImGuiTreeNodeFlags_DefaultOpen: open the first time it is shown.
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (d.planets.empty()) {
        ImGui::TextDisabled("No known planets.");
        return;
    }
    if (d.system_planet_count && *d.system_planet_count > static_cast<int>(d.planets.size()))
        ImGui::TextDisabled("(%d in the whole system, incl. companions)", *d.system_planet_count);
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollX;
    // ScrollX: if the panel is narrow the table scrolls sideways instead of
    // squashing numbers into unreadable wraps. A scrolling table is a child
    // window, so it needs an explicit height: rows + header.
    const float h = ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(d.planets.size()) + 1.4f) +
                    ImGui::GetStyle().ScrollbarSize;
    if (!ImGui::BeginTable("##planets", 5, flags, ImVec2(0.0f, h))) return;
    ImGui::TableSetupColumn("Planet");
    ImGui::TableSetupColumn("Period d");
    ImGui::TableSetupColumn("a AU");
    ImGui::TableSetupColumn(fix_units("M / R \u2295").c_str());
    ImGui::TableSetupColumn("Found");
    ImGui::TableSetupScrollFreeze(1, 1);  // keep the name column and header visible while scrolling
    ImGui::TableHeadersRow();
    for (const auto& p : d.planets) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::TableNextColumn();
        text_opt(opt(p.orbital_period_days), p.orbital_period_days && *p.orbital_period_days < 100.0f ? 2 : 0);
        ImGui::TableNextColumn();
        text_opt(opt(p.semi_major_axis_au), 3);
        ImGui::TableNextColumn();
        // Mass and radius share a column: most RV planets have only a
        // (minimum) mass, most transiting planets mostly a radius.
        if (p.mass_earth) ImGui::Text(*p.mass_earth < 10.0f ? "%.2f" : "%.1f", static_cast<double>(*p.mass_earth));
        else text_na();
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::TextDisabled("/");
        ImGui::SameLine(0.0f, 2.0f);
        if (p.radius_earth) ImGui::Text("%.2f", static_cast<double>(*p.radius_earth));
        else text_na();
        ImGui::TableNextColumn();
        const char* shortm = method_short(p.discovery_method);
        if (p.discovery_method.empty()) text_na();
        else ImGui::TextUnformatted(shortm ? shortm : p.discovery_method.c_str());
        if (!p.discovery_method.empty()) ImGui::SetItemTooltip("%s", p.discovery_method.c_str());
        ImGui::SameLine();
        if (p.disc_year) ImGui::TextDisabled("%d", *p.disc_year);
    }
    ImGui::EndTable();
    ImGui::PushTextWrapPos(0.0f);
    if (d.is_sol) ImGui::TextDisabled("Solar System values: NASA planetary fact sheet (the exoplanet archive lists none).");
    else ImGui::TextDisabled("%s", fix_units("Masses (M\u2295) from radial velocity are minimum masses (M sin i); "
                                        "radii (R\u2295) of non-transiting planets are model estimates.").c_str());
    ImGui::PopTextWrapPos();
}

}  // namespace

void StarDetailsView::draw(entt::registry& registry) {
    const entt::entity e = systems::selected_entity(registry);
    if (e == entt::null) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No star selected. Click a star in the view, or search by name above "
                            "(Ctrl+F or /). Try \"eps eri\", \"Sirius\" or \"HIP 16537\".");
        ImGui::PopTextWrapPos();
        return;
    }
    const auto* sd = registry.ctx().find<scene::SelectionDetails>();
    auto& filters = registry.ctx().get<scene::FilterSettings>();
    const auto* info = registry.try_get<ecs::StarInfo>(e);

    // ---- title ---------------------------------------------------------------------------------
    // ImGui 1.92 can change the font SIZE on the fly: PushFont(nullptr, size)
    // keeps the current font and rasterises its glyphs at the new size on
    // demand (the dynamic atlas). Before 1.92 a heading needed a second font
    // loaded at start-up.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextUnformatted(info ? info->display_name.c_str() : "?");
    ImGui::PopFont();
    if (info && info->bayer_name && *info->bayer_name != info->display_name) ImGui::TextDisabled("%s", info->bayer_name->c_str());
    // The tag is on the entity, not in the SQLite details, so it can be shown
    // before the lazy query returns. Removing it goes through TagSystem so
    // the tagged-only view and the colours update the same frame.
    if (const auto* tag = registry.try_get<scene::StarTag>(e)) {
        const glm::vec3 rgb = scene::tag_rgb(tag->color);
        const float sw = ImGui::GetTextLineHeight();
        ImGui::ColorButton("##sel_tag", ImVec4(rgb.r, rgb.g, rgb.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(sw, sw));
        ImGui::SameLine();
        ImGui::Text("Tag: %s", scene::tag_color_name(tag->color));
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove tag")) systems::clear_star_tag(registry, e);
    }

    if (filters.selection_filtered) draw_filtered_notice(registry, filters);

    if (ImGui::Button("Fly to (F)")) systems::focus_selection(registry);
    ImGui::SameLine();
    if (ImGui::Button("Deselect (Esc)")) {
        systems::clear_selection(registry);
        return;
    }

    if (!sd || sd->entity != e || (!sd->details && sd->error.empty())) {
        ImGui::TextDisabled("loading...");
        return;
    }
    if (!sd->details) {
        ImGui::TextColored(kWarn, "Could not load details: %s", sd->error.c_str());
        return;
    }
    const catalog::StarDetails& d = *sd->details;

    // ---- position -----------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Position", ImGuiTreeNodeFlags_DefaultOpen) && begin_props_table("##pos")) {
        sourced_row("Distance", d.dist_source, [&] {
            if (d.is_sol) ImGui::TextUnformatted("0 (origin)");
            else ImGui::Text("%.2f ly  %.2f pc", d.dist_ly, d.dist_pc);
        });
        sourced_row("RA (ICRS)", {}, [&] {
            if (d.ra_deg) ImGui::TextUnformatted(format_ra(*d.ra_deg).c_str());
            else text_na();
        });
        sourced_row("Dec (ICRS)", {}, [&] {
            if (d.dec_deg) ImGui::TextUnformatted(format_dec(*d.dec_deg).c_str());
            else text_na();
        });
        // Galactic longitude/latitude straight from the rendered position:
        // l = atan2(y, x) measured from the Galactic Centre towards
        // rotation, b = asin(z / r) above the plane (see ecs::Position).
        sourced_row("Gal. l, b", {}, [&] {
            const auto* pos = registry.try_get<ecs::Position>(e);
            const float r = pos ? glm::length(pos->galactic_pc) : 0.0f;
            if (!pos || r <= 0.0f) return text_na();
            const double rad2deg = 180.0 / std::numbers::pi;
            double l = std::atan2(static_cast<double>(pos->galactic_pc.y), static_cast<double>(pos->galactic_pc.x)) * rad2deg;
            if (l < 0.0) l += 360.0;
            const double b = std::asin(static_cast<double>(pos->galactic_pc.z / r)) * rad2deg;
            ImGui::Text("%.2f\u00b0, %+.2f\u00b0", l, b);
        });
        sourced_row("Parallax", d.parallax_mas ? "gaia_dr3" : "", [&] {
            if (!d.parallax_mas) return text_na();
            if (d.parallax_error_mas) ImGui::Text("%.3f \u00b1 %.3f mas", *d.parallax_mas, *d.parallax_error_mas);
            else ImGui::Text("%.3f mas", *d.parallax_mas);
        });
        ImGui::EndTable();
    }

    // ---- physical ---------------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Physical properties", ImGuiTreeNodeFlags_DefaultOpen) && begin_props_table("##phys")) {
        sourced_row("Spectral type", d.spectral_type ? d.spectral_type_source : "", [&] {
            if (!d.spectral_type || d.spectral_type->empty()) return text_na();
            ImGui::TextUnformatted(d.spectral_type->c_str());
            if (d.spectral_class) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", d.spectral_class->c_str());
            }
        });
        value_row("Temperature", d.teff_k, 0, " K");
        value_row(fix_units("Radius R\u2609").c_str(), d.radius_sol, 3);
        value_row(fix_units("Mass M\u2609").c_str(), d.mass_sol, 3);
        value_row(fix_units("Lum. L\u2609").c_str(), d.luminosity_sol, 4);
        value_row("Age", d.age_gyr, 2, " Gyr");
        {
            // "-0.05 [Fe/H]" in the value, "Hypatia" as the source.
            const auto [src, quantity] = split_quantity(d.metallicity.source);
            const std::string unit = " " + (quantity.empty() ? std::string("dex") : quantity);
            sourced_row("Metallicity", d.metallicity.value ? src : std::string(),
                        [&] { text_opt(d.metallicity.value, 2, unit.c_str()); });
        }
        ImGui::EndTable();
    }

    draw_planets(d);  // before the long tables: planets are what most people look for

    // ---- photometry -----------------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Magnitudes & colour", ImGuiTreeNodeFlags_DefaultOpen) && begin_props_table("##phot")) {
        sourced_row("Apparent",  d.app_mag ? (d.app_mag_band == "G" ? std::string("band G") : std::string("band V")) : "",
                    [&] { text_opt(d.app_mag, 2); });
        sourced_row("Absolute", {}, [&] { text_opt(d.abs_mag, 2); });
        sourced_row("V mag", {}, [&] { text_opt(d.vmag, 2); });
        sourced_row("Gaia G", d.gaia_g_mag ? "gaia_dr3" : "", [&] { text_opt(d.gaia_g_mag, 3); });
        sourced_row("BP-RP", {}, [&] { text_opt(d.bp_rp, 3); });
        sourced_row("B-V", {}, [&] { text_opt(d.ci_bv, 3); });
        ImGui::EndTable();
    }

    // ---- quality / identifiers ----------------------------------------------------------------------
    if (ImGui::CollapsingHeader("Quality & identifiers") && begin_props_table("##ids")) {
        sourced_row("RUWE", {}, [&] {
            if (!d.ruwe) return text_na();
            // RUWE (renormalised unit weight error) ~1.0 means the single-star
            // astrometric model fits; > 1.4 hints at an unresolved binary or
            // a bad solution, so the distance deserves less trust.
            if (*d.ruwe > 1.4) ImGui::TextColored(kWarn, "%.2f (poor fit)", *d.ruwe);
            else ImGui::Text("%.2f", *d.ruwe);
        });
        sourced_row("Parallax S/N", {}, [&] {
            if (!d.parallax_mas || !d.parallax_error_mas || *d.parallax_error_mas <= 0.0) return text_na();
            ImGui::Text("%.0f", *d.parallax_mas / *d.parallax_error_mas);
        });
        auto id_row = [&](const char* label, const std::optional<std::int64_t>& v) {
            sourced_row(label, {}, [&] {
                if (v) ImGui::Text("%lld", static_cast<long long>(*v));
                else text_na();
            });
        };
        id_row("Gaia DR3", d.gaia_source_id);
        id_row("HIP", d.hip);
        id_row("HD", d.hd);
        id_row("HYG id", d.hyg_id);
        sourced_row("star_id", {}, [&] { ImGui::Text("%lld", static_cast<long long>(d.star_id)); });
        ImGui::EndTable();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Sources: %s", d.sources.empty() ? "n/a" : d.sources.c_str());
        ImGui::PopTextWrapPos();
    }

    if (const auto* names = registry.ctx().find<catalog::NameIndex>()) draw_names(*names, e);

    ImGui::Spacing();
    ImGui::TextDisabled("details loaded from SQLite in %.2f ms", sd->load_ms);
}

}  // namespace starmap::ui
