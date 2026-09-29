#pragma once
// =============================================================================
// scene/Tags.h — a user-assigned colour on a star, and the menu that sets it
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Right-clicking a star asks to mark it with one of 16 colours. The mark is
//   a component (present = tagged), not a field inside StarInfo: the catalog
//   loader never invents tags, and "which stars did I mark?" is exactly
//   registry.view<StarTag>(). The colour is an index into a fixed palette so
//   the menu, the filter and the renderer cannot disagree about what "red" is.
//
//   InputHandler only records "the right button clicked at this pixel"
//   (scene::PointerInput::right_click_px). InteractionSystem picks the star
//   and fills TagMenuRequest. The ImGui popup (ui/TagMenu.cpp) is the only
//   code that draws the 16 swatches; it calls systems::set_star_tag, which
//   writes the component and the dirty flags. Same split as selection: input
//   reports, a system decides, the UI edits plain state.
//
//   The tagged-only view is a filter (FilterSettings::only_tagged), not a
//   colour scheme. While it is on, FilterSystem hides stars with no StarTag
//   and ColorSystem copies the palette into DisplayColor. Turning it off
//   leaves the tags in place and paints the scheme colours again.
// =============================================================================

#include <entt/entity/entity.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>

namespace starmap::scene {

// How many swatches the menu offers. The index stored on a star is always
// in 0 .. kTagColorCount-1; set_star_tag clamps anything else.
inline constexpr int kTagColorCount = 16;

// Present only on a star the user has tagged. Absence means "not tagged",
// which is what the tagged-only filter tests — a bool on every star would
// make "the tagged ones" a scan instead of a view.
struct StarTag {
    std::uint8_t color = 0;
};

// Palette RGB in 0..1, the same space as DisplayColor. Out-of-range indexes
// return the first entry so a corrupt component still draws something finite.
[[nodiscard]] glm::vec3 tag_rgb(std::uint8_t color);
// rgb with intensity 1. Intensity above 1 would push the additive shader
// toward white and wash out the colour the user actually picked.
[[nodiscard]] glm::vec4 tag_rgba(std::uint8_t color);
// Short name for tooltips ("Red", "Cyan"). Out of range returns "?".
[[nodiscard]] const char* tag_color_name(std::uint8_t color);

// One-shot request from InteractionSystem to the tag popup. `open` is
// consumed by the UI on the frame it draws; `visible` is written by the UI
// so the next frame's Esc key can dismiss the menu without also clearing
// the selection (events are handled before ImGui::NewFrame, so the handler
// can only see last frame's popup state — the same one-frame lag as
// WantCaptureMouse).
struct TagMenuRequest {
    bool open = false;
    bool visible = false;
    entt::entity star = entt::null;
    glm::vec2 anchor_px{0.0f};  // back-buffer pixels, same space as PointerInput
};

}  // namespace starmap::scene
