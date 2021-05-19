#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <clock.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <current.h>
#include <synch.h>


int sys_write(int filehandle, const void* buf, size_t size){
	char character;
	size_t i;
	char* buff = (char*)buf;
	if(filehandle!=STDOUT_FILENO && filehandle!=STDERR_FILENO){
		kprintf("sys write not supported for files\n");
		return -1;
	}
	for(i=0;i<size;i++){
		character = buff[i];
    	putch(character);
	}
    return (int)size;
}

int sys_read(int filehandle, void* buf, size_t size){
	int i;
	char *p = (char *)buf;

	if(filehandle!=STDIN_FILENO)
		kprintf("sys_read only to stdin\n");
		return -1;

	for(i=0;i<(int)size;i++){
		p[i] = getch();
		if(p[i]<0)
			return i;
	}
	return (int)size;
}
void sys__exit(int status){
	curproc->status = status;
	//proc_remthread(curthread); -> questo è sbagliato
	V(curproc->sem);
	thread_exit();
}