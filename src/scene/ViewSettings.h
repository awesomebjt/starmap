#pragma once
// =============================================================================
// scene/ViewSettings.h — small UI/render toggles stored in registry.ctx().
//
// Anything that is "global state of the app, but not of one entity" goes into
// the registry context instead of a global variable: systems receive the
// registry anyway, tests can create a registry with different settings, and
// there is no hidden coupling between translation units.
// =============================================================================

namespace starmap::scene {

struct ViewSettings {
    bool show_guides = true;    // distance rings + galactic-centre arrow (G key)
    bool show_help = true;      // controls text in the HUD (F1 key; H was repurposed as "home" in M3)
    bool show_hud = true;       // the whole debug-text overlay
    float exposure = 1.0f;      // global star brightness (+ / - keys)
    bool show_panel = true;     // the ImGui filter panel on the right (P / Tab keys)
    bool show_imgui_demo = false;  // Dear ImGui's built-in demo window (F12): the widget reference
    bool focus_search = false;  // one-shot: Ctrl+F or '/' asks the panel to focus its search box
    bool quit_requested = false;
};

}  // namespace starmap::scene
