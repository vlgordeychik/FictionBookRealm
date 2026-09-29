#include "filesys/disk_backend.h"

static const DiskBackend* s_backends[10] = {};

void disk_backend_register(unsigned char pdrv, const DiskBackend* backend) {
    if (pdrv < 10) s_backends[pdrv] = backend;
}

void disk_backend_unregister(unsigned char pdrv) {
    if (pdrv < 10) s_backends[pdrv] = 0;
}

const DiskBackend* disk_backend_get(unsigned char pdrv) {
    if (pdrv >= 10) return 0;
    return s_backends[pdrv];
}
