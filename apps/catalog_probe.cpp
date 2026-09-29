// catalog_probe - load stars.db into an EnTT registry and print what we got.
//
//   catalog_probe path/to/stars.db [--max-ly N] [--min-snr N] [--max-ruwe N] [--name "Epsilon Eridani"]...

#include "catalog/CatalogLoader.h"
#include "catalog/CatalogStats.h"
#include "catalog/Indexes.h"
#include "catalog/Sqlite.h"
#include "ecs/Components.h"

#include <entt/entity/registry.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace starmap;

void usage() {
    std::fprintf(stderr,
                 "usage: catalog_probe path/to/stars.db [--max-ly N] [--min-snr N] [--max-ruwe N]"
                 " [--name \"Star Name\"]...\n");
}

std::string opt_str(const std::optional<std::string>& v) { return v ? *v : "-"; }

std::string opt_num(const std::optional<float>& v, const char* fmt = "%.4g") {
    if (!v) return "-";
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, static_cast<double>(*v));
    return buf;
}

std::string opt_id(const std::optional<std::int64_t>& v) {
    return v ? std::to_string(*v) : "-";
}

void print_star(const entt::registry& reg, entt::entity e) {
    // get<T...>() returns references to the components (they must exist).
    const auto& [id, pos, info, ph, phys, ids, q] =
        reg.get<ecs::StarId, ecs::Position, ecs::StarInfo, ecs::Photometry, ecs::Physical,
                ecs::CatalogIds, ecs::Quality>(e);

    std::printf("  star_id %lld  \"%s\"%s\n", static_cast<long long>(id.value),
                info.display_name.c_str(), info.is_sol ? "  (Sol)" : "");
    std::printf("    bayer: %s   spectral: %s (class %c)\n", opt_str(info.bayer_name).c_str(),
                opt_str(info.spectral_type).c_str(), info.spectral_class);
    std::printf("    dist %.3f ly   galactic (%.3f, %.3f, %.3f) pc   equatorial (%.3f, %.3f, %.3f) pc\n",
                static_cast<double>(pos.dist_ly), static_cast<double>(pos.galactic_pc.x),
                static_cast<double>(pos.galactic_pc.y), static_cast<double>(pos.galactic_pc.z),
                static_cast<double>(pos.equatorial_pc.x), static_cast<double>(pos.equatorial_pc.y),
                static_cast<double>(pos.equatorial_pc.z));
    std::printf("    app_mag %s  abs_mag %s  bp_rp %s  B-V %s\n", opt_num(ph.app_mag).c_str(),
                opt_num(ph.abs_mag).c_str(), opt_num(ph.bp_rp).c_str(), opt_num(ph.ci_bv).c_str());
    std::printf("    teff %s K  R %s Rsun  M %s Msun  L %s Lsun  age %s Gyr  [Fe/H] %s  planets %d\n",
                opt_num(phys.teff_k, "%.0f").c_str(), opt_num(phys.radius_sol).c_str(),
                opt_num(phys.mass_sol).c_str(), opt_num(phys.luminosity_sol).c_str(),
                opt_num(phys.age_gyr).c_str(), opt_num(phys.metallicity).c_str(), phys.planet_count);
    std::printf("    ids: gaia %s  HIP %s  HD %s  HYG %s   parallax %s +/- %s mas  ruwe %s\n",
                opt_id(ids.gaia_source_id).c_str(), opt_id(ids.hip).c_str(), opt_id(ids.hd).c_str(),
                opt_id(ids.hyg_id).c_str(), opt_num(q.parallax_mas, "%.3f").c_str(),
                opt_num(q.parallax_error_mas, "%.3f").c_str(), opt_num(q.ruwe, "%.3f").c_str());

    if (const auto* prov = reg.try_get<ecs::Provenance>(e)) {
        std::printf("    sources: dist=%s teff=%s radius=%s mass=%s age=%s met=%s [%s]\n",
                    prov->dist_source.c_str(), prov->teff_source.c_str(), prov->radius_source.c_str(),
                    prov->mass_source.c_str(), prov->age_source.c_str(),
                    prov->metallicity_source.c_str(), prov->sources.c_str());
    }

    // ctx().find<T>() returns a pointer (nullptr if absent) - handy for optional singletons.
    if (const auto* names = reg.ctx().find<catalog::NameIndex>()) {
        std::printf("    names:");
        for (const auto& n : names->names_of(e)) {
            std::printf(" %s%s [%s];", n.is_primary ? "*" : "", n.name.c_str(), n.catalog.c_str());
        }
        std::printf("\n");
    }

    // try_get<T>() returns nullptr when the entity lacks the component.
    if (const auto* planets = reg.try_get<ecs::Planets>(e)) {
        for (const auto& p : planets->list) {
            std::printf("    planet %-22s %s  P=%s d  a=%s AU  e=%s  R=%s Re  M=%s Me  Teq=%s K  (%s)\n",
                        p.name.c_str(), p.disc_year ? std::to_string(*p.disc_year).c_str() : "-",
                        opt_num(p.orbital_period_days).c_str(), opt_num(p.semi_major_axis_au).c_str(),
                        opt_num(p.eccentricity, "%.3f").c_str(), opt_num(p.radius_earth).c_str(),
                        opt_num(p.mass_earth).c_str(), opt_num(p.eq_temp_k, "%.0f").c_str(),
                        p.discovery_method.c_str());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    catalog::LoadOptions opt;
    opt.db_path = argv[1];
    opt.load_planets = true;
    opt.load_provenance = true;
    std::vector<std::string> lookups;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--max-ly") opt.max_dist_ly = std::atof(next());
        else if (a == "--min-snr") opt.min_parallax_snr = std::atof(next());
        else if (a == "--max-ruwe") opt.max_ruwe = std::atof(next());
        else if (a == "--name") lookups.emplace_back(next());
        else if (a == "--lean") { opt.load_planets = false; opt.load_provenance = false; }
        else {
            usage();
            return 2;
        }
    }
    if (lookups.empty()) {
        lookups = {"Sol", "Epsilon Eridani", "Omicron^2 Eridani", "Gamma Pavonis", "Proxima Centauri"};
    }

    try {
        entt::registry registry;
        const catalog::LoadResult r = catalog::load_catalog(registry, opt);

        std::printf("Loaded %s\n", opt.db_path.string().c_str());
        std::printf("  filters: max_ly=%g", opt.max_dist_ly);
        if (opt.min_parallax_snr) std::printf("  min_snr=%g", *opt.min_parallax_snr);
        if (opt.max_ruwe) std::printf("  max_ruwe=%g", *opt.max_ruwe);
        std::printf("   (planets=%s provenance=%s)\n", opt.load_planets ? "on" : "off",
                    opt.load_provenance ? "on" : "off");
        std::printf("  stars %zu   names %zu (+%zu duplicate dropped)   planets %zu   skipped rows %zu"
                    "   in %.1f ms\n\n",
                    r.stars, r.names, r.duplicate_names, r.planets, r.skipped_rows, r.elapsed_ms);

        const auto stats = catalog::CatalogStats::compute(registry);
        std::printf("Coverage (%zu stars; spectral_type %zu = %.1f%%, with planets %zu)\n",
                    stats.total_stars, stats.with_spectral_type,
                    100.0 * static_cast<double>(stats.with_spectral_type) /
                        static_cast<double>(stats.total_stars ? stats.total_stars : 1),
                    stats.with_planets);
        std::printf("  %-15s %8s %7s %12s %12s %12s %12s\n", "field", "count", "cover", "min", "p01",
                    "p99", "max");
        for (const auto& f : stats.fields) {
            std::printf("  %-15s %8zu %6.1f%% %12.4g %12.4g %12.4g %12.4g\n", f.name.c_str(), f.count,
                        100.0 * f.coverage(stats.total_stars), f.min, f.p01, f.p99, f.max);
        }
        std::printf("  (planet_count row: stars with >= 1 planet)\n  spectral classes:");
        for (const auto& [cls, count] : stats.spectral_classes) std::printf(" %c=%zu", cls, count);
        std::printf("\n");

        const auto* names = registry.ctx().find<catalog::NameIndex>();
        for (const auto& q : lookups) {
            std::printf("\nLookup \"%s\":\n", q.c_str());
            const auto hit = names ? names->find(q) : std::nullopt;
            if (!hit) {
                std::printf("  not found (filtered out or unknown name)\n");
                continue;
            }
            print_star(registry, *hit);
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
