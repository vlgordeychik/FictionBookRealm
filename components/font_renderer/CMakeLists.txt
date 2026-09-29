# ─── font_renderer: FreeType2 wrapper for text rendering ────────
set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src")

idf_component_register(
    SRCS
        "${SRC_DIR}/font_renderer.cpp"
    INCLUDE_DIRS
        "${SRC_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/../../include"
    REQUIRES
        freetype
)
