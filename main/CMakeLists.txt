idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        fb2_core
        pugixml
        font_renderer
        filesys
        freeimage
        scenes
        platform
        M5GFX
        M5Unified
)
