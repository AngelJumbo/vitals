#ifndef MODULES_H 
#define MODULES_H
#include <sys/statvfs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

int mem_perc();
int cpu_perc();
void get_active_interface(char *interface, size_t size);
void get_network_speed( unsigned long *rx_speed, unsigned long *tx_speed, char *net_interface);
void format_speed(char *buffer, size_t size, unsigned long bytes_per_sec);
#endif
