# ─── scenes: UI scenes + scene manager ──────────────────────────
set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(STB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../stb")

idf_component_register(
    SRCS
        "${SRC_DIR}/scene.cpp"
        "${SRC_DIR}/scene_manager.cpp"
        "${SRC_DIR}/fb2_page_view.cpp"
        "${SRC_DIR}/hyphen_ru.cpp"
        "${SRC_DIR}/config.cpp"
        "${SRC_DIR}/clock.cpp"
        "${SRC_DIR}/layout_cache.cpp"
        "${SRC_DIR}/history.cpp"
        "${SRC_DIR}/fb2zip.cpp"
        "${SRC_DIR}/fbr_inflate.c"
        "${SRC_DIR}/splash_scene.cpp"
        "${SRC_DIR}/settings_scene.cpp"
        "${SRC_DIR}/browser_scene.cpp"
        "${SRC_DIR}/book_scene.cpp"
        "${SRC_DIR}/toc_scene.cpp"
        "${SRC_DIR}/gestures.cpp"
        "${SRC_DIR}/lock_scene.cpp"
        "${SRC_DIR}/bookshelf_scene.cpp"
        "${SRC_DIR}/history_scene.cpp"
        "${SRC_DIR}/icons_data.cpp"
        "${SRC_DIR}/cover_loader.cpp"
        "${SRC_DIR}/power_off_scene.cpp"
        "${SRC_DIR}/ui_overlay.cpp"
    INCLUDE_DIRS
        "${SRC_DIR}"
        "${STB_DIR}"
    REQUIRES
        fb2_core
        font_renderer
        filesys
        freeimage
        platform
        pugixml
        heap
        M5GFX
)
