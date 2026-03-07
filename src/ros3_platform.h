#ifndef ROS3_PLATFORM_H
#define ROS3_PLATFORM_H

#include "ros3.h"

int rose_platform_set_parent_death_signal(int sig);
void rose_platform_install_shutdown_handlers(void (*handler)(int));
i64 rose_platform_getpid(void);

#endif
