#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define PROC_NET_DEV "/proc/net/dev"

// Function to detect the primary active network interface
void get_active_interface(char *interface, size_t size) {
    FILE *fp = fopen(PROC_NET_DEV, "r");
    if (!fp) {
        perror("Error opening /proc/net/dev");
        exit(1);
    }

    char line[256], ifname[32];
    unsigned long rx_bytes, tx_bytes;
    unsigned long max_bytes = 0;

    // Skip first two lines (headers)
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, " %31[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu", ifname, &rx_bytes, &tx_bytes) == 3) {
            unsigned long total_bytes = rx_bytes + tx_bytes;
            if (total_bytes > max_bytes) {
                max_bytes = total_bytes;
                strncpy(interface, ifname, size);
                interface[size - 1] = '\0';  // Ensure null termination
            }
        }
    }
    fclose(fp);
}

// Function to get network usage (upload and download in bytes)
void get_network_usage(unsigned long *rx_bytes, unsigned long *tx_bytes, char *net_interface) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (fp == NULL) {
        perror("Error opening /proc/net/dev");
        exit(1);
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, net_interface)) {
            sscanf(line, "%*s %lu %*u %*u %*u %*u %*u %*u %*u %lu", rx_bytes, tx_bytes);
            break;
        }
    }
    fclose(fp);
}

// Function to calculate network speed (bytes per second)
void get_network_speed( unsigned long *rx_speed, unsigned long *tx_speed, char *net_interface) {
    static short first_read = 1;
    static unsigned long rx1 = 0;
    static unsigned long tx1 = 0;
    static unsigned long rx2 = 0;
    static unsigned long tx2 = 0;
    
    get_network_usage(&rx2, &tx2, net_interface);

    if(!first_read){
      *rx_speed = rx2 - rx1;
      *tx_speed = tx2 - tx1;
    }else{
      *rx_speed = 0;
      *tx_speed = 0;
      first_read = 0;
    }
    rx1 = rx2;
    tx1 = tx2;

}

void format_speed(char *buffer, size_t size, unsigned long bytes_per_sec) {
    double speed = bytes_per_sec;
    const char *unit = "B/s";

    if (speed >= 1024 * 1024) {
        speed /= (1024 * 1024);
        unit = "MB/s";
    } else if (speed >= 1024) {
        speed /= 1024;
        unit = "KB/s";
    }

    snprintf(buffer, size, "%.2f %s", speed, unit);
}

