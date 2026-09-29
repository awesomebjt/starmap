# =============================================================================
# cmake/Warnings.cmake — high warning levels for *our* targets only
# =============================================================================
# Third-party headers are marked as SYSTEM in Dependencies.cmake /
# AppDependencies.cmake, so they don't drown our output. (Third-party .c/.cpp
# files are compiled by their own targets, which never call this function.)
#
# Why so strict? Graphics code is full of int <-> float <-> uint16_t
# conversions (pixel sizes, vertex counts, bgfx's uint16_t handles).
# -Wconversion and -Wsign-conversion make every narrowing explicit
# (static_cast), which catches real bugs such as a negative width becoming
# 4 billion. The project builds with zero warnings, and
# -DSTARMAP_WARNINGS_AS_ERRORS=ON keeps it that way.
#
# Usage: starmap_set_warnings(<target>) after add_library/add_executable.
# =============================================================================
function(starmap_set_warnings target)
    if(MSVC)
        # /W4: high warning level. /permissive-: standards-conforming mode
        # (two-phase lookup etc.). /utf-8: source files and string literals are
        # UTF-8. Without it MSVC assumes the system code page, which garbles
        # names like 'Omicron² Eridani' and the degree signs in the HUD.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        # getenv/snprintf are fine here; don't nag about the *_s variants (C4996).
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(STARMAP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # -Wshadow: a local hiding a member or outer variable (a classic bug in
        #           long render functions).
        # -Wold-style-cast: C casts hide what kind of conversion happens.
        # -Wnon-virtual-dtor / -Woverloaded-virtual: interface mistakes, relevant
        #           because we implement bgfx::CallbackI (a virtual interface).
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
        # (-Wnull-dereference deliberately omitted: GCC 14 reports false positives from
        #  inlined EnTT const-registry code, which SYSTEM includes cannot silence.)
        if(STARMAP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
