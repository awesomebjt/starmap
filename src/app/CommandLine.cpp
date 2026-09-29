// =============================================================================
// app/CommandLine.cpp — hand-rolled argument parsing (no dependency needed
// for a dozen flags; a library like CLI11 would be the choice for many more).
// =============================================================================
#include "app/CommandLine.h"

#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>

namespace starmap::app {
namespace {

bool parse_double(std::string_view s, double& out) {
    // std::strtod rather than std::from_chars(double): floating-point from_chars
    // only arrived in Apple's libc++ recently, and this must build on macOS.
    // (strtod honours the C locale; SDL/our code never call setlocale, so the
    // decimal separator stays '.'.)
    const std::string tmp(s);  // strtod needs a NUL-terminated string
    char* end = nullptr;
    out = std::strtod(tmp.c_str(), &end);
    return !tmp.empty() && end == tmp.c_str() + tmp.size();
}

bool parse_int(std::string_view s, int& out) {
    const auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc() && ptr == end;
}

}  // namespace

std::string usage_text() {
    return
        "usage: starmap [path/to/stars.db] [options]\n"
        "  --max-ly N          only stars within N light years (default 200)\n"
        "  --min-snr N         drop Gaia stars with parallax/error < N\n"
        "  --max-ruwe N        drop Gaia stars with RUWE > N (careful: removes some bright stars)\n"
        "  --scheme NAME       temperature | spectral | diameter | age | metallicity | planets\n"
        "  --renderer NAME     auto | opengl | vulkan | d3d11 | d3d12 | metal\n"
        "  --size WxH          window size in points (default 1280x720)\n"
        "  --no-vsync          render as fast as possible (for timing)\n"
        "  --exposure X        star brightness multiplier (default 1)\n"
        "  --no-hud            hide the text overlay\n"
        "  --no-guides         hide distance rings\n"
        "  --focus NAME        orbit around a named star (e.g. \"Sol\", \"Sirius\")\n"
        "  --camera D,YAW,PITCH  initial camera: distance (pc), yaw and pitch (degrees)\n"
        "  --screenshot FILE   render --frames N frames (default 5), save a PNG, exit\n"
        "filters (same state as the panel; repeatable):\n"
        "  --filter S=LO:HI    keep stars whose value for scheme S is in [LO, HI]; either side may be\n"
        "                      empty (temperature=:4000). S: temperature|diameter|age|metallicity|planets\n"
        "                      or distance (light years from Sol; always applied, e.g. distance=10:40)\n"
        "  --hide-missing[=S]  hide stars with no data for scheme S (default: the --scheme one)\n"
        "  --classes G,K       spectral classes to keep (O B A F G K M L T Y D C S W)\n"
        "  --combine           apply every scheme's filter together (AND), not just the active one\n"
        "  --no-panel          start with the filter panel hidden\n"
        "selection:\n"
        "  --select NAME       select a star by name/id (as the search box would) and centre on it\n"
        "                      instantly, e.g. --select \"eps Eri\" or --select \"HIP 16537\"\n"
        "  --pick X,Y          simulate a left click at window point X,Y (after the first frames)\n"
        "  --search TEXT       pre-fill the search box so its suggestion list is visible\n"
        "controls: click select | right-click a star to tag it (16 colours) | left-drag rotate\n"
        "          wheel zoom | right-drag or middle-drag pan | Ctrl+F or / search\n"
        "          F fly to selection | H or Home: Sol | R reset view\n"
        "          Esc closes the tag menu, else deselects (quits if nothing selected) | Q quit\n"
        "          double-click empty space: Sol\n"
        "          1-6 colour scheme | P/Tab side panel | G guides | +/- brightness | F1 help | F12 ImGui demo\n";
}

ParseResult parse_command_line(int argc, const char* const* argv) {
    ParseResult r;
    AppOptions& o = r.options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        // Fetch the value following a flag, or record an error.
        auto value = [&](std::string_view& out) {
            if (i + 1 >= argc) {
                r.error = "missing value after " + std::string(a);
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string_view v;
        double d = 0.0;
        if (a == "-h" || a == "--help") {
            o.help = true;
        } else if (a == "--max-ly" || a == "--min-snr" || a == "--max-ruwe" || a == "--exposure") {
            if (!value(v)) return r;
            if (!parse_double(v, d)) { r.error = "not a number: " + std::string(v); return r; }
            if (a == "--max-ly") o.max_ly = d;
            else if (a == "--min-snr") o.min_snr = d;
            else if (a == "--max-ruwe") o.max_ruwe = d;
            else o.exposure = static_cast<float>(d);
        } else if (a == "--scheme") {
            if (!value(v)) return r;
            auto s = scene::scheme_from_string(v);
            if (!s) { r.error = "unknown scheme: " + std::string(v); return r; }
            o.scheme = *s;
        } else if (a == "--renderer") {
            if (!value(v)) return r;
            o.renderer = std::string(v);
        } else if (a == "--size") {
            if (!value(v)) return r;
            const auto x = v.find('x');
            if (x == std::string_view::npos || !parse_int(v.substr(0, x), o.width) ||
                !parse_int(v.substr(x + 1), o.height) || o.width < 64 || o.height < 64) {
                r.error = "bad --size (expected WxH, e.g. 1280x720): " + std::string(v);
                return r;
            }
        } else if (a == "--no-vsync") {
            o.vsync = false;
        } else if (a == "--no-hud") {
            o.hud = false;
        } else if (a == "--no-guides") {
            o.guides = false;
        } else if (a == "--focus") {
            if (!value(v)) return r;
            o.focus = std::string(v);
        } else if (a == "--camera") {
            if (!value(v)) return r;
            CameraPose p;
            const auto c1 = v.find(',');
            const auto c2 = c1 == std::string_view::npos ? c1 : v.find(',', c1 + 1);
            double dist = 0, yaw = 0, pitch = 0;
            if (c2 == std::string_view::npos || !parse_double(v.substr(0, c1), dist) ||
                !parse_double(v.substr(c1 + 1, c2 - c1 - 1), yaw) || !parse_double(v.substr(c2 + 1), pitch) ||
                dist <= 0) {
                r.error = "bad --camera (expected DIST,YAW,PITCH e.g. 150,-50,28): " + std::string(v);
                return r;
            }
            p.distance_pc = static_cast<float>(dist);
            p.yaw_deg = static_cast<float>(yaw);
            p.pitch_deg = static_cast<float>(pitch);
            o.camera = p;
        } else if (a == "--select") {
            if (!value(v)) return r;
            o.select = std::string(v);
        } else if (a == "--search") {
            if (!value(v)) return r;
            o.search = std::string(v);
        } else if (a == "--pick") {
            if (!value(v)) return r;
            const auto c = v.find(',');
            double x = 0, y = 0;
            if (c == std::string_view::npos || !parse_double(v.substr(0, c), x) || !parse_double(v.substr(c + 1), y) ||
                x < 0 || y < 0) {
                r.error = "bad --pick (expected X,Y in window points, e.g. 400,300): " + std::string(v);
                return r;
            }
            o.pick = std::array<float, 2>{static_cast<float>(x), static_cast<float>(y)};
        } else if (a == "--screenshot") {
            if (!value(v)) return r;
            o.screenshot = std::filesystem::path(v);
        } else if (a == "--frames") {
            if (!value(v)) return r;
            if (!parse_int(v, o.frames) || o.frames < 1) { r.error = "bad --frames: " + std::string(v); return r; }
        } else if (a == "--filter") {
            // SCHEME=LO:HI, e.g. temperature=5000:6500 or planets=2: (LO only).
            if (!value(v)) return r;
            const auto eq = v.find('=');
            const auto colon = eq == std::string_view::npos ? eq : v.find(':', eq + 1);
            const auto bad = [&] {
                r.error = "bad --filter (expected SCHEME=LO:HI, e.g. temperature=5000:6500 or distance=10:40): " +
                          std::string(v);
                return r;
            };
            if (colon == std::string_view::npos) return bad();
            const std::string_view key = v.substr(0, eq);
            const std::string_view lo_sv = v.substr(eq + 1, colon - eq - 1);
            const std::string_view hi_sv = v.substr(colon + 1);
            // "distance" is not a colour scheme. It fills FilterOptions::distance,
            // which always applies, instead of appending a FilterRequest.
            if (key == "distance") {
                scene::DistanceRequest req;
                if (!lo_sv.empty()) {
                    if (!parse_double(lo_sv, d)) return bad();
                    req.lo = static_cast<float>(d);
                }
                if (!hi_sv.empty()) {
                    if (!parse_double(hi_sv, d)) return bad();
                    req.hi = static_cast<float>(d);
                }
                o.filters.distance = req;
            } else {
                const auto scheme = scene::scheme_from_string(key);
                if (!scheme) return bad();
                scene::FilterRequest req;
                req.scheme = *scheme;
                if (!lo_sv.empty()) {
                    if (!parse_double(lo_sv, d)) return bad();
                    req.lo = static_cast<float>(d);
                }
                if (!hi_sv.empty()) {
                    if (!parse_double(hi_sv, d)) return bad();
                    req.hi = static_cast<float>(d);
                }
                o.filters.ranges.push_back(req);
            }
        } else if (a == "--hide-missing") {
            o.filters.hide_missing_active = true;  // resolved against --scheme later (order-independent)
        } else if (a.substr(0, 15) == "--hide-missing=") {
            const auto scheme = scene::scheme_from_string(a.substr(15));
            if (!scheme) { r.error = "unknown scheme in " + std::string(a); return r; }
            o.filters.hide_missing.push_back(*scheme);
        } else if (a == "--classes") {
            if (!value(v)) return r;
            o.filters.classes = std::string(v);  // validated against the catalog in apply_filter_options
        } else if (a == "--combine") {
            o.filters.combine = true;
        } else if (a == "--no-panel") {
            o.panel = false;
        } else if (!a.empty() && a.front() == '-') {
            r.error = "unknown option: " + std::string(a);
            return r;
        } else if (!o.db_path) {
            o.db_path = std::filesystem::path(a);
        } else {
            r.error = "unexpected argument: " + std::string(a);
            return r;
        }
    }
    return r;
}

std::optional<std::filesystem::path> find_default_db(const std::filesystem::path& exe_dir,
                                                     const std::filesystem::path& cwd,
                                                     std::vector<std::filesystem::path>* tried) {
    const std::filesystem::path candidates[] = {
        exe_dir / "stars.db",
        cwd / "stars.db",
        cwd / ".." / "starcatalog" / "stars.db",
        exe_dir / ".." / "starcatalog" / "stars.db",
        exe_dir / ".." / ".." / "starcatalog" / "stars.db",
    };
    for (const auto& c : candidates) {
        const auto p = c.lexically_normal();
        if (tried) tried->push_back(p);
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) return p;
    }
    return std::nullopt;
}

}  // namespace starmap::app
