#include "modules.h"
#include <string.h>

void get_active_interface(char *interface, size_t len) {
    // FreeBSD loopback interface
    strncpy(interface, "lo0", len);
}

void get_network_speed(unsigned long *download_speed, unsigned long *upload_speed, char *interface) {
    // Setting these to 0 prevents the /proc/net/dev crash
    *download_speed = 0;
    *upload_speed = 0;
}
