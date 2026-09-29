#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*  Status of Disk Functions (copied from FatFs diskio.h to avoid dependency)  */
typedef unsigned char  DSTATUS_FS;
#define STA_NOINIT_FS  0x01
#define STA_NODISK_FS  0x02
#define STA_PROTECT_FS 0x04

typedef enum {
    RES_OK_FS    = 0,
    RES_ERROR_FS = 1,
    RES_WRPRT_FS = 2,
    RES_NOTRDY_FS= 3,
    RES_PARERR_FS= 4
} DRESULT_FS;

typedef unsigned int   LBA_t_FS;
typedef unsigned char  BYTE_FS;
typedef unsigned int   UINT_FS;

/*  Abstract disk backend — one instance per physical drive  */
typedef struct {
    DSTATUS_FS (*init)(void);
    DSTATUS_FS (*status)(void);
    DRESULT_FS (*read)(BYTE_FS* buff, LBA_t_FS sector, UINT_FS count);
    DRESULT_FS (*write)(const BYTE_FS* buff, LBA_t_FS sector, UINT_FS count);
    DRESULT_FS (*ioctl)(unsigned char cmd, void* buff);
} DiskBackend;

/*  Register/unregister a backend for a physical drive number  */
void disk_backend_register(unsigned char pdrv, const DiskBackend* backend);
void disk_backend_unregister(unsigned char pdrv);
const DiskBackend* disk_backend_get(unsigned char pdrv);

#ifdef __cplusplus
}
#endif
