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
#include <asm/uaccess.h>

#include "vmprot.h"

#ifdef DEBUG
#define debug(fmt,args...) \
	printk(KERN_DEBUG "%s(%d line):"fmt,__FILE__,__LINE__,##args)
#else
#define debug(fmt,args...)
#endif

	
/* external variables */
extern long sys_call_table[];

/* static variables */
static long (*oldfunc)(void);

/* static variables */


static unsigned long get_cr2()
{
	unsigned long ret = 0;
	
	asm volatile (
		"movl %%cr2,%0\n\t"
		:"=r"(ret)
		:
		:"memory"
	);

	return ret;
}


void hook_fn( struct prot_info *info)
{
	unsigned long cr2 = get_cr2();

	printk("EIP: %lx\n",cr2);
	printk("PID: %u\n",info->pid);
	printk("Protect Range: %lx - %lx\n",info->start,info->end);
	
	return;
}

static long init_hook(struct prot_info *info)
{
	struct hook_node *hnp = NULL;
	int hash;

	printk("1\n");
	
	if (!info)
		return -EFAULT;

	printk("2\n");
	
	hnp = (struct hook_node *)kmalloc(sizeof(*hnp),GFP_KERNEL);

	if (!hnp)
		return -ENOMEM;

	copy_from_user(&hnp->info,info,sizeof(struct prot_info));
	hnp->info.pid = current->pid;
	hnp->fn = hook_fn;
	
	debug("pid:%d start:%lx end:%lx\n",hnp->info.pid,hnp->info.start,hnp->info.end);
	
	hash = hnp->info.pid % hn_hash_size;

	debug("%p\n",hn_hash_list);
	list_add(&hnp->list,&hn_hash_list[hash]);
	
	return 0;
}

static long clean_hook(struct prot_info *info)
{

	return 0;
}

asmlinkage long 
	sys_hook(struct prot_info *info,unsigned long opt)
{
	
	if (opt == REGISTER_HOOK)
		return init_hook(info);
	else
		return clean_hook(info);

}


int init(void)
{
	int i;
	
	hn_hash_size = 10;
	
	hn_hash_list = (struct list_head *)kmalloc(sizeof(struct list_head)*hn_hash_size,GFP_KERNEL);

	
	if (!hn_hash_list)
		return -ENOMEM;
	
	for (i=0;i<hn_hash_size;i++)
		INIT_LIST_HEAD(&hn_hash_list[i]);

	spin_lock_init(&hn_list_lock);
	
	oldfunc = (long (*)(void))(sys_call_table[255]);
	
	sys_call_table[255] = (unsigned long)sys_hook; 

	return 0;
}

void cleanup(void)
{
	if (hn_hash_list)
		kfree(hn_hash_list);
	
	sys_call_table[255] = (unsigned long)oldfunc;

	return;
}


MODULE_LICENSE("GPL");

module_init(init);
module_exit(cleanup);
