#define TB_IMPL
#include "termbox.h"
#include "modules.h"
#include "utils.h"
#include <pthread.h>
#include <signal.h>

#define MAX_DISKS 8
#define STR_LEN(s) (sizeof(s) - 1) 
#define APP_NAME " vitals "
#define APP_VERSION " 0.0.1 "
#define ALERT_MESSAGE "The terminal is too small."
#define MIN_WIDTH 80
#define MIN_HEIGHT 20

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

// Shared data structures and synchronization
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

int main(int argc, char *argv[]) {
  // Initialize termbox
  tb_init();
  
  // Set up signal handling
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  
  // Initialize shared data
  pthread_mutex_init(&shared_data.data_mutex, NULL);
  pthread_cond_init(&shared_data.data_updated, NULL);
  shared_data.running = 1;
  
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
    list_append_int(shared_data.cpu_list, cpu_perc());
    list_append_int(shared_data.mem_list, mem_perc());
    
    // Collect network stats
    unsigned long download_speed, upload_speed;
    get_network_speed(&download_speed, &upload_speed, shared_data.active_interface);
    list_append_u_long(shared_data.net_up_list, upload_speed);
    list_append_u_long(shared_data.net_down_list, download_speed);
    
    // Update titles
    sprintf(shared_data.cpu_title, "Cpu: %i%%", node_get_int(shared_data.cpu_list->last));
    sprintf(shared_data.mem_title, "Ram: %i%%", node_get_int(shared_data.mem_list->last));
    
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
      sprintf(shared_data.disk_titles[i], "%s (%s): %i%%", 
              shared_data.disk_info[i].device_name, 
              shared_data.disk_info[i].disk_type, 
              (int)shared_data.disk_info[i].busy_percent);
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
      // Render the UI
      container_render(0, 0, width, height, &shared_data.vbox_main);
      
      // Draw app name and version
      tb_printf(width - STR_LEN(APP_VERSION), height - 1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, APP_VERSION);
      tb_printf(0, height - 1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, APP_NAME);
    }
    
    pthread_mutex_unlock(&shared_data.data_mutex);
    
    // Present the rendered UI
    tb_present();
    
    // Check for events (non-blocking)
    tb_peek_event(&event, 1000);
    if (event.ch == 'q' || event.key == TB_KEY_CTRL_C || event.key == TB_KEY_ESC) {
      shared_data.running = 0;
      return NULL;
    }
      
  }
  
  return NULL;
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
  while (list->count > width) {
    Node *old_node = list->first;
    list->first = old_node->next;
    free(old_node);
    list->count--;
  }
  int x = width - list->count;
  Node *node = list->first;
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
  while (list->count > width) {
    Node *old_node = list->first;
    list->first = old_node->next;
    free(old_node);
    list->count--;
  }
  unsigned long max_value = 1;
  short max_value_change = 0;
  Node *node = list->first;
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
  
  int x = width - list->count;
  node = list->first;
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

