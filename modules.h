#ifndef MODULES_H 
#define MODULES_H
#include <sys/statvfs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "process.h"

float mem_perc();
float cpu_perc();
// Extended CPU stats
int cpu_read_perc_ex(float *total_out, float *cores_out, int max_cores, int *out_cores_count);
float cpu_temp_c();

short gpu_available();
float gpu_perc();
float vram_perc();
// Extended GPU stats
int gpu_read_stats_ex(float *gpu_util_percent, float *vram_percent,
                      unsigned long long *vram_used_bytes, unsigned long long *vram_total_bytes,
                      float *temp_c);
void get_active_interface(char *interface, size_t size);
void get_network_speed( unsigned long *rx_speed, unsigned long *tx_speed, char *net_interface);
void format_speed(char *buffer, size_t size, unsigned long bytes_per_sec);


typedef struct {
    char device_name[32];
    char disk_type[8];
    double busy_percent;
} DiskInfo;
short is_disk_device(const char *device_name);
const char* get_disk_type(const char *device_name);
double calculate_disk_busy(const char *disk);
DiskInfo* get_disk_info(int *count);
void free_disk_info(DiskInfo *disks);

#endif
