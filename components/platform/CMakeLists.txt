# ─── platform: display/touch abstraction for Paper S3 ───────────
idf_component_register(
    SRCS
        "platform.cpp"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../../include"
    REQUIRES
        M5GFX
        M5Unified
        bm8563
        driver
        esp_adc
        esp_timer
        esp_common
)
