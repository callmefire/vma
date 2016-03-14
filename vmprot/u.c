#include <unistd.h>
#include <pthread.h>
#include <asm/page.h>
#include <sys/mman.h>

#include "vmprot.h"

#ifdef DEBUG
#define debug(fmt,args...) printf("%s(%d line):"fmt,__FILE__,__LINE__,##args);
#else
#define debug(fmt,args...)
#endif

#define ROUND_UP(addr) (((addr)+PAGE_SIZE-1) & PAGE_MASK)
#define ROUND_DOWN(addr) ((addr) & PAGE_MASK)

unsigned long mycall(struct prot_info *info,int opt)
{
	unsigned long ret;
	
	asm volatile (
		"pushl %%eax\n\t"
		"pushl %%ebx\n\t"
		"movl $0xff,%%eax\n\t"	
		"int $0x80\n\t"
		"movl %%eax,%0\n\t"
		"popl %%eax"
		:"=a"(ret)
		:"b"(info),"c"(opt)
    	);
	
}

void f(char *p)
{
	int i=0;
	struct prot_info info;

	memset(&info,0,sizeof(info));
	info.start =(unsigned long)p;
	info.end = (unsigned long)(p+1024*1024);
	mycall(&info,REGISTER_HOOK);
	
	mprotect(p,1024*1024,PROT_READ);
	
	p[0] = 0;
	
	while (i++ < 30) {
		sleep(1);
		printf("%d\n",i);
	}
	
	mycall(&info,UNREGISTER_HOOK);
	
	debug("finished!\n");
	
	pthread_exit(NULL);
}

int main(void)
{
	pthread_t pt[2];
	int i;
	char *p = NULL;
	char *q = NULL;
	
	p = (char *)malloc(1024*1024);
	q = (char *)ROUND_UP((int)p);
	
	memset(q,0,1024*1024);
	
	debug("malloc mem base addr:%p\n",q);
	
	for (i=0;i<sizeof(pt)/sizeof(*pt);i++) {
		pthread_create(&pt[0],NULL,(void* (*)(void *))f,q);
	}

	for (i=0;i<sizeof(pt)/sizeof(*pt);i++) {
		pthread_join(pt[i],NULL);
	}
	
	free(p);
}
