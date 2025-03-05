#include "modules.h"

int cpu_perc(){
  static long prev_idle = 0, prev_total = 0;
    long idle, total;
    long user, nice, system, idle_time, iowait, irq, softirq;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("Failed to open /proc/stat");
        return -1;
    }

    // Read the first line from /proc/stat
    if (fscanf(fp, "cpu  %ld %ld %ld %ld %ld %ld %ld",
               &user, &nice, &system, &idle_time, &iowait, &irq, &softirq) != 7) {
        perror("Failed to parse /proc/stat");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    idle = idle_time + iowait;
    total = user + nice + system + idle + irq + softirq;

    long delta_idle = idle - prev_idle;
    long delta_total = total - prev_total;

    prev_idle = idle;
    prev_total = total;

    // Return CPU usage as a percentage
    return (delta_total - delta_idle) * 100 / delta_total;
}