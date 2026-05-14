/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
	case VM_UNINIT:
		return VM_TYPE (page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
                                vm_initializer *init, void *aux) {
	ASSERT (VM_TYPE (type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */

	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
                 struct page *page UNUSED) {
	int succ = false;
	/* TODO: Fill this function. */

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
  and return it. This always return valid address. That is, if the user pool
  memory is full, this function evicts the frame to get the available memory
  space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
	/* Fault가 발생한 주소를 page boundary에 맞춤.
	   Pintos의 page 할당/관리는 PGSIZE 단위로 이루어지므로,
	   같은 page 안의 어떤 주소에서 fault가 나도 해당 page의 시작 주소를 사용한다. */
	void *upage = pg_round_down (addr);

	/* 스택은 보통 USER_STACK에서 아래 방향으로 자람.
	   프로젝트 요구사항에 따라 최대 1MB까지만 stack growth를 허용한다. 그 이상은 process kill~ */
	if ((uint8_t *) USER_STACK - (uint8_t *) upage > STACK_MAX)
		return;

	/* 이미 SPT에 등록된 page라면 새로 만들 필요가 없음.
	   등록되어 있지 않으면 stack용 anonymous page를 만들고 즉시 claim ㄱㄱ */
	if (spt_find_page (&thread_current ()->spt, upage) == NULL) {
		if (vm_alloc_page (VM_ANON | VM_MARKER_0, upage, true))
			vm_claim_page (upage);
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
                     bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	void *rsp;

	/* NULL 접근이거나 kernel 영역 주소에 대한 접근이면
	   user process가 처리할 수 있는 유효한 page fault가 아닌겨 */
	if (addr == NULL || is_kernel_vaddr (addr))
		return false;

	/* not-present fault만 여기서 처리.
	   protection fault, 예를 들어 read-only page에 write한 경우는
	   현재 구현에서는 처리하지 않고 실패시킴 */
	if (!not_present)
		return false;

	/* fault가 발생한 주소에 해당하는 page가 이미 SPT에 등록되어 있는지 확인함
	   lazy loading된 실행 파일 page나 swap out된 page라면 여기서 찾을 수 있음. */
	page = spt_find_page (spt, addr);

	if (page == NULL) {
		/* SPT에 page가 없다면 stack growth 가능성을 검사.
		   user mode fault면 intr_frame의 rsp가 user stack pointer이고,
		   kernel mode fault면 syscall 진입 시 저장해둔 user_rsp를 사용함 */
		rsp = user ? (void *) f->rsp : thread_current ()->user_rsp;

		/* stack 접근처럼 보이는 fault만 stack growth로 처리함.
		  x86-64 PUSH 명령은 rsp를 내리기 전에 rsp - 8 위치를 먼저 검사할 수 있으므로
		  addr >= rsp - 8까지 허용함.

		  또한 USER_STACK 아래쪽 주소여야 하고,
		  프로젝트 요구사항에 따라 최대 stack 크기 1MB를 넘으면 안 된다. */
		if (rsp != NULL && addr >= rsp - 8 && addr < USER_STACK && (uint8_t *) USER_STACK - (uint8_t *) pg_round_down (addr) <= STACK_MAX) {
			vm_stack_growth (addr);

			/* stack page를 새로 만들었으므로 다시 SPT에서 찾아온다. */
			page = spt_find_page (spt, addr);
		}
	}

	/* SPT에서도 못 찾고 stack growth도 아니면 처리할 수 없는 fault다. */
	if (page == NULL)
		return false;

	/* write fault인데 해당 page가 writable이 아니라면 잘못된 접근이다. */
	if (write && !page->writable)
		return false;

	/* page에 실제 frame을 할당하고 page table에 매핑.
	   uninit page라면 이 과정에서 lazy loading도 수행된다. */
	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
                              struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}
