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
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

#include <machine/tlb.h>

#define VADDR_LEVEL_ONE_SIZE 256
#define VADDR_LEVEL_TWO_SIZE 64
#define VADDR_LEVEL_THREE_SIZE 64
#define USERSTACKSIZE 16 * PAGE_SIZE
/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;
	if ((as = kmalloc(sizeof(struct addrspace))) == NULL) {
		return NULL;
	}

	/* Initialize the first level of page table */
	if ((as->page_table = kmalloc(sizeof(uint32_t **) * VADDR_LEVEL_ONE_SIZE)) == NULL){
		kfree(as);
		return NULL;
	}

	for (int i = 0; i < VADDR_LEVEL_ONE_SIZE; i++){
		as->page_table[i] = NULL;
	}

	/* Initialise region list */
	as->as_regions_head = NULL;

	/* Initialize loading flag */
	as->loading_flag = 0;

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;
	if ((newas = as_create()) == NULL) {
		return ENOMEM;
	}

	/* Copy page table */
	for (int i = 0; i < VADDR_LEVEL_ONE_SIZE; i++) {
		if (old->page_table[i] != NULL) {
			/* Allocate level two */
			if ((newas->page_table[i] = kmalloc(VADDR_LEVEL_TWO_SIZE * sizeof(uint32_t *))) == NULL) {
				as_destroy(newas);
				*ret = NULL;
				return ENOMEM;
			}

			/* Copy level two */
			for (int j = 0; j < VADDR_LEVEL_TWO_SIZE; j++) {
				if (old->page_table[i][j] == NULL) {
					newas->page_table[i][j] = NULL;
				} else {
					/* Allocate level three */
					if ((newas->page_table[i][j] = kmalloc(VADDR_LEVEL_THREE_SIZE * sizeof(uint32_t))) == NULL) {
						as_destroy(newas);
						*ret = NULL;
						return ENOMEM;
					}

					/* Copy level three */
					for (int k = 0; k < VADDR_LEVEL_THREE_SIZE; k++) {
						if (old->page_table[i][j][k] == 0) {
							newas->page_table[i][j][k] = 0;
						} else {
							vaddr_t new_page = alloc_kpages(1);
							bzero((void *)new_page, PAGE_SIZE);

							/* Copy page frame cotent */
							memmove((void *)new_page, (const void*)PADDR_TO_KVADDR(old->page_table[i][j][k] & PAGE_FRAME), PAGE_SIZE);

							/* Copy page table content */
							newas->page_table[i][j][k] = (KVADDR_TO_PADDR(new_page) & PAGE_FRAME) | (old->page_table[i][j][k] & (~(PAGE_FRAME)));
						}
					}
				}
			}
		}
	}

	/* Copy regions list */
	struct as_region_node *curr = old->as_regions_head;
	while (curr != NULL){
		struct as_region *region = curr->as_region;
		as_define_region(newas, region->vbase, region->memsize, region->readable, region->writeable, region->executable);
		curr = curr->next;
	}

	*ret = newas;
	return 0;
}

/* Free all memory used by as */
void
as_destroy(struct addrspace *as)
{	
	if (as == NULL) return;

	/* Clear regions */
	struct as_region_node *curr = as->as_regions_head;
	struct as_region_node *prev = NULL;
	while (curr != NULL) {
		prev = curr;
		curr = curr->next;
		kfree(prev->as_region);
		kfree(prev);
	}
	
	/* Clear page table */
	if (as->page_table != NULL) {
		for (int i = 0; i < VADDR_LEVEL_ONE_SIZE; i++) {

			if (as->page_table[i] != NULL) {
				for (int j = 0; j < VADDR_LEVEL_TWO_SIZE; j++) {
					
					if (as->page_table[i][j] != NULL) {
						for (int k = 0; k < VADDR_LEVEL_THREE_SIZE; k++){
							if (as->page_table[i][j][k] != 0){
								free_kpages(PADDR_TO_KVADDR(as->page_table[i][j][k] & PAGE_FRAME));
							}
						}
						kfree(as->page_table[i][j]);
					}
				}	
				kfree(as->page_table[i]);
			}
		}
		kfree(as->page_table);
	}

	kfree(as);
}

// Flush TLB
void
as_activate(void)
{	
	struct addrspace *as;
	if ((as = proc_getas()) == NULL) return;

	tlb_flush();
}

// Flush TLB
void
as_deactivate(void)
{
	as_activate();
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
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	/* Validate input */
	if(as == NULL) return EFAULT;

	if(vaddr + memsize > USERSPACETOP) return ENOMEM;

	/* Initialize */
	struct as_region *region;
	if((region = kmalloc(sizeof(struct as_region))) == NULL){
		return ENOMEM;
	}

	region->vbase = vaddr;
	region->vtop = vaddr + memsize;
	region->memsize = memsize;

	region->readable = readable;
	region->writeable = writeable;
	region->executable = executable; 

	/* Insert the region node to the regions list */
	struct as_region_node *new_region_node;
	if((new_region_node = kmalloc(sizeof(struct as_region_node))) == NULL){
		return ENOMEM;
	}
	new_region_node->as_region = region;
	new_region_node->next = NULL;

	/* Insert to the front */
	new_region_node->next = as->as_regions_head;
	as->as_regions_head = new_region_node;

	return 0;
}

// Make readonly region to RW
int
as_prepare_load(struct addrspace *as)
{
	if (as == NULL){
		return EFAULT;
	}

	as->loading_flag = TLBLO_DIRTY;

	tlb_flush();

	return 0;
}

// Enforce R-Only again
int
as_complete_load(struct addrspace *as)
{
	if (as == NULL){
		return EFAULT;
	}

	as->loading_flag = 0;

	tlb_flush();

	return 0;
}

// Define the range of the stack for a user program
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{	
	if (as == NULL) return EFAULT;

	/* Define the stack as a region */
	int ret = as_define_region(as, USERSPACETOP - USERSTACKSIZE, USERSTACKSIZE, 1, 1, 0); 
	if (ret != 0) {
		return ret;	
	}

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

/* Flush the tlb */
void tlb_flush(void) {
	/* Disable interrupts on this CPU while frobbing the TLB. */
	int spl = splhigh();

	for (int i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}