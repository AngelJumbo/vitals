#include "modules.h"
#include <sys/types.h>
#include <sys/sysctl.h>
#include <string.h>

float cpu_perc() {
    static long last_cp_time[5] = {0};
    long current_cp_time[5];
    size_t len = sizeof(current_cp_time);

    if (sysctlbyname("kern.cp_time", &current_cp_time, &len, NULL, 0) < 0) {
        return 0.0f;
    }

    long diff[5];
    long total_diff = 0;
    for (int i = 0; i < 5; i++) {
        diff[i] = current_cp_time[i] - last_cp_time[i];
        total_diff += diff[i];
    }

    if (total_diff == 0) return 0.0f;

    float usage = 100.0 * (float)(total_diff - diff[4]) / (float)total_diff;
    memcpy(last_cp_time, current_cp_time, sizeof(current_cp_time));

    return usage;
}
