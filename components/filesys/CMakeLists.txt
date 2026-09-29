# ─── filesys: FatFs + SD card filesystem wrapper ────────────────
set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src")
set(FATFS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../fatfs")
set(INC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include")

idf_component_register(
    SRCS
        "${FATFS_DIR}/ff.c"
        "${FATFS_DIR}/ffsystem.c"
        "${FATFS_DIR}/diskio.c"
        "${FATFS_DIR}/ffunicode.c"
        "${SRC_DIR}/filesys/disk_backend.cpp"
        "${SRC_DIR}/filesys/sd_backend.cpp"
        "${SRC_DIR}/filesys/filesystem.cpp"
        "${SRC_DIR}/filesys/inireader.cpp"
    INCLUDE_DIRS
        "${INC_DIR}"
        "${FATFS_DIR}"
    REQUIRES
        driver
        esp_common
        esp_driver_sdspi
        esp_driver_sdmmc
        esp_driver_spi
        sdmmc
)
