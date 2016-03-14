all:vma.o u
	@dmesg -c
	insmod vma.o -m | grep .text

vma.o:vma.c
	gcc -D__KERNEL__ -DMODULE -DDEBUG -Wall -O -I/usr/src/linux-i386/include -c -g -o $@ $<
	
u:u.c
	gcc -DDEBUG -g -o $@ $< -lpthread

clean:
	@rm -f vma.o u *~
	@rmmod vma
