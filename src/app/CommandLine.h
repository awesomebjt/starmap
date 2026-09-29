#pragma once
// =============================================================================
// app/CommandLine.h — command-line options of the `starmap` executable.
//
//   starmap [path/to/stars.db] [--min-snr N] [--max-ly N] [--max-ruwe N]
//           [--scheme temperature|spectral|diameter|age|metallicity|planets]
//           [--renderer auto|opengl|vulkan|d3d11|d3d12|metal]
//           [--size WxH] [--no-vsync] [--exposure X] [--no-hud] [--no-guides]
//           [--focus "Star Name"] [--camera DIST,YAW,PITCH]
//           [--filter SCHEME=LO:HI]... [--filter distance=LO:HI]
//           [--hide-missing[=SCHEME]]...
//           [--classes G,K,...] [--combine] [--no-panel]
//           [--screenshot out.png [--frames N]]
//
// The filter flags exist mainly so screenshots/tests can reproduce a filter
// state without clicking; they set the same FilterSettings the panel edits.
//
// Kept free of SDL/bgfx so it can be unit-tested; the renderer name is stored
// as a string and translated by render::Graphics.
// =============================================================================

#include "scene/ColorScheme.h"
#include "scene/Filters.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace starmap::app {

struct CameraPose {
    float distance_pc = 150.0f;
    float yaw_deg = -50.0f;
    float pitch_deg = 28.0f;
};

struct AppOptions {
    std::optional<std::filesystem::path> db_path;  // nullopt -> find_default_db()
    double max_ly = 200.0;
    std::optional<double> min_snr;
    std::optional<double> max_ruwe;
    scene::ColorScheme scheme = scene::ColorScheme::Temperature;
    std::string renderer = "auto";
    int width = 1280, height = 720;                // window size in points (logical pixels)
    bool vsync = true;
    bool hud = true;
    bool guides = true;
    float exposure = 1.0f;
    std::optional<std::string> focus;              // star name to orbit around
    std::optional<CameraPose> camera;
    std::optional<std::filesystem::path> screenshot;
    int frames = 5;                                // frames to render before the screenshot
    scene::FilterOptions filters;                  // --filter / --hide-missing / --classes / --combine
    bool panel = true;                             // --no-panel hides the filter panel at start
    // Milestone 3 (selection), mainly for reproducible screenshots:
    std::optional<std::string> select;             // --select NAME: select via the name search, fly instantly
    std::optional<std::array<float, 2>> pick;      // --pick X,Y: simulate a click at window POINTS (x, y)
    std::optional<std::string> search;             // --search TEXT: pre-fill the search box (shows suggestions)
    bool help = false;
};

struct ParseResult {
    AppOptions options;
    std::string error;  // empty on success
};

[[nodiscard]] ParseResult parse_command_line(int argc, const char* const* argv);
[[nodiscard]] std::string usage_text();

// Default database search: next to the executable, in the working directory,
// then ../starcatalog/stars.db relative to the working directory and to the
// executable (and two levels up, which covers <repo>/starmap/build-*/starmap).
// Returns the first existing file; `tried` receives every candidate (for the
// error message).
[[nodiscard]] std::optional<std::filesystem::path> find_default_db(
    const std::filesystem::path& exe_dir, const std::filesystem::path& cwd,
    std::vector<std::filesystem::path>* tried = nullptr);

}  // namespace starmap::app
