#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <debug.h>
#include "threads/synch.h"

extern struct lock filesys_lock;

void syscall_init(void);
void sys_exit(int status) NO_RETURN;

#endif /* userprog/syscall.h */
