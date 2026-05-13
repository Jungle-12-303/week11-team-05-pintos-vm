#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static void close_all_files (struct thread *t);
static bool push_arguments (char **argv, int argc, struct intr_frame *if_);

struct child_status {
	tid_t tid;
	int exit_status;
	bool exited;
	bool waited;
	bool parent_alive;
	bool load_success;
	struct semaphore wait_sema;
	struct semaphore load_sema;
	struct list_elem elem;
};

struct fork_info {
	struct thread *parent;
	struct intr_frame if_;
	struct child_status *child;
};

struct initd_info {
	char *file_name;
	struct child_status *child;
};

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
	current->next_fd = 2;
	for (int i = 0; i < FD_TABLE_SIZE; i++)
		current->fd_table[i] = NULL;
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	char thread_name[16];
	char *save_ptr;
	struct initd_info *info;
	struct child_status *child;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	info = malloc (sizeof *info);
	child = malloc (sizeof *child);
	if (info == NULL || child == NULL) {
		palloc_free_page (fn_copy);
		free (info);
		free (child);
		return TID_ERROR;
	}
	strlcpy (fn_copy, file_name, PGSIZE);
	strlcpy (thread_name, file_name, sizeof thread_name);
	strtok_r (thread_name, " ", &save_ptr);

	child->tid = TID_ERROR;
	child->exit_status = -1;
	child->exited = false;
	child->waited = false;
	child->parent_alive = true;
	child->load_success = true;
	sema_init (&child->wait_sema, 0);
	sema_init (&child->load_sema, 0);
	info->file_name = fn_copy;
	info->child = child;

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (thread_name, PRI_DEFAULT, initd, info);
	if (tid == TID_ERROR) {
		palloc_free_page (fn_copy);
		free (info);
		free (child);
	} else {
		child->tid = tid;
		list_push_back (&thread_current ()->children, &child->elem);
	}
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *aux) {
	struct initd_info *info = aux;
	void *f_name = info->file_name;
	thread_current ()->child_status = info->child;
	free (info);

#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC ("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct fork_info *info;
	struct child_status *child;
	tid_t tid;

	info = malloc (sizeof *info);
	child = malloc (sizeof *child);
	if (info == NULL || child == NULL) {
		free (info);
		free (child);
		return TID_ERROR;
	}

	child->tid = TID_ERROR;
	child->exit_status = -1;
	child->exited = false;
	child->waited = false;
	child->parent_alive = true;
	child->load_success = false;

	sema_init (&child->wait_sema, 0);
	sema_init (&child->load_sema, 0);

	info->parent = thread_current ();
	memcpy (&info->if_, if_, sizeof info->if_);
	info->child = child;

	tid = thread_create (name, thread_get_priority (), __do_fork, info);
	if (tid == TID_ERROR) {
		free (info);
		free (child);
		return TID_ERROR;
	}

	child->tid = tid;
	list_push_back (&thread_current ()->children, &child->elem);
	sema_down (&child->load_sema);
	if (!child->load_success)
		return TID_ERROR;
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kern_pte (pte) || is_kernel_vaddr (va))
		return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
		return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page (PAL_USER);
	if (newpage == NULL)
		return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable (pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page (newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct fork_info *info = aux;
	struct intr_frame if_;
	struct thread *parent = info->parent;
	struct thread *current = thread_current ();
	struct child_status *child = info->child;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, &info->if_, sizeof (struct intr_frame));
	if_.R.rax = 0;
	current->child_status = child;
	current->exit_status = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create ();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	current->next_fd = parent->next_fd;
	for (int i = 2; i < FD_TABLE_SIZE; i++) {
		if (parent->fd_table[i] != NULL) {
			current->fd_table[i] = file_duplicate (parent->fd_table[i]);
			if (current->fd_table[i] == NULL)
				goto error;
		}
	}
	if (parent->executable_file != NULL) {
		current->executable_file = file_duplicate (parent->executable_file);
		if (current->executable_file == NULL)
			goto error;
	}

	child->load_success = true;
	sema_up (&child->load_sema);
	free (info);

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	child->load_success = false;
	sema_up (&child->load_sema);
	free (info);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;
	struct thread *curr = thread_current ();

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	if (curr->executable_file != NULL) {
		file_close (curr->executable_file);
		curr->executable_file = NULL;
	}

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current ();
	struct child_status *child = NULL;

	for (struct list_elem *e = list_begin (&curr->children);
	     e != list_end (&curr->children); e = list_next (e)) {
		struct child_status *candidate = list_entry (e, struct child_status, elem);
		if (candidate->tid == child_tid) {
			child = candidate;
			break;
		}
	}

	if (child == NULL || child->waited)
		return -1;

	child->waited = true;
	sema_down (&child->wait_sema);
	int status = child->exit_status;
	list_remove (&child->elem);
	free (child);
	return status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	if (curr->pml4 != NULL)
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

	close_all_files (curr);
	if (curr->executable_file != NULL) {
		file_close (curr->executable_file);
		curr->executable_file = NULL;
	}

	while (!list_empty (&curr->children)) {
		struct child_status *child = list_entry (list_pop_front (&curr->children),
		                                         struct child_status, elem);
		child->parent_alive = false;
		if (child->exited)
			free (child);
	}

	if (curr->child_status != NULL) {
		curr->child_status->exit_status = curr->exit_status;
		curr->child_status->exited = true;
		sema_up (&curr->child_status->wait_sema);
		if (!curr->child_status->parent_alive)
			free (curr->child_status);
		curr->child_status = NULL;
	}

	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

static void
close_all_files (struct thread *t) {
	for (int i = 2; i < FD_TABLE_SIZE; i++) {
		if (t->fd_table[i] != NULL) {
			file_close (t->fd_table[i]);
			t->fd_table[i] = NULL;
		}
	}
}

static bool
push_arguments (char **argv, int argc, struct intr_frame *if_) {
	uintptr_t rsp = if_->rsp;
	uintptr_t arg_addrs[64];

	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen (argv[i]) + 1;
		if (rsp < (uintptr_t) USER_STACK - PGSIZE + len)
			return false;
		rsp -= len;
		memcpy ((void *) rsp, argv[i], len);
		arg_addrs[i] = rsp;
	}

	rsp &= ~0x7;
	if (rsp < (uintptr_t) USER_STACK - PGSIZE + sizeof (char *))
		return false;
	rsp -= sizeof (char *);
	*(char **) rsp = NULL;

	for (int i = argc - 1; i >= 0; i--) {
		if (rsp < (uintptr_t) USER_STACK - PGSIZE + sizeof (char *))
			return false;
		rsp -= sizeof (char *);
		*(char **) rsp = (char *) arg_addrs[i];
	}

	if_->R.rdi = argc;
	if_->R.rsi = rsp;
	if (rsp < (uintptr_t) USER_STACK - PGSIZE + sizeof (void *))
		return false;
	rsp -= sizeof (void *);
	*(void **) rsp = NULL;
	if_->rsp = rsp;
	return true;
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0          /* Ignore. */
#define PT_LOAD    1          /* Loadable segment. */
#define PT_DYNAMIC 2          /* Dynamic linking info. */
#define PT_INTERP  3          /* Name of dynamic loader. */
#define PT_NOTE    4          /* Auxiliary info. */
#define PT_SHLIB   5          /* Reserved. */
#define PT_PHDR    6          /* Program header table. */
#define PT_STACK   0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF  ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
/* FILE_NAME에 있는 ELF 실행 파일을 현재 스레드로 로드합니다.
 * 실행 파일의 진입점을 *RIP에 저장합니다.
 * 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공하면 true, 실패하면 false를 반환합니다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	char *argv[64];
	int argc = 0;
	char *token, *save_ptr;
	char *cmd = (char *) file_name;

	for (token = strtok_r (cmd, " ", &save_ptr); token != NULL;
	     token = strtok_r (NULL, " ", &save_ptr)) {
		if (argc >= 64)
			goto done;
		argv[argc++] = token;
	}
	if (argc == 0)
		goto done;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	lock_acquire (&filesys_lock);
	file = filesys_open (argv[0]);
	lock_release (&filesys_lock);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}
	file_deny_write (file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
	    || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof (struct Phdr) || ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
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
			if (validate_segment (&phdr, file)) {
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0) {
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				} else {
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment (file, file_page, (void *) mem_page,
				                   read_bytes, zero_bytes, writable))
					goto done;
			} else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	if (!push_arguments (argv, argc, if_))
		goto done;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if (success)
		t->executable_file = file;
	else
		file_close (file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf ("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
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

/* 사용자 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의 매핑을 페이지 테이블에 추가합니다.
 * WRITABLE이 true이면 사용자 프로세스는 해당 페이지를 수정할 수 있습니다.
 * 그렇지 않으면 읽기 전용입니다.
 * UPAGE는 이미 매핑되어 있으면 안 됩니다.
 * KPAGE는 palloc_get_page()를 사용하여 사용자 풀에서 가져온 페이지여야 합니다.
 * 성공 시 true, UPAGE가 이미 매핑되어 있거나
 * 메모리 할당에 실패하면 false를 반환합니다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL && pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
/* FILE의 주소 UPAGE에서 오프셋 OFS부터 시작하는 세그먼트를 로드합니다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이 초기화됩니다.
 *
 * - UPAGE 주소의 READ_BYTES 바이트는 FILE에서 오프셋 OFS부터 읽어야 합니다.
 *
 * - UPAGE + READ_BYTES 주소의 ZERO_BYTES 바이트는 0으로 초기화해야 합니다.
 *
 * 이 함수로 초기화된 페이지는 WRITABLE이 true이면 사용자 프로세스에서 쓰기 가능해야 하고, 그렇지 않으면 읽기 전용이어야 합니다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류 또는 디스크 읽기 오류가 발생하면 false를 반환합니다. */

// 각 매개변수 역할
// file => 어떤 파일인지
// ofs => 파일에서 시작할 부분
// upage => 매핑할 유저 가상 주소
// read_bytes => 파일에서 실제로 읽어올 바이트 수
// zero_bytes => 0으로 채울 바이트 수
// writable => 쓰기 가능 여부
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	// 예외처리
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		// page_read_bytes = 현재 할당해야할 페이지의 크기
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		// PGSIZE에서 위에 연산한 page_read_bytes만큼 제외해서, 나머지 부분을 0으로 밀어줌.
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		// 여기에서 aux에 들어갈 값을 선언해줘야 함.
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
		                                     writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
