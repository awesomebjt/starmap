// =============================================================================
// tests/test_app_core.cpp — unit tests for the GPU-free part of the app
// (starmap_core): colour maps, colour/size systems, camera maths, CLI parsing.
// No window or GPU is needed, so these run in CI and under ctest.
// =============================================================================
#include "app/CommandLine.h"
#include "ecs/Components.h"
#include "render/Colormaps.h"
#include "scene/ColorScheme.h"
#include "scene/OrbitCamera.h"
#include "systems/CameraSystem.h"
#include "systems/ColorSystem.h"
#include "systems/SizeSystem.h"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include <filesystem>
#include <fstream>

using namespace starmap;

TEST_CASE("blackbody colours: red when cool, white near 6600 K, blue when hot") {
    const auto cool = render::blackbody_rgb(2000.0f);
    CHECK(cool.r == doctest::Approx(1.0f));
    CHECK(cool.b < 0.2f);
    const auto white = render::blackbody_rgb(6600.0f);
    CHECK(white.r > 0.95f);
    CHECK(white.g > 0.95f);
    CHECK(white.b > 0.95f);
    const auto hot = render::blackbody_rgb(15000.0f);
    CHECK(hot.b == doctest::Approx(1.0f));
    CHECK(hot.r < hot.b);
    const auto sun = render::blackbody_rgb(5772.0f);  // slightly warm white
    CHECK(sun.r == doctest::Approx(1.0f));
    CHECK(sun.b < sun.g);
    CHECK(sun.b > 0.8f);
    // Out-of-range input is clamped, not NaN.
    const auto extreme = render::blackbody_rgb(1.0e6f);
    CHECK(extreme.b == doctest::Approx(1.0f));
}

TEST_CASE("viridis endpoints and clamping") {
    CHECK(render::viridis(0.0f).r == doctest::Approx(0x44 / 255.0f));
    CHECK(render::viridis(1.0f).g == doctest::Approx(0xe7 / 255.0f));
    CHECK(render::viridis(-3.0f) == render::viridis(0.0f));
    CHECK(render::viridis(0.5f).g == doctest::Approx(0x91 / 255.0f));
    CHECK(render::diverging_blue_orange(-1.0f).b > render::diverging_blue_orange(-1.0f).r);
    CHECK(render::diverging_blue_orange(1.0f).r > render::diverging_blue_orange(1.0f).b);
}

TEST_CASE("colour schemes: names round-trip, missing data is grey") {
    for (int i = 0; i < static_cast<int>(scene::ColorScheme::Count); ++i) {
        const auto s = static_cast<scene::ColorScheme>(i);
        CHECK(scene::scheme_from_string(scene::scheme_info(s).key) == s);
        CHECK_FALSE(scene::scheme_legend(s, {}).empty());
    }
    CHECK_FALSE(scene::scheme_from_string("nope").has_value());

    ecs::StarInfo info;
    ecs::Physical phys;
    const scene::ColorRanges ranges;
    CHECK(systems::color_for(scene::ColorScheme::Temperature, info, phys, ranges) == systems::kNoDataColor);
    CHECK(systems::color_for(scene::ColorScheme::SpectralClass, info, phys, ranges) == systems::kNoDataColor);
    phys.teff_k = 3000.0f;
    info.spectral_class = 'M';
    const auto t = systems::color_for(scene::ColorScheme::Temperature, info, phys, ranges);
    CHECK(t.a == doctest::Approx(1.0f));
    CHECK(t.r > t.b);
    const auto m = systems::color_for(scene::ColorScheme::SpectralClass, info, phys, ranges);
    CHECK(m.r > m.g);
    CHECK(systems::color_for(scene::ColorScheme::Planets, info, phys, ranges).a < 0.5f);  // no planets: faint
    phys.planet_count = 3;
    // Hosts are emphasised: intensity > 1 makes the shader draw them brighter and larger.
    CHECK(systems::color_for(scene::ColorScheme::Planets, info, phys, ranges).a > 1.0f);
}

TEST_CASE("display size: the Sun gets the base radius, extremes are clamped") {
    ecs::Photometry ph;
    ecs::Physical phys;
    ph.abs_mag = 4.83f;
    CHECK(systems::display_radius_pc(ph, phys) == doctest::Approx(0.12f));
    ph.abs_mag = -10.0f;
    CHECK(systems::display_radius_pc(ph, phys) == doctest::Approx(0.48f));
    ph.abs_mag = 25.0f;
    CHECK(systems::display_radius_pc(ph, phys) == doctest::Approx(0.036f));
    ph.abs_mag.reset();
    phys.luminosity_sol = 1.0f;
    CHECK(systems::display_radius_pc(ph, phys) == doctest::Approx(0.12f));
}

TEST_CASE("camera: right-handed Z-up view is not mirrored") {
    // Look straight down from the North Galactic Pole with yaw = -90 deg, so the
    // camera's screen-up is +y. A right-handed frame seen from +z must then show
    // +x on the RIGHT and +y at the TOP. A left-handed matrix would mirror x.
    scene::OrbitCamera cam;
    cam.distance = 10.0f;
    cam.yaw_deg = -90.0f;
    cam.pitch_deg = 89.0f;
    systems::update(cam, 800, 600, false);
    const auto center = systems::world_to_screen(cam, {0, 0, 0});
    const auto px = systems::world_to_screen(cam, {1, 0, 0});
    const auto py = systems::world_to_screen(cam, {0, 1, 0});
    REQUIRE(center);
    REQUIRE(px);
    REQUIRE(py);
    CHECK(center->x == doctest::Approx(400.0f).epsilon(0.01));
    CHECK(center->y == doctest::Approx(300.0f).epsilon(0.01));
    CHECK(px->x > center->x + 10.0f);
    CHECK(py->y < center->y - 10.0f);
    CHECK_FALSE(systems::world_to_screen(cam, cam.eye + (cam.eye - cam.target)).has_value());  // behind the eye
}

TEST_CASE("camera: projection honours homogeneous depth") {
    scene::OrbitCamera cam;
    auto near_ndc_z = [&](bool homogeneous) {
        systems::update(cam, 800, 600, homogeneous);
        const glm::vec3 fwd = glm::normalize(cam.target - cam.eye);
        const glm::vec4 clip = cam.view_proj * glm::vec4(cam.eye + fwd * cam.near_pc, 1.0f);
        return clip.z / clip.w;
    };
    CHECK(near_ndc_z(false) == doctest::Approx(0.0f).epsilon(0.001));   // D3D/Metal/Vulkan: [0,1]
    CHECK(near_ndc_z(true) == doctest::Approx(-1.0f).epsilon(0.001));   // OpenGL: [-1,1]
}

TEST_CASE("camera controls: zoom is exponential and reversible, pitch is clamped, pan is in-plane") {
    scene::OrbitCamera cam;
    const float d0 = cam.distance;
    systems::zoom(cam, 3.0f);
    CHECK(cam.distance < d0);
    systems::zoom(cam, -3.0f);
    CHECK(cam.distance == doctest::Approx(d0));
    systems::zoom(cam, -1000.0f);
    CHECK(cam.distance == doctest::Approx(scene::kMaxDistancePc));

    systems::orbit(cam, 0.0f, 10000.0f);
    CHECK(cam.pitch_deg == doctest::Approx(scene::kMaxPitchDeg));

    cam = scene::OrbitCamera{};
    systems::update(cam, 800, 600, false);
    const glm::vec3 fwd = glm::normalize(cam.target - cam.eye);
    systems::pan(cam, 50.0f, -20.0f);
    CHECK(glm::length(cam.target) > 0.0f);
    CHECK(glm::dot(cam.target, fwd) == doctest::Approx(0.0f).epsilon(1e-4));  // moved perpendicular to view

    systems::reset(cam);
    CHECK(cam.target == glm::vec3(0.0f));
    CHECK(cam.distance == doctest::Approx(scene::OrbitCamera{}.distance));
}

TEST_CASE("command line parsing") {
    const char* argv[] = {"starmap", "my.db", "--max-ly", "50", "--min-snr", "20", "--scheme", "spectral",
                          "--screenshot", "out.png", "--frames", "7", "--camera", "20,10,-5", "--size", "640x480",
                          "--renderer", "opengl", "--focus", "Sirius"};
    const auto r = app::parse_command_line(static_cast<int>(std::size(argv)), argv);
    REQUIRE(r.error.empty());
    const auto& o = r.options;
    CHECK(o.db_path == std::filesystem::path("my.db"));
    CHECK(o.max_ly == doctest::Approx(50.0));
    CHECK(o.min_snr == doctest::Approx(20.0));
    CHECK(o.scheme == scene::ColorScheme::SpectralClass);
    CHECK(o.screenshot == std::filesystem::path("out.png"));
    CHECK(o.frames == 7);
    REQUIRE(o.camera);
    CHECK(o.camera->distance_pc == doctest::Approx(20.0f));
    CHECK(o.camera->pitch_deg == doctest::Approx(-5.0f));
    CHECK(o.width == 640);
    CHECK(o.renderer == "opengl");
    CHECK(o.focus == std::string("Sirius"));

    const char* bad1[] = {"starmap", "--max-ly"};
    CHECK_FALSE(app::parse_command_line(2, bad1).error.empty());
    const char* bad2[] = {"starmap", "--scheme", "rainbow"};
    CHECK_FALSE(app::parse_command_line(3, bad2).error.empty());
    const char* bad3[] = {"starmap", "--max-ly", "12abc"};
    CHECK_FALSE(app::parse_command_line(3, bad3).error.empty());
    const char* bad4[] = {"starmap", "--wat"};
    CHECK_FALSE(app::parse_command_line(2, bad4).error.empty());
}

TEST_CASE("default database search order") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "starmap_db_search_test";
    fs::remove_all(root);
    fs::create_directories(root / "repo" / "starmap" / "build");
    fs::create_directories(root / "repo" / "starcatalog");
    std::ofstream(root / "repo" / "starcatalog" / "stars.db") << "x";
    std::vector<fs::path> tried;
    // exe in repo/starmap/build, cwd = repo/starmap  -> ../starcatalog/stars.db relative to cwd
    auto found = app::find_default_db(root / "repo" / "starmap" / "build", root / "repo" / "starmap", &tried);
    REQUIRE(found);
    CHECK(fs::equivalent(*found, root / "repo" / "starcatalog" / "stars.db"));
    // A stars.db next to the executable wins.
    std::ofstream(root / "repo" / "starmap" / "build" / "stars.db") << "x";
    found = app::find_default_db(root / "repo" / "starmap" / "build", root, nullptr);
    REQUIRE(found);
    CHECK(fs::equivalent(*found, root / "repo" / "starmap" / "build" / "stars.db"));
    // Nothing anywhere -> nullopt and a list of candidates for the error message.
    tried.clear();
    CHECK_FALSE(app::find_default_db(root / "nowhere", root / "nowhere", &tried).has_value());
    CHECK(tried.size() == 5);
    fs::remove_all(root);
}
