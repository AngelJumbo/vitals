#include "modules.h"


void check_device_type(const char *device) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", device);

    FILE *file = fopen(path, "r");
    if (file) {
        int rotational;
        fscanf(file, "%d", &rotational);
        fclose(file);

        printf("- %s (%s)\n", device, rotational ? "HDD" : "SSD");
    } else {
        printf("- %s (Unknown)\n", device);
    }
}

void list_block_devices_with_types() {
    const char *block_path = "/sys/class/block";
    struct dirent *entry;
    DIR *dir = opendir(block_path);

    if (dir == NULL) {
        perror("opendir");
        return;
    }

    printf("Block devices:\n");
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Skip partitions and loopback devices
        if (strstr(entry->d_name, "loop") != NULL || strchr(entry->d_name, 'p') != NULL)
            continue;

        check_device_type(entry->d_name);
    }

    closedir(dir);
}

// Function to calculate the disk I/O busy percentage
double calculate_disk_busy(const char *disk) {
    char line[512];
    long long prev_time_spent = 0, curr_time_spent = 0;

    // Open /proc/diskstats to get initial stats
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
            prev_time_spent = time_spent;
            break;
        }
    }
    fclose(file);

    // Wait for 1 second
    sleep(1);

    // Open /proc/diskstats again to get updated stats
    file = fopen("/proc/diskstats", "r");
    if (!file) {
        perror("Failed to open /proc/diskstats");
        return -1.0;
    }

    while (fgets(line, sizeof(line), file)) {
        char dev_name[32];
        long long major, minor, reads, writes, ios_in_progress, time_spent;
        int fields = sscanf(line, "%lld %lld %31s %lld %lld %lld %lld %lld %lld %lld %lld",
                            &major, &minor, dev_name, &reads, &writes, &writes,
                            &writes, &writes, &writes, &ios_in_progress, &time_spent);

        if (fields >= 11 && strcmp(dev_name, disk) == 0) {
            curr_time_spent = time_spent;
            break;
        }
    }
    fclose(file);

    // Calculate the busy percentage
    if (prev_time_spent == 0 || curr_time_spent == 0) {
        fprintf(stderr, "Failed to read time spent doing I/O for disk %s\n", disk);
        return -1.0;
    }

    return (double)(curr_time_spent - prev_time_spent) / 10.0; // 10ms granularity
}

/*
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk>\n", argv[0]);
        return 1;
    }

    const char *disk = argv[1];
    double busy_percentage = calculate_disk_busy(disk);
    if (busy_percentage >= 0) {
        printf("Disk %s busy percentage: %.2f%%\n", disk, busy_percentage);
    }

    return 0;
}*/
