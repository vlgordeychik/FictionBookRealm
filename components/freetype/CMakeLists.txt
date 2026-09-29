# ─── FreeType2 for ESP-IDF ──────────────────────────────────────
# Uses umbrella/single-object approach: each subdirectory has one
# .c that #includes all sibling files via FT_MAKE_OPTION_SINGLE_OBJECT.
# This avoids duplicate symbols and speeds up compilation.

set(FT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../freetype2")

# Single-object compilation units
set(FT_SRCS
    "${FT_DIR}/src/base/ftbase.c"
    "${FT_DIR}/src/base/ftinit.c"
    "${FT_DIR}/src/truetype/truetype.c"
    "${FT_DIR}/src/sfnt/sfnt.c"
    "${FT_DIR}/src/smooth/smooth.c"
    "${FT_DIR}/src/raster/raster.c"
    "${FT_DIR}/src/cache/ftcache.c"
    "${FT_DIR}/src/psaux/psaux.c"
    "${FT_DIR}/src/pshinter/pshinter.c"
    "${FT_DIR}/src/psnames/psnames.c"
    "${FT_DIR}/src/autofit/autofit.c"
    "${FT_DIR}/src/cff/cff.c"
    "${FT_DIR}/src/gzip/ftgzip.c"
)

# Verify umbrella files exist
foreach(f ${FT_SRCS})
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "FreeType2 umbrella file not found: ${f}")
    endif()
endforeach()

idf_component_register(
    SRCS ${FT_SRCS}
    INCLUDE_DIRS
        "${FT_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}"
    PRIV_INCLUDE_DIRS
        "${FT_DIR}/src"
        "${FT_DIR}/src/gzip"
        "${FT_DIR}/src/cff"
    REQUIRES
        esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FT2_BUILD_LIBRARY
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    FT_CONFIG_OPTION_DISABLE_BZIP2
    FT_CONFIG_OPTION_DISABLE_BROTLI
    FT_CONFIG_OPTION_DISABLE_HARFBUZZ
    FT_CONFIG_OPTION_DISABLE_PNG
    FT_CONFIG_OPTION_DISABLE_ZLIB
    FT_CONFIG_OPTION_DISABLE_HVF
    FT_CONFIG_OPTION_DISABLE_SVG
    FT_CONFIG_MODULES_H="ftmodule.h"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-missing-braces
    -Wno-unused-parameter
)
