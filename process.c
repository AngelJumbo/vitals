#include "process.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long long read_total_jiffies(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;

    // cpu  user nice system idle iowait irq softirq steal guest guest_nice
    unsigned long long v[10] = {0};
    int n = fscanf(fp, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    fclose(fp);
    if (n < 4) return 0;

    unsigned long long sum = 0;
    for (int i = 0; i < 10; i++) sum += v[i];
    return sum;
}

static unsigned long long read_mem_total_kb(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0;

    char line[256];
    unsigned long total = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line, "MemTotal: %lu kB", &total);
            break;
        }
    }

    fclose(fp);
    return total;
}

int proc_init_ctx(ProcSampleCtx *ctx) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->page_size = sysconf(_SC_PAGESIZE);
    ctx->mem_total_kb = (unsigned long)read_mem_total_kb();
    ctx->last_total_jiffies = read_total_jiffies();

    ctx->pid_samples = NULL;
    ctx->pid_samples_count = 0;
    ctx->pid_samples_cap = 0;
    return 0;
}

static ProcPidSample *find_or_create_pid_sample(ProcSampleCtx *ctx, int pid) {
    for (int i = 0; i < ctx->pid_samples_count; i++) {
        if (ctx->pid_samples[i].pid == pid) return &ctx->pid_samples[i];
    }

    if (ctx->pid_samples_count >= ctx->pid_samples_cap) {
        int new_cap = (ctx->pid_samples_cap == 0) ? 256 : (ctx->pid_samples_cap * 2);
        ProcPidSample *tmp = (ProcPidSample *)realloc(ctx->pid_samples, (size_t)new_cap * sizeof(ProcPidSample));
        if (!tmp) return NULL;
        ctx->pid_samples = tmp;
        ctx->pid_samples_cap = new_cap;
    }

    ProcPidSample *slot = &ctx->pid_samples[ctx->pid_samples_count++];
    slot->pid = pid;
    slot->last_proc_time = 0;
    return slot;
}

static int name_matches_filter(const char *comm, const char *filter) {
    if (!filter || !*filter) return 1;

    // case-insensitive substring match
    for (const char *p = comm; *p; p++) {
        const char *a = p;
        const char *b = filter;
        while (*a && *b && (char)tolower((unsigned char)*a) == (char)tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static int read_proc_stat(int pid, char *comm_out, size_t comm_sz, char *state_out,
                          int *ppid_out, unsigned long long *utime_out, unsigned long long *stime_out,
                          unsigned long long *starttime_out, unsigned long long *vsize_out, long *rss_pages_out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // /proc/[pid]/stat: pid (comm) state ppid ...
    // comm can contain spaces; find '(' and last ')'
    char *l = strchr(buf, '(');
    char *r = strrchr(buf, ')');
    if (!l || !r || r <= l) return -1;

    // pid is before '('
    // comm is inside
    size_t comm_len = (size_t)(r - l - 1);
    if (comm_len >= comm_sz) comm_len = comm_sz - 1;
    memcpy(comm_out, l + 1, comm_len);
    comm_out[comm_len] = '\0';

    // now parse after ") "
    char *p = r + 2;
    char state = 0;
    int ppid = 0;

    // We need: state(3), ppid(4), utime(14), stime(15), starttime(22), vsize(23), rss(24)
    // We'll scan sequentially.
    // field 3 state
    if (sscanf(p, "%c %d", &state, &ppid) != 2) return -1;

    // advance p past state and ppid
    // easiest: tokenization by spaces
    int field = 3;
    char *save = NULL;
    char *tok = strtok_r(p, " ", &save);
    // tok should be state
    (void)tok;
    tok = strtok_r(NULL, " ", &save);
    // tok should be ppid
    (void)tok;
    field = 5;

    unsigned long long utime = 0, stime = 0, starttime = 0, vsize = 0;
    long rss_pages = 0;

    while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
        // current tok is field index
        if (field == 14) utime = strtoull(tok, NULL, 10);
        else if (field == 15) stime = strtoull(tok, NULL, 10);
        else if (field == 22) starttime = strtoull(tok, NULL, 10);
        else if (field == 23) vsize = strtoull(tok, NULL, 10);
        else if (field == 24) {
            rss_pages = strtol(tok, NULL, 10);
            break;
        }
        field++;
    }

    *state_out = state;
    *ppid_out = ppid;
    *utime_out = utime;
    *stime_out = stime;
    *starttime_out = starttime;
    *vsize_out = vsize;
    *rss_pages_out = rss_pages;
    return 0;
}

typedef enum {
    PROC_SORT_CPU = 0,
    PROC_SORT_MEM = 1,
    PROC_SORT_RSS = 2,
    PROC_SORT_PID = 3
} ProcSort;

static ProcSort g_sort_mode = PROC_SORT_CPU;

void proc_set_sort_mode(int mode) {
    if (mode < 0 || mode > 3) mode = 0;
    g_sort_mode = (ProcSort)mode;
}

static int cmp_cpu_desc(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    if (pa->cpu_percent < pb->cpu_percent) return 1;
    if (pa->cpu_percent > pb->cpu_percent) return -1;
    return (pa->pid - pb->pid);
}

static int cmp_mem_desc(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    if (pa->mem_percent < pb->mem_percent) return 1;
    if (pa->mem_percent > pb->mem_percent) return -1;
    return (pa->pid - pb->pid);
}

static int cmp_rss_desc(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    if (pa->rss_kb < pb->rss_kb) return 1;
    if (pa->rss_kb > pb->rss_kb) return -1;
    return (pa->pid - pb->pid);
}

static int cmp_pid_asc(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    return pa->pid - pb->pid;
}

static int process_cmp(const void *a, const void *b) {
    switch (g_sort_mode) {
        case PROC_SORT_MEM: return cmp_mem_desc(a, b);
        case PROC_SORT_RSS: return cmp_rss_desc(a, b);
        case PROC_SORT_PID: return cmp_pid_asc(a, b);
        case PROC_SORT_CPU:
        default: return cmp_cpu_desc(a, b);
    }
}

int proc_list(ProcessInfo **out, int *out_count, ProcSampleCtx *ctx, const char *name_filter) {
    if (!out || !out_count || !ctx) return -1;

    DIR *dir = opendir("/proc");
    if (!dir) return -1;

    unsigned long long total_jiffies_now = read_total_jiffies();
    unsigned long long total_delta = 0;
    if (ctx->last_total_jiffies && total_jiffies_now > ctx->last_total_jiffies)
        total_delta = total_jiffies_now - ctx->last_total_jiffies;
    ctx->last_total_jiffies = total_jiffies_now;

    int cap = 256;
    int count = 0;
    ProcessInfo *list = (ProcessInfo *)calloc((size_t)cap, sizeof(ProcessInfo));
    if (!list) {
        closedir(dir);
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0) continue;

        ProcessInfo info;
        memset(&info, 0, sizeof(info));
        info.pid = pid;

        unsigned long long utime = 0, stime = 0, starttime = 0, vsize = 0;
        long rss_pages = 0;
        if (read_proc_stat(pid, info.comm, sizeof(info.comm), &info.state, &info.ppid,
                           &utime, &stime, &starttime, &vsize, &rss_pages) != 0) {
            continue;
        }

        if (!name_matches_filter(info.comm, name_filter)) continue;

        unsigned long rss_kb = 0;
        if (rss_pages > 0 && ctx->page_size > 0) {
            rss_kb = (unsigned long)((unsigned long long)rss_pages * (unsigned long long)ctx->page_size / 1024ULL);
        }
        info.rss_kb = rss_kb;

        if (ctx->mem_total_kb > 0) info.mem_percent = 100.0 * ((double)rss_kb / (double)ctx->mem_total_kb);

        // CPU% using per-pid deltas vs total jiffies delta. This yields ~0..100*cores on multi-core systems.
        unsigned long long proc_time_now = utime + stime;
        ProcPidSample *ps = find_or_create_pid_sample(ctx, pid);
        unsigned long long proc_delta = 0;
        if (ps) {
            if (ps->last_proc_time && proc_time_now >= ps->last_proc_time) proc_delta = proc_time_now - ps->last_proc_time;
            ps->last_proc_time = proc_time_now;
        }

        if (total_delta > 0) info.cpu_percent = 100.0 * ((double)proc_delta / (double)total_delta);
        else info.cpu_percent = 0.0;

        if (count >= cap) {
            cap *= 2;
            ProcessInfo *tmp = (ProcessInfo *)realloc(list, (size_t)cap * sizeof(ProcessInfo));
            if (!tmp) break;
            list = tmp;
        }
        list[count++] = info;
    }

    closedir(dir);

    // use current mode when sorting
    qsort(list, (size_t)count, sizeof(ProcessInfo), process_cmp);

    *out = list;
    *out_count = count;
    return 0;
}

void proc_free(ProcessInfo *list) {
    free(list);
}

void proc_free_ctx(ProcSampleCtx *ctx) {
    if (!ctx) return;
    free(ctx->pid_samples);
    ctx->pid_samples = NULL;
    ctx->pid_samples_count = 0;
    ctx->pid_samples_cap = 0;
}

int proc_kill(int pid, int sig) {
    if (pid <= 0) return -1;
    if (kill(pid, sig) != 0) return -1;
    return 0;
}
