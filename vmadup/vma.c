#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/config.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/rwsem.h>
#include <linux/highmem.h>

#include <asm/current.h>
#include <asm/errno.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/desc.h>

#ifdef DEBUG
#define debug(fmt,args...) \
	printk(KERN_DEBUG "%s(%d line):"fmt,__FILE__,__LINE__,##args)
#else
#define debug(fmt,args...)
#endif

// Map split mode
enum SPLIT_MODE {
	LEFT,
	MID,
	RIGHT,
	ALL
};

typedef enum SPLIT_MODE split_t;
	
/* external variables */
extern long sys_call_table[];
extern struct desc_struct default_ldt[];
//extern kmem_cache_t *mm_cachep; cann't export

/* external func */
extern pgd_t *pgd_alloc(struct mm_struct *mm);
extern void pgd_free(pgd_t *pgd);
extern int init_new_context(struct task_struct *tsk,struct mm_struct *mm);
extern struct vm_area_struct *find_vma(struct mm_struct *mm,unsigned long addr);
extern void insert_vm_struct(struct mm_struct *mm,struct vm_area_struct *vma);

/* static variables */
static long (*oldfunc)(void);
static struct mm_struct *old_mm;

/* static func */

/*
 * function:create_mm
 * 1.alloc space for new_mm of current 
 * 2.copy old_mm to new_mm
 * 3.init context of new_mm
 */
static struct mm_struct 
	*create_mm(struct task_struct *tsk,struct mm_struct *old_mm)
{
	struct mm_struct *ret = NULL;
	
	ret = (struct mm_struct *)kmalloc(sizeof(*ret),GFP_KERNEL);

	if (!ret) 
		return ret;
	
	memcpy(ret,old_mm,sizeof(*ret));

	//I'm not sure if mm_init(new_mm) supposed to be called here

	//init ldt context
	if (init_new_context(current,ret))
		goto failed; 

	memset(&ret->mm_rb,0,sizeof(rb_root_t));
	ret->mmap_cache = NULL;
	ret->free_area_cache = TASK_UNMAPPED_BASE;
	atomic_set(&ret->mm_users,1);
	atomic_set(&ret->mm_count,1);
	init_rwsem(&ret->mmap_sem);
	ret->page_table_lock = SPIN_LOCK_UNLOCKED;

	/* !!!
	 * Because new_mm->map_count = old_mm->map_count,
	 * and insert_vm_struct will inc new_mm->map_count invisiblly,
	 * so Must clear before call insert_vm_struct
	 */
	ret->map_count = 0;

	return ret;
	
failed:
	kfree(ret);	

	return NULL;
}

void dump_vma(struct vm_area_struct *vma)
{
	debug("vma:%p\n        vm_start:%lx\n        vm_end:%lx\n        vm_flag:%lx\n        vm_mm:%p\n        vm_page_prot:%lx\n        vm_next_share:%p\n        vm_pprev_share:%p\n        vm_ops:%p\n        vm_pgoff:%lu\n        vm_file:%p\n        vm_raend:%lx\n        vm_private_data:%p\n"
			,vma,vma->vm_start,vma->vm_end,vma->vm_flags
			,vma->vm_mm,vma->vm_page_prot.pgprot,vma->vm_next_share
			,vma->vm_pprev_share?(*(vma->vm_pprev_share)):NULL
			,vma->vm_ops,vma->vm_pgoff,vma->vm_file,vma->vm_raend
			,vma->vm_private_data);
}

/* Debug RB Tree */
static int browse_rb(rb_node_t * rb_node) {
	int i = 0;

	if (rb_node) {
		i++;
		i += browse_rb(rb_node->rb_left);
		i += browse_rb(rb_node->rb_right);
	}
	return i;
}

/* Debug RB Tree */
static void validate_mm(struct mm_struct * mm) {
	int bug = 0;
	int i = 0;
	struct vm_area_struct * tmp = mm->mmap;

	while (tmp) {
		tmp = tmp->vm_next;
		i++;
	}
	
	debug("vma_chain = %d\n",i);
	
	if (i != mm->map_count)
		debug("map_count %d vm_next %d\n", mm->map_count, i), bug = 1;
	
	i = browse_rb(mm->mm_rb.rb_node);

	debug("browse_rb = %d\n",i);
	
	if (i != mm->map_count)
		debug("map_count %d rb %d\n", mm->map_count, i), bug = 1;
	
	if (bug) {
		debug("map_count BUG\n");
		//BUG();
	}
}

/* ############################ Delete Start ################################ */

void lock_vma_mappings(struct vm_area_struct *vma)
{
	struct address_space *mapping;

	mapping = NULL;
	if (vma->vm_file)
		mapping = vma->vm_file->f_dentry->d_inode->i_mapping;
	if (mapping)
		spin_lock(&mapping->i_shared_lock);
}

void unlock_vma_mappings(struct vm_area_struct *vma)
{
	struct address_space *mapping;

	mapping = NULL;

	if (vma->vm_file)
		mapping = vma->vm_file->f_dentry->d_inode->i_mapping;
	
	if (mapping)
		spin_unlock(&mapping->i_shared_lock);
}

static inline void __remove_shared_vm_struct(struct vm_area_struct *vma)
{
	struct file * file = vma->vm_file;

	if (file) {

		struct inode *inode = file->f_dentry->d_inode;
		
		if (vma->vm_flags & VM_DENYWRITE)
			atomic_inc(&inode->i_writecount);
	
		if(vma->vm_next_share)
			vma->vm_next_share->vm_pprev_share = vma->vm_pprev_share;
		*vma->vm_pprev_share = vma->vm_next_share;
	}
}

static inline void remove_shared_vm_struct(struct vm_area_struct *vma)
{
	lock_vma_mappings(vma);
	__remove_shared_vm_struct(vma);
	unlock_vma_mappings(vma);
}

static void release_vm(struct mm_struct *mm)
{
	struct vm_area_struct *vma = mm->mmap;

	while (vma) {
		remove_shared_vm_struct(vma);
		vma = vma->vm_next;
	}
	
	/* 
	 * new_mm->mmap is a big continue block which includes many vam struct.
	 * Needn't free one by one.
	 */
	kfree(mm->mmap);

	mm->mmap = NULL;
	memset(&mm->mm_rb,0,sizeof(rb_root_t));
	mm->mmap_cache = NULL;
	mm->free_area_cache = 0;
	mm->map_count = 0;

}

/*
 * function:create_vma
 * 1.alloc space for new vmas of new_mm
 * 2.copy old vmas to new vmas
 * 3.split the vma that addr is involved
 * 4.sort vma chain of new_mm
 * 5.link 3 vmas(prev splited,splited,next splited) into share chain
 * 6.reconstruct rb tree for new_mm
 * 
 * Don't care share chain?
 */
static int create_vma
	(struct mm_struct *new_mm, struct mm_struct *old_mm,
	 unsigned long addr, unsigned long size)
{
	struct vm_area_struct *vma = NULL;
	struct vm_area_struct *vmap = NULL;
	struct vm_area_struct *vma_head = NULL;
	int n = 0;	//new map count after split
	int i;
	split_t split;
	int ret = 0;

	n = old_mm->map_count;

	debug("old map count is %d\n",n);
	
	//search the vma addr locate
	vma = find_vma(old_mm,addr);
	
	if (!vma) {
		debug("Bad addr\n");
		return -EFAULT;
	}
	
	dump_vma(vma);

	if ( addr >= vma->vm_start && (addr+size) <= vma->vm_end ) {

		if (addr > vma->vm_start && (addr+size) < vma->vm_end) {
			split = MID;
			n += 2;
			debug("MID n:%d\n",n);
		} 
		else if (addr == vma->vm_start && (addr+size) == vma->vm_end) {
			split = ALL;
			n += 0;
			debug("ALL n:%d\n",n);
		}
		else if (addr == vma->vm_start && (addr+size) < vma->vm_end) {
			split = LEFT;
			n += 1;
			debug("LEFT n:%d\n",n);
		}
		else {
			split = RIGHT;
			n += 1;
			debug("RIGHT n:%d\n",n);
		}
		
	} else {
		debug("addr space beyonds one vma\n");
		return -EFAULT;
	}

	debug("new map count is %d\n",n);
	
	//check mmap_count
	if (n > DEFAULT_MAX_MAP_COUNT) {
		debug("n > max_map_count\n");
		return -EFAULT;
	}
	
	vma_head = (struct vm_area_struct *)
		kmalloc(n*sizeof(struct vm_area_struct),GFP_KERNEL);

	if (!vma_head) {
		ret = -ENOMEM;
		goto failed;
	}

	memset(vma_head,0,n*sizeof(struct vm_area_struct));

	vmap = new_mm->mmap;
	
	i = 0;	
	while (vmap != NULL) {

		if (i >= n) {
			debug("i(%d) is great new map_count(%d).mem leak\n",i,n);
			ret = -EFAULT;  //?
			goto failed;
		}
		
		//dump_vma(vmap);
		
		if (vmap == vma) {
		
			switch(split) {
				case MID:
					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					
					vma_head[i].vm_end = addr;
					i++;
					
					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					
					vma_head[i].vm_start = addr;
					vma_head[i].vm_end = addr + size;
					vma_head[i].vm_flags = VM_READ;
					vma_head[i].vm_page_prot = PAGE_READONLY; 
					i++;
					
					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					vma_head[i].vm_start = addr + size;
					
					break;
					
				case LEFT:
					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					vma_head[i].vm_end = addr + size;
					
					vma_head[i].vm_flags = VM_READ;
					vma_head[i].vm_page_prot = PAGE_READONLY; 
					
					i++;
					
					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					vma_head[i].vm_start = addr + size;

					break;
					
				case RIGHT:
					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					vma_head[i].vm_end = addr;

					i++;

					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					vma_head[i].vm_start= addr;
					
					vma_head[i].vm_flags = VM_READ;
					vma_head[i].vm_page_prot = PAGE_READONLY; 
					break;
					
				case ALL:
					memcpy(&vma_head[i],vmap,
						sizeof(struct vm_area_struct));
					
					vma_head[i].vm_flags = VM_READ;
					vma_head[i].vm_page_prot = PAGE_READONLY; 
					
					break;

				default:
					debug("Bad split val\n");
					goto failed;

			} //end switch(split)
		
		} else {
		
			memcpy(&vma_head[i],vmap,sizeof(struct vm_area_struct));
	
		} //end if (vma == vmap)
	
		vmap = vmap->vm_next;
		i++;

	} //end while (vmap)

	
	for (i = 0; i < n;i++) {
		
		vma_head[i].vm_mm = new_mm;
		
		memset(&(vma_head[i].vm_rb),0,sizeof(rb_node_t));
		
		vma_head[i].vm_next_share = NULL;
		vma_head[i].vm_pprev_share = NULL;

		/*		
		 * Remain the original value, Don't change it.
		 * Share files maybe execute file(bin),share lib,etc.
		 * 
		 * vma_head[i].vm_ops = NULL;
		 * vma_head[i].vm_pgoff = 0;
		 * vma_head[i].vm_file = NULL;
		 * vma_head[i].vm_raend = 0;
		 * vma_head[i].vm_private_data = NULL;
		 */

		insert_vm_struct(new_mm,&vma_head[i]);
		
	}
	
	/* 
	 * insert_vm_struct() has done these work, Don't do it again !!! 
	 * new_mm->map_count = n;    
	 * new_mm->mmap = vma_head;
	 */

	validate_mm(new_mm);
	
	return ret;

failed:
	debug("create_vma failed\n");
	kfree(vma_head);

	return ret;
	
}

static unsigned long get_cr3()
{
	unsigned long ret = 0;
	
	asm volatile (
		"movl %%cr3,%0\n\t"
		:"=r"(ret)
		:
		:"memory"
	);

	return ret;
}


static void dump_pgd(pgd_t *pgd)
{
	int i;

	debug("cr3:%lx   pgd:%p\n",get_cr3(),pgd);
	
	//for (i=0;i<PTRS_PER_PGD;i++) {
	for (i=USER_PTRS_PER_PGD;i<PTRS_PER_PGD;i++) {
		debug("pgd[%d]:%lx\n",i,pgd_val(pgd[i]));
	}	
	
	return;
}

static void cmp_pgd(pgd_t *opgd,pgd_t *npgd)
{
	int i;

	for (i=768; i<PTRS_PER_PGD; i++) {
		if (memcmp(&opgd[i],&npgd[i],sizeof(pgd_t)) != 0) {
			debug("%d:pgd copy wrong! %lx %lx\n",i,pgd_val(opgd[i]),pgd_val(npgd[i]));
		} else {
			//debug("%d:pgd copy ok!\n",i);
		}
	}
	
}

static int copy_pgd(struct mm_struct *new_mm,struct mm_struct *old_mm)
{
	pgd_t *npgd = new_mm->pgd;
	pgd_t *opgd = old_mm->pgd;
	unsigned long nvaddr = 0;
	unsigned long ovaddr = 0;
	int ret = 0;
	int i = 0;

	if (!npgd || !opgd) 
		return -EFAULT;

	for (i=0; i<USER_PTRS_PER_PGD; i++) {

		struct page *page = NULL;
		
		if ( pgd_val(opgd[i]) != 0 && pgd_val(npgd[i]) == 0 ) {

			nvaddr = __get_free_page(GFP_KERNEL);
			if (nvaddr == 0) {
				ret = -ENOMEM;
				goto failed;
			}
			
			page = virt_to_page(nvaddr);
			page->mapping = (void *)new_mm;
			page->index = i * PMD_SIZE;
			
			/* When CONFIG_HIGHMEM && CONFIG_HIGHPTE are selected,
			 * the translation between virt and phy are complex and
			 * can't transfer them by old way if the pyh addr is 
			 * located in highmem, because you can't get the virt 
			 * through phy + PAGE_OFFSET. Be careful!
			 *
			 * ovaddr = (unsigned long)
			 * 	__va(pgd_val(opgd[i]) & PAGE_MASK);
			 */
			
			ovaddr = (unsigned long)pte_offset_map((pmd_t *)(opgd+i),0);
			
			pgd_val(npgd[i]) = __pa(nvaddr) + (pgd_val(opgd[i]) & ~PAGE_MASK);
			memcpy((void *)nvaddr,(void *)ovaddr,PAGE_SIZE);
			

			//debug("ovaddr:%lx  nvaddr:%lx\n",ovaddr,nvaddr);
			//debug("opgd[%d]:%lx  npgd[%d]:%lx\n",i,pgd_val(opgd[i]),i,pgd_val(npgd[i]));

		} else if ( pgd_val(npgd[i]) != 0 ) {
			
			debug("%d:new pgd has been assigned pte table\n",i);
			ret = -EFAULT;
			goto failed;
			
		}
		
	}

	return ret;
	
failed:
	for (i--; i>=0 ;i--) {
		if ( pgd_val(npgd[i]) != 0 ) {

			unsigned long vaddr = (unsigned long)
				pte_offset_map((pmd_t *)(npgd+i),0);
			struct page *page = virt_to_page(vaddr);
			
			page->mapping = NULL;
			page->index = 0;
			
			__free_page(page);
			
			/* Be care highmem page
			 * free_page((unsigned long)__va(pgd_val(npgd[i]) 
			 * & PAGE_MASK));
			 */
			
		}
	}

	return ret;

}


static void release_pte(pgd_t *pgd)
{
	int i;

	for (i=0; i<USER_PTRS_PER_PGD; i++) {
		
		if ( pgd_val(pgd[i]) != 0 ) {
			unsigned long vaddr = (unsigned long)
				pte_offset_map((pmd_t *)(pgd+i),0);
			struct page *page = virt_to_page(vaddr);
			
			
			page->mapping = NULL;
			page->index = 0;
			
			__free_page(page);
		
		}
	}

	return;
}

static void release_pgd(struct mm_struct *mm)
{
	
	release_pte(mm->pgd);

	free_page((unsigned long)mm->pgd);
}


/* set_pte_rdonly() finish the works as follow:
 * 1.change the pte'access right of specified virt address space to Read-Only
 */
static int set_pte_rdonly(const pgd_t *pgd,unsigned long start,unsigned long size)
{
	pte_t *pte = NULL;
	int start_pgd,end_pgd,i,j;
	unsigned long vaddr = 0;
	
	if (start+size > PAGE_OFFSET)
		return -EFAULT;

	start_pgd = __pgd_offset(start);
	end_pgd = __pgd_offset((start+size));  //??

	for (i=start_pgd; i<=end_pgd; i++) {
		
		pte = pte_offset_map((pmd_t *)(pgd+i),0);

		for (j=0; j<PTRS_PER_PTE; j++) {
			
			vaddr = i*PGDIR_SIZE+j*PAGE_SIZE;

			if (vaddr >= start && vaddr < start+size) {
				
				//debug("j:%d old_pte:%lx\n",j,pte_val(pte[j]));
				pte_val(pte[j]) &= ~_PAGE_RW;
				//debug("j:%d new_pte:%lx\n",j,pte_val(pte[j]));

			} else {
				//debug("range:%lx-%lx  vaddr:%lx\n",start,start+size,vaddr);
			}
		}
	}

	return 0;
}


/* create_pgd() finish the works as follow:
 * 1.copy 3G-4G page dir from swapper_pg_dir
 * 2.copy 0-3G page dir from current
 * 3.modify the specify address space page entry's R/W
 */
static int create_pgd( struct mm_struct *new_mm,
	               struct mm_struct *old_mm,
		       unsigned long addr)
{
	int ret = 0;
	struct vm_area_struct *vma = NULL;

	if (new_mm->pgd != old_mm->pgd)
		return -EFAULT;
	
	//create page dir for thread and copy 3G-4G pgd entry
	new_mm->pgd = pgd_alloc(new_mm);

	//dump_pgd(new_mm->pgd);

	if (!new_mm->pgd) {
		ret = -ENOMEM;
		goto free_pgd;
	}

	if ( copy_pgd(new_mm,old_mm) != 0) {
		ret = -ENOMEM;
		goto free_pgd;
	}

	vma = find_vma(new_mm,addr);
	//dump_vma(vma);

	if ( set_pte_rdonly(new_mm->pgd,vma->vm_start,vma->vm_end-vma->vm_start) != 0 ) {
		ret = -EFAULT;
		goto free_pte; 
	}
		
	return 0;

free_pte:
	release_pte(new_mm->pgd);
	
free_pgd:
	pgd_free(new_mm->pgd);
	
	return ret;
	
}


static long init_dup(unsigned long addr,unsigned long size)
{
	struct mm_struct *new_mm = NULL;
	long retval = 0;

	debug("user mem addr:%lx size:%lu\n",addr,size);
	
	//store the parent's mm_struct to restore when threads quit
	old_mm = current->mm;
	
	if (!old_mm) {
		retval = -EFAULT;
		goto failed;
	}

	down_write(&old_mm->mmap_sem);
	
	if ( (addr & ~PAGE_MASK) || (addr + size) >= PAGE_OFFSET) {
		debug("Error:addr align is %lx addr+size is %lu\n",
			addr & ~PAGE_MASK,addr+size);
		retval = -EFAULT;
		goto failed;
	}
	
	//allocate new mm_struct for thread
	new_mm = create_mm(current,old_mm);
	
	debug("old_mm:%p new_mm:%p\n",old_mm,new_mm);	

	if (!new_mm) {
		debug("new_mm alloc failed\n");
		retval = -ENOMEM;
		goto failed;
	}

	if ( create_vma(new_mm,old_mm,addr,size) != 0 ) {
		debug("create_vma failed\n");
		retval = -ENOMEM;
		goto free_mm;
	}
	
	spin_lock(&old_mm->page_table_lock);
	if (create_pgd(new_mm,old_mm,addr) != 0) {
		spin_unlock(&old_mm->page_table_lock);
		debug("create_pgd failed\n");
		retval = -ENOMEM;
		goto free_vm;
	}
	spin_unlock(&old_mm->page_table_lock);


	//switch_mm(old_mm,new_mm,current,smp_processor_id());
	
	current->mm = new_mm;
	current->active_mm = new_mm;
	load_cr3(new_mm->pgd);
	//old_mm = ??? how to save the old_mm
	
	up_write(&old_mm->mmap_sem);
	return 0;

free_vm:
	release_vm(new_mm);	

free_mm:
	kfree(new_mm);

failed:
	up_write(&old_mm->mmap_sem);
	debug("failed\n");
	return retval;
}

static long clean_dup(unsigned long addr,unsigned long size)
{
	struct mm_struct *new_mm = current->active_mm;
	long ret = 0;

	debug("new_mm:%p old_mm:%p\n",new_mm,old_mm);
	down_write(&new_mm->mmap_sem);
	
	current->mm = old_mm;
	current->active_mm = old_mm;
	
	up_write(&new_mm->mmap_sem);
	
	load_cr3(old_mm->pgd);

	release_pgd(new_mm);
	release_vm(new_mm);
	kfree(new_mm);
	
	
	debug("cleanup\n");

	return ret;
}

asmlinkage long 
	sys_context_dup(unsigned long addr,unsigned size,unsigned long opt)
{
	
	if (!opt)
		return init_dup(addr,size);
	else
		return clean_dup(addr,size);

}


int init(void)
{
	
	oldfunc = (long (*)(void))(sys_call_table[255]);
	
	sys_call_table[255] = (unsigned long)sys_context_dup; 

	return 0;
}

void cleanup(void)
{
	sys_call_table[255] = (unsigned long)oldfunc;

	return;
}

MODULE_LICENSE("GPL");

module_init(init);
module_exit(cleanup);

//asm/pgtable.c __PAGE_* --> vm_flags
//linux/mm.h VM_* --> vm_pgrot
