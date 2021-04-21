/*
 *  mm/mprotect.c
 *
 *  (C) Copyright 1994 Linus Torvalds
 *  (C) Copyright 2002 Christoph Hellwig
 *
 *  Address space accounting code	<alan@lxorguk.ukuu.org.uk>
 *  (C) Copyright 2002 Red Hat Inc, All Rights Reserved
 */

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/mempolicy.h>
#include <linux/personality.h>
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/perf_event.h>
#include <linux/ksm.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include "internal.h"

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/init.h>
#include <linux/errno.h>

#include <xen/interface/xen.h>
#include <xen/interface/hvm/hvm_op.h>
#include <xen/hvm.h>



void register_pfn(uint64_t pfn)
{
	struct spp_pfn *ptr = get_current()->spp_head;
	if (ptr == NULL) {
		printk("register %lx\n", pfn);
		ptr = (struct spp_pfn *) kmalloc(sizeof(struct spp_pfn), GFP_KERNEL);
		ptr->pfn = pfn;
		ptr->next = NULL;
		get_current()->spp_head = ptr;
		return;
	}
	struct spp_pfn *it = ptr;
	
	while(it) {
		if (it->pfn == pfn) return;
		it = it->next;
	}
	
	printk("register %lx\n", pfn);
	it = (struct spp_pfn *) kmalloc(sizeof(struct spp_pfn), GFP_KERNEL);
	it->pfn = pfn;
	it->next = ptr;
	get_current()->spp_head = it;
	return;
}

void ajouter_list(xen_hvm_subpage_t* spp, uint64_t courant){
	
	xen_hvm_subpage_t *pel = (xen_hvm_subpage_t *) kmalloc(sizeof(xen_hvm_subpage_t), GFP_KERNEL);
	//on remplit une nouvelle structure
	pel->domid = DOMID_SELF;
	pel->subpage = 100;
	pel->gfn = courant;
	pel->next = NULL;

	if (spp == NULL){
		//on ajoute la structure
		spp = pel;
		return ;
	}
	xen_hvm_subpage_t *tmp = spp;
	while (tmp->next != NULL){
		tmp = tmp->next;
	}
	//on ajoute la structure
	tmp->next = pel;
	//kfree(pel);
	kfree(tmp);
}

//Modifications Augusta
void unregister_pfn(void)
{

	xen_hvm_subpage_t spp, *tmp;
	spp.domid = DOMID_SELF;
	spp.subpage = 100;

	struct spp_pfn *ptr = get_current()->spp_head;
	struct spp_pfn *it = ptr;
	
	while(ptr) {
		printk("unregister: %lx\n", ptr->pfn);
		spp.gfn = ptr->pfn;
		ptr = ptr->next;
		kfree(it);
		it = ptr;

		ajouter_list(tmp, spp.gfn);
	}
	HYPERVISOR_hvm_op(HVMOP_set_subpage, tmp);

	
	if(tmp!=NULL){
		xen_hvm_subpage_t *pel = tmp;
		while (tmp != NULL){
			kfree(tmp);
			pel = pel->next;
			tmp=pel;
		}
	}

	get_current()->spp_head = NULL;
}


static long get_pfn(unsigned long user_addr, 
				 unsigned long *pfn)
{
	struct vm_area_struct *vma;
	long ret;

	down_read(&current->mm->mmap_sem);
	ret = -EINVAL;
	vma = find_vma(current->mm, user_addr);
	if (!vma)
		goto out;
	unsigned long old = vma->vm_flags;
	vma->vm_flags |= VM_IO | VM_PFNMAP;
	ret = follow_pfn(vma, user_addr, pfn);
out:
	vma->vm_flags = old;
	up_read(&current->mm->mmap_sem);
	return ret;
}



/*
 * For a prot_numa update we only hold mmap_sem for read so there is a
 * potential race with faulting where a pmd was temporarily none. This
 * function checks for a transhuge pmd under the appropriate lock. It
 * returns a pte if it was successfully locked or NULL if it raced with
 * a transhuge insertion.
 */
static pte_t *lock_pte_protection(struct vm_area_struct *vma, pmd_t *pmd,
			unsigned long addr, int prot_numa, spinlock_t **ptl)
{
	pte_t *pte;
	spinlock_t *pmdl;

	/* !prot_numa is protected by mmap_sem held for write */
	if (!prot_numa)
		return pte_offset_map_lock(vma->vm_mm, pmd, addr, ptl);

	pmdl = pmd_lock(vma->vm_mm, pmd);
	if (unlikely(pmd_trans_huge(*pmd) || pmd_none(*pmd))) {
		spin_unlock(pmdl);
		return NULL;
	}

	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, ptl);
	spin_unlock(pmdl);
	return pte;
}

static unsigned long change_pte_range(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long addr, unsigned long end, pgprot_t newprot,
		int dirty_accountable, int prot_numa)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte, oldpte;
	spinlock_t *ptl;
	unsigned long pages = 0;

	pte = lock_pte_protection(vma, pmd, addr, prot_numa, &ptl);
	if (!pte)
		return 0;

	flush_tlb_batched_pending(vma->vm_mm);
	arch_enter_lazy_mmu_mode();
	do {
		oldpte = *pte;
		if (pte_present(oldpte)) {
			pte_t ptent;
			bool preserve_write = prot_numa && pte_write(oldpte);

			/*
			 * Avoid trapping faults against the zero or KSM
			 * pages. See similar comment in change_huge_pmd.
			 */
			if (prot_numa) {
				struct page *page;

				page = vm_normal_page(vma, addr, oldpte);
				if (!page || PageKsm(page))
					continue;

				/* Avoid TLB flush if possible */
				if (pte_protnone(oldpte))
					continue;
			}

			ptent = ptep_modify_prot_start(mm, addr, pte);
			ptent = pte_modify(ptent, newprot);
			if (preserve_write)
				ptent = pte_mkwrite(ptent);

			/* Avoid taking write faults for known dirty pages */
			if (dirty_accountable && pte_dirty(ptent) &&
					(pte_soft_dirty(ptent) ||
					 !(vma->vm_flags & VM_SOFTDIRTY))) {
				ptent = pte_mkwrite(ptent);
			}
			ptep_modify_prot_commit(mm, addr, pte, ptent);
			pages++;
		} else if (IS_ENABLED(CONFIG_MIGRATION)) {
			swp_entry_t entry = pte_to_swp_entry(oldpte);

			if (is_write_migration_entry(entry)) {
				pte_t newpte;
				/*
				 * A protection check is difficult so
				 * just be safe and disable write
				 */
				make_migration_entry_read(&entry);
				newpte = swp_entry_to_pte(entry);
				if (pte_swp_soft_dirty(oldpte))
					newpte = pte_swp_mksoft_dirty(newpte);
				set_pte_at(mm, addr, pte, newpte);

				pages++;
			}
		}
	} while (pte++, addr += PAGE_SIZE, addr != end);
	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(pte - 1, ptl);

	return pages;
}

static inline unsigned long change_pmd_range(struct vm_area_struct *vma,
		pud_t *pud, unsigned long addr, unsigned long end,
		pgprot_t newprot, int dirty_accountable, int prot_numa)
{
	pmd_t *pmd;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long next;
	unsigned long pages = 0;
	unsigned long nr_huge_updates = 0;
	unsigned long mni_start = 0;

	pmd = pmd_offset(pud, addr);
	do {
		unsigned long this_pages;

		next = pmd_addr_end(addr, end);
		if (!pmd_trans_huge(*pmd) && pmd_none_or_clear_bad(pmd))
			continue;

		/* invoke the mmu notifier if the pmd is populated */
		if (!mni_start) {
			mni_start = addr;
			mmu_notifier_invalidate_range_start(mm, mni_start, end);
		}

		if (pmd_trans_huge(*pmd)) {
			if (next - addr != HPAGE_PMD_SIZE)
				split_huge_page_pmd(vma, addr, pmd);
			else {
				int nr_ptes = change_huge_pmd(vma, pmd, addr,
						newprot, prot_numa);

				if (nr_ptes) {
					if (nr_ptes == HPAGE_PMD_NR) {
						pages += HPAGE_PMD_NR;
						nr_huge_updates++;
					}

					/* huge pmd was handled */
					continue;
				}
			}
			/* fall through, the trans huge pmd just split */
		}
		this_pages = change_pte_range(vma, pmd, addr, next, newprot,
				 dirty_accountable, prot_numa);
		pages += this_pages;
	} while (pmd++, addr = next, addr != end);

	if (mni_start)
		mmu_notifier_invalidate_range_end(mm, mni_start, end);

	if (nr_huge_updates)
		count_vm_numa_events(NUMA_HUGE_PTE_UPDATES, nr_huge_updates);
	return pages;
}

static inline unsigned long change_pud_range(struct vm_area_struct *vma,
		pgd_t *pgd, unsigned long addr, unsigned long end,
		pgprot_t newprot, int dirty_accountable, int prot_numa)
{
	pud_t *pud;
	unsigned long next;
	unsigned long pages = 0;

	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		pages += change_pmd_range(vma, pud, addr, next, newprot,
				 dirty_accountable, prot_numa);
	} while (pud++, addr = next, addr != end);

	return pages;
}

static unsigned long change_protection_range(struct vm_area_struct *vma,
		unsigned long addr, unsigned long end, pgprot_t newprot,
		int dirty_accountable, int prot_numa)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	unsigned long next;
	unsigned long start = addr;
	unsigned long pages = 0;

	BUG_ON(addr >= end);
	pgd = pgd_offset(mm, addr);
	flush_cache_range(vma, addr, end);
	set_tlb_flush_pending(mm);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		pages += change_pud_range(vma, pgd, addr, next, newprot,
				 dirty_accountable, prot_numa);
	} while (pgd++, addr = next, addr != end);

	/* Only flush the TLB if we actually modified any entries: */
	if (pages)
		flush_tlb_range(vma, start, end);
	clear_tlb_flush_pending(mm);

	return pages;
}

unsigned long change_protection(struct vm_area_struct *vma, unsigned long start,
		       unsigned long end, pgprot_t newprot,
		       int dirty_accountable, int prot_numa)
{
	unsigned long pages;

	if (is_vm_hugetlb_page(vma))
		pages = hugetlb_change_protection(vma, start, end, newprot);
	else
		pages = change_protection_range(vma, start, end, newprot, dirty_accountable, prot_numa);

	return pages;
}

static int prot_none_pte_entry(pte_t *pte, unsigned long addr,
			       unsigned long next, struct mm_walk *walk)
{
	return pfn_modify_allowed(pte_pfn(*pte), *(pgprot_t *)(walk->private)) ?
		0 : -EACCES;
}

static int prot_none_hugetlb_entry(pte_t *pte, unsigned long hmask,
				   unsigned long addr, unsigned long next,
				   struct mm_walk *walk)
{
	return pfn_modify_allowed(pte_pfn(*pte), *(pgprot_t *)(walk->private)) ?
		0 : -EACCES;
}

static int prot_none_test(unsigned long addr, unsigned long next,
			  struct mm_walk *walk)
{
	return 0;
}

static int prot_none_walk(struct vm_area_struct *vma, unsigned long start,
			   unsigned long end, unsigned long newflags)
{
	pgprot_t new_pgprot = vm_get_page_prot(newflags);
	struct mm_walk prot_none_walk = {
		.pte_entry = prot_none_pte_entry,
		.hugetlb_entry = prot_none_hugetlb_entry,
		.test_walk = prot_none_test,
		.mm = current->mm,
		.private = &new_pgprot,
	};

	return walk_page_range(start, end, &prot_none_walk);
}

int
mprotect_fixup(struct vm_area_struct *vma, struct vm_area_struct **pprev,
	unsigned long start, unsigned long end, unsigned long newflags)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long oldflags = vma->vm_flags;
	long nrpages = (end - start) >> PAGE_SHIFT;
	unsigned long charged = 0;
	pgoff_t pgoff;
	int error;
	int dirty_accountable = 0;

	if (newflags == oldflags) {
		*pprev = vma;
		return 0;
	}

	/*
	 * Do PROT_NONE PFN permission checks here when we can still
	 * bail out without undoing a lot of state. This is a rather
	 * uncommon case, so doesn't need to be very optimized.
	 */
	if (arch_has_pfn_modify_check() &&
	    (vma->vm_flags & (VM_PFNMAP|VM_MIXEDMAP)) &&
	    (newflags & (VM_READ|VM_WRITE|VM_EXEC)) == 0) {
		error = prot_none_walk(vma, start, end, newflags);
		if (error)
			return error;
	}

	/*
	 * If we make a private mapping writable we increase our commit;
	 * but (without finer accounting) cannot reduce our commit if we
	 * make it unwritable again. hugetlb mapping were accounted for
	 * even if read-only so there is no need to account for them here
	 */
	if (newflags & VM_WRITE) {
		if (!(oldflags & (VM_ACCOUNT|VM_WRITE|VM_HUGETLB|
						VM_SHARED|VM_NORESERVE))) {
			charged = nrpages;
			if (security_vm_enough_memory_mm(mm, charged))
				return -ENOMEM;
			newflags |= VM_ACCOUNT;
		}
	}

	/*
	 * First try to merge with previous and/or next vma.
	 */
	pgoff = vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT);
	*pprev = vma_merge(mm, *pprev, start, end, newflags,
			   vma->anon_vma, vma->vm_file, pgoff, vma_policy(vma),
			   vma->vm_userfaultfd_ctx);
	if (*pprev) {
		vma = *pprev;
		goto success;
	}

	*pprev = vma;

	if (start != vma->vm_start) {
		error = split_vma(mm, vma, start, 1);
		if (error)
			goto fail;
	}

	if (end != vma->vm_end) {
		error = split_vma(mm, vma, end, 0);
		if (error)
			goto fail;
	}

success:
	/*
	 * vm_flags and vm_page_prot are protected by the mmap_sem
	 * held in write mode.
	 */
	vma->vm_flags = newflags;
	dirty_accountable = vma_wants_writenotify(vma);
	vma_set_page_prot(vma);

	change_protection(vma, start, end, vma->vm_page_prot,
			  dirty_accountable, 0);

	/*
	 * Private VM_LOCKED VMA becoming writable: trigger COW to avoid major
	 * fault on access.
	 */
	if ((oldflags & (VM_WRITE | VM_SHARED | VM_LOCKED)) == VM_LOCKED &&
			(newflags & VM_WRITE)) {
		populate_vma_page_range(vma, start, end, NULL);
	}

	vm_stat_account(mm, oldflags, vma->vm_file, -nrpages);
	vm_stat_account(mm, newflags, vma->vm_file, nrpages);
	perf_event_mmap(vma);
	return 0;

fail:
	vm_unacct_memory(charged);
	return error;
}

SYSCALL_DEFINE3(mprotect, unsigned long, start, size_t, len,
		unsigned long, prot)
{
	xen_hvm_subpage_t spp;

	/* set subpage */
	if (prot == 16) {
		spp.domid = DOMID_SELF;
		spp.subpage = (start & 0xfff) >> 7;
		unsigned long pfn;

		int ret = get_pfn(start, &pfn);
		if (ret != 0) {
			printk("get pfn error %d\n", ret);
			return ret;
		}
		spp.gfn = (uint64_t) pfn;
		register_pfn(spp.gfn);
		printk("Hypercall: set subpage %lx %llx %u\n",start, spp.gfn, spp.subpage); 

		HYPERVISOR_hvm_op(HVMOP_set_subpage, &spp);
		return 0;	
	}


	/* unset subpage */
	if (prot == 32) {
		spp.domid = DOMID_SELF;
		spp.subpage = (start & 0xfff) >> 7;
		spp.subpage = spp.subpage | 0x80000000;
		unsigned long pfn;

		int ret = get_pfn(start, &pfn);
		if (ret != 0) {
			printk("get pfn error %d\n", ret);
			return ret;

		}
		spp.gfn = (uint64_t) pfn;
		register_pfn(spp.gfn);
		printk("Hypercall: unset subpage %lx %llx %u\n", start, spp.gfn, spp.subpage);

		HYPERVISOR_hvm_op(HVMOP_set_subpage, &spp);
	}

	/* release page */
	if (prot == 64){
		unregister_pfn();
		return;
	}
	unsigned long vm_flags, nstart, end, tmp, reqprot;
	struct vm_area_struct *vma, *prev;
	int error = -EINVAL;
	const int grows = prot & (PROT_GROWSDOWN|PROT_GROWSUP);
	prot &= ~(PROT_GROWSDOWN|PROT_GROWSUP);
	if (grows == (PROT_GROWSDOWN|PROT_GROWSUP)) /* can't be both */
		return -EINVAL;

	if (start & ~PAGE_MASK)
		return -EINVAL;
	if (!len)
		return 0;
	len = PAGE_ALIGN(len);
	end = start + len;
	if (end <= start)
		return -ENOMEM;
	if (!arch_validate_prot(prot))
		return -EINVAL;

	reqprot = prot;
	/*
	 * Does the application expect PROT_READ to imply PROT_EXEC:
	 */
	if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC))
		prot |= PROT_EXEC;

	vm_flags = calc_vm_prot_bits(prot);

	down_write(&current->mm->mmap_sem);

	vma = find_vma(current->mm, start);
	error = -ENOMEM;
	if (!vma)
		goto out;
	prev = vma->vm_prev;
	if (unlikely(grows & PROT_GROWSDOWN)) {
		if (vma->vm_start >= end)
			goto out;
		start = vma->vm_start;
		error = -EINVAL;
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto out;
	} else {
		if (vma->vm_start > start)
			goto out;
		if (unlikely(grows & PROT_GROWSUP)) {
			end = vma->vm_end;
			error = -EINVAL;
			if (!(vma->vm_flags & VM_GROWSUP))
				goto out;
		}
	}
	if (start > vma->vm_start)
		prev = vma;

	for (nstart = start ; ; ) {
		unsigned long newflags;

		/* Here we know that vma->vm_start <= nstart < vma->vm_end. */

		newflags = vm_flags;
		newflags |= (vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC));

		/* newflags >> 4 shift VM_MAY% in place of VM_% */
		if ((newflags & ~(newflags >> 4)) & (VM_READ | VM_WRITE | VM_EXEC)) {
			error = -EACCES;
			goto out;
		}

		error = security_file_mprotect(vma, reqprot, prot);
		if (error)
			goto out;

		tmp = vma->vm_end;
		if (tmp > end)
			tmp = end;
		error = mprotect_fixup(vma, &prev, nstart, tmp, newflags);
		if (error)
			goto out;
		nstart = tmp;

		if (nstart < prev->vm_end)
			nstart = prev->vm_end;
		if (nstart >= end)
			goto out;

		vma = prev->vm_next;
		if (!vma || vma->vm_start != nstart) {
			error = -ENOMEM;
			goto out;
		}
	}
out:
	up_write(&current->mm->mmap_sem);
	return error;
}