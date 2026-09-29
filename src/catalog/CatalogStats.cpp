// =============================================================================
// CatalogStats.cpp — one pass over the registry, then per-field reductions.
// See CatalogStats.h for why percentiles are used and how the app uses them.
// =============================================================================
#include "catalog/CatalogStats.h"

#include "ecs/Components.h"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace starmap::catalog {
namespace {

using namespace starmap::ecs;

// Collects values of one field, then reduces them to FieldStats.
struct Accumulator {
    std::string name;
    std::vector<float> values;

    // Skips "unknown" (nullopt) and non-finite values: a NaN would break
    // the ordering that nth_element/minmax_element rely on (NaN compares
    // false with everything, which violates strict weak ordering, and the
    // result is undefined behaviour).
    void add(const std::optional<float>& v) {
        if (v && std::isfinite(*v)) values.push_back(*v);
    }
    FieldStats finish() {
        FieldStats s;
        s.name = std::move(name);
        s.count = values.size();
        if (values.empty()) return s;
        // Nearest-rank percentile via std::nth_element. It partially sorts
        // so that element k is the one a full sort would put there, in O(n)
        // on average instead of O(n log n) for std::sort. Calling it twice
        // (p01 then p99) on the same vector is fine: each call only needs
        // the vector to be a permutation of the values, which it always is.
        // "+ 0.5" rounds to the nearest index.
        auto pct = [this](double q) {
            const auto k = static_cast<std::size_t>(q * static_cast<double>(values.size() - 1) + 0.5);
            std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
            return static_cast<double>(values[k]);
        };
        // Structured binding of the returned pair of iterators; one pass finds both ends.
        const auto [mn, mx] = std::minmax_element(values.begin(), values.end());
        s.min = *mn;
        s.max = *mx;
        s.p01 = pct(0.01);
        s.p99 = pct(0.99);
        return s;
    }
};

}  // namespace

CatalogStats CatalogStats::compute(const entt::registry& registry) {
    CatalogStats out;

    // Plain (unscoped) enum used as array indices into `acc`: acc[Teff] reads
    // better than acc[5]. FieldCount must stay last. The order must match the
    // initializer list below.
    enum Field { Dist, AppMag, AbsMag, BpRp, CiBv, Teff, Radius, Mass, Lum, Age, Met,
                 Planets_, Plx, PlxSnr, Ruwe, FieldCount };
    std::vector<Accumulator> acc{
        {"dist_ly", {}}, {"app_mag", {}}, {"abs_mag", {}}, {"bp_rp", {}}, {"ci_bv", {}},
        {"teff_k", {}}, {"radius_sol", {}}, {"mass_sol", {}}, {"luminosity_sol", {}},
        {"age_gyr", {}}, {"metallicity", {}}, {"planet_count", {}},
        {"parallax_mas", {}}, {"parallax_snr", {}}, {"ruwe", {}}};

    // EnTT idiom: a view iterates every entity that has ALL the listed
    // components. On a const registry the component types must be const too.
    // each() hands us references straight into the packed storages.
    const auto view = registry.view<const Position, const StarInfo, const Photometry,
                                    const Physical, const Quality>();
    // size_hint(): a multi-component view can't know its exact size without
    // iterating (it walks the smallest storage and tests the others), so it
    // reports an upper bound, which is fine for reserve().
    for (auto& a : acc) a.values.reserve(view.size_hint());

    // each(lambda): EnTT passes the components as arguments, in the order they
    // were listed in view<...>. There is also an overload whose first
    // parameter is the entity (e.g. [](entt::entity e, const Position& p)).
    view.each([&](const Position& pos, const StarInfo& info, const Photometry& ph,
                  const Physical& phys, const Quality& q) {
        ++out.total_stars;
        ++out.spectral_classes[info.spectral_class];
        if (info.spectral_type) ++out.with_spectral_type;
        if (phys.planet_count > 0) ++out.with_planets;

        acc[Dist].add(pos.dist_ly);
        acc[AppMag].add(ph.app_mag);
        acc[AbsMag].add(ph.abs_mag);
        acc[BpRp].add(ph.bp_rp);
        acc[CiBv].add(ph.ci_bv);
        acc[Teff].add(phys.teff_k);
        acc[Radius].add(phys.radius_sol);
        acc[Mass].add(phys.mass_sol);
        acc[Lum].add(phys.luminosity_sol);
        acc[Age].add(phys.age_gyr);
        acc[Met].add(phys.metallicity);
        // Only hosts: otherwise the ~99% zeros would make p01 = p99 = 0.
        if (phys.planet_count > 0) acc[Planets_].add(static_cast<float>(phys.planet_count));
        acc[Plx].add(q.parallax_mas);
        acc[PlxSnr].add(q.parallax_snr());
        acc[Ruwe].add(q.ruwe);
    });

    out.fields.reserve(FieldCount);
    for (auto& a : acc) out.fields.push_back(a.finish());
    return out;
}

const FieldStats* CatalogStats::field(std::string_view name) const {
    for (const auto& f : fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

}  // namespace starmap::catalog
