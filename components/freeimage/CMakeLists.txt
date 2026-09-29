# ─── freeimage: stb_image + image decoder ───────────────────────
set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(STB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../stb")
set(INC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include")

idf_component_register(
    SRCS
        "${SRC_DIR}/image_decoder.cpp"
    INCLUDE_DIRS
        "${STB_DIR}"
        "${SRC_DIR}"
        "${INC_DIR}"
)
