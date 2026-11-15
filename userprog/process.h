#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <list.h>
#include "threads/synch.h"
#include "threads/thread.h"

/* Most words a command line may have, counting the program name.
 * The longest test passes 23 (see tests/userprog/args-many.ck), and the
 * pintos utility itself caps a kernel command line at 128 bytes. */
#define MAX_ARGS 128

/* A parent's record of one child process.
 *
 * This cannot live in the child's `struct thread', because a parent may
 * call wait() long after the child has died and must still be able to
 * collect its exit status.  So the record is reference counted: the
 * parent holds one reference and the child holds the other, and whoever
 * lets go last frees it.  That covers both orders -- child exits first,
 * or parent exits first and leaves an orphan. */
struct child_status {
    tid_t tid;                  /* The child's thread id. */
    int   exit_status;          /* What the child passed to exit(); -1 if
                                   the kernel killed it. */
    bool  load_success;         /* Did the child's executable load? */
    bool  waited;               /* Has the parent waited already?  Read and
                                   written only by the parent. */

    int   ref_cnt;              /* 2 while both sides are alive. */
    struct lock ref_lock;       /* Protects REF_CNT. */

    struct semaphore load_sema; /* Up'd once LOAD_SUCCESS is valid, so that
                                   exec() can report failure. */
    struct semaphore wait_sema; /* Up'd once EXIT_STATUS is valid, so that
                                   wait() can report it. */

    char *cmdline;              /* Command line handed to the child at
                                   startup; NULL once the child has it. */
    struct list_elem elem;      /* Element of the parent's `children' list. */
};

tid_t process_execute(const char *file_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(void);

#endif /* userprog/process.h */
