/*-----------------------------------------------------------------------*/
/* Low level disk I/O module — dispatches to registered DiskBackends     */
/*-----------------------------------------------------------------------*/

#include "ff.h"
#include "diskio.h"
#include "filesys/disk_backend.h"

/*-----------------------------------------------------------------------*/
/* Volume-to-Partition mapping (required when FF_MULTI_PARTITION=1)      */
/*-----------------------------------------------------------------------*/

#if FF_MULTI_PARTITION
PARTITION VolToPart[FF_VOLUMES] = {
    {0, 0}   /* Volume 0 → physical drive 0, auto partition */
};
#endif

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status(BYTE pdrv) {
    const DiskBackend* b = disk_backend_get(pdrv);
    if (!b || !b->status) return STA_NOINIT;
    return b->status();
}

/*-----------------------------------------------------------------------*/
/* Initialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize(BYTE pdrv) {
    const DiskBackend* b = disk_backend_get(pdrv);
    if (!b || !b->init) return STA_NOINIT;
    return b->init();
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    const DiskBackend* b = disk_backend_get(pdrv);
    if (!b || !b->read) return RES_PARERR;
    return b->read(buff, sector, count);
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    const DiskBackend* b = disk_backend_get(pdrv);
    if (!b || !b->write) return RES_PARERR;
    return b->write((BYTE_FS*)buff, sector, count);
}

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    const DiskBackend* b = disk_backend_get(pdrv);
    if (!b || !b->ioctl) return RES_PARERR;
    return b->ioctl(cmd, buff);
}
