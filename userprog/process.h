#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <list.h>
#include "threads/synch.h"
#include "threads/thread.h"

/* Most words a command line may have, including the program name. */
#define MAX_ARGS 128

/* A parent's record of one child.
 *
 * Outlives the child's `struct thread', since a parent may wait() long
 * after the child died.  Reference counted, one reference per side, so
 * whichever exits last frees it. */
struct child_status {
    tid_t tid;
    int   exit_status;          /* -1 if the kernel killed the child. */
    bool  load_success;
    bool  waited;               /* Parent only. */

    int   ref_cnt;              /* 2 while both sides are alive. */
    struct lock ref_lock;       /* Protects ref_cnt. */

    struct semaphore load_sema; /* Up'd when load_success is valid. */
    struct semaphore wait_sema; /* Up'd when exit_status is valid. */

    char *cmdline;              /* Handed to the child at startup. */
    struct list_elem elem;      /* In the parent's `children' list. */
};

tid_t process_execute(const char *file_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(void);

#endif /* userprog/process.h */
