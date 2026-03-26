/*
 * QEMU RISC-V Quard Star Board doorbell device
 *
 * Copyright (c) 2021-2022 qiao qiming <2014500726@smail.xtu.edu.cn>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/hw.h"
#include "hw/misc/quard_star_doorbell.h"

enum {
    L2R_SET_REG = 0x0,
    L2R_CLR_REG = 0x4,
    R2L_SET_REG = 0x8,
    R2L_CLR_REG = 0xc,
    STATUS_REG  = 0x10,
};

static void quard_star_doorbell_update_irq(QuardStarDoorbellState *s)
{
    qemu_set_irq(s->irq_l2r,
                 (s->status & QUARD_STAR_DOORBELL_L2R_PENDING) != 0);
    qemu_set_irq(s->irq_r2l,
                 (s->status & QUARD_STAR_DOORBELL_R2L_PENDING) != 0);
}

static uint64_t quard_star_doorbell_read(void *opaque, hwaddr addr,
                                         unsigned int size)
{
    QuardStarDoorbellState *s = opaque;

    switch (addr) {
    case STATUS_REG:
        return s->status;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read: addr=0x%x\n",
                      __func__, (int)addr);
        return 0;
    }
}

static void quard_star_doorbell_write(void *opaque, hwaddr addr,
                                      uint64_t val64, unsigned int size)
{
    QuardStarDoorbellState *s = opaque;

    switch (addr) {
    case L2R_SET_REG:
        s->status |= QUARD_STAR_DOORBELL_L2R_PENDING;
        quard_star_doorbell_update_irq(s);
        return;
    case L2R_CLR_REG:
        s->status &= ~QUARD_STAR_DOORBELL_L2R_PENDING;
        quard_star_doorbell_update_irq(s);
        return;
    case R2L_SET_REG:
        s->status |= QUARD_STAR_DOORBELL_R2L_PENDING;
        quard_star_doorbell_update_irq(s);
        return;
    case R2L_CLR_REG:
        s->status &= ~QUARD_STAR_DOORBELL_R2L_PENDING;
        quard_star_doorbell_update_irq(s);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write: addr=0x%x val=0x%016" PRIx64 "\n",
                      __func__, (int)addr, val64);
        return;
    }
}

static const MemoryRegionOps quard_star_doorbell_ops = {
    .read = quard_star_doorbell_read,
    .write = quard_star_doorbell_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void quard_star_doorbell_init(Object *obj)
{
    QuardStarDoorbellState *s = QUARD_STAR_DOORBELL_DEV(obj);

    memory_region_init_io(&s->mmio, obj, &quard_star_doorbell_ops, s,
                          TYPE_QUARD_STAR_DOORBELL, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_l2r);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_r2l);
}

static void quard_star_doorbell_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo quard_star_doorbell_info = {
    .name          = TYPE_QUARD_STAR_DOORBELL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QuardStarDoorbellState),
    .instance_init = quard_star_doorbell_init,
    .class_init    = quard_star_doorbell_class_init,
};

static void quard_star_doorbell_register_types(void)
{
    type_register_static(&quard_star_doorbell_info);
}

type_init(quard_star_doorbell_register_types)

DeviceState *quard_star_doorbell_create(hwaddr addr, qemu_irq irq_l2r,
                                        qemu_irq irq_r2l)
{
    DeviceState *dev = qdev_new(TYPE_QUARD_STAR_DOORBELL);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq_l2r);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, irq_r2l);

    return dev;
}
