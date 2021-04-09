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
#include <spinlock.h>
#include <proc.h>
#include <addrspace.h>
#include <vm.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct addrspace* as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
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

void
as_destroy(struct addrspace *as)
{
	dumbvm_can_sleep();
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
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
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
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

	if (as->as_vbase_code == 0) {
		as->as_vbase_code = vaddr;
		as->as_npages_code = npages;
		return 0;
	}

	if (as->as_vbase_data == 0) {
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

int as_define_kernel_region(struct addrspace *as, vaddr_t vaddr, paddr_t paddr, size_t npages){
	as->as_pbase_code = paddr;
	as->as_vbase_code = vaddr;
	as->as_npages_code = npages;
	return 0;

}
int insert_addrspace_in_list(struct addrspace* as, struct addrspace_list* vm_addrspace_list){
	struct node_list* old_head = vm_addrspace_list->head;
	struct node_list* new_node = kmalloc(sizeof(struct node_list));
	if (new_node==NULL)
		return 0;
	new_node->as = as;
	vm_addrspace_list->head = new_node;
	vm_addrspace_list->head->next = old_head;
	return 1;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

//TODO to modify to insert bitmap
int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_pbase_code == 0);
	KASSERT(as->as_pbase_data == 0);
	KASSERT(as->as_pbase_stack == 0);

	dumbvm_can_sleep();

	as->as_pbase_code = getppages(as->as_npages_code);
	if (as->as_pbase_code == 0) {
		return ENOMEM;
	}

	as->as_pbase_data = getppages(as->as_npages_data);
	if (as->as_pbase_data == 0) {
		return ENOMEM;
	}

	as->as_pbase_stack = getppages(DUMBVM_STACKPAGES);
	if (as->as_pbase_stack == 0) {
		return ENOMEM;
	}

	as_zero_region(as->as_pbase_code, as->as_npages_code);
	as_zero_region(as->as_pbase_data, as->as_npages_data);
	as_zero_region(as->as_pbase_stack, DUMBVM_STACKPAGES);

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	dumbvm_can_sleep();
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_pbase_stack != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	dumbvm_can_sleep();

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase_code = old->as_vbase_code;
	new->as_npages_code = old->as_npages_code;
	new->as_vbase_data = old->as_vbase_data;
	new->as_npages_data = old->as_npages_data;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase_code != 0);
	KASSERT(new->as_pbase_data != 0);
	KASSERT(new->as_pbase_stack != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase_code),
		(const void *)PADDR_TO_KVADDR(old->as_pbase_code),
		old->as_npages_code*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase_data),
		(const void *)PADDR_TO_KVADDR(old->as_pbase_data),
		old->as_npages_data*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase_stack),
		(const void *)PADDR_TO_KVADDR(old->as_pbase_stack),
		DUMBVM_STACKPAGES*PAGE_SIZE);

	*ret = new;
	return 0;
}