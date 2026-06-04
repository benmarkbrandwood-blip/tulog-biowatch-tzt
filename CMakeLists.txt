cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tulog_biowatch_tzt)

# ─── ThorVG warning-as-error workarounds ─────────────────────────────────────
# LVGL 9.5 bundles ThorVG (under managed_components/lvgl__lvgl/src/libs/thorvg)
# whose source triggers GCC warnings that ESP-IDF promotes to errors via
# -Werror. Specifically tvgCompressor.cpp:466 and :470 do  `encoded[3] < 0`
# where `encoded` is `const char*`. On the xtensa toolchain `char` is unsigned
# by default, so the comparison is always false and -Wtype-limits fires.
#
# This is an upstream ThorVG/LVGL issue, not anything in our code. Rather than
# patching managed_components/ (which would be overwritten on every component
# update), we relax warning-as-error for the LVGL component only. All other
# components keep -Werror in full force.
#
# `__idf_lvgl__lvgl` is the CMake target name ESP-IDF generates for the
# `lvgl/lvgl` managed component (component name = lvgl__lvgl, prefixed with
# __idf_).
if(TARGET __idf_lvgl__lvgl)
    target_compile_options(__idf_lvgl__lvgl PRIVATE
        -Wno-error=type-limits
        -Wno-type-limits
        # ThorVG also occasionally trips these on xtensa-gcc; harmless to silence.
        -Wno-error=maybe-uninitialized
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=missing-field-initializers
    )
endif()