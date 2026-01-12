#include "modules.h"

float mem_perc() {
  FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo == NULL) {
        perror("Error opening /proc/meminfo");
        return -1;
    }

    char line[256];
    unsigned long total_mem = 0;
    unsigned long available_mem = 0;

    while (fgets(line, sizeof(line), meminfo)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line, "MemTotal: %lu kB", &total_mem);
        }
        if (strncmp(line, "MemAvailable:", 12) == 0) {
            sscanf(line, "MemAvailable: %lu kB", &available_mem);
        }
    }

    fclose(meminfo);

    if (total_mem == 0) {
        return -1;
    }

    return (((float)(total_mem - available_mem) / total_mem) * 100);
    
}
