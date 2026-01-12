#include <sys/types.h>
#include <sys/sysctl.h>
#include <stddef.h>

float mem_perc() {
    unsigned long total;
    int page_size;
    unsigned int free_count;
    size_t len, p_len, f_len;

    len = sizeof(total);
    p_len = sizeof(page_size);
    f_len = sizeof(free_count);

    if (sysctlbyname("hw.physmem", &total, &len, NULL, 0) < 0) return -1;
    if (sysctlbyname("hw.pagesize", &page_size, &p_len, NULL, 0) < 0) return -1;
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_count, &f_len, NULL, 0) < 0) return -1;

    long free_mem = (long)free_count * page_size;
    float used_percent = 100.0f * (float)(total - free_mem) / (float)total;

    return used_percent;
}
