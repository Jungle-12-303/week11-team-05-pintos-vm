#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

struct lock filesys_lock;

static void check_address (const void *addr);
static void check_string (const char *str);
static void check_buffer (const void *buffer, unsigned size);
static struct file *get_file (int fd);
static int add_file (struct file *file);
static void close_fd (int fd);
static void exit_process (int status);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	lock_init (&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	uint64_t syscall_no = f->R.rax;

	switch (syscall_no) {
		case SYS_HALT:
			power_off ();
			break;
		case SYS_EXIT:
			exit_process ((int) f->R.rdi);
			break;
		case SYS_FORK:
			check_string ((const char *) f->R.rdi);
			f->R.rax = process_fork ((const char *) f->R.rdi, f);
			break;
		case SYS_EXEC: {
			const char *file = (const char *) f->R.rdi;
			char *copy;
			check_string (file);
			copy = palloc_get_page (0);
			if (copy == NULL) {
				f->R.rax = -1;
				break;
			}
			strlcpy (copy, file, PGSIZE);
			f->R.rax = process_exec (copy);
			break;
		}
		case SYS_WAIT:
			f->R.rax = process_wait ((tid_t) f->R.rdi);
			break;
		case SYS_CREATE:
			check_string ((const char *) f->R.rdi);
			lock_acquire (&filesys_lock);
			f->R.rax = filesys_create ((const char *) f->R.rdi,
					(off_t) f->R.rsi);
			lock_release (&filesys_lock);
			break;
		case SYS_REMOVE:
			check_string ((const char *) f->R.rdi);
			lock_acquire (&filesys_lock);
			f->R.rax = filesys_remove ((const char *) f->R.rdi);
			lock_release (&filesys_lock);
			break;
		case SYS_OPEN: {
			struct file *file;
			check_string ((const char *) f->R.rdi);
			lock_acquire (&filesys_lock);
			file = filesys_open ((const char *) f->R.rdi);
			lock_release (&filesys_lock);
			f->R.rax = add_file (file);
			break;
		}
		case SYS_FILESIZE: {
			struct file *file = get_file ((int) f->R.rdi);
			if (file == NULL) {
				f->R.rax = -1;
				break;
			}
			lock_acquire (&filesys_lock);
			f->R.rax = file_length (file);
			lock_release (&filesys_lock);
			break;
		}
		case SYS_READ: {
			int fd = (int) f->R.rdi;
			void *buffer = (void *) f->R.rsi;
			unsigned size = (unsigned) f->R.rdx;
			check_buffer (buffer, size);
			if (fd == STDIN_FILENO) {
				uint8_t *buf = buffer;
				for (unsigned i = 0; i < size; i++)
					buf[i] = input_getc ();
				f->R.rax = size;
			} else {
				struct file *file = get_file (fd);
				if (file == NULL || fd == STDOUT_FILENO) {
					f->R.rax = -1;
				} else {
					lock_acquire (&filesys_lock);
					f->R.rax = file_read (file, buffer, size);
					lock_release (&filesys_lock);
				}
			}
			break;
		}
		case SYS_WRITE: {
			int fd = (int) f->R.rdi;
			const void *buffer = (const void *) f->R.rsi;
			unsigned size = (unsigned) f->R.rdx;
			check_buffer (buffer, size);
			if (fd == STDOUT_FILENO) {
				putbuf (buffer, size);
				f->R.rax = size;
			} else {
				struct file *file = get_file (fd);
				if (file == NULL || fd == STDIN_FILENO) {
					f->R.rax = -1;
				} else {
					lock_acquire (&filesys_lock);
					f->R.rax = file_write (file, buffer, size);
					lock_release (&filesys_lock);
				}
			}
			break;
		}
		case SYS_SEEK: {
			struct file *file = get_file ((int) f->R.rdi);
			if (file != NULL) {
				lock_acquire (&filesys_lock);
				file_seek (file, (off_t) f->R.rsi);
				lock_release (&filesys_lock);
			}
			break;
		}
		case SYS_TELL: {
			struct file *file = get_file ((int) f->R.rdi);
			if (file == NULL) {
				f->R.rax = -1;
			} else {
				lock_acquire (&filesys_lock);
				f->R.rax = file_tell (file);
				lock_release (&filesys_lock);
			}
			break;
		}
		case SYS_CLOSE:
			close_fd ((int) f->R.rdi);
			break;
		default:
			exit_process (-1);
			break;
	}
}

static void
check_address (const void *addr) {
	if (addr == NULL || is_kernel_vaddr (addr)
			|| pml4_get_page (thread_current ()->pml4, addr) == NULL)
		exit_process (-1);
}

static void
check_string (const char *str) {
	check_address (str);
	while (true) {
		check_address (str);
		if (*str == '\0')
			return;
		str++;
	}
}

static void
check_buffer (const void *buffer, unsigned size) {
	const uint8_t *begin = buffer;
	const uint8_t *end;

	if (size == 0)
		return;
	check_address (begin);
	end = begin + size - 1;
	check_address (end);
	for (const uint8_t *page = pg_round_down (begin);
			page <= end; page += PGSIZE)
		check_address (page);
}

static struct file *
get_file (int fd) {
	struct thread *curr = thread_current ();

	if (fd < 2 || fd >= FD_TABLE_SIZE)
		return NULL;
	return curr->fd_table[fd];
}

static int
add_file (struct file *file) {
	struct thread *curr = thread_current ();

	if (file == NULL)
		return -1;
	for (int fd = curr->next_fd; fd < FD_TABLE_SIZE; fd++) {
		if (curr->fd_table[fd] == NULL) {
			curr->fd_table[fd] = file;
			curr->next_fd = fd + 1;
			return fd;
		}
	}
	for (int fd = 2; fd < curr->next_fd; fd++) {
		if (curr->fd_table[fd] == NULL) {
			curr->fd_table[fd] = file;
			curr->next_fd = fd + 1;
			return fd;
		}
	}
	file_close (file);
	return -1;
}

static void
close_fd (int fd) {
	struct file *file = get_file (fd);

	if (file != NULL) {
		thread_current ()->fd_table[fd] = NULL;
		file_close (file);
	}
}

static void
exit_process (int status) {
	thread_current ()->exit_status = status;
	thread_exit ();
}
