# ─── fb2_core: FB2 document parser & renderer ──────────────────
set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(INC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include")

idf_component_register(
    SRCS
        "${SRC_DIR}/fb2/fb2_metadata.cpp"
        "${SRC_DIR}/fb2/fb2_index.cpp"
        "${SRC_DIR}/fb2/fb2_document.cpp"
        "${SRC_DIR}/fb2/fb2_page_provider.cpp"
        "${SRC_DIR}/fb2/fb2_renderer.cpp"
        "${SRC_DIR}/fb2/fb2_navigator.cpp"
        "${SRC_DIR}/fb2/fb2_image_cache.cpp"
    INCLUDE_DIRS
        "${INC_DIR}"
        "${SRC_DIR}"
    REQUIRES
        pugixml
        freeimage
)
