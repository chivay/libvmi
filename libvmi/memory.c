/*
 * The LibVMI Library is an introspection library that simplifies access to 
 * memory in a target virtual machine or in a file containing a dump of 
 * a system's physical memory.  LibVMI is based on the XenAccess Library.
 *
 * Copyright (C) 2010 Sandia National Laboratories
 * Author: Bryan D. Payne (bpayne@sandia.gov)
 */

#include <stdlib.h>
#include <sys/mman.h>
#include "libvmi.h"
#include "private.h"

/* hack to get this to compile on xen 3.0.4 */
#ifndef XENMEM_maximum_gpfn
#define XENMEM_maximum_gpfn 0
#endif

/* convert a pfn to a mfn based on the live mapping tables */
unsigned long helper_pfn_to_mfn (vmi_instance_t instance, unsigned long pfn)
{
#ifdef ENABLE_XEN
    shared_info_t *live_shinfo = NULL;
    unsigned long *live_pfn_to_mfn_frame_list_list = NULL;
    unsigned long *live_pfn_to_mfn_frame_list = NULL;

    /* Live mapping of the table mapping each PFN to its current MFN. */
    unsigned long *live_pfn_to_mfn_table = NULL;
    unsigned long nr_pfns = 0;
    unsigned long ret = -1;
//    unsigned long mfn;
//    int i;

    if (instance->hvm){
        return pfn;
    }

    if (NULL == instance->m.xen.live_pfn_to_mfn_table){
        live_shinfo = vmi_mmap_mfn(
            instance, PROT_READ, instance->m.xen.info.shared_info_frame);
        if (live_shinfo == NULL){
            fprintf(stderr, "ERROR: failed to init live_shinfo\n");
            goto error_exit;
        }

        if (instance->m.xen.xen_version == VMI_XENVER_3_1_0){
            nr_pfns = xc_memory_op(
                        instance->m.xen.xc_handle,
                        XENMEM_maximum_gpfn,
                        &(instance->m.xen.domain_id)) + 1;
        }
        else{
            nr_pfns = live_shinfo->arch.max_pfn;
        }

        live_pfn_to_mfn_frame_list_list = vmi_mmap_mfn(
            instance, PROT_READ, live_shinfo->arch.pfn_to_mfn_frame_list_list);
        if (live_pfn_to_mfn_frame_list_list == NULL){
            fprintf(stderr, "ERROR: failed to init live_pfn_to_mfn_frame_list_list\n");
            goto error_exit;
        }

        live_pfn_to_mfn_frame_list = xc_map_foreign_batch(
            instance->m.xen.xc_handle,
            instance->m.xen.domain_id,
            PROT_READ,
            live_pfn_to_mfn_frame_list_list,
            (nr_pfns+(fpp*fpp)-1)/(fpp*fpp) );
        if (live_pfn_to_mfn_frame_list == NULL){
            fprintf(stderr, "ERROR: failed to init live_pfn_to_mfn_frame_list\n");
            goto error_exit;
        }

        live_pfn_to_mfn_table = xc_map_foreign_batch(
            instance->m.xen.xc_handle,
            instance->m.xen.domain_id,
            PROT_READ,
            live_pfn_to_mfn_frame_list, (nr_pfns+fpp-1)/fpp );
        if (live_pfn_to_mfn_table  == NULL){
            fprintf(stderr, "ERROR: failed to init live_pfn_to_mfn_table\n");
            goto error_exit;
        }

        /*TODO validate the mapping */
//        for (i = 0; i < nr_pfns; ++i){
//            mfn = live_pfn_to_mfn_table[i];
//            if( (mfn != INVALID_P2M_ENTRY) && (mfn_to_pfn(mfn) != i) )
//            {
//                DPRINTF("i=0x%x mfn=%lx live_m2p=%lx\n", i,
//                        mfn, mfn_to_pfn(mfn));
//                err++;
//            }
//        }

        /* save mappings for later use */
        instance->m.xen.live_pfn_to_mfn_table = live_pfn_to_mfn_table;
        instance->m.xen.nr_pfns = nr_pfns;
    }

    ret = instance->m.xen.live_pfn_to_mfn_table[pfn];

error_exit:
    if (live_shinfo) munmap(live_shinfo, XC_PAGE_SIZE);
    if (live_pfn_to_mfn_frame_list_list)
        munmap(live_pfn_to_mfn_frame_list_list, XC_PAGE_SIZE);
    if (live_pfn_to_mfn_frame_list)
        munmap(live_pfn_to_mfn_frame_list, XC_PAGE_SIZE);

    return ret;
#else
    return 0;
#endif /* ENABLE_XEN */
}

void *vmi_mmap_mfn (vmi_instance_t instance, int prot, unsigned long mfn)
{
//    dbprint("--MapMFN: Mapping mfn = 0x%.8x.\n", (unsigned int)mfn);
    return vmi_map_page(instance, prot, mfn);
}

void *vmi_mmap_pfn (vmi_instance_t instance, int prot, unsigned long pfn)
{
    unsigned long mfn = -1;

    if (VMI_MODE_XEN == instance->mode){
        mfn = helper_pfn_to_mfn(instance, pfn);
    }
    else if (VMI_MODE_FILE == instance->mode){
        mfn = pfn;
    }

    if (-1 == mfn){
        fprintf(stderr, "ERROR: pfn to mfn mapping failed.\n");
        return NULL;
    }
    else{
//        dbprint("--MapPFN: Mapping mfn = %lu / pfn = %lu.\n", mfn, pfn);
        return vmi_map_page(instance, prot, mfn);
    }
}

/* bit flag testing */
int entry_present (unsigned long entry){
    return vmi_get_bit(entry, 0);
}

int page_size_flag (unsigned long entry){
    return vmi_get_bit(entry, 7);
}

/* page directory pointer table */
uint32_t get_pdptb (uint32_t pdpr){
    return pdpr & 0xFFFFFFE0;
}

uint32_t pdpi_index (uint32_t pdpi){
    return (pdpi >> 30) * sizeof(uint64_t);
}

uint64_t get_pdpi (vmi_instance_t instance, uint32_t vaddr, uint32_t cr3)
{
    uint64_t value;
    uint32_t pdpi_entry = get_pdptb(cr3) + pdpi_index(vaddr);
    dbprint("--PTLookup: pdpi_entry = 0x%.8x\n", pdpi_entry);
    vmi_read_long_long_mach(instance, pdpi_entry, &value);
    return value;
}

/* page directory */
uint32_t pgd_index (vmi_instance_t instance, uint32_t address){
    if (!instance->pae){
        return (((address) >> 22) & 0x3FF) * sizeof(uint32_t);
    }
    else{
        return (((address) >> 21) & 0x1FF) * sizeof(uint64_t);
    }
}

uint32_t pdba_base_nopae (uint32_t pdpe){
    return pdpe & 0xFFFFF000;
}

uint64_t pdba_base_pae (uint64_t pdpe){
    return pdpe & 0xFFFFFF000ULL;
}

uint32_t get_pgd_nopae (vmi_instance_t instance, uint32_t vaddr, uint32_t pdpe)
{
    uint32_t value;
    uint32_t pgd_entry = pdba_base_nopae(pdpe) + pgd_index(instance, vaddr);
    dbprint("--PTLookup: pgd_entry = 0x%.8x\n", pgd_entry);
    vmi_read_long_mach(instance, pgd_entry, &value);
    return value;
}

uint64_t get_pgd_pae (vmi_instance_t instance, uint32_t vaddr, uint64_t pdpe)
{
    uint64_t value;
    uint32_t pgd_entry = pdba_base_pae(pdpe) + pgd_index(instance, vaddr);
    dbprint("--PTLookup: pgd_entry = 0x%.8x\n", pgd_entry);
    vmi_read_long_long_mach(instance, pgd_entry, &value);
    return value;
}

/* page table */
uint32_t pte_index (vmi_instance_t instance, uint32_t address){
    if (!instance->pae){
        return (((address) >> 12) & 0x3FF) * sizeof(uint32_t);
    }
    else{
        return (((address) >> 12) & 0x1FF) * sizeof(uint64_t); 
    }
}
        
uint32_t ptba_base_nopae (uint32_t pde){
    return pde & 0xFFFFF000;
}

uint64_t ptba_base_pae (uint64_t pde){
    return pde & 0xFFFFFF000ULL;
}

uint32_t get_pte_nopae (vmi_instance_t instance, uint32_t vaddr, uint32_t pgd){
    uint32_t value;
    uint32_t pte_entry = ptba_base_nopae(pgd) + pte_index(instance, vaddr);
    dbprint("--PTLookup: pte_entry = 0x%.8x\n", pte_entry);
    vmi_read_long_mach(instance, pte_entry, &value);
    return value;
}

uint64_t get_pte_pae (vmi_instance_t instance, uint32_t vaddr, uint64_t pgd){
    uint64_t value;
    uint32_t pte_entry = ptba_base_pae(pgd) + pte_index(instance, vaddr);
    dbprint("--PTLookup: pte_entry = 0x%.8x\n", pte_entry);
    vmi_read_long_long_mach(instance, pte_entry, &value);
    return value;
}

/* page */
uint32_t pte_pfn_nopae (uint32_t pte){
    return pte & 0xFFFFF000;
}

uint64_t pte_pfn_pae (uint64_t pte){
    return pte & 0xFFFFFF000ULL;
}

uint32_t get_paddr_nopae (uint32_t vaddr, uint32_t pte){
    return pte_pfn_nopae(pte) | (vaddr & 0xFFF);
}

uint64_t get_paddr_pae (uint32_t vaddr, uint64_t pte){
    return pte_pfn_pae(pte) | (vaddr & 0xFFF);
}

uint32_t get_large_paddr (
        vmi_instance_t instance, uint32_t vaddr, uint32_t pgd_entry)
{
    if (!instance->pae){
        return (pgd_entry & 0xFFC00000) | (vaddr & 0x3FFFFF);
    }
    else{
        return (pgd_entry & 0xFFE00000) | (vaddr & 0x1FFFFF);
    }
}

/* "buffalo" routines
 * see "Using Every Part of the Buffalo in Windows Memory Analysis" by
 * Jesse D. Kornblum for details. 
 * for now, just test the bits and print out details */
int get_transition_bit(uint32_t entry)
{
    return vmi_get_bit(entry, 11);
}

int get_prototype_bit(uint32_t entry)
{
    return vmi_get_bit(entry, 10);
}

void buffalo_nopae (vmi_instance_t instance, uint32_t entry, int pde)
{
    /* similar techniques are surely doable in linux, but for now
     * this is only testing for windows domains */
    if (!instance->os_type == VMI_OS_WINDOWS){
        return;
    }

    if (!get_transition_bit(entry) && !get_prototype_bit(entry)){
        uint32_t pfnum = (entry >> 1) & 0xF;
        uint32_t pfframe = entry & 0xFFFFF000;

        /* pagefile */
        if (pfnum != 0 && pfframe != 0){
            dbprint("--Buffalo: page file = %d, frame = 0x%.8x\n",
                pfnum, pfframe);
        }
        /* demand zero */
        else if (pfnum == 0 && pfframe == 0){
            dbprint("--Buffalo: demand zero page\n");
        }
    }

    else if (get_transition_bit(entry) && !get_prototype_bit(entry)){
        /* transition */
        dbprint("--Buffalo: page in transition\n");
    }

    else if (!pde && get_prototype_bit(entry)){
        /* prototype */
        dbprint("--Buffalo: prototype entry\n");
    }

    else if (entry == 0){
        /* zero */
        dbprint("--Buffalo: entry is zero\n");
    }

    else{
        /* zero */
        dbprint("--Buffalo: unknown\n");
    }
}

/* translation */
uint32_t v2p_nopae(vmi_instance_t instance, uint32_t cr3, uint32_t vaddr)
{
    uint32_t paddr = 0;
    uint32_t pgd, pte;
        
    dbprint("--PTLookup: lookup vaddr = 0x%.8x\n", vaddr);
    dbprint("--PTLookup: cr3 = 0x%.8x\n", cr3);
    pgd = get_pgd_nopae(instance, vaddr, cr3);
    dbprint("--PTLookup: pgd = 0x%.8x\n", pgd);
        
    if (entry_present(pgd)){
        if (page_size_flag(pgd)){
            paddr = get_large_paddr(instance, vaddr, pgd);
            dbprint("--PTLookup: 4MB page\n", pgd);
        }
        else{
            pte = get_pte_nopae(instance, vaddr, pgd);
            dbprint("--PTLookup: pte = 0x%.8x\n", pte);
            if (entry_present(pte)){
                paddr = get_paddr_nopae(vaddr, pte);
            }
            else{
                buffalo_nopae(instance, pte, 1);
            }
        }
    }
    else{
        buffalo_nopae(instance, pgd, 0);
    }
    dbprint("--PTLookup: paddr = 0x%.8x\n", paddr);
    return paddr;
}

uint32_t v2p_pae (vmi_instance_t instance, uint32_t cr3, uint32_t vaddr)
{
    uint32_t paddr = 0;
    uint64_t pdpe, pgd, pte;
        
    dbprint("--PTLookup: lookup vaddr = 0x%.8x\n", vaddr);
    dbprint("--PTLookup: cr3 = 0x%.8x\n", cr3);
    pdpe = get_pdpi(instance, vaddr, cr3);
    dbprint("--PTLookup: pdpe = 0x%.16x\n", pdpe);
    if (!entry_present(pdpe)){
        return paddr;
    }
    pgd = get_pgd_pae(instance, vaddr, pdpe);
    dbprint("--PTLookup: pgd = 0x%.16x\n", pgd);

    if (entry_present(pgd)){
        if (page_size_flag(pgd)){
            paddr = get_large_paddr(instance, vaddr, pgd);
            dbprint("--PTLookup: 2MB page\n");
        }
        else{
            pte = get_pte_pae(instance, vaddr, pgd);
            dbprint("--PTLookup: pte = 0x%.16x\n", pte);
            if (entry_present(pte)){
                paddr = get_paddr_pae(vaddr, pte);
            }
        }
    }
    dbprint("--PTLookup: paddr = 0x%.8x\n", paddr);
    return paddr;
}

/* convert address to machine address via page tables */
uint32_t vmi_pagetable_lookup (
            vmi_instance_t instance,
            uint32_t cr3,
            uint32_t vaddr)
{
    if (instance->pae){
        return v2p_pae(instance, cr3, vaddr);
    }
    else{
        return v2p_nopae(instance, cr3, vaddr);
    }
}

uint32_t vmi_current_cr3 (vmi_instance_t instance, uint32_t *cr3)
{
    int ret = VMI_SUCCESS;
#ifdef ENABLE_XEN
#ifdef HAVE_CONTEXT_ANY
    vcpu_guest_context_any_t ctxt_any;
#endif /* HAVE_CONTEXT_ANY */
    vcpu_guest_context_t ctxt;
#endif /* ENABLE_XEN */

    if (VMI_MODE_XEN == instance->mode){
#ifdef ENABLE_XEN
#ifdef HAVE_CONTEXT_ANY
        if ((ret = xc_vcpu_getcontext(
                instance->m.xen.xc_handle,
                instance->m.xen.domain_id,
                0, /*TODO vcpu, assuming only 1 for now */
                &ctxt_any)) != 0){
#else
        if ((ret = xc_vcpu_getcontext(
                instance->m.xen.xc_handle,
                instance->m.xen.domain_id,
                0, /*TODO vcpu, assuming only 1 for now */
                &ctxt)) != 0){
#endif /* HAVE_CONTEXT_ANY */
            fprintf(stderr, "ERROR: failed to get context information.\n");
            ret = VMI_FAILURE;
            goto error_exit;
        }
#ifdef HAVE_CONTEXT_ANY
        *cr3 = ctxt_any.c.ctrlreg[3] & 0xFFFFF000;
#else
        *cr3 = ctxt.ctrlreg[3] & 0xFFFFF000;
#endif /* HAVE_CONTEXT_ANY */
#endif /* ENABLE_XEN */
    }
    else if (VMI_MODE_FILE == instance->mode){
        *cr3 = instance->kpgd - instance->page_offset;
    }

error_exit:
    return ret;
}

/* expose virtual to physical mapping via api call */
uint32_t vmi_translate_kv2p(vmi_instance_t instance, uint32_t virt_address)
{
    uint32_t cr3 = 0;
    vmi_current_cr3(instance, &cr3);
    return vmi_pagetable_lookup(instance, cr3, virt_address);
}

/* map memory given a kernel symbol */
void *vmi_access_kernel_sym (
        vmi_instance_t instance, char *symbol, uint32_t *offset, int prot)
{
    if (VMI_OS_LINUX == instance->os_type){
        return linux_access_kernel_symbol(instance, symbol, offset, prot);
    }
    else if (VMI_OS_WINDOWS == instance->os_type){
        return windows_access_kernel_symbol(instance, symbol, offset, prot);
    }
    else{
        return NULL;
    }
}

/* finds the address of the page global directory for a given pid */
uint32_t vmi_pid_to_pgd (vmi_instance_t instance, int pid)
{
    /* first check the cache */
    uint32_t pgd = 0;
    if (vmi_check_pid_cache(instance, pid, &pgd)){
        /* nothing */
    }

    /* otherwise do the lookup */
    else if (VMI_OS_LINUX == instance->os_type){
        pgd = linux_pid_to_pgd(instance, pid);
    }
    else if (VMI_OS_WINDOWS == instance->os_type){
        pgd = windows_pid_to_pgd(instance, pid);
    }

    return pgd;
}

void *vmi_access_user_va (
        vmi_instance_t instance,
        uint32_t virt_address,
        uint32_t *offset,
        int pid,
        int prot)
{
    uint32_t address = 0;

    /* check the LRU cache */
    if (vmi_check_cache_virt(instance, virt_address, pid, &address)){
        return vmi_access_ma(instance, address, offset, prot);
    }

    /* use kernel page tables */
    /*TODO HYPERVISOR_VIRT_START = 0xFC000000 so we can't go over that.
      Figure out what this should be b/c there still may be a fixed
      mapping range between the page'd addresses and VIRT_START */
    if (!pid){
        uint32_t cr3 = 0;
        vmi_current_cr3(instance, &cr3);
        address = vmi_pagetable_lookup(instance, cr3, virt_address);
        if (!address){
            fprintf(stderr, "ERROR: address not in page table (0x%x)\n", virt_address);
            return NULL;
        }
    }

    /* use user page tables */
    else{
        uint32_t pgd = vmi_pid_to_pgd(instance, pid);
        dbprint("--UserVirt: pgd for pid=%d is 0x%.8x.\n", pid, pgd);

        if (pgd){
            address = vmi_pagetable_lookup(instance, pgd, virt_address);
        }

        if (!address){
            fprintf(stderr, "ERROR: address not in page table (0x%x)\n", virt_address);
            return NULL;
        }
    }

    /* update cache and map the memory */
    vmi_update_cache(instance, NULL, virt_address, pid, address);
    return vmi_access_ma(instance, address, offset, prot);
}

/*TODO find a way to support this in file mode */
void *vmi_access_user_va_range (
        vmi_instance_t instance,
        uint32_t virt_address,
		uint32_t size,
        uint32_t *offset,
        int pid,
        int prot)
{
#ifdef ENABLE_XEN
    int i = 0;
    uint32_t num_pages = size / instance->page_size + 1;
    uint32_t pgd = 0;

    if (pid){
        pgd = vmi_pid_to_pgd(instance, pid);
    }
    else{
        vmi_current_cr3(instance, &pgd);
    }
    xen_pfn_t* pfns = (xen_pfn_t*) malloc(sizeof(xen_pfn_t) * num_pages);
	
    uint32_t start = virt_address & ~(instance->page_size - 1);
    for (i = 0; i < num_pages; i++){
        /* Virtual address for each page we will map */
        uint32_t addr = start + i * instance->page_size;
	
        if(!addr) {
            fprintf(stderr, "ERROR: address not in page table (%p)\n", addr);
            return NULL;
        }

        /* Physical page frame number of each page */
        pfns[i] = vmi_pagetable_lookup(
            instance, pgd, addr) >> instance->page_shift;
    }

    *offset = virt_address - start;

    return xc_map_foreign_pages(
        instance->m.xen.xc_handle,
        instance->m.xen.domain_id, prot, pfns, num_pages
    );
#else
    return NULL;
#endif /* ENABLE_XEN */
}

void *vmi_access_kernel_va (
        vmi_instance_t instance,
        uint32_t virt_address,
        uint32_t *offset,
        int prot)
{
    return vmi_access_user_va(instance, virt_address, offset, 0, prot);
}

void *vmi_access_kernel_va_range (
	vmi_instance_t instance,
	uint32_t virt_address,
	uint32_t size,
	uint32_t* offset,
    int prot)
{
	return vmi_access_user_va_range(
        instance, virt_address, size, offset, 0, prot);
}

void *vmi_access_pa (
        vmi_instance_t instance,
        uint32_t phys_address,
        uint32_t *offset,
        int prot)
{
    unsigned long pfn;
    int i;
    
    /* page frame number = physical address >> PAGE_SHIFT */
    pfn = phys_address >> instance->page_shift;
    
    /* get the offset */
    *offset = (instance->page_size-1) & phys_address;
    
    /* access the memory */
    return vmi_mmap_pfn(instance, prot, pfn);
}

void *vmi_access_ma (
        vmi_instance_t instance,
        uint32_t mach_address,
        uint32_t *offset,
        int prot)
{
    unsigned long mfn;
    int i;

    /* machine frame number = machine address >> PAGE_SHIFT */
    mfn = mach_address >> instance->page_shift;

    /* get the offset */
    *offset = (instance->page_size-1) & mach_address;

    /* access the memory */
    return vmi_mmap_mfn(instance, prot, mfn);
}

