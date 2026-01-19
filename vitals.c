#define TB_IMPL
#include "termbox.h"
#include "modules.h"
#include "utils.h"
#include <pthread.h>
#include <signal.h>

#define MAX_DISKS 8
#define STR_LEN(s) (sizeof(s) - 1) 
#define APP_NAME " vitals "
#define APP_VERSION " 0.1.0 "
#define ALERT_MESSAGE "The terminal is too small."
#define MIN_WIDTH 80
#define MIN_HEIGHT 20

// Tabs
typedef enum { TAB_OVERVIEW = 0, TAB_CPU = 1, TAB_GPU = 2, TAB_PROCESSES = 3 } ActiveTab;

// Process sorting
typedef enum {
  PROC_SORT_CPU = 0,
  PROC_SORT_MEM = 1,
  PROC_SORT_RSS = 2,
  PROC_SORT_PID = 3
} ProcSort;

// Input mode for process tab
typedef enum { PROC_MODE_NORMAL = 0, PROC_MODE_FILTER = 1 } ProcInputMode;

extern char buf[1024];
static struct tb_event event = {0};

char *blocks[8]={"▁","▂","▃","▄","▅","▆","▇","█"};
char *box[8] = {"┌", "┐", "└", "┘", "─", "│", "┤", "├"};

typedef void (*draw_bars)(List *, int, int, int, int);

typedef enum { BOX, VBOX, HBOX } ContainerType;

typedef struct Container {
  ContainerType type;
  union {
    struct { 
      List *list; 
      char *title; 
      draw_bars draw_func; 
    } box;
    struct { 
      struct Container **children; 
      int count; 
    } group;
  };
} Container;

typedef struct {
  List *cpu_list;
  List *cpu_temp_list;
  List *mem_list;
  List *gpu_list;
  List *vram_list;
  List *gpu_temp_list;
  List *net_up_list;
  List *net_down_list;
  List *disk_lists[MAX_DISKS];
  char cpu_title[100];
  char cpu_temp_title[100];
  char mem_title[100];
  char gpu_title[100];
  char vram_title[100];
  char gpu_temp_title[100];
  char net_up_title[100];
  char net_down_title[100];
  char disk_titles[MAX_DISKS][100];
  int disk_count;
  DiskInfo *disk_info;
  char active_interface[32];
  short has_gpu;

  // Detailed CPU/GPU stats
  int cpu_core_count;
  float cpu_core_perc[128];
  float cpu_temp_c;
  float gpu_temp_c;
  unsigned long long gpu_vram_used_b;
  unsigned long long gpu_vram_total_b;
  volatile short running;
  pthread_mutex_t data_mutex;
  pthread_cond_t data_updated;

  // Process view
  ProcSampleCtx proc_ctx;
  ProcessInfo *proc_entries;
  int proc_count;
  int proc_selected;
  int proc_scroll;
  ActiveTab active_tab;

  ProcInputMode proc_mode;
  char proc_filter[64];
  char proc_filter_prev[64]; // New field for storing previous filter
  ProcSort proc_sort;

  //Containers
  Container cpu_box;
  Container mem_box;
  Container gpu_box;
  Container vram_box;
  Container hbox_cpu_mem;
  Container hbox_gpu_mem;
  Container net_up_box;
  Container net_down_box;
  Container hbox_net;
  Container disk_boxes[MAX_DISKS];
  Container hbox_disks;
  Container vbox_main;
  Container *hbox_cpu_mem_children[2];
  Container *hbox_gpu_mem_children[2];
  Container *hbox_net_children[2];
  Container *hbox_disk_children[MAX_DISKS];
  Container *vbox_children[4];
} SharedData;

// Global shared data
SharedData shared_data;

// Function prototypes
void draw_box(int x, int y, int x2, int y2, List *list, char *title, draw_bars draw_b);
void draw_bars_perc(List *list, int width, int height, int min_x, int min_y);
void draw_scale_bars(List *list, int width, int height, int min_x, int min_y);
void container_render_vbox(int x, int y, int width, int height, Container *container);
void container_render_hbox(int x, int y, int width, int height, Container *container);
void container_render(int x, int y, int width, int height, Container *container);
void *stats_collection_thread(void *arg);
void *render_thread(void *arg);
void setup_containers();
void cleanup_resources();
void handle_signal(int signal);
void list_trim(List *list, int width);

static void draw_tabs(int width, ActiveTab active);
static void render_process_view(int width, int height);
static void process_handle_key(uint16_t key, uint32_t ch);
static void draw_hline(int x, int y, int w);
static void render_cpu_view(int width, int height);
static void render_gpu_view(int width, int height);
static void draw_sparkline(int x, int y, int w, List *list, int max_val);
static void draw_hbar(int x, int y, int w, int percent);
static ActiveTab next_tab(ActiveTab cur);
static void draw_frame(int x, int y, int w, int h, const char *title);

int main(int argc, char *argv[]) {
  // Initialize termbox
  tb_init();

  // Ensure special keys like arrows are decoded into TB_KEY_ARROW_*.
  // Enable mouse events so tabs can be clicked.
  tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
  
  // Set up signal handling
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  
  // Initialize shared data
  pthread_mutex_init(&shared_data.data_mutex, NULL);
  pthread_cond_init(&shared_data.data_updated, NULL);
  shared_data.running = 1;

  shared_data.active_tab = TAB_OVERVIEW;
  shared_data.proc_entries = NULL;
  shared_data.proc_count = 0;
  shared_data.proc_selected = 0;
  shared_data.proc_scroll = 0;
  shared_data.proc_mode = PROC_MODE_NORMAL;
  shared_data.proc_filter[0] = '\0';
  shared_data.proc_filter_prev[0] = '\0';
  shared_data.proc_sort = PROC_SORT_CPU;
  proc_init_ctx(&shared_data.proc_ctx);
  
  // Create lists
  shared_data.cpu_list = list_create();
  shared_data.cpu_temp_list = list_create();
  shared_data.mem_list = list_create();
  shared_data.gpu_list = list_create();
  shared_data.vram_list = list_create();
  shared_data.gpu_temp_list = list_create();
  shared_data.net_up_list = list_create();
  shared_data.net_down_list = list_create();

  // Detect GPU once (layout stays stable)
  shared_data.has_gpu = gpu_available();

  shared_data.cpu_core_count = 0;
  memset(shared_data.cpu_core_perc, 0, sizeof(shared_data.cpu_core_perc));
  shared_data.cpu_temp_c = -1;
  shared_data.gpu_temp_c = -1;
  shared_data.gpu_vram_used_b = 0;
  shared_data.gpu_vram_total_b = 0;
  
  // Get active network interface
  get_active_interface(shared_data.active_interface, sizeof(shared_data.active_interface));
  
  // Initialize disk info
  shared_data.disk_info = NULL;
  shared_data.disk_count = 0;
  get_disk_info(&shared_data.disk_count);
  if (shared_data.disk_count > MAX_DISKS) shared_data.disk_count = MAX_DISKS;
  
  for (int i = 0; i < shared_data.disk_count; i++) {
    shared_data.disk_lists[i] = list_create();
  }
  
  // Set up containers for UI layout
  setup_containers();
  
  // Create threads
  pthread_t stats_thread, ui_thread;
  pthread_create(&stats_thread, NULL, stats_collection_thread, NULL);
  pthread_create(&ui_thread, NULL, render_thread, NULL);
  
  // Wait for threads to finish
  pthread_join(stats_thread, NULL);
  pthread_join(ui_thread, NULL);
  
  // Clean up resources
  cleanup_resources();
  
  return 0;
}

// Stats collection thread
void *stats_collection_thread(void *arg) {
  while (shared_data.running) {
    pthread_mutex_lock(&shared_data.data_mutex);

    // Collect CPU (total + per-core) and memory usage
    float cpu_usage = -1;
    int cores = 0;
    (void)cpu_read_perc_ex(&cpu_usage, shared_data.cpu_core_perc, (int)(sizeof(shared_data.cpu_core_perc) / sizeof(shared_data.cpu_core_perc[0])), &cores);
    shared_data.cpu_core_count = cores;
    shared_data.cpu_temp_c = cpu_temp_c();

    float ram_usage = mem_perc();
    list_append_int(shared_data.cpu_list, (int) cpu_usage);
    list_append_int(shared_data.mem_list, (int) ram_usage);

    // CPU temp history (clamped to 0..100 for percent graph)
    int cpu_tc = (shared_data.cpu_temp_c >= 0) ? (int)(shared_data.cpu_temp_c + 0.5f) : 0;
    if (cpu_tc < 0) cpu_tc = 0;
    if (cpu_tc > 100) cpu_tc = 100;
    list_append_int(shared_data.cpu_temp_list, cpu_tc);

    // Collect GPU usage if available
    if (shared_data.has_gpu) {
      float gpu_usage = -1;
      float vram_usage = -1;
      float gpu_temp = -1;
      unsigned long long used_b = 0, total_b = 0;
      (void)gpu_read_stats_ex(&gpu_usage, &vram_usage, &used_b, &total_b, &gpu_temp);
      shared_data.gpu_temp_c = gpu_temp;
      shared_data.gpu_vram_used_b = used_b;
      shared_data.gpu_vram_total_b = total_b;

      list_append_int(shared_data.gpu_list, gpu_usage >= 0 ? (int)gpu_usage : 0);
      list_append_int(shared_data.vram_list, vram_usage >= 0 ? (int)vram_usage : 0);

      int gpu_tc = (gpu_temp >= 0) ? (int)(gpu_temp + 0.5f) : 0;
      if (gpu_tc < 0) gpu_tc = 0;
      if (gpu_tc > 100) gpu_tc = 100;
      list_append_int(shared_data.gpu_temp_list, gpu_tc);

      if (gpu_usage >= 0) sprintf(shared_data.gpu_title, "Gpu: %.1f%%", gpu_usage);
      else sprintf(shared_data.gpu_title, "Gpu: N/A");

      if (vram_usage >= 0) sprintf(shared_data.vram_title, "Vram: %.1f%%", vram_usage);
      else sprintf(shared_data.vram_title, "Vram: N/A");

      if (gpu_temp >= 0) sprintf(shared_data.gpu_temp_title, "Gpu temp: %.1fC", gpu_temp);
      else sprintf(shared_data.gpu_temp_title, "Gpu temp: N/A");
    }
    
    // Collect network stats
    unsigned long download_speed, upload_speed;
    get_network_speed(&download_speed, &upload_speed, shared_data.active_interface);
    list_append_u_long(shared_data.net_up_list, upload_speed);
    list_append_u_long(shared_data.net_down_list, download_speed);
    
    // Update titles
    sprintf(shared_data.cpu_title, "Cpu: %.1f%%", cpu_usage);
    if (shared_data.cpu_temp_c >= 0) sprintf(shared_data.cpu_temp_title, "Cpu temp: %.1fC", shared_data.cpu_temp_c);
    else sprintf(shared_data.cpu_temp_title, "Cpu temp: N/A");
    sprintf(shared_data.mem_title, "Ram: %.1f%%", ram_usage);
    
    char speed_str[16];
    format_speed(speed_str, sizeof(speed_str), upload_speed);
    sprintf(shared_data.net_up_title, "N. up: %s", speed_str);
    format_speed(speed_str, sizeof(speed_str), download_speed);
    sprintf(shared_data.net_down_title, "N. down: %s", speed_str);
    
    // Collect disk stats
    if (shared_data.disk_info) free_disk_info(shared_data.disk_info);
    shared_data.disk_info = get_disk_info(&shared_data.disk_count);
    if (shared_data.disk_count > MAX_DISKS) shared_data.disk_count = MAX_DISKS;
    
    for (int i = 0; i < shared_data.disk_count; i++) {
      list_append_int(shared_data.disk_lists[i], (int)shared_data.disk_info[i].busy_percent);
      sprintf(shared_data.disk_titles[i], "%s (%s): %.2f%%", 
              shared_data.disk_info[i].device_name, 
              shared_data.disk_info[i].disk_type, 
              shared_data.disk_info[i].busy_percent);
    }
    int max_width = tb_width();

    // Trim the lists
    list_trim(shared_data.cpu_list, max_width); 
    list_trim(shared_data.cpu_temp_list, max_width);
    list_trim(shared_data.mem_list, max_width); 
    if (shared_data.has_gpu) {
      list_trim(shared_data.gpu_list, max_width);
      list_trim(shared_data.vram_list, max_width);
      list_trim(shared_data.gpu_temp_list, max_width);
    }
    list_trim(shared_data.net_up_list, max_width); 
    list_trim(shared_data.net_down_list, max_width); 
    for (int i = 0; i < shared_data.disk_count; i++) {
      list_trim(shared_data.disk_lists[i], max_width); 
    }
    
    // Process list (only sample when on process tab to reduce work)
    if (shared_data.active_tab == TAB_PROCESSES) {
      // apply selected sort mode before sampling
      proc_set_sort_mode((int)shared_data.proc_sort);

      if (shared_data.proc_entries) {
        proc_free(shared_data.proc_entries);
        shared_data.proc_entries = NULL;
        shared_data.proc_count = 0;
      }
      ProcessInfo *plist = NULL;
      int pcount = 0;
      if (proc_list(&plist, &pcount, &shared_data.proc_ctx, shared_data.proc_filter) == 0) {
        shared_data.proc_entries = plist;
        shared_data.proc_count = pcount;
        if (shared_data.proc_selected >= shared_data.proc_count) shared_data.proc_selected = shared_data.proc_count - 1;
        if (shared_data.proc_selected < 0) shared_data.proc_selected = 0;
      }
    }

    // Signal that new data is available
    pthread_cond_signal(&shared_data.data_updated);
    pthread_mutex_unlock(&shared_data.data_mutex);

    // Sleep for 1 second before collecting stats again
    usleep(1000000);
  }

  return NULL;
}

// Rendering and event handling thread
void *render_thread(void *arg) {
  int width = 0, height = 0;

  while (shared_data.running) {
    int new_width = tb_width();
    int new_height = tb_height();

    pthread_mutex_lock(&shared_data.data_mutex);

    // Check if terminal size changed
    if (new_width != width || new_height != height) {
      width = new_width;
      height = new_height;
    }

    tb_clear();

    if (width < MIN_WIDTH || height < MIN_HEIGHT) {
      tb_printf(width / 2 - (STR_LEN(ALERT_MESSAGE) / 2), height / 2, TB_RED, TB_DEFAULT, ALERT_MESSAGE);
    } else {
      // Tabs header
      draw_tabs(width, shared_data.active_tab);

      // Render active tab content below header
      if (shared_data.active_tab == TAB_OVERVIEW) {
        container_render(0, 1, width, height - 1, &shared_data.vbox_main);
      } else if (shared_data.active_tab == TAB_CPU) {
        render_cpu_view(width, height);
      } else if (shared_data.active_tab == TAB_GPU) {
        render_gpu_view(width, height);
      } else {
        render_process_view(width, height);
      }

      // Footer app name and version
      tb_printf(width - STR_LEN(APP_VERSION), height - 1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, APP_VERSION);
      tb_printf(0, height - 1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, APP_NAME);
    }

    pthread_mutex_unlock(&shared_data.data_mutex);

    tb_present();

    // Non-blocking event read so UI keeps updating.
    // Small timeout keeps CPU low while still responsive.
    event.type = 0;
    tb_peek_event(&event, 50);

    if (event.type == TB_EVENT_MOUSE) {
      // Click on the top row toggles/selects tabs
      if (event.y == 0 && event.key == TB_KEY_MOUSE_LEFT) {
        // Recompute tab hit-boxes exactly like draw_tabs()
        const char *labels[4];
        ActiveTab tabs[4];
        int count = 0;
        labels[count] = "Overview"; tabs[count++] = TAB_OVERVIEW;
        labels[count] = "CPU";      tabs[count++] = TAB_CPU;
        if (shared_data.has_gpu) { labels[count] = "GPU"; tabs[count++] = TAB_GPU; }
        labels[count] = "Processes"; tabs[count++] = TAB_PROCESSES;

        char rendered[4][64];
        int widths[4];
        int tabs_w = 0;
        for (int i = 0; i < count; i++) {
          snprintf(rendered[i], sizeof(rendered[i]), "[ %s ]", labels[i]);
          widths[i] = (int)strlen(rendered[i]);
          tabs_w += widths[i];
          if (i != count - 1) tabs_w += 1;
        }
        int start_x = (width > tabs_w) ? (width - tabs_w) / 2 : 0;

        int x = start_x;
        for (int i = 0; i < count; i++) {
          int x1 = x;
          int x2 = x + widths[i] - 1;
          if (event.x >= x1 && event.x <= x2) {
            shared_data.active_tab = tabs[i];
            if (shared_data.active_tab == TAB_OVERVIEW) shared_data.proc_mode = PROC_MODE_NORMAL;
            if (shared_data.active_tab != TAB_PROCESSES) shared_data.proc_mode = PROC_MODE_NORMAL;
            break;
          }
          x += widths[i] + 1;
        }
      }

      // Click on the process header selects sort column
      if (shared_data.active_tab == TAB_PROCESSES && shared_data.proc_mode == PROC_MODE_NORMAL &&
          event.key == TB_KEY_MOUSE_LEFT && event.y == 1) {
        // Column layout must match render_process_view() header rendering
        // PID: 0..6, S: 8..9, CPU%: 11..16, MEM%: 18..24, RSS: 26..33
        if (event.x >= 0 && event.x <= 6) shared_data.proc_sort = PROC_SORT_PID;
        else if (event.x >= 11 && event.x <= 16) shared_data.proc_sort = PROC_SORT_CPU;
        else if (event.x >= 18 && event.x <= 24) shared_data.proc_sort = PROC_SORT_MEM;
        else if (event.x >= 26 && event.x <= 33) shared_data.proc_sort = PROC_SORT_RSS;
      }
      continue;
    }

    if (event.type != TB_EVENT_KEY) continue;

    if (event.ch == 'q' || event.key == TB_KEY_CTRL_C) {
      shared_data.running = 0;
      return NULL;
    }

    // Tab switching: Tab is the only way
    if (event.key == TB_KEY_TAB) {
      shared_data.active_tab = next_tab(shared_data.active_tab);
      if (shared_data.active_tab != TAB_PROCESSES) shared_data.proc_mode = PROC_MODE_NORMAL;
      continue;
    }

    // Per-tab keys
    if (shared_data.active_tab == TAB_PROCESSES) {
      process_handle_key(event.key, event.ch);
    }
  }

  return NULL;
}

static void draw_hline(int x, int y, int w) {
  for (int i = 0; i < w; i++) tb_printf(x + i, y, TB_DEFAULT, TB_DEFAULT, "-");
}

static void draw_tabs(int width, ActiveTab active) {
  const char *labels[4];
  ActiveTab tabs[4];
  int count = 0;

  labels[count] = "Overview"; tabs[count++] = TAB_OVERVIEW;
  labels[count] = "CPU";      tabs[count++] = TAB_CPU;
  if (shared_data.has_gpu) { labels[count] = "GPU"; tabs[count++] = TAB_GPU; }
  labels[count] = "Processes"; tabs[count++] = TAB_PROCESSES;

  char rendered[4][64];
  int widths[4];
  int tabs_w = 0;
  for (int i = 0; i < count; i++) {
    snprintf(rendered[i], sizeof(rendered[i]), "[ %s ]", labels[i]);
    widths[i] = (int)strlen(rendered[i]);
    tabs_w += widths[i];
    if (i != count - 1) tabs_w += 1;
  }
  int start_x = (width > tabs_w) ? (width - tabs_w) / 2 : 0;

  uintattr_t a_fg = TB_DEFAULT | TB_BOLD;
  uintattr_t i_fg = TB_DEFAULT;

  int x = start_x;
  for (int i = 0; i < count; i++) {
    tb_printf(x, 0, active == tabs[i] ? a_fg : i_fg, TB_DEFAULT, "%s", rendered[i]);
    x += widths[i] + 1;
  }
}

static ActiveTab next_tab(ActiveTab cur) {
  // Cycle tabs: Overview -> CPU -> (GPU?) -> Processes -> Overview
  if (cur == TAB_OVERVIEW) return TAB_CPU;
  if (cur == TAB_CPU) return shared_data.has_gpu ? TAB_GPU : TAB_PROCESSES;
  if (cur == TAB_GPU) return TAB_PROCESSES;
  return TAB_OVERVIEW;
}

static void draw_sparkline(int x, int y, int w, List *list, int max_val) {
  if (!list || w <= 0) return;
  if (max_val <= 0) max_val = 100;

  int count = list->count;
  Node *node = list->first;
  while (count > w) {
    node = node->next;
    count--;
  }
  int pad = w - count;
  for (int i = 0; i < pad; i++) tb_printf(x + i, y, TB_DEFAULT, TB_DEFAULT, " ");

  int i = pad;
  while (node && i < w) {
    int v = node_get_int(node);
    if (v < 0) v = 0;
    if (v > max_val) v = max_val;
    int idx = (v * 8) / (max_val + 1);
    if (idx < 0) idx = 0;
    if (idx > 7) idx = 7;
    tb_printf(x + i, y, TB_CYAN, TB_DEFAULT, "%s", blocks[idx]);
    node = node->next;
    i++;
  }
}

static void draw_frame(int x, int y, int w, int h, const char *title) {
  if (w < 4 || h < 3) return;
  int x2 = x + w;
  int y2 = y + h;

  short skipLine = 0;
  int hLine = (y2 - y) / 2 + y - 1;

  char lineChar[2] = {(y2 - y) % 2 == 0 ? '_' : '-'};
  hLine += (lineChar[0] == '-' ? 1 : 0);

  for (int i = x + 1; i < x2 - 1; i++) {
    tb_printf(i, y, TB_DEFAULT, TB_DEFAULT, box[4]);
    tb_printf(i, y2 - 1, TB_DEFAULT, TB_DEFAULT, box[4]);
    if (skipLine) tb_printf(i, hLine, TB_DEFAULT, TB_DEFAULT, lineChar);
    skipLine = !skipLine;
  }
  for (int i = y + 1; i < y2 - 1; i++) {
    tb_printf(x, i, TB_DEFAULT, TB_DEFAULT, box[5]);
    tb_printf(x2 - 1, i, TB_DEFAULT, TB_DEFAULT, box[5]);
  }
  tb_printf(x, y, TB_DEFAULT, TB_DEFAULT, box[0]);
  tb_printf(x2 - 1, y, TB_DEFAULT, TB_DEFAULT, box[1]);
  tb_printf(x, y2 - 1, TB_DEFAULT, TB_DEFAULT, box[2]);
  tb_printf(x2 - 1, y2 - 1, TB_DEFAULT, TB_DEFAULT, box[3]);

  if (title && title[0]) {
    tb_printf(x + 2, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, " %s ", title);
  }
}

static void draw_hbar(int x, int y, int w, int percent) {
  if (w <= 0) return;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  int fill = (percent * w) / 100;
  uintattr_t color = TB_GREEN;
  if (percent >= 80) color = TB_RED;
  else if (percent >= 50) color = TB_YELLOW;

  for (int i = 0; i < w; i++) {
    if (i < fill) tb_printf(x + i, y, color, TB_DEFAULT, "█");
    else tb_printf(x + i, y, TB_DEFAULT, TB_DEFAULT, " ");
  }
}

static void render_cpu_view(int width, int height) {
  int content_y = 1;
  int usable_h = height - 2; // keep footer
  if (usable_h < 6) {
    tb_printf(0, content_y, TB_YELLOW | TB_BOLD, TB_DEFAULT, "Terminal too small for CPU view.");
    return;
  }

  // Layout:
  // [ Summary ]
  // [ History ] [ Per-core ]
  int margin = 1;
  int x0 = margin;
  int w0 = width - 2 * margin;
  int y0 = content_y;
  int sum_h = 5;
  if (sum_h > usable_h - 3) sum_h = 4;

  int below_y = y0 + sum_h;
  int below_h = usable_h - sum_h;
  if (below_h < 6) below_h = 6;

  int left_w = (w0 / 3);
  if (left_w < 26) left_w = 26;
  int right_w = w0 - left_w - 1;
  if (right_w < 30) {
    // fall back to single column
    left_w = w0;
    right_w = 0;
  }

  // Summary box
  draw_frame(x0, y0, w0, sum_h, " CPU ");
  int total = 0;
  if (shared_data.cpu_list && shared_data.cpu_list->last) total = node_get_int(shared_data.cpu_list->last);
  if (total < 0) total = 0;
  if (total > 100) total = 100;

  int sx = x0 + 2;
  int sy = y0 + 1;
  int sw = w0 - 4;
  if (sw < 10) sw = 10;

  tb_printf(sx, sy, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "Total");
  draw_hbar(sx + 7, sy, sw - 18, total);
  tb_printf(sx + sw - 8, sy, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "%3d%%", total);
  sy++;

  if (shared_data.cpu_temp_c >= 0) {
    tb_printf(sx, sy, TB_DEFAULT, TB_DEFAULT, "Temp: %.1fC   Cores: %d", shared_data.cpu_temp_c, shared_data.cpu_core_count);
  } else {
    tb_printf(sx, sy, TB_DEFAULT, TB_DEFAULT, "Temp: N/A    Cores: %d", shared_data.cpu_core_count);
  }

  // History box
  draw_frame(x0, below_y, left_w, below_h, " History ");
  int hx = x0 + 2;
  int hy = below_y + 1;
  int hw = left_w - 4;
  if (hw > 10) {
    tb_printf(hx, hy, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "CPU%%");
    draw_sparkline(hx, hy + 1, hw, shared_data.cpu_list, 100);
    hy += 3;
    tb_printf(hx, hy, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "Temp");
    draw_sparkline(hx, hy + 1, hw, shared_data.cpu_temp_list, 100);
  }

  // Per-core box
  if (right_w > 0) {
    draw_frame(x0 + left_w + 1, below_y, right_w, below_h, " Per-core ");
  } else {
    draw_frame(x0, below_y, left_w, below_h, " Per-core ");
  }

  int px = (right_w > 0) ? (x0 + left_w + 3) : (x0 + 2);
  int py = below_y + 1;
  int pw = (right_w > 0) ? (right_w - 4) : (left_w - 4);
  int ph = below_h - 2;

  if (pw < 20 || ph < 3) return;

  int cols = (pw >= 70) ? 2 : 1;
  int col_gap = 2;
  int col_w = (cols == 2) ? (pw - col_gap) / 2 : pw;
  int rows = ph;
  if (rows < 1) rows = 1;

  int label_w = 6;
  int pct_w = 5;
  int bar_w = col_w - label_w - pct_w - 2;
  if (bar_w < 8) bar_w = 8;

  int max_cores = shared_data.cpu_core_count;
  int cap = (int)(sizeof(shared_data.cpu_core_perc) / sizeof(shared_data.cpu_core_perc[0]));
  if (max_cores > cap) max_cores = cap;
  int max_show = rows * cols;
  int to_show = max_cores;
  if (to_show > max_show) to_show = max_show;

  for (int i = 0; i < to_show; i++) {
    int col = (cols == 2) ? (i / rows) : 0;
    int row = (cols == 2) ? (i % rows) : i;
    int rx = px + col * (col_w + col_gap);
    int ry = py + row;

    float p = shared_data.cpu_core_perc[i];
    int ip = (p >= 0) ? (int)(p + 0.5f) : 0;
    if (ip < 0) ip = 0;
    if (ip > 100) ip = 100;

    tb_printf(rx, ry, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "c%02d", i);
    draw_hbar(rx + label_w, ry, bar_w, ip);
    tb_printf(rx + label_w + bar_w + 1, ry, TB_DEFAULT, TB_DEFAULT, "%3d%%", ip);
  }

  if (to_show < max_cores) {
    tb_printf(px, below_y + below_h - 2, TB_YELLOW | TB_BOLD, TB_DEFAULT,
              "Showing %d/%d cores (enlarge terminal)", to_show, max_cores);
  }
}

static void render_gpu_view(int width, int height) {
  int content_y = 1;
  int usable_h = height - 2;
  if (usable_h < 6) {
    tb_printf(0, content_y, TB_YELLOW | TB_BOLD, TB_DEFAULT, "Terminal too small for GPU view.");
    return;
  }

  int margin = 1;
  int x0 = margin;
  int w0 = width - 2 * margin;
  int y0 = content_y;

  draw_frame(x0, y0, w0, usable_h, " GPU ");

  int x = x0 + 2;
  int y = y0 + 1;
  int w = w0 - 4;

  if (!shared_data.has_gpu) {
    tb_printf(x, y, TB_YELLOW | TB_BOLD, TB_DEFAULT, "No GPU detected (tab will appear only if detected on startup). ");
    return;
  }

  int util = 0;
  int vram = 0;
  if (shared_data.gpu_list && shared_data.gpu_list->last) util = node_get_int(shared_data.gpu_list->last);
  if (shared_data.vram_list && shared_data.vram_list->last) vram = node_get_int(shared_data.vram_list->last);
  if (util < 0) util = 0;
  if (util > 100) util = 100;
  if (vram < 0) vram = 0;
  if (vram > 100) vram = 100;

  // Util bar
  tb_printf(x, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "Util");
  draw_hbar(x + 6, y, w - 16, util);
  tb_printf(x + w - 5, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "%3d%%", util);
  y++;

  // VRAM bar
  tb_printf(x, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "VRAM");
  draw_hbar(x + 6, y, w - 16, vram);
  tb_printf(x + w - 5, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "%3d%%", vram);
  y++;

  if (shared_data.gpu_temp_c >= 0) tb_printf(x, y++, TB_DEFAULT, TB_DEFAULT, "Temp: %.1fC", shared_data.gpu_temp_c);
  else tb_printf(x, y++, TB_DEFAULT, TB_DEFAULT, "Temp: N/A");

  if (shared_data.gpu_vram_total_b > 0) {
    double used_mib = (double)shared_data.gpu_vram_used_b / (1024.0 * 1024.0);
    double total_mib = (double)shared_data.gpu_vram_total_b / (1024.0 * 1024.0);
    tb_printf(x, y++, TB_DEFAULT, TB_DEFAULT, "VRAM: %.0f MiB / %.0f MiB", used_mib, total_mib);
  }

  y++;
  int spark_w = w - 12;
  if (spark_w > 10) {
    tb_printf(x, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "Util hist");
    draw_sparkline(x, y + 1, w, shared_data.gpu_list, 100);
    y += 3;

    tb_printf(x, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "Temp hist");
    draw_sparkline(x, y + 1, w, shared_data.gpu_temp_list, 100);
  }
}

static void render_process_view(int width, int height) {
  int header_y = 1;
  int list_y = 2;
  int list_h = height - 4; // leave room for status bar above global footer
  int status_y = height - 2;

  // Header
  if (shared_data.proc_mode == PROC_MODE_FILTER) {
    tb_printf(0, header_y, TB_DEFAULT | TB_BOLD, TB_DEFAULT,
              "Filter: /%s ", shared_data.proc_filter);

    // also show status bar while filtering
    for (int x = 0; x < width; x++) tb_printf(x, status_y, TB_DEFAULT, TB_DEFAULT, " ");
    tb_printf(0, status_y, TB_DEFAULT | TB_BOLD, TB_DEFAULT,
              " Tab=switch tabs   Enter=apply   Esc=cancel   Backspace=delete ");
  } else {
    const char *sort_name = "CPU%";
    if (shared_data.proc_sort == PROC_SORT_MEM) sort_name = "MEM%";
    else if (shared_data.proc_sort == PROC_SORT_RSS) sort_name = "RSS";
    else if (shared_data.proc_sort == PROC_SORT_PID) sort_name = "PID";

    // Draw column headers with per-column highlighting
    int x = 0;
    uintattr_t on = TB_YELLOW | TB_BOLD;
    uintattr_t off = TB_DEFAULT | TB_BOLD;

    // PID (0..6)
    tb_printf(x, header_y, shared_data.proc_sort == PROC_SORT_PID ? on : off, TB_DEFAULT, "%-7s", "PID");
    x += 7;
    tb_printf(x++, header_y, off, TB_DEFAULT, " ");

    // State
    tb_printf(x, header_y, off, TB_DEFAULT, "%-2s", "S");
    x += 2;
    tb_printf(x++, header_y, off, TB_DEFAULT, " ");

    // CPU%
    tb_printf(x, header_y, shared_data.proc_sort == PROC_SORT_CPU ? on : off, TB_DEFAULT, "%6s", "CPU%");
    x += 6;
    tb_printf(x++, header_y, off, TB_DEFAULT, " ");

    // MEM%
    tb_printf(x, header_y, shared_data.proc_sort == PROC_SORT_MEM ? on : off, TB_DEFAULT, "%7s", "MEM%");
    x += 7;
    tb_printf(x++, header_y, off, TB_DEFAULT, " ");

    // RSS
    tb_printf(x, header_y, shared_data.proc_sort == PROC_SORT_RSS ? on : off, TB_DEFAULT, "%8s", "RSS(KB)");
    x += 8;
    tb_printf(x, header_y, off, TB_DEFAULT, "  ");
    x += 2;

    // COMMAND
    tb_printf(x, header_y, off, TB_DEFAULT, "%-s", "COMMAND");

    // Bottom status bar
    for (int i = 0; i < width; i++) tb_printf(i, status_y, TB_DEFAULT, TB_DEFAULT, " ");
    tb_printf(0, status_y, TB_DEFAULT | TB_BOLD, TB_DEFAULT,
              " Tab=switch tabs   /=filter   1=CPU 2=MEM 3=RSS 4=PID   x=SIGTERM  X=SIGKILL   sort:%s ",
              sort_name);
  }

  if (!shared_data.proc_entries || shared_data.proc_count <= 0) {
    tb_printf(0, list_y, TB_YELLOW | TB_BOLD, TB_DEFAULT, "No process data yet (wait 1s) or insufficient permissions.");
    return;
  }

  if (shared_data.proc_selected < 0) shared_data.proc_selected = 0;
  if (shared_data.proc_selected >= shared_data.proc_count) shared_data.proc_selected = shared_data.proc_count - 1;

  if (shared_data.proc_selected < shared_data.proc_scroll) shared_data.proc_scroll = shared_data.proc_selected;
  if (shared_data.proc_selected >= shared_data.proc_scroll + list_h) shared_data.proc_scroll = shared_data.proc_selected - list_h + 1;
  if (shared_data.proc_scroll < 0) shared_data.proc_scroll = 0;

  for (int row = 0; row < list_h; row++) {
    int idx = shared_data.proc_scroll + row;
    if (idx >= shared_data.proc_count) break;

    ProcessInfo *p = &shared_data.proc_entries[idx];

    // More readable highlight: keep background default, just make text bold + cyan
    uintattr_t fg = (idx == shared_data.proc_selected) ? (TB_CYAN | TB_BOLD) : TB_DEFAULT;
    uintattr_t bg = TB_DEFAULT;

    char line[256];
    snprintf(line, sizeof(line), "%-7d %-2c %6.1f %7.1f %8lu  %.60s",
             p->pid, p->state ? p->state : '?', p->cpu_percent, p->mem_percent, p->rss_kb, p->comm);

    for (int x = 0; x < width; x++) tb_printf(x, list_y + row, fg, bg, " ");
    tb_printf(0, list_y + row, fg, bg, "%.*s", width, line);
  }
}

static void process_handle_key( uint16_t key, uint32_t ch) {
  // Filter mode eats most keys
  if (shared_data.proc_mode == PROC_MODE_FILTER) {
    if (key == TB_KEY_ESC) {
      // cancel: restore previous filter
      strncpy(shared_data.proc_filter, shared_data.proc_filter_prev, sizeof(shared_data.proc_filter));
      shared_data.proc_filter[sizeof(shared_data.proc_filter) - 1] = '\0';
      shared_data.proc_mode = PROC_MODE_NORMAL;
      return;
    }
    if (key == TB_KEY_ENTER) {
      shared_data.proc_mode = PROC_MODE_NORMAL;
      // apply: commit filter
      strncpy(shared_data.proc_filter_prev, shared_data.proc_filter, sizeof(shared_data.proc_filter_prev));
      shared_data.proc_filter_prev[sizeof(shared_data.proc_filter_prev) - 1] = '\0';
      shared_data.proc_selected = 0;
      shared_data.proc_scroll = 0;
      return;
    }
    if (key == TB_KEY_BACKSPACE || key == TB_KEY_BACKSPACE2) {
      size_t n = strlen(shared_data.proc_filter);
      if (n > 0) shared_data.proc_filter[n - 1] = '\0';
      return;
    }
    if (ch >= 32 && ch <= 126) {
      size_t n = strlen(shared_data.proc_filter);
      if (n + 1 < sizeof(shared_data.proc_filter)) {
        shared_data.proc_filter[n] = (char)ch;
        shared_data.proc_filter[n + 1] = '\0';
      }
      return;
    }
    return;
  }

  // Normal mode
  if (ch == '/') {
    shared_data.proc_mode = PROC_MODE_FILTER;
    // keep current filter for editing; also snapshot for cancel
    strncpy(shared_data.proc_filter_prev, shared_data.proc_filter, sizeof(shared_data.proc_filter_prev));
    shared_data.proc_filter_prev[sizeof(shared_data.proc_filter_prev) - 1] = '\0';
    return;
  }

  // Sort selection (numbers)
  if (ch == '1') { shared_data.proc_sort = PROC_SORT_CPU; return; }
  if (ch == '2') { shared_data.proc_sort = PROC_SORT_MEM; return; }
  if (ch == '3') { shared_data.proc_sort = PROC_SORT_RSS; return; }
  if (ch == '4') { shared_data.proc_sort = PROC_SORT_PID; return; }

  if (ch == 'j' || key == TB_KEY_ARROW_DOWN) {
    if (shared_data.proc_selected < shared_data.proc_count - 1) shared_data.proc_selected++;
  } else if (ch == 'k' || key == TB_KEY_ARROW_UP) {
    if (shared_data.proc_selected > 0) shared_data.proc_selected--;
  } else if (key == TB_KEY_PGDN) {
    shared_data.proc_selected += 10;
    if (shared_data.proc_selected >= shared_data.proc_count) shared_data.proc_selected = shared_data.proc_count - 1;
  } else if (key == TB_KEY_PGUP) {
    shared_data.proc_selected -= 10;
    if (shared_data.proc_selected < 0) shared_data.proc_selected = 0;
  } else if (ch == 'x') {
    if (shared_data.proc_entries && shared_data.proc_count > 0) {
      int pid = shared_data.proc_entries[shared_data.proc_selected].pid;
      proc_kill(pid, SIGTERM);
    }
  } else if (ch == 'X') {
    if (shared_data.proc_entries && shared_data.proc_count > 0) {
      int pid = shared_data.proc_entries[shared_data.proc_selected].pid;
      proc_kill(pid, SIGKILL);
    }
  }
}

void setup_containers() {
  pthread_mutex_lock(&shared_data.data_mutex);
  
  // Set up CPU and memory boxes
  shared_data.cpu_box = (Container){BOX, .box = {shared_data.cpu_list, shared_data.cpu_title, draw_bars_perc}};
  shared_data.mem_box = (Container){BOX, .box = {shared_data.mem_list, shared_data.mem_title, draw_bars_perc}};
  shared_data.gpu_box = (Container){BOX, .box = {shared_data.gpu_list, shared_data.gpu_title, draw_bars_perc}};
  shared_data.vram_box = (Container){BOX, .box = {shared_data.vram_list, shared_data.vram_title, draw_bars_perc}};
  shared_data.net_up_box = (Container){BOX, .box = {shared_data.net_up_list, shared_data.net_up_title, draw_scale_bars}};
  shared_data.net_down_box = (Container){BOX, .box = {shared_data.net_down_list, shared_data.net_down_title, draw_scale_bars}};

  // CPU+RAM row when GPU exists
  shared_data.hbox_cpu_mem_children[0] = &shared_data.cpu_box;
  shared_data.hbox_cpu_mem_children[1] = &shared_data.mem_box;
  shared_data.hbox_cpu_mem = (Container){HBOX, .group = {shared_data.hbox_cpu_mem_children, 2}};

  // GPU+VRAM row when GPU exists
  shared_data.hbox_gpu_mem_children[0] = &shared_data.gpu_box;
  shared_data.hbox_gpu_mem_children[1] = &shared_data.vram_box;
  shared_data.hbox_gpu_mem = (Container){HBOX, .group = {shared_data.hbox_gpu_mem_children, 2}};
  
  // Create network container
  shared_data.hbox_net_children[0] = &shared_data.net_up_box;
  shared_data.hbox_net_children[1] = &shared_data.net_down_box;
  shared_data.hbox_net = (Container){HBOX, .group = {shared_data.hbox_net_children, 2}};
  
  // Set up disk boxes
  for (int i = 0; i < shared_data.disk_count; i++) {
    shared_data.disk_boxes[i] = (Container){BOX, .box = {shared_data.disk_lists[i], shared_data.disk_titles[i], draw_bars_perc}};
    shared_data.hbox_disk_children[i] = &shared_data.disk_boxes[i];
  }
  
  // Create disk container (horizontal layout)
  shared_data.hbox_disks = (Container){HBOX, .group = {shared_data.hbox_disk_children, shared_data.disk_count}};
  
  // Main container
  if (shared_data.has_gpu) {
    shared_data.vbox_children[0] = &shared_data.hbox_cpu_mem;
    shared_data.vbox_children[1] = &shared_data.hbox_gpu_mem;
    shared_data.vbox_children[2] = &shared_data.hbox_net;
    shared_data.vbox_children[3] = &shared_data.hbox_disks;
  } else {
    // Keep existing layout when no GPU
    shared_data.vbox_children[0] = &shared_data.cpu_box;
    shared_data.vbox_children[1] = &shared_data.mem_box;
    shared_data.vbox_children[2] = &shared_data.hbox_net;
    shared_data.vbox_children[3] = &shared_data.hbox_disks;
  }
  shared_data.vbox_main = (Container){VBOX, .group = {shared_data.vbox_children, 4}};
  
  pthread_mutex_unlock(&shared_data.data_mutex);
}

void cleanup_resources() {
  // Free resources and clean up
  tb_shutdown();
  
  // Free lists
  list_free(shared_data.cpu_list);
  list_free(shared_data.cpu_temp_list);
  list_free(shared_data.mem_list);
  list_free(shared_data.gpu_list);
  list_free(shared_data.vram_list);
  list_free(shared_data.gpu_temp_list);
  list_free(shared_data.net_up_list);
  list_free(shared_data.net_down_list);
  
  for (int i = 0; i < shared_data.disk_count; i++) {
    list_free(shared_data.disk_lists[i]);
  }
  
  if (shared_data.disk_info) free_disk_info(shared_data.disk_info);
  if (shared_data.proc_entries) proc_free(shared_data.proc_entries);
  proc_free_ctx(&shared_data.proc_ctx);

  // Destroy synchronization primitives
  pthread_mutex_destroy(&shared_data.data_mutex);
  pthread_cond_destroy(&shared_data.data_updated);
}

void handle_signal(int signal) {
  // Set the running flag to false to terminate threads
  shared_data.running = 0;
}

// Keep the original drawing functions unchanged
void draw_box(int x, int y, int x2, int y2, List *list, char* title, draw_bars draw_b) {
  short skipLine = 0;
  int hLine = (y2 - y)/2 + y -1;

  char lineChar[2] = {(y2-y)%2==0?'_':'-'};
  hLine+=(lineChar[0]=='-'?1:0);
  for(int i=x+1;i<x2-1;i++){
    tb_printf(i, y, TB_DEFAULT, TB_DEFAULT, box[4]); 
    tb_printf(i, y2-1, TB_DEFAULT, TB_DEFAULT, box[4]);
    if(skipLine){
        tb_printf(i , hLine, TB_DEFAULT, TB_DEFAULT, lineChar);
    }
    skipLine = !skipLine;
  }
  for(int i=y+1;i<y2-1;i++){
    tb_printf(x, i, TB_DEFAULT, TB_DEFAULT, box[5]);
    tb_printf(x2-1, i, TB_DEFAULT, TB_DEFAULT, box[5]);
  }
  tb_printf(x, y, TB_DEFAULT, TB_DEFAULT, box[0]);
  tb_printf(x2-1, y, TB_DEFAULT, TB_DEFAULT, box[1]);
  tb_printf(x, y2-1, TB_DEFAULT, TB_DEFAULT, box[2]);
  tb_printf(x2-1, y2-1, TB_DEFAULT, TB_DEFAULT, box[3]);
  tb_printf(x+2, y, TB_DEFAULT | TB_BOLD, TB_DEFAULT, " %s ", title);

  draw_b(list, (x2-1)-(x+1), (y2-1)-(y+1), x+1, y+1);
}

void draw_bars_perc(List *list, int width, int height, int min_x, int min_y) {

  int count = list->count;
  Node *node = list->first;
  while(count > width){
    node = node->next;
    count--;
  }
  int x = width - count;
  while (node != NULL && x < width) {
    int bar_h = (node_get_int(node) * height) / 100; // Full blocks
    int bar_h_e = ((node_get_int(node) * height) % 100) * 8 / 100; // Extra fractional block
    // Draw blocks from bottom to top
    for (int y = height - 1; y >= 0; y--) {
      uintattr_t color = TB_RED;
      if(y > height/2) color = TB_GREEN;
      else if(y > height*1/4 - 1) color = TB_YELLOW;
      
      if (y >= height - bar_h) {
        tb_printf(min_x+x, min_y+y, color, TB_DEFAULT, "█"); // Full block
      } else if (y == height - bar_h - 1 && bar_h_e > 0) {
        tb_printf(min_x+x, min_y+y, color, TB_DEFAULT, "%s", blocks[bar_h_e - 1]); // Partial block
      }
    }

    node = node->next;
    x++;
  }
}

void draw_scale_bars(List *list, int width, int height, int min_x, int min_y) {
  int count = list->count;
  Node *node = list->first;
  while(count > width){
    node = node->next;
    count--;
  }
  Node *node_ref = node; 
  unsigned long max_value = 1;
  short max_value_change = 0;
  while (node != NULL) {
    unsigned long curr=node_get_u_long(node);
    if(curr>0) max_value_change=1;
    max_value = curr > max_value ? curr : max_value;
      node = node->next;
  }

  char max_str[50] = "";
  format_speed(max_str, sizeof(max_str),max_value_change? max_value:0);
  char max_str_present[50] = "max: ";
  strcat(max_str_present, max_str);
  tb_printf(min_x + width - (strlen(max_str_present)) - 2, min_y -1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, " %s ", max_str_present);
  
  int x = width - count;
  node = node_ref;
  while (node != NULL && x < width) {
    int bar_h = (int)((node_get_u_long(node) * height) / max_value);
    int bar_h_e = (int)(((node_get_u_long(node) * height) % max_value) * 8 / max_value);

    for (int y = height - 1; y >= 0; y--) {
      if (y >= height - bar_h) {
        tb_printf(min_x + x, min_y + y, TB_BLUE, TB_DEFAULT, "█");
      } else if (y == height - bar_h - 1 && bar_h_e > 0) {
        tb_printf(min_x + x, min_y + y, TB_BLUE, TB_DEFAULT, "%s", blocks[bar_h_e - 1]);
      }
    }
    node = node->next;
    x++;
  }
}

void container_render(int x, int y, int width, int height, Container *container) {
  if (!container) return;  // Add null check to prevent segfault
  
  if (container->type == BOX) {
    draw_box(x, y, x + width, y + height, container->box.list, container->box.title, container->box.draw_func);
  } else if (container->type == HBOX) {
    container_render_hbox(x, y, width, height, container);
  } else if (container->type == VBOX) {
    container_render_vbox(x, y, width, height, container);
  }
}

void container_render_vbox(int x, int y, int width, int height, Container *container) {
  if (!container || !container->group.count) return;  // Add null check
  
  int box_height = height / container->group.count;
  for (int i = 0; i < container->group.count; i++) {
    int h = (i == container->group.count - 1) ? height - i * box_height : box_height;
    Container *child = container->group.children[i];
    if (child) {  // Add null check for child
      container_render(x, y + i * box_height, width, h, child);
    }
  }
}

void container_render_hbox(int x, int y, int width, int height, Container *container) {
  if (!container || !container->group.count) return;  // Add null check
  
  int box_width = width / container->group.count;
  for (int i = 0; i < container->group.count; i++) {
    int w = (i == container->group.count - 1) ? width - i * box_width : box_width; 
    Container *child = container->group.children[i];
    if (child) {  // Add null check for child
      container_render(x + i * box_width, y, w, height, child);
    }
  }
}

void list_trim(List *list, int width){
  while (list->count > width) {
    Node *old_node = list->first;
    list->first = old_node->next;
    free(old_node->value);
    free(old_node);
    list->count--;
  }
}
