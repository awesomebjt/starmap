#pragma once
// =============================================================================
// ui/UiUtil.h — small ImGui helpers shared by the side-panel sections
// =============================================================================
//
// Milestone 2 kept these in an anonymous namespace inside FilterPanel.cpp.
// Milestone 3 adds the search box and the star details view, which need the
// same helpers, so they moved here instead of being copy-pasted (copies drift:
// one gets a bug fix, the other doesn't).
//
// All of these must be called between ImGui::NewFrame() and ImGui::Render(),
// like any other ImGui call; fix_units() also needs the font atlas to exist.
// =============================================================================

#include <glm/vec3.hpp>
#include <imgui.h>

#include <cstddef>
#include <optional>
#include <string>

namespace starmap::ui {

// Linear RGB 0..1 -> ImGui's packed 32-bit colour (0xAABBGGRR).
[[nodiscard]] ImU32 to_u32(const glm::vec3& c, float alpha = 1.0f);

// "91998" -> "91,998": counts are easier to read with separators.
[[nodiscard]] std::string with_commas(std::size_t n);

// Replaces symbols the loaded font cannot draw (U+2609 Sun, U+2295 Earth)
// with ASCII ("Rsun", "Mearth"), so ImGui never shows a '?' box.
[[nodiscard]] std::string fix_units(std::string text);

// A small "(?)" after the previous widget that shows `text` on hover.
void help_marker(const char* text);

// The placeholder for a missing value: a dim "n/a". One function so every
// table in the app says it the same way.
void text_na();

// `v` with `decimals` digits after the point and an optional unit suffix
// (" K", " Gyr") if it has a value, else text_na().
void text_opt(const std::optional<double>& v, int decimals, const char* suffix = "");

// Label/value two-column row inside an ImGui table (BeginTable with 2 columns).
// Starts a new row, prints `label` dimmed in column 0 and leaves the cursor in
// column 1 for the caller to draw the value with any widget.
void row_label(const char* label);

}  // namespace starmap::ui
