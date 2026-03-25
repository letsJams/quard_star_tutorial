/*
 * QEMU RISC-V Quard Star Board demo char device
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
#include "hw/misc/quard_star_demochar.h"

#define QUARD_STAR_DEMOCHAR_VERSION 0x00010000

enum {
    DATA_REG = 0x0,
    VERSION_REG = 0x4,
};

static uint64_t quard_star_demochar_read(void *opaque, hwaddr addr,
                                         unsigned int size)
{
    QuardStarDemoCharState *s = opaque;

    switch (addr) {
    case DATA_REG:
        return s->data;
    case VERSION_REG:
        return QUARD_STAR_DEMOCHAR_VERSION;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read: addr=0x%x\n",
                      __func__, (int)addr);
        return 0;
    }
}

static void quard_star_demochar_write(void *opaque, hwaddr addr,
                                      uint64_t val64, unsigned int size)
{
    QuardStarDemoCharState *s = opaque;

    switch (addr) {
    case DATA_REG:
        s->data = (uint32_t)val64;
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write: addr=0x%x val=0x%016" PRIx64 "\n",
                      __func__, (int)addr, val64);
        return;
    }
}

static const MemoryRegionOps quard_star_demochar_ops = {
    .read = quard_star_demochar_read,
    .write = quard_star_demochar_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void quard_star_demochar_init(Object *obj)
{
    QuardStarDemoCharState *s = QUARD_STAR_DEMOCHAR_DEV(obj);

    memory_region_init_io(&s->mmio, obj, &quard_star_demochar_ops, s,
                          TYPE_QUARD_STAR_DEMOCHAR, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void quard_star_demochar_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo quard_star_demochar_info = {
    .name          = TYPE_QUARD_STAR_DEMOCHAR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QuardStarDemoCharState),
    .instance_init = quard_star_demochar_init,
    .class_init    = quard_star_demochar_class_init,
};

static void quard_star_demochar_register_types(void)
{
    type_register_static(&quard_star_demochar_info);
}

type_init(quard_star_demochar_register_types)

DeviceState *quard_star_demochar_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_QUARD_STAR_DEMOCHAR);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);

    return dev;
}
