#ifndef QUARD_STAR_TRUSTED_DOORBELL_H
#define QUARD_STAR_TRUSTED_DOORBELL_H

#include <stdint.h>
#include <FreeRTOS.h>
#include <task.h>

void quard_star_doorbell_init(void);
void quard_star_doorbell_register_task(TaskHandle_t task);
void quard_star_doorbell_ring_l2r(void);
int quard_star_doorbell_handle_irq(void);

#endif
