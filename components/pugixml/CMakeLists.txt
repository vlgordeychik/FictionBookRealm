# ─── pugixml: lightweight XML parser ────────────────────────────
set(PUGIXML_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pugixml")

idf_component_register(
    SRCS
        "${PUGIXML_DIR}/pugixml.cpp"
    INCLUDE_DIRS
        "${PUGIXML_DIR}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)
