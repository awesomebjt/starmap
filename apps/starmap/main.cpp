// =============================================================================
// apps/starmap/main.cpp — entry point of the interactive star map.
//
// Deliberately tiny: parse the command line, build the App, run it, and turn
// exceptions into a readable message. All the interesting code is in src/.
// =============================================================================

// SDL_main.h must be included in exactly ONE source file: the one defining
// main(). On Windows it provides WinMain() for GUI-subsystem builds; on iOS
// and Android it hooks into the platform's app lifecycle; on desktop Linux and
// macOS it does next to nothing. Including it everywhere keeps the program
// portable without #ifdefs.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/App.h"
#include "app/CommandLine.h"

#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
    const starmap::app::ParseResult parsed = starmap::app::parse_command_line(argc, argv);
    if (!parsed.error.empty()) {
        std::fprintf(stderr, "starmap: %s\n\n%s", parsed.error.c_str(), starmap::app::usage_text().c_str());
        return 2;
    }
    if (parsed.options.help) {
        std::printf("%s", starmap::app::usage_text().c_str());
        return 0;
    }
    try {
        starmap::app::App app(parsed.options);
        return app.run();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "starmap: %s\n", ex.what());
        // Users who double-click the executable never see stderr: show a dialog
        // too (SDL allows this even without an initialised video subsystem).
        if (!parsed.options.screenshot) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "starmap", ex.what(), nullptr);
        }
        return 1;
    }
}
