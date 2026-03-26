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

#ifndef HW_QUARD_STAR_DOORBELL_H
#define HW_QUARD_STAR_DOORBELL_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_QUARD_STAR_DOORBELL "quard-star-doorbell"
#define QUARD_STAR_DOORBELL_L2R_PENDING 0x1u
#define QUARD_STAR_DOORBELL_R2L_PENDING 0x2u

typedef struct QuardStarDoorbellState QuardStarDoorbellState;
DECLARE_INSTANCE_CHECKER(QuardStarDoorbellState, QUARD_STAR_DOORBELL_DEV,
                         TYPE_QUARD_STAR_DOORBELL)

struct QuardStarDoorbellState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq_l2r;
    qemu_irq irq_r2l;
    uint32_t status;
};

DeviceState *quard_star_doorbell_create(hwaddr addr, qemu_irq irq_l2r,
                                        qemu_irq irq_r2l);

#endif
