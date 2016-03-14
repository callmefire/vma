#ifndef __VMPROT_H
#define __VMPROT_H

#ifndef __KERNEL__
#include <sys/types.h>
#else
#include <linux/types.h>
#endif

#define REGISTER_HOOK	1
#define UNREGISTER_HOOK 0

struct prot_info
{
	pid_t pid;
	unsigned long start;
	unsigned long end;
};


#ifdef __KERNEL__

#include <linux/list.h>

extern struct list_head *hn_hash_list;
extern unsigned long hn_hash_size;
extern spinlock_t hn_list_lock;

struct hook_node
{
	struct list_head list;
	void (*fn)(struct prot_info *);
	struct prot_info info;
};


static void run_list(unsigned long addr)
{
	int hash = current->pid % hn_hash_size;
	struct list_head *p = hn_hash_list[hash].next;
	unsigned long flags = 0;
	struct hook_node *hp;

	spin_lock_irqsave(&hn_list_lock,flags);
	
	while ( p != &hn_hash_list[hash] ) {

		hp = list_entry(p,struct hook_node,list);
		
		if (hp->info.pid == current->pid && addr <= hp->info.end && addr >= hp->info.start) {
			hp->fn(&hp->info);
			printk("Bad memory address: %lx\n",addr);
			break;
		}

		p = p->next;
	}
	
	spin_unlock_irqrestore(&hn_list_lock,flags);

	return;
}
#endif // #ifdef __KERNEL__


#endif // #ifndef __VMPROT_H
