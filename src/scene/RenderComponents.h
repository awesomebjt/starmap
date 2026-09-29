#pragma once
// =============================================================================
// scene/RenderComponents.h — components that exist only for rendering.
//
// The catalog loader (src/catalog) knows nothing about the app; it emplaces the
// data components from ecs/Components.h (plus a default DisplayColor). The app
// adds these extra components itself after loading — a common ECS pattern:
// different subsystems attach their own components to the same entities
// without the loader having to know about them.
// =============================================================================

namespace starmap::scene {

// Physical size used for the billboard, in parsecs (see SizeSystem.cpp for the
// formula). The vertex shader turns it into pixels and clamps it.
struct DisplaySize {
    float radius_pc = 0.1f;
};

// Tag component (no data): the star is drawn with a highlight ring.
// The app puts it on Sol; a future "select star" feature can reuse it.
// EnTT stores no per-entity payload for empty types — only membership.
struct Marked {};

}  // namespace starmap::scene
