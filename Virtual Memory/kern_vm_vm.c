#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>

#include <spl.h>
#include <current.h>
#include <proc.h>

/* Place your page table functions here */

/* Return the page_table_entry of the VPN, 0 if not exist */
uint32_t page_table_lookup(struct addrspace *as, vaddr_t fault_addr) {
    /* Extract page table indices 8 + 6 + 6 */
    uint32_t first_level_index = fault_addr >> 24;
    uint32_t second_level_index = (fault_addr << 8) >> 26;
    uint32_t third_level_index = (fault_addr << 14) >> 26;

    if (as->page_table == NULL) return 0;
    if (as->page_table[first_level_index] == NULL) return 0;
    if (as->page_table[first_level_index][second_level_index] == NULL) return 0;
    if (as->page_table[first_level_index][second_level_index][third_level_index] == 0) return 0;

    return as->page_table[first_level_index][second_level_index][third_level_index];
}

/* Insert a new mapping to the page number, -1 if unsuccess */
int insert_into_page_table(struct addrspace *as, uint32_t new_pte, vaddr_t fault_addr) {
    /* Extract page table indices 8 + 6 + 6 */
    uint32_t first_level_index = (fault_addr & VADDR_LEVEL_ONE) >> VADDR_LEVEL_ONE_SHIFT;
    uint32_t second_level_index = (fault_addr & VADDR_LEVEL_TWO) >> VADDR_LEVEL_TWO_SHIFT;
    uint32_t third_level_index = (fault_addr & VADDR_LEVEL_THREE) >> VADDR_LEVEL_THREE_SHIFT;

    /* Allocate page table if needed */
    if (as->page_table[first_level_index] == NULL) {
        if ((as->page_table[first_level_index] = kmalloc(sizeof(uint32_t *) * VADDR_LEVEL_TWO_SIZE)) == NULL) return ENOMEM;
    
        /* Initialize to all null */
        for (int i = 0; i < VADDR_LEVEL_TWO_SIZE; i++) as->page_table[first_level_index][i] = NULL;
    }
    if (as->page_table[first_level_index][second_level_index] == NULL) {
        if ((as->page_table[first_level_index][second_level_index] = kmalloc(sizeof(uint32_t) * VADDR_LEVEL_THREE_SIZE)) == NULL) return ENOMEM;
    
        /* Initialize to all 0 */
        for (int i = 0; i < VADDR_LEVEL_THREE_SIZE; i++) as->page_table[first_level_index][second_level_index][i] = 0;
    }

    /* Insert the new_pte */
    as->page_table[first_level_index][second_level_index][third_level_index] = new_pte;

    /* Success */
    return 0;
}

/* Return the as_region that the address belongs to, if not found, return NULL */
struct as_region *addr_to_region(struct addrspace *as, vaddr_t fault_addr) {
    struct as_region *corresponding_region = NULL;
    struct as_region_node *curr = as->as_regions_head;

    /* Locate the address in the as_regions */
    while (curr != NULL) {
        struct as_region *as_region = curr->as_region;

        /*If the fault_addr is within this region */
        if ((fault_addr >= as_region->vbase) && (fault_addr < as_region->vtop)) {
            corresponding_region = as_region;
            break;
        }

        curr = curr->next;
    }

    return corresponding_region;
}

/* Initialize pte based on as_region permission and PFN */
uint32_t init_pte(struct as_region *fault_region, vaddr_t new_page) {
    uint32_t new_pte = new_page & TLBLO_PPAGE;

    /* Set book keeping bits */
    if (fault_region->writeable) new_pte |= TLBLO_DIRTY;

    if (fault_region->readable || fault_region->writeable || fault_region->executable) new_pte |= TLBLO_VALID;

    return new_pte;
}

/* Load EntryHi, EntryLo pair into TLB*/
void load_into_tlb(vaddr_t fault_addr, uint32_t pte) {
    int spl = splhigh();
    tlb_random(fault_addr & TLBHI_VPAGE, pte);
    splx(spl);
}

void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}

// TLB exception handler
int
vm_fault(int faulttype, vaddr_t faultaddress)
{   
    struct addrspace *as;

    switch (faulttype) {
	    case VM_FAULT_READONLY:             // Write to Read-only page
            return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

    if (curproc == NULL) {
		return EFAULT;
	}

    as = proc_getas();
	if (as == NULL) {
		return EFAULT;
	}

    uint32_t pte = page_table_lookup(as, faultaddress);

    /* If the page exists in memory */
    if (pte & TLBLO_VALID) {

        /* Check write permission */
        if ((faulttype == VM_FAULT_WRITE) && ((pte & TLBLO_DIRTY) == 0) && (as->loading_flag == 0)) {
            return EFAULT;  /* Write to read-only page */
        }

        /* Load the mapping to TLB, if loading_flag is set, write is allowed */
        load_into_tlb(faultaddress, pte | as->loading_flag);

        /* Return 0 on success */
        return 0;
    }

    /* If no valid translation, allocate new page */
    struct as_region *fault_region = addr_to_region(as, faultaddress);
    if (fault_region == NULL) return EFAULT;    /* Region not exist, bad memory reference */

    /* Check write permission */
    if ((faulttype == VM_FAULT_WRITE) && (fault_region->writeable == 0) && (as->loading_flag == 0)) {
        return EFAULT;  /* Write to read-only page */
    }

    /* Allocate a new page */
    vaddr_t new_page = alloc_kpages(1);     /* This is kernal space address */
    if (new_page == 0) return ENOMEM;       /* Not enough memory */

    /* Zero out the new page */
    bzero((void *) new_page, 4096);
    
    /* Initialize a page of a certain region */
    uint32_t new_pte = init_pte(fault_region, KVADDR_TO_PADDR(new_page));

    /* Insert the page table entry into the process's page table */
    int ret = insert_into_page_table(as, new_pte, faultaddress);

    if (ret) {
        free_kpages(new_page);

        return ret;
    }

    /* Load the mapping to TLB */
    load_into_tlb(faultaddress, new_pte | as->loading_flag);

    /* Success */
    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}
