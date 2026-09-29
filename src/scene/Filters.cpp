// =============================================================================
// scene/Filters.cpp — filter domains, predicates and CLI requests
// (see Filters.h for the rules and the data flow).
// =============================================================================
#include "scene/Filters.h"

#include "catalog/CatalogStats.h"
#include "ecs/Components.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace starmap::scene {
namespace {

// Order MUST match the ColorScheme enum (static_assert below).
// "R\u2609" is "R☉" (U+2609 SUN, the astronomical symbol for the Sun), written
// as a universal character name so the source stays ASCII; the compiler
// stores it as UTF-8, which is what ImGui expects. The UI falls back to
// "Rsun" if the font has no glyph for it (see ui/FilterPanel.cpp).
constexpr std::array<FilterFieldInfo, FilterSettings::kSchemes> kFieldInfo = {{
    {"temperature", "K", "%.0f"},
    {"spectral class", "", "%.0f"},
    {"radius", "R\u2609", "%.3g"},
    {"age", "Gyr", "%.2f"},
    {"metallicity", "dex", "%+.2f"},
    {"planet count", "planets", "%.0f"},
}};
static_assert(kFieldInfo.size() == static_cast<std::size_t>(ColorScheme::Count));

// Spectral classes in the order astronomers list them: the temperature
// sequence O (hottest) .. M, then brown dwarfs L T Y, white dwarfs D, and
// the rare carbon (C), S-type and Wolf-Rayet (W) classes.
constexpr std::string_view kClassOrder = "OBAFGKMLTYDCSW";

// Tolerance (in value units) for comparisons against a slider end near the
// value `at`. Floats that went through log10/pow or a text field rarely land
// exactly on the domain's end value, so "is this handle at the end?" and
// "is this star exactly on the limit?" need a little slack:
//   * linear slider: a ten-thousandth of the whole slider width;
//   * log slider: a ten-thousandth of the value itself (relative). A log
//     slider gives every decade the same width, so an absolute tolerance
//     computed from the full span (86 R_sun) would swallow the whole bottom
//     decade (0.013..0.1 R_sun) and make small trims undetectable.
float tolerance(const FilterDomain& d, float at) {
    if (d.log_scale) return 1e-4f * std::fabs(at);
    return 1e-4f * std::max(d.max - d.min, 1e-6f);
}

FilterDomain domain_from(const catalog::FieldStats* f, bool log_scale, bool integer) {
    FilterDomain d;
    d.log_scale = log_scale;
    d.integer = integer;
    if (!f || f->count == 0) return d;  // field absent: keep the harmless 0..1 default
    d.min = static_cast<float>(f->min);
    d.max = static_cast<float>(f->max);
    d.p01 = static_cast<float>(f->p01);
    d.p99 = static_cast<float>(f->p99);
    d.with_data = f->count;
    if (log_scale && d.min <= 0.0f) {
        // log10(0) = -inf: a log slider needs a strictly positive lower end.
        d.min = d.p01 > 0.0f ? d.p01 * 0.5f : 1e-3f;
    }
    if (d.max <= d.min) d.max = d.min + 1.0f;  // degenerate (all values equal): give the slider some width
    return d;
}

std::string format_value(ColorScheme s, float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, filter_field_info(s).format, static_cast<double>(v));
    return buf;
}

}  // namespace

const FilterFieldInfo& filter_field_info(ColorScheme s) { return kFieldInfo[static_cast<std::size_t>(s)]; }

bool is_range_scheme(ColorScheme s) { return s != ColorScheme::SpectralClass && s != ColorScheme::Count; }

FilterSettings make_filter_settings(const catalog::CatalogStats& stats) {
    FilterSettings fs;
    fs.total_stars = stats.total_stars;
    fs.visible = fs.total_stars;  // nothing filtered until the first FilterSystem pass says otherwise
    fs.domain(ColorScheme::Temperature) = domain_from(stats.field("teff_k"), false, false);
    // Log scale for radius: values span 0.01 (white dwarfs) .. 90 R_sun (giants),
    // four decades — on a linear slider everything below 1 R_sun would share
    // the leftmost pixel. Matches the Diameter colormap, which is also log.
    fs.domain(ColorScheme::Diameter) = domain_from(stats.field("radius_sol"), true, false);
    fs.domain(ColorScheme::Age) = domain_from(stats.field("age_gyr"), false, false);
    fs.domain(ColorScheme::Metallicity) = domain_from(stats.field("metallicity"), false, false);
    // CatalogStats records planet_count only for hosts (>= 1), so min is 1.
    // Log + integer: 1, 2, 3 planets get as much slider room as 4..8.
    fs.domain(ColorScheme::Planets) = domain_from(stats.field("planet_count"), true, true);

    // Distance is linear in light years. A log axis cannot start at 0, and
    // Sol's distance is 0, so log is the wrong tool here. The catalog's own
    // edge (~200 ly) is the domain when a hand-made CatalogStats has no
    // dist_ly field (the unit tests); the real app always has the field.
    if (const auto* dist = stats.field("dist_ly"); dist && dist->count > 0) {
        fs.distance_domain = domain_from(dist, false, false);
    } else {
        fs.distance_domain.min = 0.0f;
        fs.distance_domain.max = 200.0f;
        fs.distance_domain.p01 = 0.0f;
        fs.distance_domain.p99 = 200.0f;
        fs.distance_domain.with_data = stats.total_stars;
    }

    // Spectral classes actually present, canonical order first, then any
    // unexpected letters alphabetically (std::map iterates sorted).
    for (char c : kClassOrder) {
        if (auto it = stats.spectral_classes.find(c); it != stats.spectral_classes.end() && it->second > 0)
            fs.classes.push_back({c, it->second});
    }
    for (const auto& [c, n] : stats.spectral_classes) {
        if (c != '?' && kClassOrder.find(c) == std::string_view::npos && n > 0) fs.classes.push_back({c, n});
    }
    if (auto it = stats.spectral_classes.find('?'); it != stats.spectral_classes.end()) fs.unknown_class = it->second;
    fs.domain(ColorScheme::SpectralClass).with_data = fs.total_stars - fs.unknown_class;

    for (std::size_t i = 0; i < FilterSettings::kSchemes; ++i) reset_filter(static_cast<ColorScheme>(i), fs);
    reset_distance(fs);  // lo/hi = the full domain; also sets dirty
    fs.only_tagged = false;
    fs.dirty = true;
    return fs;
}

std::optional<float> scheme_value(ColorScheme s, const ecs::StarInfo& info, const ecs::Physical& phys) {
    (void)info;  // only SpectralClass (which has no numeric value) would need it
    switch (s) {
        case ColorScheme::Temperature: return phys.teff_k;
        case ColorScheme::Diameter:
            // A radius <= 0 is a catalog glitch; ColorSystem also treats it as missing.
            if (phys.radius_sol && *phys.radius_sol > 0.0f) return phys.radius_sol;
            return std::nullopt;
        case ColorScheme::Age: return phys.age_gyr;
        case ColorScheme::Metallicity: return phys.metallicity;
        case ColorScheme::Planets:
            // 0 known planets = no data (see Filters.h).
            if (phys.planet_count > 0) return static_cast<float>(phys.planet_count);
            return std::nullopt;
        case ColorScheme::SpectralClass:
        case ColorScheme::Count:
            break;
    }
    return std::nullopt;
}

bool has_data(ColorScheme s, const ecs::StarInfo& info, const ecs::Physical& phys) {
    if (s == ColorScheme::SpectralClass) return info.spectral_class != '?';
    return scheme_value(s, info, phys).has_value();
}

bool range_is_trimmed(const SchemeFilter& f, const FilterDomain& d) {
    return f.lo > d.min + tolerance(d, d.min) || f.hi < d.max - tolerance(d, d.max);
}

bool distance_is_trimmed(const FilterSettings& settings) {
    // Reuse the scheme-filter test so "is the handle at the end?" has one
    // definition. hide_missing stays false: distance has no missing values.
    SchemeFilter stand_in;
    stand_in.lo = settings.distance_lo;
    stand_in.hi = settings.distance_hi;
    return range_is_trimmed(stand_in, settings.distance_domain);
}

void reset_distance(FilterSettings& settings) {
    settings.distance_lo = settings.distance_domain.min;
    settings.distance_hi = settings.distance_domain.max;
    settings.dirty = true;
}

bool is_default(ColorScheme s, const FilterSettings& settings) {
    const SchemeFilter& f = settings.filter(s);
    if (f.hide_missing) return false;
    if (s == ColorScheme::SpectralClass) {
        for (const auto& c : settings.classes) {
            if (!f.classes.test(static_cast<unsigned char>(c.letter))) return false;
        }
        return true;
    }
    return !range_is_trimmed(f, settings.domain(s));
}

void reset_filter(ColorScheme s, FilterSettings& settings) {
    SchemeFilter& f = settings.filter(s);
    const FilterDomain& d = settings.domain(s);
    f.hide_missing = false;
    f.lo = d.min;
    f.hi = d.max;
    f.classes.reset();
    if (s == ColorScheme::SpectralClass) {
        for (const auto& c : settings.classes) f.classes.set(static_cast<unsigned char>(c.letter));
    }
    settings.dirty = true;
}

void set_all_classes(FilterSettings& settings, bool all) {
    SchemeFilter& f = settings.filter(ColorScheme::SpectralClass);
    f.classes.reset();
    if (all) {
        for (const auto& c : settings.classes) f.classes.set(static_cast<unsigned char>(c.letter));
    }
    settings.dirty = true;
}

bool passes(ColorScheme s, const SchemeFilter& f, const FilterDomain& d, const ecs::StarInfo& info,
            const ecs::Physical& phys) {
    if (s == ColorScheme::SpectralClass) {
        if (info.spectral_class == '?') return !f.hide_missing;
        // static_cast<unsigned char>: a char may be signed; a negative index
        // into the bitset would be out of range. Non-ASCII letters (>= 128)
        // can't be enabled in the UI, so they are treated as unchecked.
        const auto idx = static_cast<unsigned char>(info.spectral_class);
        return idx < f.classes.size() && f.classes.test(idx);
    }
    const std::optional<float> v = scheme_value(s, info, phys);
    if (!v) return !f.hide_missing;           // no data: only the toggle decides
    if (!range_is_trimmed(f, d)) return true;  // full range: nothing to test
    // Inclusive ends, forgiving float round-off.
    return *v >= f.lo - tolerance(d, f.lo) && *v <= f.hi + tolerance(d, f.hi);
}

std::vector<ColorScheme> applied_filters(const FilterSettings& settings, ColorScheme active) {
    std::vector<ColorScheme> out;
    if (!settings.combine) {
        if (!is_default(active, settings)) out.push_back(active);
        return out;
    }
    for (std::size_t i = 0; i < FilterSettings::kSchemes; ++i) {
        const auto s = static_cast<ColorScheme>(i);
        if (!is_default(s, settings)) out.push_back(s);
    }
    return out;
}

bool star_visible(const FilterSettings& settings, const std::vector<ColorScheme>& applied,
                  const ecs::StarInfo& info, const ecs::Physical& phys, float dist_ly, bool tagged) {
    if (info.is_sol) return true;  // the reference point never disappears (see Filters.h)
    // std::all_of short-circuits: the first failing filter ends the test, so
    // with several filters combined the typical star costs one or two checks.
    const bool schemes_ok = std::all_of(applied.begin(), applied.end(), [&](ColorScheme s) {
        return passes(s, settings.filter(s), settings.domain(s), info, phys);
    });
    if (!schemes_ok) return false;
    // Distance is AND, not another scheme: it applies even when `applied`
    // is empty (combine off, active scheme at its default).
    if (distance_is_trimmed(settings)) {
        const FilterDomain& d = settings.distance_domain;
        if (dist_ly < settings.distance_lo - tolerance(d, settings.distance_lo) ||
            dist_ly > settings.distance_hi + tolerance(d, settings.distance_hi))
            return false;
    }
    if (settings.only_tagged && !tagged) return false;
    return true;
}

std::string describe_filter(ColorScheme s, const FilterSettings& settings) {
    if (is_default(s, settings)) return {};
    const SchemeFilter& f = settings.filter(s);
    std::string out;
    if (s == ColorScheme::SpectralClass) {
        std::size_t on = 0;
        std::string letters;
        for (const auto& c : settings.classes) {
            if (f.classes.test(static_cast<unsigned char>(c.letter))) {
                ++on;
                letters += c.letter;
                letters += ' ';
            }
        }
        if (on < settings.classes.size()) {
            out = on == 0 ? "no classes" : "classes " + letters;
            if (!out.empty() && out.back() == ' ') out.pop_back();
        }
    } else if (range_is_trimmed(f, settings.domain(s))) {
        const auto& info = filter_field_info(s);
        out = format_value(s, f.lo) + " - " + format_value(s, f.hi) + " " + info.unit;
    }
    if (f.hide_missing) {
        if (!out.empty()) out += ", ";
        out += s == ColorScheme::Planets ? "hosts only" : "no-data hidden";
    }
    return out;
}

std::string apply_filter_options(FilterSettings& settings, const FilterOptions& options, ColorScheme active) {
    for (const FilterRequest& r : options.ranges) {
        if (!is_range_scheme(r.scheme)) return "--filter needs a range scheme (use --classes for spectral classes)";
        const FilterDomain& d = settings.domain(r.scheme);
        SchemeFilter& f = settings.filter(r.scheme);
        f.lo = std::clamp(r.lo.value_or(d.min), d.min, d.max);
        f.hi = std::clamp(r.hi.value_or(d.max), d.min, d.max);
        if (f.lo > f.hi) std::swap(f.lo, f.hi);
    }
    for (ColorScheme s : options.hide_missing) settings.filter(s).hide_missing = true;
    if (options.hide_missing_active) settings.filter(active).hide_missing = true;
    if (options.classes) {
        SchemeFilter& f = settings.filter(ColorScheme::SpectralClass);
        f.classes.reset();
        for (char ch : *options.classes) {
            if (ch == ',' || ch == ' ') continue;
            const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            const bool present = std::any_of(settings.classes.begin(), settings.classes.end(),
                                             [up](const SpectralClassEntry& c) { return c.letter == up; });
            if (!present) return std::string("--classes: no stars of class '") + ch + "' in the catalog";
            f.classes.set(static_cast<unsigned char>(up));
        }
    }
    if (options.distance) {
        const FilterDomain& d = settings.distance_domain;
        settings.distance_lo = std::clamp(options.distance->lo.value_or(d.min), d.min, d.max);
        settings.distance_hi = std::clamp(options.distance->hi.value_or(d.max), d.min, d.max);
        if (settings.distance_lo > settings.distance_hi) std::swap(settings.distance_lo, settings.distance_hi);
    }
    settings.combine = settings.combine || options.combine;
    settings.dirty = true;
    return {};
}

}  // namespace starmap::scene
