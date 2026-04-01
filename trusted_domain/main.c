#include <FreeRTOS.h>
#include <task.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "debug_log.h"
#include "doorbell.h"
#include "ipc.h"
#include "quard_star.h"

static TaskHandle_t ipc_task_handle;

enum {
    QUARD_STAR_GPIO_OUTPUT_EN_REG = 0x8,
    QUARD_STAR_GPIO_OUTPUT_VAL_REG = 0xc,
    QUARD_STAR_LED0 = 0,
    QUARD_STAR_DEMOCHAR_DATA_REG = 0x0,
    QUARD_STAR_DEMOCHAR_VERSION_REG = 0x4,
    QUARD_STAR_DEMOCHAR_VERSION = 0x00010000U,
};

static inline void quard_star_writel(uint32_t value, uintptr_t addr)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t quard_star_readl(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static int ipc_send_with_retry(const void *buf, size_t len)
{
    int ret;

    do {
        ret = quard_star_ipc_send((const char *)buf, len);
        if (ret == QUARD_STAR_IPC_ERR_NOSPC)
            vTaskDelay(pdMS_TO_TICKS(10));
    } while (ret == QUARD_STAR_IPC_ERR_NOSPC);

    if (ret >= 0)
        quard_star_doorbell_ring_r2l();

    return ret;
}

static int ipc_send_response(unsigned int seq, unsigned char service,
                             unsigned char opcode, int status,
                             const void *payload, unsigned int payload_len)
{
    unsigned char tx[QUARD_STAR_IPC_MAX_MSG];
    struct quard_star_ipc_msg_hdr hdr;
    size_t total_len = sizeof(hdr) + payload_len;

    if (total_len > sizeof(tx))
        return QUARD_STAR_IPC_STATUS_TOOLONG;

    hdr.magic = QUARD_STAR_IPC_MSG_MAGIC;
    hdr.version = QUARD_STAR_IPC_MSG_VERSION;
    hdr.type = QUARD_STAR_IPC_MSG_TYPE_RESP;
    hdr.service = service;
    hdr.opcode = opcode;
    hdr.seq = seq;
    hdr.status = status;
    hdr.len = payload_len;

    memcpy(tx, &hdr, sizeof(hdr));
    if (payload_len)
        memcpy(tx + sizeof(hdr), payload, payload_len);

    return ipc_send_with_retry(tx, total_len);
}

static int ipc_parse_header(const unsigned char *buf, size_t len,
                            struct quard_star_ipc_msg_hdr *hdr)
{
    if (len < sizeof(*hdr))
        return 0;

    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->magic != QUARD_STAR_IPC_MSG_MAGIC ||
        hdr->version != QUARD_STAR_IPC_MSG_VERSION)
        return 0;

    if (hdr->len > QUARD_STAR_IPC_MAX_MSG - sizeof(*hdr))
        return -1;

    if (sizeof(*hdr) + hdr->len != len)
        return -1;

    return 1;
}

static int ipc_handle_demochar_request(const struct quard_star_ipc_msg_hdr *hdr,
                                       const unsigned char *payload)
{
    struct quard_star_ipc_demochar_value value;

    switch (hdr->opcode) {
    case QUARD_STAR_IPC_DEMOCHAR_READ:
        if (hdr->len != 0U)
            return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                     QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);

        value.value = quard_star_readl(DEMOCHAR_ADDR +
                                       QUARD_STAR_DEMOCHAR_DATA_REG);
        return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                 QUARD_STAR_IPC_STATUS_OK,
                                 &value, sizeof(value));
    case QUARD_STAR_IPC_DEMOCHAR_WRITE:
        if (hdr->len != sizeof(value))
            return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                     QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);

        memcpy(&value, payload, sizeof(value));
        quard_star_writel(value.value, DEMOCHAR_ADDR +
                          QUARD_STAR_DEMOCHAR_DATA_REG);
        return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                 QUARD_STAR_IPC_STATUS_OK, NULL, 0);
    case QUARD_STAR_IPC_DEMOCHAR_VERSION:
        if (hdr->len != 0U)
            return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                     QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);

        value.value = quard_star_readl(DEMOCHAR_ADDR +
                                       QUARD_STAR_DEMOCHAR_VERSION_REG);
        return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                 QUARD_STAR_IPC_STATUS_OK,
                                 &value, sizeof(value));
    default:
        return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                 QUARD_STAR_IPC_STATUS_NOSYS, NULL, 0);
    }
}

static void quard_star_led_write(unsigned int led, unsigned int value)
{
    uint32_t bit = 1U << led;
    uint32_t out_en = quard_star_readl(GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_EN_REG);
    uint32_t out_val = quard_star_readl(GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_VAL_REG);

    out_en |= bit;
    if (value)
        out_val |= bit;
    else
        out_val &= ~bit;

    quard_star_writel(out_en, GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_EN_REG);
    quard_star_writel(out_val, GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_VAL_REG);
}

static unsigned int quard_star_led_read(unsigned int led)
{
    uint32_t out_val = quard_star_readl(GPIO_ADDR + QUARD_STAR_GPIO_OUTPUT_VAL_REG);

    return !!(out_val & (1U << led));
}

static int ipc_handle_led_request(const struct quard_star_ipc_msg_hdr *hdr,
                                  const unsigned char *payload)
{
    struct quard_star_ipc_led_state state;

    switch (hdr->opcode) {
    case QUARD_STAR_IPC_LED_GET:
        if (hdr->len != sizeof(state))
            return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                     QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);

        memcpy(&state, payload, sizeof(state));
        if (state.led != QUARD_STAR_LED0)
            return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                     QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);

        state.value = quard_star_led_read(state.led);
        return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                 QUARD_STAR_IPC_STATUS_OK,
                                 &state, sizeof(state));
    case QUARD_STAR_IPC_LED_SET:
        if (hdr->len != sizeof(state))
            return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                     QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);

        memcpy(&state, payload, sizeof(state));
        if (state.led != QUARD_STAR_LED0)
            return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                     QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);

        quard_star_led_write(state.led, !!state.value);
        state.value = quard_star_led_read(state.led);
        return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                 QUARD_STAR_IPC_STATUS_OK,
                                 &state, sizeof(state));
    default:
        return ipc_send_response(hdr->seq, hdr->service, hdr->opcode,
                                 QUARD_STAR_IPC_STATUS_NOSYS, NULL, 0);
    }
}

static int ipc_handle_service_request(const unsigned char *buf, size_t len)
{
    struct quard_star_ipc_msg_hdr hdr;
    int ret = ipc_parse_header(buf, len, &hdr);

    if (ret <= 0)
        return ret;

    if (hdr.type != QUARD_STAR_IPC_MSG_TYPE_REQ) {
        ipc_send_response(hdr.seq, hdr.service, hdr.opcode,
                          QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);
        return 1;
    }

    switch (hdr.service) {
    case QUARD_STAR_IPC_SERVICE_DEMOCHAR:
        ipc_handle_demochar_request(&hdr, buf + sizeof(hdr));
        return 1;
    case QUARD_STAR_IPC_SERVICE_LED:
        ipc_handle_led_request(&hdr, buf + sizeof(hdr));
        return 1;
    default:
        ipc_send_response(hdr.seq, hdr.service, hdr.opcode,
                          QUARD_STAR_IPC_STATUS_NOSYS, NULL, 0);
        return 1;
    }
}

static void ipc_task(void *p_arg)
{
    unsigned char rx[QUARD_STAR_IPC_MAX_MSG + 1];
    char tx[QUARD_STAR_IPC_MAX_MSG + 1];
    int len;
    int ret;

    quard_star_ipc_init();
    debug_log("ipc_task ready, shared memory @ 0x%lx\n",
              (unsigned long)QUARD_STAR_IPC_SHM_BASE);
    debug_log("doorbell wait enabled on irq %u\n", QUARD_STAR_DOORBELL_L2R_IRQ);

    for (;;) {
        len = quard_star_ipc_recv((char *)rx, QUARD_STAR_IPC_MAX_MSG);
        if (len == QUARD_STAR_IPC_ERR_EMPTY) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            debug_log("ipc_task woke by doorbell\n");
            continue;
        }

        if (len < 0) {
            debug_log("ipc recv error %d\n", len);
            continue;
        }

        ret = ipc_handle_service_request(rx, (size_t)len);
        if (ret > 0)
            continue;
        if (ret < 0) {
            struct quard_star_ipc_msg_hdr hdr;

            if ((size_t)len >= sizeof(hdr)) {
                memcpy(&hdr, rx, sizeof(hdr));
                ipc_send_response(hdr.seq, hdr.service, hdr.opcode,
                                  QUARD_STAR_IPC_STATUS_INVAL, NULL, 0);
            }
            debug_log("ipc malformed service packet len=%d\n", len);
            continue;
        }

        rx[len] = '\0';
        debug_log("ipc rx: %s\n", (char *)rx);

        if (!strncmp((const char *)rx, "ping-", 5))
            len = snprintf(tx, sizeof(tx), "pong-%s", (const char *)rx + 5);
        else
            len = snprintf(tx, sizeof(tx), "ack:%s", (const char *)rx);

        if (len < 0)
            continue;
        if (len > QUARD_STAR_IPC_MAX_MSG)
            len = QUARD_STAR_IPC_MAX_MSG;

        ret = ipc_send_with_retry(tx, (size_t)len);
        if (ret < 0) {
            debug_log("ipc send error %d\n", ret);
            continue;
        }
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
