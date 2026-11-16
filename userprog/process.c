#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"

#define LOGGING_LEVEL 6

#include <log.h>

static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void(**eip) (void), void **esp);
static void push_args(void **esp, int argc, char *argv[]);
static void first_word(const char *s, char *dst, size_t size);
static void child_status_release(struct child_status *cs);

/* Drops one reference to CS, freeing it once both sides have let go. */
static void
child_status_release(struct child_status *cs)
{
    bool last;

    lock_acquire(&cs->ref_lock);
    last = (--cs->ref_cnt == 0);
    lock_release(&cs->ref_lock);
    if (last) {
        free(cs);
    }
}

/* Copies the first word of S (the program name) into DST, of SIZE bytes. */
static void
first_word(const char *s, char *dst, size_t size)
{
    s += strspn(s, " \t");
    strlcpy(dst, s, size);
    dst[strcspn(dst, " \t")] = '\0';
}

/* Starts a user program: the first word of FILE_NAME names the executable,
 * the rest are its arguments.
 *
 * Blocks until the child has tried to load, since exec() must report -1 for
 * a program that cannot be loaded.  Returns the new process's thread id, or
 * TID_ERROR if the thread could not be created or the load failed. */
tid_t
process_execute(const char *file_name)
{
    char name[16];
    char *cmdline;
    struct child_status *cs;
    tid_t tid;

    // NOTE:
    // To see this print, make sure LOGGING_LEVEL in this file is <= L_TRACE (6)
    // AND LOGGING_ENABLE = 1 in lib/log.h
    // Also, probably won't pass with logging enabled.
    log(L_TRACE, "Started process execute: %s", file_name);

    /* Make a copy of FILE_NAME.
     * Otherwise there's a race between the caller and load(). */
    cmdline = palloc_get_page(0);
    if (cmdline == NULL) {
        return TID_ERROR;
    }
    strlcpy(cmdline, file_name, PGSIZE);

    /* Name the thread after the program, not the whole command line:
     * process_exit() prints this name in the exit message. */
    first_word(cmdline, name, sizeof name);

    cs = malloc(sizeof *cs);
    if (cs == NULL) {
        palloc_free_page(cmdline);
        return TID_ERROR;
    }
    cs->tid = TID_ERROR;
    cs->exit_status = -1;
    cs->load_success = false;
    cs->waited = false;
    cs->ref_cnt = 2;                     /* this parent, plus the child */
    lock_init(&cs->ref_lock);
    sema_init(&cs->load_sema, 0);
    sema_init(&cs->wait_sema, 0);
    cs->cmdline = cmdline;

    tid = thread_create(name, PRI_DEFAULT, start_process, cs);
    if (tid == TID_ERROR) {
        palloc_free_page(cmdline);
        free(cs);               /* the child never ran, so never took a ref */
        return TID_ERROR;
    }
    cs->tid = tid;

    sema_down(&cs->load_sema);
    if (!cs->load_success) {
        /* Not recorded as a child, so wait() can never find it. */
        child_status_release(cs);
        return TID_ERROR;
    }

    list_push_back(&thread_current()->children, &cs->elem);
    return tid;
}

/* Builds the initial user stack, following the 80x86 calling convention in
 * userprog/README.md ("Program Startup Details").  Clobbers ARGV[i] with
 * the address its string landed at. */
static void
push_args(void **esp, int argc, char *argv[])
{
    uint8_t *sp = *esp;
    uint32_t *w, *base;
    int i;

    /* Strings last-first, so argv[0]'s lands lowest, then word-align. */
    for (i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        sp -= n;
        memcpy(sp, argv[i], n);
        argv[i] = (char *)sp;
    }
    sp -= (uintptr_t)sp % 4;

    w = (uint32_t *)sp;
    *--w = 0;                           /* argv[argc]: null sentinel */
    for (i = argc - 1; i >= 0; i--) {
        *--w = (uintptr_t)argv[i];      /* argv[i] */
    }
    base = w;
    *--w = (uintptr_t)base;             /* argv */
    *--w = argc;
    *--w = 0;                           /* fake return address */

    *esp = w;
}

/* A thread function that loads a user process and starts it
 * running. */
static void
start_process(void *cs_)
{
    struct child_status *cs = cs_;
    char *cmdline = cs->cmdline;
    char *argv[MAX_ARGS];
    char *token, *saveptr;
    int argc = 0;
    size_t bytes;
    bool ok;
    int i;
    struct intr_frame if_;
    bool success;

    log(L_TRACE, "start_process()");

    /* Take our reference to the record the parent allocated for us. */
    thread_current()->cs = cs;
    cs->cmdline = NULL;

    /* Split the command line in place; strtok_r() collapses runs of
     * separators, so "a  b" yields the same two words as "a b".  argv[]
     * points into the page, which lives until push_args() has copied the
     * strings onto the user stack. */
    ok = true;
    for (token = strtok_r(cmdline, " \t", &saveptr); token != NULL;
         token = strtok_r(NULL, " \t", &saveptr)) {
        if (argc == MAX_ARGS) {
            ok = false;
            break;
        }
        argv[argc++] = token;
    }

    /* setup_stack() maps one page, so refuse a command line whose argument
     * block would not fit rather than let push_args() run off the end.
     * Counts strings, 3 bytes of padding, argv[0..argc], and the words for
     * argv, argc and the return address. */
    bytes = (argc + 1) * sizeof(char *) + 3 * sizeof(uint32_t) + 3;
    for (i = 0; i < argc; i++) {
        bytes += strlen(argv[i]) + 1;
    }
    if (bytes > PGSIZE) {
        ok = false;
    }

    /* Initialize interrupt frame and load executable. */
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = ok && argc > 0 && load(argv[0], &if_.eip, &if_.esp);
    if (success) {
        push_args(&if_.esp, argc, argv);
    }

    palloc_free_page(cmdline);

    /* Release the parent, which is blocked in process_execute() waiting to
     * learn whether we loaded. */
    cs->load_success = success;
    sema_up(&cs->load_sema);

    /* If load failed, quit. */
    if (!success) {
        thread_exit();
    }

    /* Start the user process by simulating a return from an
     * interrupt, implemented by intr_exit (in
     * threads/intr-stubs.S).  Because intr_exit takes all of its
     * arguments on the stack in the form of a `struct intr_frame',
     * we just point the stack pointer (%esp) to our stack frame
     * and jump to it. */
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
    NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting. */
int
process_wait(tid_t child_tid)
{
    struct thread *cur = thread_current();
    struct child_status *cs = NULL;
    struct list_elem *e;
    int status;

    /* Searching our own list, not all threads, is what makes wait() on a
     * grandchild fail. */
    for (e = list_begin(&cur->children); e != list_end(&cur->children);
         e = list_next(e)) {
        struct child_status *c = list_entry(e, struct child_status, elem);
        if (c->tid == child_tid) {
            cs = c;
            break;
        }
    }
    if (cs == NULL || cs->waited) {
        return -1;              /* not our child, or waited for already */
    }
    cs->waited = true;

    sema_down(&cs->wait_sema);  /* returns at once if it already exited */
    status = cs->exit_status;

    list_remove(&cs->elem);     /* a child may be waited for only once */
    child_status_release(cs);
    return status;
}

/* Free the current process's resources. */
void
process_exit(void)
{
    struct thread *cur = thread_current();
    uint32_t *pd;
    int fd;

    /* Close every file, however we got here: sys_exit() does this for a
     * process that called exit(), but kill() in exception.c reaches
     * thread_exit() directly.  Already-closed entries are NULL.  Closing
     * the executable re-allows writes to it. */
    lock_acquire(&filesys_lock);
    for (fd = 2; fd < FD_MAX; fd++) {
        if (cur->fds[fd] != NULL) {
            file_close(cur->fds[fd]);
            cur->fds[fd] = NULL;
        }
    }
    if (cur->exec_file != NULL) {
        file_close(cur->exec_file);
        cur->exec_file = NULL;
    }
    lock_release(&filesys_lock);

    /* Release our children's records.  A child still running holds its own
     * reference, so its record survives: that is how orphans work. */
    while (!list_empty(&cur->children)) {
        struct child_status *c = list_entry(list_pop_front(&cur->children),
                                            struct child_status, elem);
        child_status_release(c);
    }

    /* Report the exit and hand the status to our parent.  Only processes
     * from process_execute() have a record, so kernel threads print
     * nothing, and halt() never reaches here. */
    if (cur->cs != NULL) {
        printf("%s: exit(%d)\n", cur->name, cur->exit_status);
        cur->cs->exit_status = cur->exit_status;
        sema_up(&cur->cs->wait_sema);   /* only after storing the status */
        child_status_release(cur->cs);
        cur->cs = NULL;
    }

    /* Destroy the current process's page directory and switch back
     * to the kernel-only page directory. */
    pd = cur->pagedir;
    if (pd != NULL) {
        /* Correct ordering here is crucial.  We must set
         * cur->pagedir to NULL before switching page directories,
         * so that a timer interrupt can't switch back to the
         * process page directory.  We must activate the base page
         * directory before destroying the process's page
         * directory, or our active page directory will be one
         * that's been freed (and cleared). */
        cur->pagedir = NULL;
        pagedir_activate(NULL);
        pagedir_destroy(pd);
    }
}

/* Sets up the CPU for running user code in the current
 * thread.
 * This function is called on every context switch. */
void
process_activate(void)
{
    struct thread *t = thread_current();

    /* Activate thread's page tables. */
    pagedir_activate(t->pagedir);

    /* Set thread's kernel stack for use in processing
     * interrupts. */
    tss_update();
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
 * There are e_phnum of these, starting at file offset e_phoff
 * (see [ELF1] 1-6). */
struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0          /* Ignore. */
#define PT_LOAD    1          /* Loadable segment. */
#define PT_DYNAMIC 2          /* Dynamic linking info. */
#define PT_INTERP  3          /* Name of dynamic loader. */
#define PT_NOTE    4          /* Auxiliary info. */
#define PT_SHLIB   5          /* Reserved. */
#define PT_PHDR    6          /* Program header table. */
#define PT_STACK   0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void **esp);

static bool validate_segment(const struct Elf32_Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *EIP
 * and its initial stack pointer into *ESP.
 * Returns true if successful, false otherwise. */
bool
load(const char *file_name, void(**eip) (void), void **esp)
{
    log(L_TRACE, "load()");
    struct thread *t = thread_current();
    struct Elf32_Ehdr ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* The provided file system has no internal synchronization, so hold the
     * same lock the syscalls use.  Held across the whole function so every
     * `goto done' unlocks. */
    lock_acquire(&filesys_lock);

    /* Allocate and activate page directory. */
    t->pagedir = pagedir_create();
    if (t->pagedir == NULL) {
        goto done;
    }
    process_activate();

    /* Open executable file. */
    file = filesys_open(file_name);
    if (file == NULL) {
        printf("load: %s: open failed\n", file_name);
        goto done;
    }

    /* Hold the executable open with writes denied for as long as we run, so
     * nobody can modify code we are executing; process_exit() closes it. */
    file_deny_write(file);
    t->exec_file = file;

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr
        || memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7)
        || ehdr.e_type != 2
        || ehdr.e_machine != 3
        || ehdr.e_version != 1
        || ehdr.e_phentsize != sizeof(struct Elf32_Phdr)
        || ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Elf32_Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file)) {
            goto done;
        }
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr) {
            goto done;
        }
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;
        case PT_LOAD:
            if (validate_segment(&phdr, file)) {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint32_t file_page = phdr.p_offset & ~PGMASK;
                uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint32_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0) {
                    /* Normal segment.
                     * Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE)
                                  - read_bytes);
                } else {
                    /* Entirely zero.
                     * Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment(file, file_page, (void *)mem_page,
                                  read_bytes, zero_bytes, writable)) {
                    goto done;
                }
            } else {
                goto done;
            }
            break;
        }
    }

    /* Set up stack. */
    if (!setup_stack(esp)) {
        goto done;
    }

    /* Start address. */
    *eip = (void (*)(void))ehdr.e_entry;

    success = true;

done:
    /* Reached whether or not the load worked.  On success the executable
     * stays open as t->exec_file; only a failure closes it here. */
    if (!success) {
        file_close(file);
        t->exec_file = NULL;
    }
    lock_release(&filesys_lock);
    return success;
}

/* load() helpers. */

static bool install_page(void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Elf32_Phdr *phdr, struct file *file)
{
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) {
        return false;
    }

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (Elf32_Off)file_length(file)) {
        return false;
    }

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz) {
        return false;
    }

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0) {
        return false;
    }

    /* The virtual memory region must both start and end within the
     * user address space range. */
    if (!is_user_vaddr((void *)phdr->p_vaddr)) {
        return false;
    }
    if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz))) {
        return false;
    }

    /* The region cannot "wrap around" across the kernel virtual
     * address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) {
        return false;
    }

    /* Disallow mapping page 0.
     * Not only is it a bad idea to map page 0, but if we allowed
     * it then user code that passed a null pointer to system calls
     * could quite likely panic the kernel by way of null pointer
     * assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE) {
        return false;
    }

    /* It's okay. */
    return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 *      - READ_BYTES bytes at UPAGE must be read from FILE
 *        starting at offset OFS.
 *
 *      - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    log(L_TRACE, "load_segment()");

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0) {
        /* Calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t *kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL) {
            return false;
        }

        /* Load this page. */
        if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable)) {
            palloc_free_page(kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
 * user virtual memory. */
static bool
setup_stack(void **esp)
{
    uint8_t *kpage;
    bool success = false;

    log(L_TRACE, "setup_stack()");

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        success = install_page(((uint8_t *)PHYS_BASE) - PGSIZE, kpage, true);
        if (success) {
            *esp = PHYS_BASE;
        } else {
            palloc_free_page(kpage);
        }
        // hex_dump( *(int*)esp, *esp, 128, true ); // NOTE: uncomment this to check arg passing
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return pagedir_get_page(t->pagedir, upage) == NULL
           && pagedir_set_page(t->pagedir, upage, kpage, writable);
}
