#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>

typedef struct {
    int pid;
    char comm[64];
    char state;
    int ppid;
    double cpu_percent;
    double mem_percent;
    unsigned long rss_kb;
} ProcessInfo;

typedef struct {
    int pid;
    unsigned long long last_proc_time;
} ProcPidSample;

typedef struct {
    long page_size;
    unsigned long mem_total_kb;
    unsigned long long last_total_jiffies;

    ProcPidSample *pid_samples;
    int pid_samples_count;
    int pid_samples_cap;
} ProcSampleCtx;

int proc_init_ctx(ProcSampleCtx *ctx);
int proc_list(ProcessInfo **out, int *out_count, ProcSampleCtx *ctx, const char *name_filter);
void proc_free(ProcessInfo *list);
int proc_kill(int pid, int sig);
void proc_free_ctx(ProcSampleCtx *ctx);
void proc_set_sort_mode(int mode);

#endif
