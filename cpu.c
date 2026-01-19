#include "modules.h"

#include <errno.h>

typedef struct {
    int cores;
    long double total_prev[7];
    long double *core_prev; // cores * 7
    int initialized;
} CpuPrev;

static CpuPrev cpu_prev = {0};

static int read_first_float_file(const char *path, float *out) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    long long v = 0;
    int ok = fscanf(fp, "%lld", &v) == 1;
    fclose(fp);
    if (!ok) return 0;
    // many sysfs temps are milli-degC
    *out = (float)v / 1000.0f;
    return 1;
}

static int starts_with_ci(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        char a = *s;
        char b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

float cpu_temp_c() {
    // Try to pick a CPU-like thermal zone.
    // Preference order: type contains "x86_pkg_temp" or starts with "cpu".
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) return -1;

    float best = -1;
    int best_score = -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;

        char type_path[256];
        char temp_path[256];
        snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type", ent->d_name);
        snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/%s/temp", ent->d_name);

        FILE *tf = fopen(type_path, "r");
        if (!tf) continue;
        char type[128] = {0};
        if (!fgets(type, sizeof(type), tf)) {
            fclose(tf);
            continue;
        }
        fclose(tf);

        // trim newline
        size_t n = strlen(type);
        while (n > 0 && (type[n - 1] == '\n' || type[n - 1] == '\r')) type[--n] = '\0';

        float t = -1;
        if (!read_first_float_file(temp_path, &t)) continue;
        if (t < 0 || t > 150) continue;

        int score = 0;
        if (starts_with_ci(type, "x86_pkg_temp")) score = 3;
        else if (starts_with_ci(type, "cpu")) score = 2;
        else if (strstr(type, "pkg") || strstr(type, "Package")) score = 1;

        if (score > best_score) {
            best_score = score;
            best = t;
        } else if (best_score < 0 && best < 0) {
            // fallback: first valid temp
            best = t;
        }
    }

    closedir(dir);
    return best;
}

static int parse_cpu_line(const char *prefix, long double out[7], FILE *fp) {
    // prefix is "cpu" or "cpuN"
    // Format: cpuN user nice system idle iowait irq softirq steal
    return fscanf(fp, "%*s %Lf %Lf %Lf %Lf %Lf %Lf %Lf",
                             &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6]) == 7;
}

int cpu_read_perc_ex(float *total_out, float *cores_out, int max_cores, int *out_cores_count) {
    if (total_out) *total_out = -1;
    if (out_cores_count) *out_cores_count = 0;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    // Read total cpu line first
    char label[16];
    if (fscanf(fp, "%15s", label) != 1) {
        fclose(fp);
        return -1;
    }
    if (strcmp(label, "cpu") != 0) {
        fclose(fp);
        return -1;
    }

    long double total_now[7] = {0};
    if (fscanf(fp, "%Lf %Lf %Lf %Lf %Lf %Lf %Lf",
                         &total_now[0], &total_now[1], &total_now[2], &total_now[3],
                         &total_now[4], &total_now[5], &total_now[6]) != 7) {
        fclose(fp);
        return -1;
    }

    // Count cores by scanning remaining lines for cpuN
    // We'll read the rest using fgets for robustness.
    int cores_found = 0;
    char line[256];
    // consume end of line
    fgets(line, sizeof(line), fp);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] < '0' || line[3] > '9') continue;
        cores_found++;
    }

    // Ensure prev storage
    if (!cpu_prev.initialized || cpu_prev.cores != cores_found) {
        if (cpu_prev.core_prev) {
            free(cpu_prev.core_prev);
            cpu_prev.core_prev = NULL;
        }
        cpu_prev.cores = cores_found;
        if (cores_found > 0) {
            cpu_prev.core_prev = (long double *)calloc((size_t)cores_found * 7, sizeof(long double));
            if (!cpu_prev.core_prev) {
                fclose(fp);
                return -1;
            }
        }
        memset(cpu_prev.total_prev, 0, sizeof(cpu_prev.total_prev));
        cpu_prev.initialized = 1;
    }

    // Rewind and re-read core lines now that we know count
    rewind(fp);
    // skip total line again
    if (fscanf(fp, "%15s", label) != 1) {
        fclose(fp);
        return -1;
    }
    long double tmp[7];
    if (fscanf(fp, "%Lf %Lf %Lf %Lf %Lf %Lf %Lf",
                         &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5], &tmp[6]) != 7) {
        fclose(fp);
        return -1;
    }
    // consume endline
    fgets(line, sizeof(line), fp);

    // Compute total
    float total_perc = -1;
    if (cpu_prev.total_prev[0] != 0) {
        long double prev_sum = 0, now_sum = 0;
        for (int i = 0; i < 7; i++) { prev_sum += cpu_prev.total_prev[i]; now_sum += total_now[i]; }
        long double sum = prev_sum - now_sum;
        if (sum != 0) {
            long double prev_nonidle = cpu_prev.total_prev[0] + cpu_prev.total_prev[1] + cpu_prev.total_prev[2] + cpu_prev.total_prev[5] + cpu_prev.total_prev[6];
            long double now_nonidle = total_now[0] + total_now[1] + total_now[2] + total_now[5] + total_now[6];
            total_perc = (float)(100.0L * ((prev_nonidle - now_nonidle) / sum));
            if (total_perc < 0) total_perc = 0;
            if (total_perc > 100) total_perc = 100;
        }
    }
    memcpy(cpu_prev.total_prev, total_now, sizeof(total_now));
    if (total_out) *total_out = total_perc;

    // Now read cpuN lines and compute per-core
    int out_cores = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] < '0' || line[3] > '9') continue;

        if (out_cores >= cpu_prev.cores) break;

        // Parse using sscanf from line to avoid mixed fscanf/fgets state
        long double nowv[7] = {0};
        // cpuN  user nice system idle iowait irq softirq steal
        if (sscanf(line, "%*s %Lf %Lf %Lf %Lf %Lf %Lf %Lf",
                             &nowv[0], &nowv[1], &nowv[2], &nowv[3], &nowv[4], &nowv[5], &nowv[6]) != 7) {
            continue;
        }

        long double *prevv = &cpu_prev.core_prev[out_cores * 7];
        float perc = -1;
        if (prevv[0] != 0) {
            long double prev_sum = 0, now_sum = 0;
            for (int i = 0; i < 7; i++) { prev_sum += prevv[i]; now_sum += nowv[i]; }
            long double sum = prev_sum - now_sum;
            if (sum != 0) {
                long double prev_nonidle = prevv[0] + prevv[1] + prevv[2] + prevv[5] + prevv[6];
                long double now_nonidle = nowv[0] + nowv[1] + nowv[2] + nowv[5] + nowv[6];
                perc = (float)(100.0L * ((prev_nonidle - now_nonidle) / sum));
                if (perc < 0) perc = 0;
                if (perc > 100) perc = 100;
            }
        }
        memcpy(prevv, nowv, sizeof(nowv));

        if (cores_out && out_cores < max_cores) {
            cores_out[out_cores] = perc;
        }
        out_cores++;
    }
    fclose(fp);

    if (out_cores_count) *out_cores_count = out_cores;
    return 0;
}

float cpu_perc() {
  float total = -1;
  (void)cpu_read_perc_ex(&total, NULL, 0, NULL);
  return total;
}

