#include <FreeRTOS.h>
#include <task.h>
#include <stdio.h>
#include <string.h>
#include "debug_log.h"
#include "doorbell.h"
#include "ipc.h"
#include "quard_star.h"

static TaskHandle_t ipc_task_handle;

static void ipc_task(void *p_arg)
{
    char rx[QUARD_STAR_IPC_MAX_MSG + 1];
    char tx[QUARD_STAR_IPC_MAX_MSG + 1];
    BaseType_t selftest_done = pdFALSE;
    int len;
    int ret;

    quard_star_ipc_init();
    debug_log("ipc_task ready, shared memory @ 0x%lx\n",
              (unsigned long)QUARD_STAR_IPC_SHM_BASE);
    debug_log("doorbell wait enabled on irq %u\n", QUARD_STAR_DOORBELL_IRQ);

    if (selftest_done == pdFALSE) {
        selftest_done = pdTRUE;
        debug_log("doorbell selftest trigger\n");
        quard_star_doorbell_ring_l2r();
    }

    for (;;) {
        len = quard_star_ipc_recv(rx, QUARD_STAR_IPC_MAX_MSG);
        if (len == QUARD_STAR_IPC_ERR_EMPTY) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            debug_log("ipc_task woke by doorbell\n");
            continue;
        }

        if (len < 0) {
            debug_log("ipc recv error %d\n", len);
            continue;
        }

        rx[len] = '\0';
        debug_log("ipc rx: %s\n", rx);

        if (!strncmp(rx, "ping-", 5))
            len = snprintf(tx, sizeof(tx), "pong-%s", rx + 5);
        else
            len = snprintf(tx, sizeof(tx), "ack:%s", rx);

        if (len < 0)
            continue;
        if (len > QUARD_STAR_IPC_MAX_MSG)
            len = QUARD_STAR_IPC_MAX_MSG;

        do {
            ret = quard_star_ipc_send(tx, (size_t)len);
            if (ret == QUARD_STAR_IPC_ERR_NOSPC)
                vTaskDelay(pdMS_TO_TICKS(10));
        } while (ret == QUARD_STAR_IPC_ERR_NOSPC);

        if (ret < 0)
            debug_log("ipc send error %d\n", ret);
    }
}

int main(void)
{
    BaseType_t ret;

    debug_log_init();
    debug_log("Hello FreeRTOS IPC doorbell!\n");

    debug_log("main: before ipc init\n");
    quard_star_ipc_init();
    debug_log("main: after ipc init\n");

    debug_log("main: before doorbell init\n");
    quard_star_doorbell_init();
    debug_log("main: after doorbell init\n");

    debug_log("main: before xTaskCreate\n");
    ret = xTaskCreate(ipc_task, "ipc_task", 2048, NULL, 4, &ipc_task_handle);
    if (ret != pdPASS) {
        debug_log("xTaskCreate ipc_task failed: %d\n", ret);
        return 1;
    }
    debug_log("main: after xTaskCreate handle=0x%lx\n",
              (unsigned long)ipc_task_handle);
    quard_star_doorbell_register_task(ipc_task_handle);
    debug_log("main: after register task\n");

    debug_log("main: before scheduler\n");
    vTaskStartScheduler();
    debug_log("main: scheduler returned unexpectedly\n");
    return 0;
}
