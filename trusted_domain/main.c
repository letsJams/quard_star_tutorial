#include <FreeRTOS.h>
#include <task.h>
#include <stdio.h>
#include <string.h>
#include "debug_log.h"
#include "ipc.h"

static void ipc_task(void *p_arg)
{
    char rx[QUARD_STAR_IPC_MAX_MSG + 1];
    char tx[QUARD_STAR_IPC_MAX_MSG + 1];
    int len;
    int ret;

    quard_star_ipc_init();
    debug_log("ipc_task ready, shared memory @ 0x%lx\n",
              (unsigned long)QUARD_STAR_IPC_SHM_BASE);

    for (;;) {
        len = quard_star_ipc_recv(rx, QUARD_STAR_IPC_MAX_MSG);
        if (len == QUARD_STAR_IPC_ERR_EMPTY) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (len < 0) {
            debug_log("ipc recv error %d\n", len);
            vTaskDelay(pdMS_TO_TICKS(10));
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
    debug_log_init();
    debug_log("Hello FreeRTOS IPC!\n");

    quard_star_ipc_init();
    xTaskCreate(ipc_task, "ipc_task", 2048, NULL, 4, NULL);

    vTaskStartScheduler();
    return 0;
}
