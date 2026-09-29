// =============================================================================
// tests/test_filters.cpp — unit tests for milestone 2's filtering logic
// =============================================================================
//
// Everything tested here lives in starmap_core and needs neither a GPU nor a
// window nor Dear ImGui:
//   * the per-star predicate (scene::passes / star_visible): ranges, log
//     ranges, the "hide stars with no data" toggle, the spectral-class set,
//     combine mode, the distance range, the tagged-only view, and the
//     "Sol is always visible" rule;
//   * the bookkeeping helpers (is_default, applied_filters, describe_filter,
//     apply_filter_options) and the CLI flags that feed them;
//   * the ECS pass (systems::update_filters) on a tiny hand-made registry;
//   * the pure maths of the two-handle range slider (ui::SliderScale etc.).
// Keeping this logic out of the ImGui/bgfx code is precisely what makes it
// testable: the widget and the panel are thin shells around these functions.
// =============================================================================
#include "app/CommandLine.h"
#include "catalog/CatalogStats.h"
#include "ecs/Components.h"
#include "scene/ColorScheme.h"
#include "scene/Filters.h"
#include "scene/Tags.h"
#include "systems/ColorSystem.h"
#include "systems/FilterSystem.h"
#include "systems/TagSystem.h"
#include "ui/RangeSliderMath.h"

#include <doctest/doctest.h>
#include <entt/entity/registry.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cmath>

using namespace starmap;
using scene::ColorScheme;

namespace {

// Hand-made statistics resembling the real catalog, so the tests don't need
// stars.db. Field names are the db column names CatalogStats uses.
catalog::CatalogStats fake_stats() {
    catalog::CatalogStats st;
    st.total_stars = 100;
    auto field = [&](const char* name, std::size_t n, double mn, double mx, double p01, double p99) {
        catalog::FieldStats f;
        f.name = name;
        f.count = n;
        f.min = mn;
        f.max = mx;
        f.p01 = p01;
        f.p99 = p99;
        st.fields.push_back(f);
    };
    field("teff_k", 40, 575.0, 18290.0, 2926.0, 7607.0);
    field("radius_sol", 30, 0.0131, 86.4, 0.149, 2.495);
    field("age_gyr", 10, 0.0064, 15.9, 0.1, 12.0);
    field("metallicity", 20, -5.06, 1.07, -1.0, 0.4);
    field("planet_count", 5, 1.0, 8.0, 1.0, 6.0);
    field("dist_ly", 100, 0.0, 200.0, 8.0, 195.0);
    st.spectral_classes = {{'G', 10}, {'K', 20}, {'M', 30}, {'D', 5}, {'?', 35}};
    return st;
}

ecs::StarInfo info_of(char cls, bool sol = false) {
    ecs::StarInfo i;
    i.spectral_class = cls;
    i.is_sol = sol;
    return i;
}

}  // namespace

TEST_CASE("filters: defaults show everything") {
    const auto fs = scene::make_filter_settings(fake_stats());
    CHECK(fs.visible == fs.total_stars);  // nothing hidden before the first pass
    CHECK(fs.dirty);
    for (int i = 0; i < static_cast<int>(ColorScheme::Count); ++i) {
        const auto s = static_cast<ColorScheme>(i);
        CHECK(scene::is_default(s, fs));
        CHECK(scene::describe_filter(s, fs).empty());
    }
    // Domains come from the full min..max; log for diameter/planets, integer planets.
    CHECK(fs.domain(ColorScheme::Temperature).min == doctest::Approx(575.0f));
    CHECK(fs.domain(ColorScheme::Temperature).max == doctest::Approx(18290.0f));
    CHECK(fs.domain(ColorScheme::Diameter).log_scale);
    CHECK(fs.domain(ColorScheme::Planets).log_scale);
    CHECK(fs.domain(ColorScheme::Planets).integer);
    CHECK_FALSE(fs.domain(ColorScheme::Age).log_scale);
    // Classes: canonical order (G K M before D), '?' is NOT a class entry.
    REQUIRE(fs.classes.size() == 4);
    CHECK(fs.classes[0].letter == 'G');
    CHECK(fs.classes[2].letter == 'M');
    CHECK(fs.classes[3].letter == 'D');
    CHECK(fs.unknown_class == 35);
    CHECK(fs.domain(ColorScheme::SpectralClass).with_data == 65);
    CHECK(scene::applied_filters(fs, ColorScheme::Temperature).empty());
    // Distance is its own range, full span, linear, not a scheme filter.
    CHECK(fs.distance_domain.min == doctest::Approx(0.0f));
    CHECK(fs.distance_domain.max == doctest::Approx(200.0f));
    CHECK_FALSE(fs.distance_domain.log_scale);
    CHECK(fs.distance_lo == doctest::Approx(0.0f));
    CHECK(fs.distance_hi == doctest::Approx(200.0f));
    CHECK_FALSE(scene::distance_is_trimmed(fs));
    CHECK_FALSE(fs.only_tagged);
}

TEST_CASE("filters: linear range with inclusive ends, no-data only hidden by the toggle") {
    auto fs = scene::make_filter_settings(fake_stats());
    const auto s = ColorScheme::Temperature;
    auto& f = fs.filter(s);
    f.lo = 5000.0f;
    f.hi = 6500.0f;
    CHECK_FALSE(scene::is_default(s, fs));
    const auto& d = fs.domain(s);
    const auto info = info_of('G');
    ecs::Physical p;
    p.teff_k = 5772.0f;
    CHECK(scene::passes(s, f, d, info, p));
    p.teff_k = 5000.0f;  // ends are inclusive
    CHECK(scene::passes(s, f, d, info, p));
    p.teff_k = 6500.0f;
    CHECK(scene::passes(s, f, d, info, p));
    p.teff_k = 4990.0f;  // (the tolerance is 1e-4 of the 17,700 K span = 1.8 K)
    CHECK_FALSE(scene::passes(s, f, d, info, p));
    p.teff_k = 6510.0f;
    CHECK_FALSE(scene::passes(s, f, d, info, p));
    // A star with NO temperature is not "outside the range": only the toggle hides it.
    p.teff_k.reset();
    CHECK(scene::passes(s, f, d, info, p));
    f.hide_missing = true;
    CHECK_FALSE(scene::passes(s, f, d, info, p));
    // The toggle alone (full range) keeps every star that has data.
    f.lo = d.min;
    f.hi = d.max;
    CHECK_FALSE(scene::range_is_trimmed(f, d));
    p.teff_k = 18290.0f;
    CHECK(scene::passes(s, f, d, info, p));
    CHECK(scene::describe_filter(s, fs).find("no-data") != std::string::npos);
}

TEST_CASE("filters: diameter (log domain) and planets (0 = no data)") {
    auto fs = scene::make_filter_settings(fake_stats());
    const auto info = info_of('K');
    {
        const auto s = ColorScheme::Diameter;
        auto& f = fs.filter(s);
        f.lo = 0.5f;
        f.hi = 2.0f;
        ecs::Physical p;
        p.radius_sol = 1.0f;
        CHECK(scene::passes(s, f, fs.domain(s), info, p));
        p.radius_sol = 0.012f;  // white dwarf
        CHECK_FALSE(scene::passes(s, f, fs.domain(s), info, p));
        // Small trims on a log domain are detected (relative tolerance):
        f.lo = 0.02f;  // just above the 0.0131 minimum
        f.hi = fs.domain(s).max;
        CHECK(scene::range_is_trimmed(f, fs.domain(s)));
        p.radius_sol = 0.015f;
        CHECK_FALSE(scene::passes(s, f, fs.domain(s), info, p));
        p.radius_sol = 0.0f;    // glitch value counts as "no data", like ColorSystem does
        CHECK_FALSE(scene::has_data(s, info, p));
        CHECK(scene::passes(s, f, fs.domain(s), info, p));
    }
    {
        const auto s = ColorScheme::Planets;
        auto& f = fs.filter(s);
        f.hide_missing = true;
        ecs::Physical p;
        p.planet_count = 0;
        CHECK_FALSE(scene::has_data(s, info, p));
        CHECK_FALSE(scene::passes(s, f, fs.domain(s), info, p));
        p.planet_count = 3;
        CHECK(scene::passes(s, f, fs.domain(s), info, p));
        f.lo = 4.0f;  // "at least 4 planets"
        CHECK_FALSE(scene::passes(s, f, fs.domain(s), info, p));
        p.planet_count = 8;
        CHECK(scene::passes(s, f, fs.domain(s), info, p));
    }
}

TEST_CASE("filters: spectral class set, unknown class follows the no-data toggle") {
    auto fs = scene::make_filter_settings(fake_stats());
    const auto s = ColorScheme::SpectralClass;
    scene::set_all_classes(fs, false);
    auto& f = fs.filter(s);
    f.classes.set('G');
    f.classes.set('K');
    CHECK_FALSE(scene::is_default(s, fs));
    const ecs::Physical p;
    CHECK(scene::passes(s, f, fs.domain(s), info_of('G'), p));
    CHECK(scene::passes(s, f, fs.domain(s), info_of('K'), p));
    CHECK_FALSE(scene::passes(s, f, fs.domain(s), info_of('M'), p));
    CHECK_FALSE(scene::passes(s, f, fs.domain(s), info_of('D'), p));
    CHECK(scene::passes(s, f, fs.domain(s), info_of('?'), p));  // unknown: shown unless the toggle is on
    f.hide_missing = true;
    CHECK_FALSE(scene::passes(s, f, fs.domain(s), info_of('?'), p));
    // A non-ASCII class byte must not index out of the bitset.
    CHECK_FALSE(scene::passes(s, f, fs.domain(s), info_of(static_cast<char>(0xE9)), p));
    scene::set_all_classes(fs, true);
    f.hide_missing = false;
    CHECK(scene::is_default(s, fs));
    const auto desc = [&] {
        scene::set_all_classes(fs, false);
        fs.filter(s).classes.set('K');
        return scene::describe_filter(s, fs);
    }();
    CHECK(desc.find('K') != std::string::npos);
}

TEST_CASE("filters: only the active scheme applies unless combine is on; Sol is always visible") {
    auto fs = scene::make_filter_settings(fake_stats());
    fs.filter(ColorScheme::Temperature).lo = 5000.0f;
    fs.filter(ColorScheme::Temperature).hi = 6500.0f;
    fs.filter(ColorScheme::Planets).hide_missing = true;

    auto applied = scene::applied_filters(fs, ColorScheme::Temperature);
    REQUIRE(applied.size() == 1);
    CHECK(applied[0] == ColorScheme::Temperature);
    CHECK(scene::applied_filters(fs, ColorScheme::Age).empty());  // Age has no filter of its own

    ecs::Physical hot_no_planets;
    hot_no_planets.teff_k = 5800.0f;
    CHECK(scene::star_visible(fs, applied, info_of('G'), hot_no_planets));

    fs.combine = true;
    applied = scene::applied_filters(fs, ColorScheme::Age);  // active scheme irrelevant when combined
    REQUIRE(applied.size() == 2);
    CHECK_FALSE(scene::star_visible(fs, applied, info_of('G'), hot_no_planets));  // AND: fails planets
    ecs::Physical host = hot_no_planets;
    host.planet_count = 2;
    CHECK(scene::star_visible(fs, applied, info_of('G'), host));
    host.teff_k = 3000.0f;
    CHECK_FALSE(scene::star_visible(fs, applied, info_of('M'), host));  // AND: fails temperature

    // Sol passes no matter what, even with data that would fail.
    ecs::Physical sol_phys;
    sol_phys.teff_k = 100.0f;
    CHECK(scene::star_visible(fs, applied, info_of('G', true), sol_phys));
}

TEST_CASE("filters: FilterSystem tags FilteredOut, counts visible, runs only when needed") {
    entt::registry reg;
    auto& fs = reg.ctx().emplace<scene::FilterSettings>(scene::make_filter_settings(fake_stats()));
    auto& colors = reg.ctx().emplace<scene::ColorSettings>();
    colors.scheme = ColorScheme::Temperature;
    fs.total_stars = 4;
    fs.visible = 4;

    auto make = [&](std::optional<float> teff, bool sol) {
        const auto e = reg.create();
        reg.emplace<ecs::StarInfo>(e, info_of('G', sol));
        ecs::Physical p;
        p.teff_k = teff;
        reg.emplace<ecs::Physical>(e, p);
        return e;
    };
    const auto sol = make(1.0f, true);
    const auto warm = make(5500.0f, false);
    const auto cool = make(3000.0f, false);
    const auto none = make(std::nullopt, false);

    CHECK(systems::update_filters(reg));      // dirty after construction
    CHECK_FALSE(systems::update_filters(reg));  // nothing changed -> skipped
    CHECK(fs.visible == 4);

    fs.filter(ColorScheme::Temperature).lo = 5000.0f;
    fs.filter(ColorScheme::Temperature).hi = 6500.0f;
    fs.filter(ColorScheme::Temperature).hide_missing = true;
    fs.dirty = true;
    CHECK(systems::update_filters(reg));
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(sol));
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(warm));
    CHECK(reg.all_of<scene::FilteredOut>(cool));
    CHECK(reg.all_of<scene::FilteredOut>(none));
    CHECK(fs.visible == 2);
    CHECK(fs.last_pass_ms >= 0.0);

    // The render view pattern: exclude<FilteredOut> skips hidden stars.
    int drawn = 0;
    for (auto e : reg.view<ecs::StarInfo>(entt::exclude<scene::FilteredOut>)) {
        (void)e;
        ++drawn;
    }
    CHECK(drawn == 2);

    // Switching scheme re-runs even without the dirty flag; Age has no filter
    // (and combine is off), so every tag is cleared.
    colors.scheme = ColorScheme::Age;
    CHECK(systems::update_filters(reg));
    CHECK(fs.visible == 4);
    CHECK(reg.storage<scene::FilteredOut>().empty());

    // Back to Temperature: the remembered filter applies again.
    colors.scheme = ColorScheme::Temperature;
    CHECK(systems::update_filters(reg));
    CHECK(fs.visible == 2);
}

TEST_CASE("filters: apply_filter_options (the CLI path)") {
    auto fs = scene::make_filter_settings(fake_stats());
    scene::FilterOptions o;
    o.ranges.push_back({ColorScheme::Temperature, 6500.0f, 5000.0f});   // reversed -> swapped
    o.ranges.push_back({ColorScheme::Age, std::nullopt, 100.0f});        // clamped to max
    o.hide_missing = {ColorScheme::Planets};
    o.hide_missing_active = true;
    o.classes = "g,K";
    o.combine = true;
    scene::DistanceRequest dist;
    dist.lo = 40.0f;
    dist.hi = 10.0f;  // reversed -> swapped, same rule as a scheme range
    o.distance = dist;
    CHECK(scene::apply_filter_options(fs, o, ColorScheme::Diameter).empty());
    CHECK(fs.filter(ColorScheme::Temperature).lo == doctest::Approx(5000.0f));
    CHECK(fs.filter(ColorScheme::Temperature).hi == doctest::Approx(6500.0f));
    CHECK(fs.filter(ColorScheme::Age).hi == doctest::Approx(fs.domain(ColorScheme::Age).max));
    CHECK(scene::is_default(ColorScheme::Age, fs));  // full range after clamping
    CHECK(fs.filter(ColorScheme::Planets).hide_missing);
    CHECK(fs.filter(ColorScheme::Diameter).hide_missing);  // bare --hide-missing = active scheme
    CHECK(fs.filter(ColorScheme::SpectralClass).classes.test('G'));
    CHECK(fs.filter(ColorScheme::SpectralClass).classes.test('K'));
    CHECK_FALSE(fs.filter(ColorScheme::SpectralClass).classes.test('M'));
    CHECK(fs.combine);
    CHECK(fs.distance_lo == doctest::Approx(10.0f));  // reversed 40:10, clamped into 0..200
    CHECK(fs.distance_hi == doctest::Approx(40.0f));
    CHECK(scene::distance_is_trimmed(fs));

    scene::FilterOptions bad;
    bad.classes = "Q";
    CHECK_FALSE(scene::apply_filter_options(fs, bad, ColorScheme::Temperature).empty());
    scene::FilterOptions bad2;
    bad2.ranges.push_back({ColorScheme::SpectralClass, 1.0f, 2.0f});
    CHECK_FALSE(scene::apply_filter_options(fs, bad2, ColorScheme::Temperature).empty());
}

TEST_CASE("command line: filter flags") {
    const char* argv[] = {"starmap", "--filter", "temperature=5000:6500", "--filter", "planets=2:",
                          "--hide-missing", "--hide-missing=age", "--classes", "G,K", "--combine", "--no-panel"};
    const auto r = app::parse_command_line(static_cast<int>(std::size(argv)), argv);
    REQUIRE(r.error.empty());
    const auto& f = r.options.filters;
    REQUIRE(f.ranges.size() == 2);
    CHECK(f.ranges[0].scheme == ColorScheme::Temperature);
    CHECK(*f.ranges[0].lo == doctest::Approx(5000.0f));
    CHECK(*f.ranges[0].hi == doctest::Approx(6500.0f));
    CHECK(f.ranges[1].scheme == ColorScheme::Planets);
    CHECK(*f.ranges[1].lo == doctest::Approx(2.0f));
    CHECK_FALSE(f.ranges[1].hi.has_value());
    CHECK(f.hide_missing_active);
    REQUIRE(f.hide_missing.size() == 1);
    CHECK(f.hide_missing[0] == ColorScheme::Age);
    CHECK(*f.classes == "G,K");
    CHECK(f.combine);
    CHECK_FALSE(r.options.panel);

    for (const char* badv : {"temperature", "temperature=5000", "nope=1:2", "temperature=a:2"}) {
        const char* a2[] = {"starmap", "--filter", badv};
        CHECK_FALSE(app::parse_command_line(3, a2).error.empty());
    }
    const char* a3[] = {"starmap", "--hide-missing=nope"};
    CHECK_FALSE(app::parse_command_line(2, a3).error.empty());

    const char* dist_argv[] = {"starmap", "--filter", "distance=10:40", "--filter", "distance=:15"};
    const auto dist = app::parse_command_line(static_cast<int>(std::size(dist_argv)), dist_argv);
    REQUIRE(dist.error.empty());
    REQUIRE(dist.options.filters.distance.has_value());
    CHECK_FALSE(dist.options.filters.distance->lo.has_value());  // the second flag replaces the first
    CHECK(*dist.options.filters.distance->hi == doctest::Approx(15.0f));
    const char* bad_dist[] = {"starmap", "--filter", "distance=abc:1"};
    CHECK_FALSE(app::parse_command_line(3, bad_dist).error.empty());
}

TEST_CASE("filters: distance range and tagged-only view, ANDed with scheme filters") {
    auto fs = scene::make_filter_settings(fake_stats());
    fs.distance_lo = 10.0f;
    fs.distance_hi = 40.0f;
    ecs::Physical phys;
    const auto applied = scene::applied_filters(fs, ColorScheme::Temperature);  // still empty
    CHECK(applied.empty());
    CHECK(scene::star_visible(fs, applied, info_of('G'), phys, 15.0f, false));
    CHECK(scene::star_visible(fs, applied, info_of('G'), phys, 10.0f, false));  // inclusive end
    CHECK(scene::star_visible(fs, applied, info_of('G'), phys, 40.0f, false));
    CHECK_FALSE(scene::star_visible(fs, applied, info_of('G'), phys, 5.0f, false));
    CHECK_FALSE(scene::star_visible(fs, applied, info_of('G'), phys, 80.0f, false));
    // Sol is the origin and stays, even outside the window.
    CHECK(scene::star_visible(fs, applied, info_of('G', true), phys, 0.0f, false));

    fs.only_tagged = true;
    CHECK_FALSE(scene::star_visible(fs, applied, info_of('G'), phys, 15.0f, false));
    CHECK(scene::star_visible(fs, applied, info_of('G'), phys, 15.0f, true));
    // A tag does not excuse a star from the distance window.
    CHECK_FALSE(scene::star_visible(fs, applied, info_of('G'), phys, 80.0f, true));
    CHECK(scene::star_visible(fs, applied, info_of('G', true), phys, 0.0f, false));

    // Scheme filter still ANDs: hot + in range + tagged passes; cool does not.
    fs.filter(ColorScheme::Temperature).lo = 5000.0f;
    fs.filter(ColorScheme::Temperature).hi = 6500.0f;
    const auto temp = scene::applied_filters(fs, ColorScheme::Temperature);
    phys.teff_k = 5800.0f;
    CHECK(scene::star_visible(fs, temp, info_of('G'), phys, 15.0f, true));
    phys.teff_k = 3000.0f;
    CHECK_FALSE(scene::star_visible(fs, temp, info_of('G'), phys, 15.0f, true));
}

TEST_CASE("filters: FilterSystem applies distance and tags; Sol stays") {
    entt::registry reg;
    auto& fs = reg.ctx().emplace<scene::FilterSettings>(scene::make_filter_settings(fake_stats()));
    auto& colors = reg.ctx().emplace<scene::ColorSettings>();
    colors.scheme = ColorScheme::Temperature;
    fs.total_stars = 4;

    auto make = [&](float dist, bool sol) {
        const auto e = reg.create();
        reg.emplace<ecs::StarInfo>(e, info_of('G', sol));
        reg.emplace<ecs::Physical>(e);
        reg.emplace<ecs::Position>(e, ecs::Position{{}, {}, dist});
        return e;
    };
    const auto sol = make(0.0f, true);
    const auto near = make(15.0f, false);
    const auto mid = make(20.0f, false);
    const auto far = make(80.0f, false);

    fs.distance_lo = 10.0f;
    fs.distance_hi = 40.0f;
    fs.dirty = true;
    CHECK(systems::update_filters(reg));
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(sol));
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(near));
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(mid));
    CHECK(reg.all_of<scene::FilteredOut>(far));
    CHECK(fs.visible == 3);

    fs.only_tagged = true;
    reg.emplace<scene::StarTag>(near, scene::StarTag{std::uint8_t{2}});
    fs.dirty = true;
    CHECK(systems::update_filters(reg));
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(sol));   // reference point
    CHECK_FALSE(reg.all_of<scene::FilteredOut>(near));  // tagged and in range
    CHECK(reg.all_of<scene::FilteredOut>(mid));         // in range, no tag
    CHECK(reg.all_of<scene::FilteredOut>(far));
    CHECK(fs.visible == 2);

    scene::reset_distance(fs);
    fs.only_tagged = false;
    CHECK(systems::update_filters(reg));
    CHECK(fs.visible == 4);
    CHECK(reg.storage<scene::FilteredOut>().empty());
}

TEST_CASE("tags: palette, set/clear, and tag colours while the tagged view is on") {
    CHECK(scene::kTagColorCount == 16);
    for (int i = 0; i < scene::kTagColorCount; ++i) {
        const auto index = static_cast<std::uint8_t>(i);
        const glm::vec3 c = scene::tag_rgb(index);
        CHECK(c.r >= 0.0f);
        CHECK(c.r <= 1.0f);
        CHECK(c.g >= 0.0f);
        CHECK(c.g <= 1.0f);
        CHECK(c.b >= 0.0f);
        CHECK(c.b <= 1.0f);
        CHECK(scene::tag_color_name(index)[0] != '\0');
        CHECK(scene::tag_rgba(index).a == doctest::Approx(1.0f));
        if (i > 0) {
            const glm::vec3 prev = scene::tag_rgb(static_cast<std::uint8_t>(i - 1));
            const bool differ = std::fabs(c.r - prev.r) > 0.05f || std::fabs(c.g - prev.g) > 0.05f ||
                                std::fabs(c.b - prev.b) > 0.05f;
            CHECK(differ);
        }
    }
    CHECK(std::string(scene::tag_color_name(255)) == "?");

    entt::registry reg;
    auto& colors = reg.ctx().emplace<scene::ColorSettings>();
    colors.scheme = ColorScheme::Temperature;
    auto& filters = reg.ctx().emplace<scene::FilterSettings>(scene::make_filter_settings(fake_stats()));
    ecs::StarInfo info = info_of('G');
    ecs::Physical phys;
    phys.teff_k = 5800.0f;
    const auto tagged_star = reg.create();
    const auto plain = reg.create();
    for (entt::entity e : {tagged_star, plain}) {
        reg.emplace<ecs::StarInfo>(e, info);
        reg.emplace<ecs::Physical>(e, phys);
        reg.emplace<ecs::DisplayColor>(e);
    }

    // View off: a tag is stored and the scheme colour is left alone.
    colors.dirty = false;
    filters.only_tagged = false;
    systems::set_star_tag(reg, tagged_star, 0);
    CHECK(reg.get<scene::StarTag>(tagged_star).color == 0);
    CHECK_FALSE(colors.dirty);
    colors.dirty = true;
    CHECK(systems::update_colors(reg));
    const glm::vec4 scheme = systems::color_for(ColorScheme::Temperature, info, phys, colors.ranges);
    CHECK(reg.get<ecs::DisplayColor>(tagged_star).rgba.r == doctest::Approx(scheme.r));
    CHECK(reg.get<ecs::DisplayColor>(plain).rgba.r == doctest::Approx(scheme.r));

    // View on: the tagged star takes the palette, the other keeps the scheme.
    filters.only_tagged = true;
    colors.dirty = true;
    CHECK(systems::update_colors(reg));
    const glm::vec4 red = scene::tag_rgba(0);
    CHECK(reg.get<ecs::DisplayColor>(tagged_star).rgba.r == doctest::Approx(red.r));
    CHECK(reg.get<ecs::DisplayColor>(tagged_star).rgba.g == doctest::Approx(red.g));
    CHECK(reg.get<ecs::DisplayColor>(tagged_star).rgba.b == doctest::Approx(red.b));
    CHECK(reg.get<ecs::DisplayColor>(plain).rgba.r == doctest::Approx(scheme.r));

    // Changing the colour schedules one recolour; the same colour again does not.
    colors.dirty = false;
    filters.dirty = false;
    systems::set_star_tag(reg, tagged_star, 4);
    CHECK(colors.dirty);
    CHECK_FALSE(filters.dirty);  // already tagged: visibility unchanged
    colors.dirty = false;
    systems::set_star_tag(reg, tagged_star, 4);
    CHECK_FALSE(colors.dirty);
    CHECK(reg.get<scene::StarTag>(tagged_star).color == 4);

    // Out of range clamps. Clearing while the view is on dirties both passes.
    systems::set_star_tag(reg, tagged_star, 20);
    CHECK(reg.get<scene::StarTag>(tagged_star).color == 0);
    colors.dirty = false;
    filters.dirty = false;
    systems::clear_star_tag(reg, tagged_star);
    CHECK_FALSE(reg.all_of<scene::StarTag>(tagged_star));
    CHECK(colors.dirty);
    CHECK(filters.dirty);
    colors.dirty = false;
    filters.dirty = false;
    systems::clear_star_tag(reg, tagged_star);  // already clear: no-op
    CHECK_FALSE(colors.dirty);
    systems::set_star_tag(reg, entt::null, 1);
}

TEST_CASE("range slider maths: linear, log and integer scales") {
    const ui::SliderScale lin{0.0f, 100.0f, false, false};
    CHECK(lin.to_t(25.0f) == doctest::Approx(0.25f));
    CHECK(lin.to_t(-5.0f) == 0.0f);   // clamped
    CHECK(lin.to_t(500.0f) == 1.0f);
    CHECK(lin.from_t(0.5f) == doctest::Approx(50.0f));

    const ui::SliderScale logs{0.01f, 100.0f, true, false};  // four decades
    CHECK(logs.to_t(1.0f) == doctest::Approx(0.5f));
    CHECK(logs.to_t(0.1f) == doctest::Approx(0.25f));
    CHECK(logs.from_t(0.75f) == doctest::Approx(10.0f));
    // Ends are exact (no pow/log round-off), so "untrimmed" is detectable.
    CHECK(logs.from_t(0.0f) == 0.01f);
    CHECK(logs.from_t(1.0f) == 100.0f);
    for (float t : {0.1f, 0.33f, 0.9f}) CHECK(logs.to_t(logs.from_t(t)) == doctest::Approx(t).epsilon(1e-4));

    const ui::SliderScale planets{1.0f, 8.0f, true, true};
    CHECK(planets.from_t(0.0f) == 1.0f);
    CHECK(planets.from_t(1.0f) == 8.0f);
    const float mid = planets.from_t(0.5f);  // sqrt(8) = 2.83 -> 3
    CHECK(mid == 3.0f);
    CHECK(std::floor(planets.from_t(0.61f)) == planets.from_t(0.61f));  // always whole
}

TEST_CASE("range slider maths: picking and dragging handles") {
    using ui::Handle;
    CHECK(ui::pick_handle(0.1f, 0.2f, 0.8f) == Handle::Low);
    CHECK(ui::pick_handle(0.9f, 0.2f, 0.8f) == Handle::High);
    CHECK(ui::pick_handle(0.45f, 0.2f, 0.8f) == Handle::Low);
    CHECK(ui::pick_handle(0.55f, 0.2f, 0.8f) == Handle::High);
    // Merged handles: the side of the click decides...
    CHECK(ui::pick_handle(0.4f, 0.5f, 0.5f) == Handle::Low);
    CHECK(ui::pick_handle(0.6f, 0.5f, 0.5f) == Handle::High);
    // ...and exactly on the pair: High, except at the top end where only Low can move.
    CHECK(ui::pick_handle(0.5f, 0.5f, 0.5f) == Handle::High);
    CHECK(ui::pick_handle(1.0f, 1.0f, 1.0f) == Handle::Low);
    CHECK(ui::pick_handle(0.0f, 0.0f, 0.0f) == Handle::High);

    const ui::SliderScale lin{0.0f, 100.0f, false, false};
    float lo = 20.0f, hi = 80.0f;
    CHECK(ui::drag_handle(Handle::Low, 0.3f, lin, lo, hi));
    CHECK(lo == doctest::Approx(30.0f));
    CHECK(ui::drag_handle(Handle::Low, 0.95f, lin, lo, hi));  // tries to pass High: stops at it
    CHECK(lo == doctest::Approx(80.0f));
    CHECK(hi == doctest::Approx(80.0f));
    CHECK(ui::drag_handle(Handle::High, 0.1f, lin, lo, hi) == false);  // can't go below Low
    CHECK(hi == doctest::Approx(80.0f));
    CHECK_FALSE(ui::drag_handle(Handle::None, 0.5f, lin, lo, hi));
}

TEST_CASE("range slider maths: sanitising typed-in values") {
    const ui::SliderScale lin{0.0f, 100.0f, false, false};
    float lo = -10.0f, hi = 150.0f;
    ui::sanitize_range(lin, true, lo, hi);
    CHECK(lo == 0.0f);
    CHECK(hi == 100.0f);
    lo = 70.0f;
    hi = 40.0f;
    ui::sanitize_range(lin, true, lo, hi);  // user typed a min above the max: max follows
    CHECK(lo == 70.0f);
    CHECK(hi == 70.0f);
    lo = 70.0f;
    hi = 40.0f;
    ui::sanitize_range(lin, false, lo, hi);  // user typed a max below the min: min follows
    CHECK(lo == 40.0f);
    CHECK(hi == 40.0f);
    const ui::SliderScale ints{1.0f, 8.0f, true, true};
    lo = 2.4f;
    hi = 6.6f;
    ui::sanitize_range(ints, true, lo, hi);
    CHECK(lo == 2.0f);
    CHECK(hi == 7.0f);
}
