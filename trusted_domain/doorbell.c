#include <stdint.h>
#include <FreeRTOS.h>
#include <task.h>
#include "debug_log.h"
#include "doorbell.h"
#include "quard_star.h"

enum {
    QUARD_STAR_DOORBELL_L2R_SET = 0x0,
    QUARD_STAR_DOORBELL_L2R_CLR = 0x4,
    QUARD_STAR_DOORBELL_R2L_SET = 0x8,
    QUARD_STAR_DOORBELL_R2L_CLR = 0xc,
    QUARD_STAR_DOORBELL_STATUS  = 0x10,
    QUARD_STAR_DOORBELL_L2R_PENDING = 0x1,
    QUARD_STAR_DOORBELL_MAX_TASKS = 4,
};

static TaskHandle_t quard_star_doorbell_tasks[QUARD_STAR_DOORBELL_MAX_TASKS];

static inline void quard_star_writel(uint32_t value, uintptr_t addr)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t quard_star_readl(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uintptr_t quard_star_doorbell_reg(uintptr_t offset)
{
    return DOORBELL_ADDR + offset;
}

static inline void quard_star_doorbell_clear_l2r(void)
{
    quard_star_writel(1U, quard_star_doorbell_reg(QUARD_STAR_DOORBELL_L2R_CLR));
}

void quard_star_doorbell_ring_l2r(void)
{
    quard_star_writel(1U, quard_star_doorbell_reg(QUARD_STAR_DOORBELL_L2R_SET));
}

void quard_star_doorbell_ring_r2l(void)
{
    quard_star_writel(1U, quard_star_doorbell_reg(QUARD_STAR_DOORBELL_R2L_SET));
}

static inline uint32_t quard_star_doorbell_status(void)
{
    return quard_star_readl(quard_star_doorbell_reg(QUARD_STAR_DOORBELL_STATUS));
}

static uint32_t quard_star_plic_claim(void)
{
    return quard_star_readl(
        QUARD_STAR_PLIC_CONTEXT_CLAIM(QUARD_STAR_PLIC_CONTEXT_ID_S));
}

static void quard_star_plic_complete(uint32_t irq)
{
    quard_star_writel(irq,
        QUARD_STAR_PLIC_CONTEXT_CLAIM(QUARD_STAR_PLIC_CONTEXT_ID_S));
}

void quard_star_doorbell_register_task(TaskHandle_t task)
{
    unsigned int i;

    if (!task)
        return;

    for (i = 0; i < QUARD_STAR_DOORBELL_MAX_TASKS; i++) {
        if (quard_star_doorbell_tasks[i] == task)
            return;
        if (!quard_star_doorbell_tasks[i]) {
            quard_star_doorbell_tasks[i] = task;
            return;
        }
    }

    debug_log("doorbell task list full\n");
}

void quard_star_doorbell_init(void)
{
    uint32_t word = QUARD_STAR_DOORBELL_L2R_IRQ / 32U;
    uint32_t bit = 1U << (QUARD_STAR_DOORBELL_L2R_IRQ % 32U);
    uintptr_t enable_addr =
        QUARD_STAR_PLIC_ENABLE_WORD(QUARD_STAR_PLIC_CONTEXT_ID_S, word);
    uint32_t enable_val;

    /* Start from a quiescent level so enabling the source cannot latch stale state. */
    debug_log("doorbell_init: before clear source\n");
    quard_star_doorbell_clear_l2r();
    debug_log("doorbell_init: after clear source\n");

    debug_log("doorbell_init: before set priority\n");
    quard_star_writel(1U,
        QUARD_STAR_PLIC_SOURCE_PRIO(QUARD_STAR_DOORBELL_L2R_IRQ));
    debug_log("doorbell_init: after set priority\n");

    debug_log("doorbell_init: before set threshold\n");
    quard_star_writel(0U,
        QUARD_STAR_PLIC_CONTEXT_THRESHOLD(QUARD_STAR_PLIC_CONTEXT_ID_S));
    debug_log("doorbell_init: after set threshold\n");

    debug_log("doorbell_init: before read enable\n");
    enable_val = quard_star_readl(enable_addr);
    debug_log("doorbell_init: after read enable\n");

    debug_log("doorbell_init: before write enable\n");
    quard_star_writel(enable_val | bit, enable_addr);
    debug_log("doorbell_init: after write enable\n");
}

int quard_star_doorbell_handle_irq(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t irq = quard_star_plic_claim();

    if (!irq)
        return 0;

    if (irq != QUARD_STAR_DOORBELL_L2R_IRQ) {
        debug_log("unexpected external irq %u\n", irq);
        quard_star_plic_complete(irq);
        return 0;
    }

    /*
     * The doorbell is modeled as a level-triggered source. Clear the device
     * state before PLIC complete so a still-asserted level cannot retrigger.
     */
    if (quard_star_doorbell_status() & QUARD_STAR_DOORBELL_L2R_PENDING)
        quard_star_doorbell_clear_l2r();

    quard_star_plic_complete(irq);

    for (unsigned int i = 0; i < QUARD_STAR_DOORBELL_MAX_TASKS; i++) {
        if (quard_star_doorbell_tasks[i])
            vTaskNotifyGiveFromISR(quard_star_doorbell_tasks[i], &woken);
    }

    portYIELD_FROM_ISR(woken);

    return 1;
}
