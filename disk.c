#include "modules.h"
#include <stdlib.h>

// This stub returns 0 disks to stop the /sys/block crash
DiskInfo* get_disk_info(int *count) {
    *count = 0;
    return NULL;
}

void free_disk_info(DiskInfo *info) {
    // Nothing to free in the stub
}
