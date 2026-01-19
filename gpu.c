#include "modules.h"

#include <ctype.h>
#include <limits.h>

typedef struct {
  short initialized;
  short available;
  char device_path[PATH_MAX];
  unsigned int vendor_id; // 0x10de, 0x1002, 0x8086
} GpuCtx;

static GpuCtx gpu_ctx = {0};

static int read_file_ull(const char *path, unsigned long long *out) {
  FILE *fp = fopen(path, "r");
  if (!fp) return 0;
  unsigned long long value = 0;
  int ok = fscanf(fp, "%llu", &value) == 1;
  fclose(fp);
  if (!ok) return 0;
  *out = value;
  return 1;
}

static int read_file_hex_uint(const char *path, unsigned int *out) {
  FILE *fp = fopen(path, "r");
  if (!fp) return 0;
  unsigned int value = 0;
  int ok = fscanf(fp, "%x", &value) == 1;
  fclose(fp);
  if (!ok) return 0;
  *out = value;
  return 1;
}

static int file_exists(const char *path) {
  return access(path, R_OK) == 0;
}

static int build_path(char *dst, size_t dstsz, const char *base, const char *suffix) {
  size_t base_len = strnlen(base, dstsz);
  size_t suffix_len = strlen(suffix);
  if (base_len == dstsz) return 0;
  if (base_len + suffix_len + 1 > dstsz) return 0;
  memcpy(dst, base, base_len);
  memcpy(dst + base_len, suffix, suffix_len + 1);
  return 1;
}

static void gpu_try_init() {
  if (gpu_ctx.initialized) return;
  gpu_ctx.initialized = 1;
  gpu_ctx.available = 0;
  gpu_ctx.device_path[0] = '\0';
  gpu_ctx.vendor_id = 0;

  DIR *dir = opendir("/sys/class/drm");
  if (!dir) return;

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    // Look for cardN
    if (strncmp(ent->d_name, "card", 4) != 0) continue;
    if (!isdigit((unsigned char)ent->d_name[4])) continue;

    char vendor_path[PATH_MAX];
    snprintf(vendor_path, sizeof(vendor_path), "/sys/class/drm/%s/device/vendor", ent->d_name);

    unsigned int vendor = 0;
    if (!read_file_hex_uint(vendor_path, &vendor)) continue;
    if (vendor == 0) continue;

    // Filter out vgem and similar (vendor 0x0000 or missing device)
    char device_dir[PATH_MAX];
    snprintf(device_dir, sizeof(device_dir), "/sys/class/drm/%s/device", ent->d_name);
    if (!file_exists(device_dir)) continue;

    // Found a GPU-like device
    strncpy(gpu_ctx.device_path, device_dir, sizeof(gpu_ctx.device_path));
    gpu_ctx.device_path[sizeof(gpu_ctx.device_path) - 1] = '\0';
    gpu_ctx.vendor_id = vendor;
    gpu_ctx.available = 1;
    break;
  }

  closedir(dir);
}

short gpu_available() {
  gpu_try_init();
  return gpu_ctx.available;
}

static int read_gpu_busy_percent_sysfs(int *out_percent) {
  if (!gpu_ctx.available) return 0;

  char path[PATH_MAX];
  if (!build_path(path, sizeof(path), gpu_ctx.device_path, "/gpu_busy_percent")) return 0;

  unsigned long long busy = 0;
  if (!read_file_ull(path, &busy)) return 0;
  if (busy > 100) busy = 100;
  *out_percent = (int)busy;
  return 1;
}

static int read_vram_sysfs(unsigned long long *used_bytes, unsigned long long *total_bytes) {
  if (!gpu_ctx.available) return 0;

  char used_path[PATH_MAX];
  char total_path[PATH_MAX];

  if (!build_path(used_path, sizeof(used_path), gpu_ctx.device_path, "/mem_info_vram_used")) return 0;
  if (!build_path(total_path, sizeof(total_path), gpu_ctx.device_path, "/mem_info_vram_total")) return 0;

  unsigned long long used = 0, total = 0;
  if (!read_file_ull(used_path, &used)) return 0;
  if (!read_file_ull(total_path, &total)) return 0;
  if (total == 0) return 0;

  *used_bytes = used;
  *total_bytes = total;
  return 1;
}

static int read_nvidia_smi(int *gpu_util_percent, unsigned long long *mem_used_bytes, unsigned long long *mem_total_bytes) {
  FILE *fp = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
  if (!fp) return 0;

  char line[256];
  if (!fgets(line, sizeof(line), fp)) {
    pclose(fp);
    return 0;
  }
  pclose(fp);

  int util = -1;
  unsigned long long used_mib = 0, total_mib = 0;

  // Expected: "12, 345, 7982" (with optional spaces)
  if (sscanf(line, " %d , %llu , %llu", &util, &used_mib, &total_mib) != 3) {
    return 0;
  }

  if (util < 0) util = 0;
  if (util > 100) util = 100;

  *gpu_util_percent = util;
  *mem_used_bytes = used_mib * 1024ULL * 1024ULL;
  *mem_total_bytes = total_mib * 1024ULL * 1024ULL;
  return 1;
}

static int read_nvidia_smi_ext(int *gpu_util_percent, unsigned long long *mem_used_bytes, unsigned long long *mem_total_bytes, int *temp_c) {
  FILE *fp = popen("nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
  if (!fp) return 0;

  char line[256];
  if (!fgets(line, sizeof(line), fp)) {
    pclose(fp);
    return 0;
  }
  pclose(fp);

  int util = -1;
  unsigned long long used_mib = 0, total_mib = 0;
  int temp = -1;

  if (sscanf(line, " %d , %llu , %llu , %d", &util, &used_mib, &total_mib, &temp) != 4) {
    return 0;
  }

  if (util < 0) util = 0;
  if (util > 100) util = 100;
  if (temp < 0 || temp > 150) temp = -1;

  *gpu_util_percent = util;
  *mem_used_bytes = used_mib * 1024ULL * 1024ULL;
  *mem_total_bytes = total_mib * 1024ULL * 1024ULL;
  *temp_c = temp;
  return 1;
}

static int read_gpu_temp_sysfs(float *out_c) {
  if (!gpu_ctx.available) return 0;

  // Many DRM drivers expose temperature via hwmon under the device.
  char hwmon_dir[PATH_MAX];
  snprintf(hwmon_dir, sizeof(hwmon_dir), "%s/hwmon", gpu_ctx.device_path);
  DIR *dir = opendir(hwmon_dir);
  if (!dir) return 0;

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, "hwmon", 5) != 0) continue;

    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s/%s/temp1_input", hwmon_dir, ent->d_name);

    unsigned long long v = 0;
    if (read_file_ull(temp_path, &v)) {
      float c = (float)v / 1000.0f;
      if (c >= 0 && c <= 150) {
        *out_c = c;
        closedir(dir);
        return 1;
      }
    }
  }
  closedir(dir);
  return 0;
}

int gpu_read_stats_ex(float *gpu_util_percent, float *vram_percent,
                      unsigned long long *vram_used_bytes, unsigned long long *vram_total_bytes,
                      float *temp_c) {
  gpu_try_init();
  if (!gpu_ctx.available) return -1;

  int util_i = -1;
  unsigned long long used_b = 0, total_b = 0;
  float temp = -1;

  // Prefer sysfs util (AMD/Intel)
  if (read_gpu_busy_percent_sysfs(&util_i)) {
    (void)read_vram_sysfs(&used_b, &total_b);
    (void)read_gpu_temp_sysfs(&temp);
  } else if (gpu_ctx.vendor_id == 0x10de) {
    int temp_i = -1;
    if (read_nvidia_smi_ext(&util_i, &used_b, &total_b, &temp_i)) {
      if (temp_i >= 0) temp = (float)temp_i;
    } else {
      // fallback to older query (no temp)
      (void)read_nvidia_smi(&util_i, &used_b, &total_b);
    }
  }

  float util = (util_i >= 0) ? (float)util_i : -1;
  float vram = -1;
  if (total_b > 0) {
    double p = (double)used_b * 100.0 / (double)total_b;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    vram = (float)p;
  }

  if (gpu_util_percent) *gpu_util_percent = util;
  if (vram_percent) *vram_percent = vram;
  if (vram_used_bytes) *vram_used_bytes = used_b;
  if (vram_total_bytes) *vram_total_bytes = total_b;
  if (temp_c) *temp_c = temp;
  return 0;
}

float gpu_perc() {
  gpu_try_init();
  if (!gpu_ctx.available) return -1;

  int util = -1;

  // Prefer sysfs busy percent when available (AMD/Intel)
  if (read_gpu_busy_percent_sysfs(&util)) {
    return (float)util;
  }

  // NVIDIA fallback: nvidia-smi
  if (gpu_ctx.vendor_id == 0x10de) {
    unsigned long long used_b = 0, total_b = 0;
    if (read_nvidia_smi(&util, &used_b, &total_b)) {
      return (float)util;
    }
  }

  return -1;
}

float vram_perc() {
  gpu_try_init();
  if (!gpu_ctx.available) return -1;

  unsigned long long used_b = 0, total_b = 0;

  // Sysfs path (typically amdgpu; sometimes other drivers)
  if (read_vram_sysfs(&used_b, &total_b)) {
    double perc = (double)used_b * 100.0 / (double)total_b;
    if (perc < 0) perc = 0;
    if (perc > 100) perc = 100;
    return (float)perc;
  }

  // NVIDIA fallback: nvidia-smi
  if (gpu_ctx.vendor_id == 0x10de) {
    int util = 0;
    if (read_nvidia_smi(&util, &used_b, &total_b) && total_b > 0) {
      double perc = (double)used_b * 100.0 / (double)total_b;
      if (perc < 0) perc = 0;
      if (perc > 100) perc = 100;
      return (float)perc;
    }
  }

  return -1;
}
