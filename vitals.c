#define TB_IMPL
#include "termbox.h"
#include "modules.h"
#include "utils.h"
#define MAX_DISKS 8
#define NAME_STR_LEN(s) (sizeof(s) - 1) 
#define APP_NAME " vitals "
#define APP_VERSION " 0.0.1 "
#define ALERT_MESSAGE "The terminal is too small."
#define MIN_WIDTH 80
#define MIN_HEIGHT 20

extern char buf[1024];
static struct tb_event event = {0};
int cpu_perc();

char *blocks[8]={"▁","▂","▃","▄","▅","▆","▇","█"};
//char *box[8] = {"╔", "╗", "╚", "╝", "─", "│", "┤", "├"};
char *box[8] = {"┌", "┐", "└", "┘", "─", "│", "┤", "├"};
//char *box[8] = {"━", "━", "━", "━", "━", "┃", "━", "━"};




typedef void (*draw_bars)(List *, int, int, int, int);

void draw_box(int x,int y,int x2,int y2,List *list, char *title, draw_bars draw_b);
void draw_bars_perc(List *list, int width, int height, int min_x, int min_y );
void draw_scale_bars(List *list, int width, int height, int min_x, int min_y);


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

void container_render_vbox(int x, int y, int width, int height, Container *container);

void container_render_hbox(int x, int y, int width, int height, Container *container);

void container_render(int x, int y, int width, int height, Container *container) ;


int main(int argc, char *argv[]) {
  tb_init();
  List *cpu_list = list_create();
  List *mem_list = list_create();
  List *net_up_list = list_create();
  List *net_down_list = list_create();


  List *disk_lists[MAX_DISKS];
  char disk_titles[MAX_DISKS][100];
  int disk_count = 0;
  DiskInfo *disk_info = NULL;

  get_disk_info(&disk_count);
  if (disk_count > MAX_DISKS) disk_count = MAX_DISKS;
  
  for (int i = 0; i < disk_count; i++) {
    disk_lists[i] = list_create();
  }

  char cpu_title[100] = "";
  char mem_title[100] = "";
  char net_up_title[100] = "";
  char net_down_title[100] = "";
  char speed_str[16];
  int width,height;
  int max_cpu=15;

  char active_interface[32];

  get_active_interface(active_interface, sizeof(active_interface));
  unsigned long download_speed, upload_speed;

  // Set up disk boxes
  Container disk_boxes[MAX_DISKS];
  for (int i = 0; i < disk_count; i++) {
    disk_boxes[i] = (Container){BOX, .box = {disk_lists[i], disk_titles[i], draw_bars_perc}};
  }

  // Set up CPU and memory boxes
  Container cpu_box = {BOX, .box = {cpu_list, cpu_title, draw_bars_perc}};
  Container mem_box = {BOX, .box = {mem_list, mem_title, draw_bars_perc}};
  Container net_up_box = {BOX, .box = {net_up_list, net_up_title, draw_scale_bars}};
  Container net_down_box = {BOX, .box = {net_down_list, net_down_title, draw_scale_bars}};


  // Create network container
  Container *hbox_net_children[] = {&net_up_box, &net_down_box};
  Container hbox_net = {HBOX, .group = {hbox_net_children, 2}};

  // Create disk container (horizontal layout)
  Container *hbox_disk_children[MAX_DISKS];
  for (int i = 0; i < disk_count; i++) {
    hbox_disk_children[i] = &disk_boxes[i];
  }
  Container hbox_disks = {HBOX, .group = {hbox_disk_children, disk_count}};

  // Main container
  Container *vbox_children[] = {&cpu_box, &mem_box, &hbox_net, &hbox_disks};
  Container vbox_main = {VBOX, .group = {vbox_children, 4}};


  
  while (1) {
    
    int new_width = tb_width();
    int new_height = tb_height();

    if (new_width < MIN_WIDTH || new_height < MIN_HEIGHT) {
      tb_clear();
      tb_printf(new_width / 2 - (NAME_STR_LEN(ALERT_MESSAGE) / 2), new_height / 2, TB_RED, TB_DEFAULT, ALERT_MESSAGE);
    }else{

      if(new_width!=width|| new_height!=height){
        width=new_width;
        height=new_height;
      }
      get_network_speed(&download_speed, &upload_speed,active_interface);
      tb_clear();
      
      // Add new CPU usage percentage
      list_append_int(cpu_list, cpu_perc());
      // Add new ram usage percentage
      list_append_int(mem_list, mem_perc());
      // Add new net upload speed
      list_append_u_long(net_up_list, upload_speed);
      // Add new net download speed
      list_append_u_long(net_down_list, download_speed);

      // Get disk info and update disk data
      if (disk_info) free_disk_info(disk_info);
      disk_info = get_disk_info(&disk_count);
      if (disk_count > MAX_DISKS) disk_count = MAX_DISKS;
      
      for (int i = 0; i < disk_count; i++) {
        list_append_int(disk_lists[i], (int)disk_info[i].busy_percent);
        sprintf(disk_titles[i], "%s (%s): %i%%", 
                disk_info[i].device_name, 
                disk_info[i].disk_type, 
                (int) disk_info[i].busy_percent);
      }


      sprintf(cpu_title,"Cpu: %i%%",node_get_int(cpu_list->last));
      sprintf(mem_title,"Ram: %i%%",node_get_int(mem_list->last));
      format_speed(speed_str, sizeof(speed_str), upload_speed);
      sprintf(net_up_title,"Net. up: %s",speed_str);
      format_speed(speed_str, sizeof(speed_str), download_speed);
      sprintf(net_down_title,"Net. down: %s",speed_str);

      container_render(0, 0, width, height, &vbox_main);
    

      tb_printf(width-NAME_STR_LEN(APP_VERSION), height-1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, APP_VERSION);
      tb_printf(0, height-1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, APP_NAME);
    }
    tb_peek_event(&event, 10);
    tb_present();
    if(event.ch=='q') break;
    usleep(1000000); // 1 second delay
  }

  tb_shutdown();
  list_free(cpu_list);
  list_free(mem_list);
  list_free(net_up_list);
  list_free(net_down_list);

  for (int i = 0; i < disk_count; i++) {
    list_free(disk_lists[i]);
  }
  
  if (disk_info) free_disk_info(disk_info);
  return 0;

}

void draw_box(int x,int y,int x2,int y2,List *list, char* title, draw_bars draw_b){
  short skipLine = 0;
  int hLine = (y2 - y)/2 + y -1;

  char lineChar[1] = {(y2-y)%2==0?'_':'-'};
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
  //tb_printf(x+2,y,TB_DEFAULT, TB_DEFAULT,"%s%s%s", box[6],title,box[7]);
  tb_printf(x+2,y,TB_DEFAULT | TB_BOLD, TB_DEFAULT," %s ", title);

  //tb_printf(x+1, hLine, TB_DEFAULT, TB_DEFAULT, "50%%");
  draw_b(list,(x2-1)-(x+1),(y2-1)-(y+1),x+1,y+1);
  
}


void draw_bars_perc(List *list, int width, int height, int min_x, int min_y){

  while (list->count > width){
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
        tb_printf(min_x+x,min_y+y, color, TB_DEFAULT, "█"); // Full block
      } else if (y == height - bar_h - 1 && bar_h_e > 0) {
        tb_printf(min_x+x,min_y+y, color, TB_DEFAULT, "%s", blocks[bar_h_e - 1]); // Partial block
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
  unsigned long max_value=1;
  Node *node = list->first;
  while (node != NULL) {
    max_value=node_get_u_long(node) > max_value? node_get_u_long(node) : max_value;
    node = node->next;
  }
  

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
  if (container->type == BOX) {
    draw_box(x, y, x + width, y + height, container->box.list, container->box.title, container->box.draw_func);
  } else if (container->type == HBOX) {
    container_render_hbox(x, y, width, height, container);
  } else if (container->type == VBOX) {
    container_render_vbox(x, y, width, height, container);
  }
}


void container_render_vbox(int x, int y, int width, int height, Container *container) {
  int box_height = height / container->group.count;
  for (int i = 0; i < container->group.count; i++) {
    int h = (i == container->group.count - 1) ? height - i * box_height : box_height;
    Container *child = container->group.children[i];
    container_render(x, y + i * box_height, width, h, child);
  }
}

void container_render_hbox(int x, int y, int width, int height, Container *container) {
  int box_width = width / container->group.count;
  for (int i = 0; i < container->group.count; i++) {
    int w = (i == container->group.count - 1) ? width - i * box_width : box_width; 
    Container *child = container->group.children[i];
    container_render(x + i * box_width, y, w, height, child);
  }
}

