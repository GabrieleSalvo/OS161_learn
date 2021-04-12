/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <vm.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 *
 * NOTE: it's been found over the years that students often begin on
 * the VM assignment by copying dumbvm.c and trying to improve it.
 * This is not recommended. dumbvm is (more or less intentionally) not
 * a good design reference. The first recommendation would be: do not
 * look at dumbvm at all. The second recommendation would be: if you
 * do, be sure to review it from the perspective of comparing it to
 * what a VM system is supposed to do, and understanding what corners
 * it's cutting (there are many) and why, and more importantly, how.
 */

#define DUMBVM_STACKPAGES 18
#define SetBit(A, k) (A[k / 32] |= (1 << (k % 32)))
#define ClearBit(A, k) (A[k / 32] &= ~(1 << (k % 32)))
#define TestBit(A, k) (A[k / 32] & (1 << (k % 32)))
#define SIZE_BITMAP 0xffffffff / (PAGE_SIZE * 32)

/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock freemem_lock = SPINLOCK_INITIALIZER;

static int *freeRamFrames = NULL;
static unsigned long *allocSize = NULL;
static int nRamFrames = 0;

static int allocTableActive = 0;

static int isTableActive()
{
	int active;
	spinlock_acquire(&freemem_lock);
	active = allocTableActive;
	spinlock_release(&freemem_lock);
	return active;
}

void vm_bootstrap(void)
{
	int i;
	nRamFrames = ((int)ram_getsize()) / PAGE_SIZE;
	freeRamFrames = kmalloc(sizeof(unsigned char) * (nRamFrames * 32));
	KASSERT(sizeof(freeRamFrames) == SIZE_BITMAP);

	if (freeRamFrames == NULL)
		return;
	allocSize = kmalloc(sizeof(unsigned long) * nRamFrames);
	if (allocSize == NULL)
	{
		/* reset to disable this vm management */
		freeRamFrames = NULL;
		return;
	}
	for (i = 0; i < nRamFrames; i++)
	{
		ClearBit(freeRamFrames, i);
		allocSize[i] = 0;
	}
	spinlock_acquire(&freemem_lock);
	allocTableActive = 1;
	spinlock_release(&freemem_lock);
}

/*
 * Check if we're in a context that can sleep. While most of the
 * operations in dumbvm don't in fact sleep, in a real VM system many
 * of them would. In those, assert that sleeping is ok. This helps
 * avoid the situation where syscall-layer code that works ok with
 * dumbvm starts blowing up during the VM assignment.
 */

void dumbvm_can_sleep(void)
{
	if (CURCPU_EXISTS())
	{
		/* must not hold spinlocks */
		KASSERT(curcpu->c_spinlocks == 0);

		/* must not be in an interrupt handler */
		KASSERT(curthread->t_in_interrupt == 0);
	}
}

paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	/* try freed pages first */
	addr = getfreeppages(npages);
	if (addr == 0)
	{
		/* call stealmem */
		spinlock_acquire(&stealmem_lock);
		addr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	}
	if (addr != 0 && isTableActive())
	{
		spinlock_acquire(&freemem_lock);
		allocSize[addr / PAGE_SIZE] = npages;
		spinlock_release(&freemem_lock);
	}

	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
{
	paddr_t pa;
	dumbvm_can_sleep();
	pa = getppages(npages);
	if (pa == 0)
	{
		return 0;
	}
	vaddr_t va = PADDR_TO_KVADDR(pa);

	return va;
}

void free_kpages(vaddr_t addr)
{
	if (isTableActive())
	{
		paddr_t paddr = addr - MIPS_KSEG0;
		long first = paddr / PAGE_SIZE;
		KASSERT(allocSize != NULL);
		KASSERT(nRamFrames > first);
		freeppages(paddr, allocSize[first]);
	}
}

paddr_t
getfreeppages(unsigned long npages)
{
	paddr_t addr;
	long i, first, found, np = (long)npages;

	if (!isTableActive())
		return 0;
	spinlock_acquire(&freemem_lock);
	for (i = 0, first = found = -1; i < nRamFrames; i++)
	{
		if (TestBit(freeRamFrames, i))
		{
			if (i == 0 || !TestBit(freeRamFrames, i - 1))
				first = i; /* set first free in an interval */
			if (i - first + 1 >= np)
			{
				found = first;
				break;
			}
		}
	}

	if (found >= 0)
	{
		for (i = found; i < found + np; i++)
		{
			ClearBit(freeRamFrames, i);
		}
		allocSize[found] = np;
		addr = (paddr_t)found * PAGE_SIZE;
	}
	else
	{
		addr = 0;
	}

	spinlock_release(&freemem_lock);

	return addr;
}
int
freeppages(paddr_t addr, unsigned long npages)
{
	long i, first, np = (long)npages;

	if (!isTableActive())
		return 0;
	first = addr / PAGE_SIZE;
	KASSERT(allocSize != NULL);
	KASSERT(nRamFrames > first);

	spinlock_acquire(&freemem_lock);
	for (i = first; i < first + np; i++)
	{
		SetBit(freeRamFrames, i);
	}
	spinlock_release(&freemem_lock);

	return 1;
}

void vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase_code, vtop_code, vbase_data, vtop_data, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype)
	{
	case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	case VM_FAULT_READ:
	case VM_FAULT_WRITE:
		break;
	default:
		return EINVAL;
	}

	if (curproc == NULL)
	{
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL)
	{
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase_code != 0);
	KASSERT(as->as_pbase_code != 0);
	KASSERT(as->as_npages_code != 0);
	KASSERT(as->as_vbase_data != 0);
	KASSERT(as->as_pbase_data != 0);
	KASSERT(as->as_npages_data != 0);
	KASSERT(as->as_pbase_stack != 0);
	KASSERT((as->as_vbase_code & PAGE_FRAME) == as->as_vbase_code);
	KASSERT((as->as_pbase_code & PAGE_FRAME) == as->as_pbase_code);
	KASSERT((as->as_vbase_data & PAGE_FRAME) == as->as_vbase_data);
	KASSERT((as->as_pbase_data & PAGE_FRAME) == as->as_pbase_data);
	KASSERT((as->as_pbase_stack & PAGE_FRAME) == as->as_pbase_stack);

	vbase_code = as->as_vbase_code;
	vtop_code = vbase_code + as->as_npages_code * PAGE_SIZE;
	vbase_data = as->as_vbase_data;
	vtop_data = vbase_data + as->as_npages_data * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase_code && faultaddress < vtop_code)
	{
		paddr = (faultaddress - vbase_code) + as->as_pbase_code;
	}
	else if (faultaddress >= vbase_data && faultaddress < vtop_data)
	{
		paddr = (faultaddress - vbase_data) + as->as_pbase_data;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop)
	{
		paddr = (faultaddress - stackbase) + as->as_pbase_stack;
	}
	else
	{
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i = 0; i < NUM_TLB; i++)
	{
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID)
		{
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

struct addrspace *as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as == NULL)
	{
		return NULL;
	}

	as->as_vbase_code = 0;
	as->as_pbase_code = 0;
	as->as_npages_code = 0;
	as->as_vbase_data = 0;
	as->as_pbase_data = 0;
	as->as_npages_data = 0;
	as->as_pbase_stack = 0;

	return as;
}

void as_destroy(struct addrspace *as)
{
	dumbvm_can_sleep();
	kfree(as);
}

void as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL)
	{
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i = 0; i < NUM_TLB; i++)
	{
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void as_deactivate(void)
{
	/* nothing */
}
/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
					 int readable, int writeable, int executable)
{
	size_t npages;

	dumbvm_can_sleep();

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase_code == 0)
	{
		as->as_vbase_code = vaddr;
		as->as_npages_code = npages;
		return 0;
	}

	if (as->as_vbase_data == 0)
	{
		as->as_vbase_data = vaddr;
		as->as_npages_data = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return ENOSYS;
}

int as_define_kernel_region(struct addrspace *as, vaddr_t vaddr, paddr_t paddr, size_t npages)
{
	as->as_pbase_code = paddr;
	as->as_vbase_code = vaddr;
	as->as_npages_code = npages;
	return 0;
}

static void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

//TODO to modify to insert bitmap
int as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_pbase_code == 0);
	KASSERT(as->as_pbase_data == 0);
	KASSERT(as->as_pbase_stack == 0);

	dumbvm_can_sleep();

	as->as_pbase_code = getppages(as->as_npages_code);
	if (as->as_pbase_code == 0)
	{
		return ENOMEM;
	}

	as->as_pbase_data = getppages(as->as_npages_data);
	if (as->as_pbase_data == 0)
	{
		return ENOMEM;
	}

	as->as_pbase_stack = getppages(DUMBVM_STACKPAGES);
	if (as->as_pbase_stack == 0)
	{
		return ENOMEM;
	}

	as_zero_region(as->as_pbase_code, as->as_npages_code);
	as_zero_region(as->as_pbase_data, as->as_npages_data);
	as_zero_region(as->as_pbase_stack, DUMBVM_STACKPAGES);

	return 0;
}

int as_complete_load(struct addrspace *as)
{
	dumbvm_can_sleep();
	(void)as;
	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_pbase_stack != 0);

	*stackptr = USERSTACK;
	return 0;
}

int as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	dumbvm_can_sleep();

	new = as_create();
	if (new == NULL)
	{
		return ENOMEM;
	}

	new->as_vbase_code = old->as_vbase_code;
	new->as_npages_code = old->as_npages_code;
	new->as_vbase_data = old->as_vbase_data;
	new->as_npages_data = old->as_npages_data;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new))
	{
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase_code != 0);
	KASSERT(new->as_pbase_data != 0);
	KASSERT(new->as_pbase_stack != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase_code),
			(const void *)PADDR_TO_KVADDR(old->as_pbase_code),
			old->as_npages_code * PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase_data),
			(const void *)PADDR_TO_KVADDR(old->as_pbase_data),
			old->as_npages_data * PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase_stack),
			(const void *)PADDR_TO_KVADDR(old->as_pbase_stack),
			DUMBVM_STACKPAGES * PAGE_SIZE);

	*ret = new;
	return 0;
}
