#include <stddef.h>
#include <metal/io.h>
#include <metal/sys.h>
#include "riscv_asm.h"
#include "riscv_encoding.h"

void sys_irq_restore_enable(unsigned int flags)
{
	if (flags & SSTATUS_SIE)
		csr_set(CSR_SSTATUS, SSTATUS_SIE);
}

unsigned int sys_irq_save_disable(void)
{
	return (unsigned int)csr_read_clear(CSR_SSTATUS, SSTATUS_SIE);
}

void sys_irq_enable(unsigned int vector)
{
	(void)vector;
}

void sys_irq_disable(unsigned int vector)
{
	(void)vector;
}

void metal_machine_cache_flush(void *addr, unsigned int len)
{
	(void)addr;
	(void)len;
}

void metal_machine_cache_invalidate(void *addr, unsigned int len)
{
	(void)addr;
	(void)len;
}

void metal_generic_default_poll(void)
{
}

void *metal_machine_io_mem_map(void *va, metal_phys_addr_t pa,
			       size_t size, unsigned int flags)
{
	(void)pa;
	(void)size;
	(void)flags;

	return va;
}
