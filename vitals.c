#define TB_IMPL
#include "termbox.h"
#include "modules.h"
#include "utils.h"
#include <pthread.h>
#include <signal.h>

#define MAX_DISKS 8
#define STR_LEN(s) (sizeof(s) - 1) 
#define APP_NAME " vitals "
#define APP_VERSION " 0.0.3 "
#define ALERT_MESSAGE "The terminal is too small."
#define MIN_WIDTH 80
#define MIN_HEIGHT 20

// Tabs
typedef enum { TAB_VITALS = 0, TAB_PROCESSES = 1 } ActiveTab;

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
  List *mem_list;
  List *net_up_list;
  List *net_down_list;
  List *disk_lists[MAX_DISKS];
  char cpu_title[100];
  char mem_title[100];
  char net_up_title[100];
  char net_down_title[100];
  char disk_titles[MAX_DISKS][100];
  int disk_count;
  DiskInfo *disk_info;
  char active_interface[32];
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
  Container net_up_box;
  Container net_down_box;
  Container hbox_net;
  Container disk_boxes[MAX_DISKS];
  Container hbox_disks;
  Container vbox_main;
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
static void process_handle_key(short key, uint32_t ch);
static void draw_hline(int x, int y, int w);

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

  shared_data.active_tab = TAB_VITALS;
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
  shared_data.mem_list = list_create();
  shared_data.net_up_list = list_create();
  shared_data.net_down_list = list_create();
  
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

    // Collect CPU and memory usage
    float cpu_usage = cpu_perc();
    float ram_usage = mem_perc();
    list_append_int(shared_data.cpu_list, (int) cpu_usage);
    list_append_int(shared_data.mem_list, (int) ram_usage);
    
    // Collect network stats
    unsigned long download_speed, upload_speed;
    get_network_speed(&download_speed, &upload_speed, shared_data.active_interface);
    list_append_u_long(shared_data.net_up_list, upload_speed);
    list_append_u_long(shared_data.net_down_list, download_speed);
    
    // Update titles
    sprintf(shared_data.cpu_title, "Cpu: %.1f%%", cpu_usage);
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
    list_trim(shared_data.mem_list, max_width); 
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
      if (shared_data.active_tab == TAB_VITALS) {
        container_render(0, 1, width, height - 1, &shared_data.vbox_main);
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
        const char *t1 = "Overview";
        const char *t2 = "Processes";
        char left[64];
        char right[64];
        snprintf(left, sizeof(left), "[ %s ]", t1);
        snprintf(right, sizeof(right), "[ %s ]", t2);
        int tabs_w = (int)strlen(left) + 1 + (int)strlen(right);
        int start_x = (width > tabs_w) ? (width - tabs_w) / 2 : 0;

        int left_x1 = start_x;
        int left_x2 = start_x + (int)strlen(left) - 1;
        int right_x1 = start_x + (int)strlen(left) + 1;
        int right_x2 = right_x1 + (int)strlen(right) - 1;

        if (event.x >= left_x1 && event.x <= left_x2) {
          shared_data.active_tab = TAB_VITALS;
          shared_data.proc_mode = PROC_MODE_NORMAL;
        } else if (event.x >= right_x1 && event.x <= right_x2) {
          shared_data.active_tab = TAB_PROCESSES;
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

    if (event.ch == 'q' || event.key == TB_KEY_CTRL_C || event.key == TB_KEY_ESC) {
      shared_data.running = 0;
      return NULL;
    }

    // Tab switching: Tab is the only way
    if (event.key == TB_KEY_TAB) {
      shared_data.active_tab = (shared_data.active_tab == TAB_VITALS) ? TAB_PROCESSES : TAB_VITALS;
      if (shared_data.active_tab == TAB_VITALS) shared_data.proc_mode = PROC_MODE_NORMAL;
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
  // Centered tabs: [ Overview ] [ Processes ]
  const char *t1 = "Overview";
  const char *t2 = "Processes";

  char left[64];
  char right[64];
  snprintf(left, sizeof(left), "[ %s ]", t1);
  snprintf(right, sizeof(right), "[ %s ]", t2);

  int tabs_w = (int)strlen(left) + 1 + (int)strlen(right);
  int start_x = (width > tabs_w) ? (width - tabs_w) / 2 : 0;

  uintattr_t a_fg = TB_DEFAULT | TB_BOLD;
  uintattr_t i_fg = TB_DEFAULT;

  tb_printf(start_x, 0, active == TAB_VITALS ? a_fg : i_fg, TB_DEFAULT, "%s", left);
  tb_printf(start_x + (int)strlen(left) + 1, 0, active == TAB_PROCESSES ? a_fg : i_fg, TB_DEFAULT, "%s", right);
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

static void process_handle_key(short key, uint32_t ch) {
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
  shared_data.net_up_box = (Container){BOX, .box = {shared_data.net_up_list, shared_data.net_up_title, draw_scale_bars}};
  shared_data.net_down_box = (Container){BOX, .box = {shared_data.net_down_list, shared_data.net_down_title, draw_scale_bars}};
  
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
  shared_data.vbox_children[0] = &shared_data.cpu_box;
  shared_data.vbox_children[1] = &shared_data.mem_box;
  shared_data.vbox_children[2] = &shared_data.hbox_net;
  shared_data.vbox_children[3] = &shared_data.hbox_disks;
  shared_data.vbox_main = (Container){VBOX, .group = {shared_data.vbox_children, 4}};
  
  pthread_mutex_unlock(&shared_data.data_mutex);
}

void cleanup_resources() {
  // Free resources and clean up
  tb_shutdown();
  
  // Free lists
  list_free(shared_data.cpu_list);
  list_free(shared_data.mem_list);
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
