# Компонент для ESP-IDF. Файл обрабатывается в script-режиме
# (component_get_requirements.cmake), поэтому здесь допустим только
# idf_component_register(): ни project(), ни cmake_minimum_required().
# Автономная сборка драйвера по-прежнему работает через component.mk и Makefile.
idf_component_register(
    SRCS "bm8563.c"
    INCLUDE_DIRS "."
)
