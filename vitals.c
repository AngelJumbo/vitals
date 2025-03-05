#include "modules.h"
#include "utils.h"
#define TB_IMPL
#include "termbox.h"
#define MAX_DISKS 8
#define NAME_STR_LEN(s) (sizeof(s) - 1) 
#define APP_NAME " vitals "
#define APP_VERSION " 0.0.1 "

extern char buf[1024];
static struct tb_event event = {0};
int cpu_perc();

char *blocks[8]={"▁","▂","▃","▄","▅","▆","▇","█"};
char *box[8] = {"╒", "╕", "╘", "╛", "═", "│","╡","╞"};


typedef void (*draw_bars)(List *, int, int, int, int);

void draw_bars_box(int x,int y,int x2,int y2,List *list, char *title, draw_bars draw_b);
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

void renderVBox(int x, int y, int width, int height, Container *container);

void renderHBox(int x, int y, int width, int height, Container *container);



int main(int argc, char *argv[]) {
  tb_init();
  List *cpu_list = list_create();
  List *mem_list = list_create();
  List *net_up_list = list_create();
  List *net_down_list = list_create();
  char cpu_title[100] = "";
  char mem_title[100] = "";
  char net_up_title[100] = "";
  char net_down_title[100] = "";
  char speed_str[16];
  //init disk lists here
  int width,height,box_height,init_x=-1;
  int max_cpu=15;

  char active_interface[32];

  get_active_interface(active_interface, sizeof(active_interface));
  unsigned long download_speed, upload_speed;

  Container cpu_box = {BOX, .box = {cpu_list, cpu_title, draw_bars_perc}};
  Container mem_box = {BOX, .box = {mem_list, mem_title, draw_bars_perc}};
  Container net_up_box = {BOX, .box = {net_up_list, net_up_title, draw_scale_bars}};
  Container net_down_box = {BOX, .box = {net_down_list, net_down_title, draw_scale_bars}};

  Container *hbox_children[] = {&net_up_box, &net_down_box};
  Container hbox_net = {HBOX, .group = {hbox_children, 2}};

  Container *vbox_children[] = {&cpu_box, &mem_box, &hbox_net};
  Container vbox_main = {VBOX, .group = {vbox_children, 3}};
  
  short first_read=0; 
  while (1) {
    
    int new_width = tb_width();
    int new_height = tb_height();
    if(new_width!=width|| new_height!=height){
      width=new_width;
      height=new_height;
      box_height = height/3; 
      init_x=width-1;
    }
    get_network_speed(&download_speed, &upload_speed,active_interface);
    tb_clear();
    
    // Add new CPU usage percentage
    list_append_int(cpu_list, cpu_perc());
    // Add new ram usage percentage
    list_append_int(mem_list, mem_perc());
    // Add new net upload speed
    list_append_u_long(net_up_list,upload_speed);
    // Add new net download speed
    list_append_u_long(net_down_list,download_speed);
    
    sprintf(cpu_title,"CPU: %i%%",node_get_int(cpu_list->last));
    sprintf(mem_title,"RAM: %i%%",node_get_int(mem_list->last));
    format_speed(speed_str, sizeof(speed_str), upload_speed);
    sprintf(net_up_title,"NET UP: %s",speed_str);
    format_speed(speed_str, sizeof(speed_str), download_speed);
    sprintf(net_down_title,"NET DOWN: %s",speed_str);

    renderVBox(0, 0, width, height, &vbox_main);
  
    /*
    int height_box = height/3;
    draw_bars_box(0,0,width,height_box,cpu_list,cpu_title,draw_bars_perc);
    draw_bars_box(0,height_box,width,height_box*2,mem_list,mem_title,draw_bars_perc);
    draw_bars_box(0,height_box*2,width/2,height_box*3,net_up_list,net_up_title,draw_scale_bars);
    draw_bars_box(width/2,height_box*2,width,height_box*3,net_down_list,net_down_title,draw_scale_bars);
*/

    tb_printf(width-NAME_STR_LEN(APP_VERSION), height-1, TB_DEFAULT, TB_DEFAULT, APP_VERSION);
    tb_printf(0, height-1, TB_DEFAULT, TB_DEFAULT, APP_NAME);
    
    tb_peek_event(&event, 10);
    tb_present();
    if(event.ch=='q') break;
    usleep(1000000); // 1 second delay
    if(init_x>0) init_x--;
  }

  tb_shutdown();
  list_free(cpu_list);
  list_free(mem_list);
  list_free(net_up_list);
  list_free(net_down_list);
  return 0;

}

void draw_bars_box(int x,int y,int x2,int y2,List *list, char* title, draw_bars draw_b){
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
  tb_printf(x+2,y,TB_DEFAULT, TB_DEFAULT," %s ", title);

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
  unsigned long max_value=0;
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

void renderVBox(int x, int y, int width, int height, Container *container) {
    int box_height = height / container->group.count;
    for (int i = 0; i < container->group.count; i++) {
        Container *child = container->group.children[i];
        if (child->type == BOX) {
            draw_bars_box(x, y + i * box_height, x + width, y + (i + 1) * box_height,
                          child->box.list, child->box.title, child->box.draw_func);
        } else if (child->type == HBOX) {
            renderHBox(x, y + i * box_height, width, box_height, child);
        } else if (child->type == VBOX) {
            renderVBox(x, y + i * box_height, width, box_height, child);
        }
    }
}

void renderHBox(int x, int y, int width, int height, Container *container) {
    int box_width = width / container->group.count;
    for (int i = 0; i < container->group.count; i++) {
        Container *child = container->group.children[i];
        if (child->type == BOX) {
            draw_bars_box(x + i * box_width, y, x + (i + 1) * box_width, y + height,
                          child->box.list, child->box.title, child->box.draw_func);
        } else if (child->type == HBOX) {
            renderHBox(x + i * box_width, y, box_width, height, child);
        } else if (child->type == VBOX) {
            renderVBox(x + i * box_width, y, box_width, height, child);
        }
    }
}

