#include "filesys/disk_backend.h"
#include "filesys/image_backend.h"
#include <stdio.h>
#include <string.h>

/* ─── File-image backend state ─────────────────────────────── */

static FILE*         s_file       = 0;
static long          s_file_size  = 0;
static unsigned char s_initialized = 0;

/* ─── Backend callbacks ────────────────────────────────────── */

static DSTATUS_FS img_init(void) {
    if (s_file) {
        s_initialized = 1;
        return 0;
    }
    return STA_NOINIT_FS;
}

static DSTATUS_FS img_status(void) {
    if (s_file)
        return s_initialized ? 0 : STA_NOINIT_FS;
    return STA_NODISK_FS;
}

static DRESULT_FS img_read(BYTE_FS* buff, LBA_t_FS sector, UINT_FS count) {
    if (!s_file) return RES_NOTRDY_FS;

    long offset = (long)sector * 512L;
    if (fseek(s_file, offset, SEEK_SET) != 0)
        return RES_ERROR_FS;

    size_t total = (size_t)count * 512UL;
    if (fread(buff, 1, total, s_file) != total) {
        if (ferror(s_file)) return RES_ERROR_FS;
    }
    return RES_OK_FS;
}

static DRESULT_FS img_write(const BYTE_FS* buff, LBA_t_FS sector, UINT_FS count) {
    if (!s_file) return RES_NOTRDY_FS;

    long offset = (long)sector * 512L;
    if (fseek(s_file, offset, SEEK_SET) != 0)
        return RES_ERROR_FS;

    size_t total = (size_t)count * 512UL;
    if (fwrite(buff, 1, total, s_file) != total)
        return RES_ERROR_FS;

    return RES_OK_FS;
}

static DRESULT_FS img_ioctl(unsigned char cmd, void* buff) {
    switch (cmd) {
    case 0: /* CTRL_SYNC */
        if (s_file) fflush(s_file);
        return RES_OK_FS;
    case 1: { /* GET_SECTOR_COUNT */
        if (!buff) return RES_PARERR_FS;
        *(unsigned int*)buff = (unsigned int)(s_file_size / 512L);
        return RES_OK_FS;
    }
    case 2: /* GET_SECTOR_SIZE */
        if (!buff) return RES_PARERR_FS;
        *(unsigned short*)buff = 512;
        return RES_OK_FS;
    case 3: /* GET_BLOCK_SIZE */
        if (!buff) return RES_PARERR_FS;
        *(unsigned int*)buff = 1;
        return RES_OK_FS;
    default:
        return RES_PARERR_FS;
    }
}

static const DiskBackend s_img_backend = {
    img_init,
    img_status,
    img_read,
    img_write,
    img_ioctl
};

/* ─── Public API ───────────────────────────────────────────── */

void image_backend_init(const char* path) {
    if (s_file) image_backend_shutdown();

    s_file = fopen(path, "r+b");
    if (!s_file) return;

    fseek(s_file, 0, SEEK_END);
    s_file_size = ftell(s_file);

    disk_backend_register(0, &s_img_backend);
}

void image_backend_shutdown(void) {
    disk_backend_unregister(0);
    if (s_file) { fclose(s_file); s_file = 0; }
    s_file_size = 0;
    s_initialized = 0;
}

int image_backend_is_ready(void) {
    return s_file != 0;
}
