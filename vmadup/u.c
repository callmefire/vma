#include <unistd.h>
#include <pthread.h>
#include <asm/page.h>

#ifdef DEBUG
#define debug(fmt,args...) printf("%s(%d line):"fmt,__FILE__,__LINE__,##args);
#else
#define debug(fmt,args...)
#endif

#define ROUND_UP(addr) (((addr)+PAGE_SIZE-1) & PAGE_MASK)
#define ROUND_DOWN(addr) ((addr) & PAGE_MASK)

static void *p = NULL;

int mycall(unsigned long addr,unsigned long size,unsigned long opt)
{
	asm volatile (
		"pushl %%eax\n\t"	
		"pushl %%ebx\n\t"	
		"pushl %%ecx\n\t"	
		"pushl %%edx\n\t"	
		"movl $0xff,%%eax\n\t"	
		"int $0x80\n\t"
		"popl %%edx\n\t"
		"popl %%ecx\n\t"
		"popl %%ebx\n\t"
		"popl %%eax"
		:
		:"b"(addr),"c"(size),"d"(opt)
    	);
	
}

void f(void *m)
{
	int i=0;
	//char *p = NULL;
	
	mycall((unsigned long)m,1024*1024,0);
	
	//p = (char *)malloc(1024*1024);
	//debug("p:%p\n",p);

	/* !!! error */
	*(char *)m = 'a';
	
	while (i++ < 30) {
		sleep(1);
		printf("%d\n",i);
	}
	mycall(0,0,1);

	//free(p);
	
	debug("finished!\n");
	
	pthread_exit(NULL);
}

int main(void)
{
	pthread_t pt[1];
	int i;
	void *q;
	void *t;
	
	p = (void *)malloc(1024*1024);
	q = (void *)ROUND_UP((int)p);
	memset(q,0,1024*1024-(q-p));
	strcpy(q,"1234567\0");
	
	debug("malloc mem base addr:%p\n",q);
	
	for (i=0;i<sizeof(pt)/sizeof(*pt);i++) {
		pthread_create(&pt[i],NULL,(void* (*)(void *))f,q);
	}

	t = malloc(1024*1024);
	
	for (i=0;i<sizeof(pt)/sizeof(*pt);i++) {
		pthread_join(pt[i],NULL);
	}
	
	free(t);
	free(p);
}
