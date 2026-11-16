#include <stdio.h>
#include <syscall-nr.h>

#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"

struct lock filesys_lock;

static void syscall_handler(struct intr_frame *);

void
syscall_init(void)
{
    lock_init(&filesys_lock);
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Terminates the current process with STATUS. */
void
sys_exit(int status)
{
    struct thread *cur = thread_current();
    int fd;

    if (lock_held_by_current_thread(&filesys_lock)) {
        lock_release(&filesys_lock);
    }
    lock_acquire(&filesys_lock);
    for (fd = 2; fd < FD_MAX; fd++) {
        if (cur->fds[fd] != NULL) {
            file_close(cur->fds[fd]);
            cur->fds[fd] = NULL;
        }
    }
    lock_release(&filesys_lock);

    cur->exit_status = status;
    thread_exit();
}

/* Kills the process unless UADDR is a mapped user address. */
static void
check_ptr(const void *uaddr)
{
    if (uaddr == NULL || !is_user_vaddr(uaddr)
        || pagedir_get_page(thread_current()->pagedir, uaddr) == NULL) {
        sys_exit(-1);
    }
}

/* Checks every page touched by the SIZE bytes at BUF. */
static void
check_buffer(const void *buf, unsigned size)
{
    const uint8_t *p;

    check_ptr(buf);
    for (p = pg_round_down(buf); p < (const uint8_t *)buf + size; p += PGSIZE) {
        check_ptr(p);
    }
}

/* Checks a null-terminated user string. */
static void
check_string(const char *s)
{
    check_ptr(s);
    while (*s != '\0') {
        check_ptr(++s);
    }
}

/* Returns the Nth 32-bit word on the user stack (0 is the call number). */
static uint32_t
get_arg(struct intr_frame *f, int n)
{
    uint32_t *p = (uint32_t *)f->esp + n;

    check_buffer(p, sizeof *p);
    return *p;
}

static struct file *
fd_file(int fd)
{
    if (fd < 2 || fd >= FD_MAX) {
        return NULL;
    }
    return thread_current()->fds[fd];
}

static int
sys_open(const char *name)
{
    struct thread *cur = thread_current();
    struct file *file;
    int fd;

    check_string(name);
    lock_acquire(&filesys_lock);
    file = filesys_open(name);
    if (file == NULL) {
        lock_release(&filesys_lock);
        return -1;
    }
    for (fd = 2; fd < FD_MAX; fd++) {
        if (cur->fds[fd] == NULL) {
            cur->fds[fd] = file;
            lock_release(&filesys_lock);
            return fd;
        }
    }
    file_close(file);
    lock_release(&filesys_lock);
    return -1;
}

static int
sys_read(int fd, uint8_t *buf, unsigned size)
{
    struct file *file;
    unsigned i;
    int n;

    check_buffer(buf, size);
    if (fd == 0) {
        for (i = 0; i < size; i++) {
            buf[i] = input_getc();
        }
        return size;
    }
    file = fd_file(fd);
    if (file == NULL) {
        return -1;
    }
    lock_acquire(&filesys_lock);
    n = file_read(file, buf, size);
    lock_release(&filesys_lock);
    return n;
}

static int
sys_write(int fd, const void *buf, unsigned size)
{
    struct file *file;
    int n;

    check_buffer(buf, size);
    if (fd == 1) {
        putbuf(buf, size);
        return size;
    }
    file = fd_file(fd);
    if (file == NULL) {
        return -1;
    }
    lock_acquire(&filesys_lock);
    n = file_write(file, buf, size);
    lock_release(&filesys_lock);
    return n;
}

static void
syscall_handler(struct intr_frame *f)
{
    const char *name;
    struct file *file;
    int fd;

    switch (get_arg(f, 0)) {
    case SYS_HALT:
        shutdown_power_off();

    case SYS_EXIT:
        sys_exit((int)get_arg(f, 1));

    case SYS_EXEC:
        name = (const char *)get_arg(f, 1);
        check_string(name);
        f->eax = process_execute(name);
        break;

    case SYS_WAIT:
        f->eax = process_wait((tid_t)get_arg(f, 1));
        break;

    case SYS_CREATE:
        name = (const char *)get_arg(f, 1);
        check_string(name);
        lock_acquire(&filesys_lock);
        f->eax = filesys_create(name, get_arg(f, 2));
        lock_release(&filesys_lock);
        break;

    case SYS_REMOVE:
        name = (const char *)get_arg(f, 1);
        check_string(name);
        lock_acquire(&filesys_lock);
        f->eax = filesys_remove(name);
        lock_release(&filesys_lock);
        break;

    case SYS_OPEN:
        f->eax = sys_open((const char *)get_arg(f, 1));
        break;

    case SYS_FILESIZE:
        file = fd_file(get_arg(f, 1));
        if (file == NULL) {
            f->eax = -1;
            break;
        }
        lock_acquire(&filesys_lock);
        f->eax = file_length(file);
        lock_release(&filesys_lock);
        break;

    case SYS_READ:
        f->eax = sys_read(get_arg(f, 1), (uint8_t *)get_arg(f, 2), get_arg(f, 3));
        break;

    case SYS_WRITE:
        f->eax = sys_write(get_arg(f, 1), (const void *)get_arg(f, 2), get_arg(f, 3));
        break;

    case SYS_SEEK:
        file = fd_file(get_arg(f, 1));
        if (file != NULL) {
            lock_acquire(&filesys_lock);
            file_seek(file, get_arg(f, 2));
            lock_release(&filesys_lock);
        }
        break;

    case SYS_TELL:
        file = fd_file(get_arg(f, 1));
        if (file == NULL) {
            f->eax = -1;
            break;
        }
        lock_acquire(&filesys_lock);
        f->eax = file_tell(file);
        lock_release(&filesys_lock);
        break;

    case SYS_CLOSE:
        fd = get_arg(f, 1);
        file = fd_file(fd);
        if (file != NULL) {
            lock_acquire(&filesys_lock);
            file_close(file);
            lock_release(&filesys_lock);
            thread_current()->fds[fd] = NULL;
        }
        break;

    default:
        sys_exit(-1);
    }
}
