#include "modules.h"

#include <sys/types.h>
#include <ctype.h>
#include <time.h>

double calculate_disk_busy(const char *disk) {
    char line[512];
    static struct {
        char disk_name[32];
        long long time_spent;
        struct timespec timestamp;
    } prev_stats[32] = {0}; // Cache for up to 32 disks
    static int num_tracked_disks = 0;
    
    long long curr_time_spent = 0;
    struct timespec curr_time;
    clock_gettime(CLOCK_MONOTONIC, &curr_time);
    
    // Look for this disk in our previous stats
    int disk_idx = -1;
    for (int i = 0; i < num_tracked_disks; i++) {
        if (strcmp(prev_stats[i].disk_name, disk) == 0) {
            disk_idx = i;
            break;
        }
    }
    
    // Open /proc/diskstats to get current stats
    FILE *file = fopen("/proc/diskstats", "r");
    if (!file) {
        perror("Failed to open /proc/diskstats");
        return -1.0;
    }
    
    // Find the specified disk and extract its busy time
    while (fgets(line, sizeof(line), file)) {
        char dev_name[32];
        long long major, minor, reads, writes, ios_in_progress, time_spent;
        int fields = sscanf(line, "%lld %lld %31s %lld %lld %lld %lld %lld %lld %lld %lld",
                            &major, &minor, dev_name, &reads, &writes, &writes,
                            &writes, &writes, &writes, &ios_in_progress, &time_spent);
        if (fields >= 11 && strcmp(dev_name, disk) == 0) {
            curr_time_spent = time_spent;
            
            // If this is a new disk or first time seeing it
            if (disk_idx == -1) {
                // If we have room, add it to our tracking
                if (num_tracked_disks < 32) {
                    disk_idx = num_tracked_disks++;
                    strncpy(prev_stats[disk_idx].disk_name, disk, sizeof(prev_stats[disk_idx].disk_name) - 1);
                    prev_stats[disk_idx].disk_name[sizeof(prev_stats[disk_idx].disk_name) - 1] = '\0';
                    prev_stats[disk_idx].time_spent = curr_time_spent;
                    prev_stats[disk_idx].timestamp = curr_time;
                    fclose(file);
                    return 0.0; // First reading, return 0
                }
            } else {
                // Calculate time difference in milliseconds
                double time_diff = (curr_time.tv_sec - prev_stats[disk_idx].timestamp.tv_sec) * 1000.0 +
                                 (curr_time.tv_nsec - prev_stats[disk_idx].timestamp.tv_nsec) / 1000000.0;
                
                // Calculate busy percentage
                double busy_percent = 0.0;
                if (time_diff > 0) {
                    busy_percent = (curr_time_spent - prev_stats[disk_idx].time_spent) / (time_diff / 10.0);
                    
                    // Cap at 100%
                    if (busy_percent > 100.0) busy_percent = 100.0;
                }
                
                // Update previous values
                prev_stats[disk_idx].time_spent = curr_time_spent;
                prev_stats[disk_idx].timestamp = curr_time;
                
                fclose(file);
                return busy_percent;
            }
            break;
        }
    }
    
    fclose(file);
    return -1.0; // Disk not found
}

// Function to check if a device is a disk (HDD or SSD)
short is_disk_device(const char *device_name) {
    // Exclude loop, ram, dm- devices, and partitions (devices with numbers)
    if (strncmp(device_name, "loop", 4) == 0 ||
        strncmp(device_name, "ram", 3) == 0 ||
        strncmp(device_name, "dm-", 3) == 0) {
        return 0;
    }
    
    // Check if the device name ends with a number (likely a partition)
    int len = strlen(device_name);
    if (len > 0 && isdigit(device_name[len-1])) {
        return 0;
    }
    
    return 1;
}

// Function to determine if a disk is an SSD or HDD
const char* get_disk_type(const char *device_name) {
    char path[256];
    char buffer[256];
    
    // Check if rotational (0 for SSD, 1 for HDD)
    snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", device_name);
    FILE *file = fopen(path, "r");
    if (file) {
        if (fgets(buffer, sizeof(buffer), file)) {
            fclose(file);
            return (buffer[0] == '0') ? "SSD" : "HDD";
        }
        fclose(file);
    }
    
    return "Unknown";
}


DiskInfo* get_disk_info(int *count) {
    DIR *dir;
    struct dirent *entry;
    DiskInfo *disks = NULL;
    int max_disks = 10;
    int disk_count = 0;
    
    // Allocate initial memory for disk info
    disks = (DiskInfo*)malloc(max_disks * sizeof(DiskInfo));
    if (!disks) {
        perror("Memory allocation failed");
        *count = 0;
        return NULL;
    }
    
    // Open /sys/block directory
    dir = opendir("/sys/block");
    if (!dir) {
        perror("Failed to open /sys/block");
        free(disks);
        *count = 0;
        return NULL;
    }
    
    // Iterate through all devices
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue; // Skip hidden directories
        }
        
        if (is_disk_device(entry->d_name)) {
            // Resize array if needed
            if (disk_count >= max_disks) {
                max_disks *= 2;
                DiskInfo *temp = (DiskInfo*)realloc(disks, max_disks * sizeof(DiskInfo));
                if (!temp) {
                    perror("Memory reallocation failed");
                    free(disks);
                    closedir(dir);
                    *count = 0;
                    return NULL;
                }
                disks = temp;
            }
            
            // Store disk information
            strncpy(disks[disk_count].device_name, entry->d_name, sizeof(disks[disk_count].device_name) - 1);
            disks[disk_count].device_name[sizeof(disks[disk_count].device_name) - 1] = '\0';
            
            const char *disk_type = get_disk_type(entry->d_name);
            strncpy(disks[disk_count].disk_type, disk_type, sizeof(disks[disk_count].disk_type) - 1);
            disks[disk_count].disk_type[sizeof(disks[disk_count].disk_type) - 1] = '\0';
            
            disks[disk_count].busy_percent = calculate_disk_busy(entry->d_name);
            
            // Always include the disk, even on first reading when busy_percent is 0
            disk_count++;
        }
    }
    
    closedir(dir);
    *count = disk_count;
    return disks;
}

// Function to free disk info
void free_disk_info(DiskInfo *disks) {
    free(disks);
}
