#ifndef QUARD_STAR_OPENAMP_H
#define QUARD_STAR_OPENAMP_H

#include <FreeRTOS.h>
#include <task.h>

BaseType_t quard_star_openamp_start(TaskHandle_t *task_handle);
void quard_star_openamp_handle_kick(void);

#endif
