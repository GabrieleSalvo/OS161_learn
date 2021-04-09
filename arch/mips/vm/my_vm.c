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
#include <lib.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <lib.h>

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



/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

void
vm_bootstrap(void)
{
    unsigned int i;
    for(i=0;i<SIZE_BITMAP;i++)
        pages_bitmap[i]=0;
	struct node_list* head = kmalloc(sizeof(struct node_list));
	if(head == NULL)
		panic("not enough space for node list");
	head->next=NULL;
	vm_addrspace_list = kmalloc(sizeof(struct addrspace_list)) ;
	if(vm_addrspace_list==NULL)
		panic("not enough space for vm_addrspace_list");
	vm_addrspace_list->head=NULL;
}

/*
 * Check if we're in a context that can sleep. While most of the
 * operations in dumbvm don't in fact sleep, in a real VM system many
 * of them would. In those, assert that sleeping is ok. This helps
 * avoid the situation where syscall-layer code that works ok with
 * dumbvm starts blowing up during the VM assignment.
 */

void
dumbvm_can_sleep(void)
{
	if (CURCPU_EXISTS()) {
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
    unsigned long page_index, base_page;
	spinlock_acquire(&stealmem_lock);

    
	addr = ram_stealmem(npages);

    base_page = addr/PAGE_SIZE;
    for (page_index=0;page_index<npages;page_index++){
        SetBit(pages_bitmap, base_page+page_index);
    }

	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
{
	paddr_t pa;
	struct addrspace* as = as_create();
	
	dumbvm_can_sleep();
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	vaddr_t va = PADDR_TO_KVADDR(pa);

	as_define_kernel_region(as, va, pa, npages);

	if (insert_addrspace_in_list(as)==0)
		return 0;
	
	return va;
}
int insert_addrspace_in_list(struct addrspace* as){
	struct node_list* old_head = vm_addrspace_list->head;
	struct node_list* new_node = kmalloc(sizeof(struct node_list));
	if (new_node==NULL)
		return 0;
	new_node->as = as;
	vm_addrspace_list->head = new_node;
	vm_addrspace_list->head->next = old_head;
	return 1;
}

void
free_kpages(vaddr_t addr)
{
    //TODO implement ram_freemem and update bitmap
    paddr_t pa = KVADDR_TO_PADDR(addr);
	(void)pa;
	//freeppages()

}


void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase_code, vtop_code, vbase_data, vtop_data, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
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

	if (faultaddress >= vbase_code && faultaddress < vtop_code) {
		paddr = (faultaddress - vbase_code) + as->as_pbase_code;
	}
	else if (faultaddress >= vbase_data && faultaddress < vtop_data) {
		paddr = (faultaddress - vbase_data) + as->as_pbase_data;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_pbase_stack;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
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

