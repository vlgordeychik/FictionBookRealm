# ESP-IDF build root.
cmake_minimum_required(VERSION 3.16)

# Драйвер часов BM8563 лежит в корне, а не в components/, поэтому его нужно
# указать явно: у драйвера уже есть ветка ESP_PLATFORM с idf_component_register.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/bm8563")

# M5Stack-библиотеки не входят в архив исходников — они подтягиваются скриптом
# tools/fetch_deps.py перед сборкой. Без них проект не собирается, поэтому
# проверяем наличие заранее и даём понятную ошибку вместо падения в графе
# компонентов.
set(FBR_PREREQ_COMPONENTS M5GFX M5Unified)
set(FBR_MISSING_PREREQS "")
foreach(_comp IN LISTS FBR_PREREQ_COMPONENTS)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/${_comp}/CMakeLists.txt")
        list(APPEND FBR_MISSING_PREREQS ${_comp})
    endif()
endforeach()
if(FBR_MISSING_PREREQS)
    string(REPLACE ";" ", " _missing "${FBR_MISSING_PREREQS}")
    message(FATAL_ERROR
        "Missing third-party components: ${_missing}\n"
        "They are prerequisites and are not shipped in this source archive.\n"
        "Fetch them once, then build again:\n"
        "\n"
        "    python tools/fetch_deps.py\n"
        "\n"
        "To verify what is present without downloading anything:\n"
        "    python tools/fetch_deps.py --check")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# FictionBook Realm. Имя проекта в CMake — без пробелов, оно же имя бинарника
# (build/fictionbook_realm.bin) и значение project_name в esp_app_desc_t.
project(fictionbook_realm)
