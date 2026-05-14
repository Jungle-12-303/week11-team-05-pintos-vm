/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "kernel/hash.h"

#define STACK_MAX (1 << 20)
#include "threads/mmu.h"

static struct list frame_table;


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

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	/* frame_table은 일단 리스트로 구현 중 */
	list_init(&frame_table);
	/* --------------[HELIX]>>>>>>>>>>>>>> */
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
static uint64_t spt_page_hash (const struct hash_elem *e, void *aux);
static bool spt_page_less (const struct hash_elem *a, const struct hash_elem *b,
                           void *aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
                                vm_initializer *init, void *aux) {
	ASSERT (VM_TYPE (type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;
	bool (*initializer) (struct page *, enum vm_type, void *) = NULL;

	upage = pg_round_down (upage);

	if (spt_find_page (spt, upage) == NULL) {
		switch (VM_TYPE (type)) {
		case VM_ANON:
			initializer = anon_initializer;
			break;

		case VM_FILE:
			initializer = file_backed_initializer;
			break;
		default:
			goto err;
		}

		struct page *page = malloc (sizeof (struct page));
		if (page == NULL) {
			goto err;
		}
		// 아직 실제 내용 로딩되지 않은 uninit page 상태로 초기화
		uninit_new (page, upage, init, type, aux, initializer);
		page->writable = writable;
		page->owner = thread_current ();
		if (!spt_insert_page (spt, page)) {
			free (page);
			goto err;
		}

		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	/* TODO: Fill this function. */
	va = pg_round_down (va);
	struct page temp;
	temp.va = va;

	struct hash_elem *find_elem = hash_find (&spt->pages, &temp.spt_elem);
	if (find_elem != NULL) {
		return hash_entry (find_elem, struct page, spt_elem);
	} else {
		return NULL;
	}
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
                 struct page *page) {
	int succ = false;
	page->va = pg_round_down (page->va);
	if (hash_insert (&spt->pages, &page->spt_elem) == NULL) {
		succ = true;
	}

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	hash_delete (&spt->pages, &page->spt_elem);
	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim;

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	/* TODO: The policy for eviction is up to you. */
	if (list_empty(&frame_table)){
		return NULL;
	}
	struct list_elem *el = list_pop_front(&frame_table);
	victim = list_entry(el, struct frame, elem);

	list_push_back(&frame_table, el);
	/* --------------[HELIX]>>>>>>>>>>>>>> */
	
	return victim;
}

/* Evict one page and return the corresponding frame.
* Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	/* TODO: swap out the victim and return the evicted frame. */
	if (victim == NULL){
		PANIC("FUBAR");
	}
	
	struct page *page = victim->page;
	if (page == NULL){
		return victim;
	}

	if (!swap_out(page)){
		PANIC("swap_out fail");
	}

	pml4_clear_page(page->owner->pml4, page->va);
	page->frame = NULL;
	victim->page = NULL;
	/* --------------[HELIX]>>>>>>>>>>>>>> */

	return victim;
}

static uint64_t
spt_page_hash (const struct hash_elem *e, void *aux) {
	struct page *page = hash_entry (e, struct page, spt_elem);
	return hash_bytes (&page->va, sizeof page->va);
}

static bool
spt_page_less (const struct hash_elem *a, const struct hash_elem *b,
               void *aux) {
	struct page *pa = hash_entry (a, struct page, spt_elem);
	struct page *pb = hash_entry (b, struct page, spt_elem);

	return (pa->va) < (pb->va);
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	void *kva = palloc_get_page(PAL_USER);

	if (kva == NULL){
		return vm_evict_frame;
	}

	struct frame *frame = malloc(sizeof(frame));
	if (frame == NULL){
		PANIC("와.. 여기서 할당 안되면 어째해야하노");
	}
	frame->kva = kva;
	frame->page = NULL;

	/* 일단 FIFO */
	list_push_back (&frame_table, &frame->elem);
	/* --------------[HELIX]>>>>>>>>>>>>>> */

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
	/* Fault가 발생한 주소를 page boundary에 맞춘다.
	 * Pintos의 page 할당/관리는 PGSIZE 단위로 이루어지므로,
	 * 같은 page 안의 어떤 주소에서 fault가 나도 해당 page의 시작 주소를 사용한다. */
	void *upage = pg_round_down (addr);

	/* 스택은 USER_STACK에서 아래 방향으로 자란다.
	 * 프로젝트 요구사항에 맞춰 최대 1MB까지만 stack growth를 허용한다. */
	if ((uint8_t *) USER_STACK - (uint8_t *) upage > STACK_MAX)
		return;

	/* 이미 SPT에 등록된 page라면 새로 만들 필요가 없다.
	 * 등록되어 있지 않으면 stack용 anonymous page를 만들고 즉시 claim한다. */
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
	uint8_t *fault_addr = addr;

	/* NULL 접근이거나 kernel 영역 주소에 대한 접근이면
	 * user process가 처리할 수 있는 유효한 page fault가 아니다. */
	if (addr == NULL || is_kernel_vaddr (addr))
		return false;

	/* not-present fault만 여기서 처리한다.
	 * protection fault, 예를 들어 read-only page에 write한 경우는
	 * 현재 구현에서는 처리하지 않고 실패시킨다. */
	if (!not_present)
		return false;

	/* fault가 발생한 주소에 해당하는 page가 이미 SPT에 등록되어 있는지 확인한다.
	 * lazy loading된 실행 파일 page나 swap out된 page라면 여기서 찾을 수 있다. */
	page = spt_find_page (spt, addr);
	if (page == NULL) {
		/* SPT에 page가 없다면 stack growth 가능성을 검사한다.
		 * 현재 구조에서는 user mode fault의 rsp를 intr_frame에서 가져온다. */
		rsp = (void *) f->rsp;
		uint8_t *stack_pointer = rsp;

		/* stack 접근처럼 보이는 fault만 stack growth로 처리한다.
		 * x86-64 PUSH 계열 명령은 rsp보다 8바이트 낮은 위치에서 fault가 날 수 있으므로
		 * fault 주소가 rsp - 8 이상이면 정상 stack 접근 후보로 본다. */
		if (rsp != NULL
		    && fault_addr >= stack_pointer - 8
		    && fault_addr < (uint8_t *) USER_STACK
		    && (uint8_t *) USER_STACK - (uint8_t *) pg_round_down (addr) <= STACK_MAX) {
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

	/* page에 실제 frame을 할당하고 page table에 매핑한다.
	 * uninit page라면 이 과정에서 lazy loading도 수행된다. */
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
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)){
		return false;
	}


	
	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init (&spt->pages, spt_page_hash, spt_page_less, NULL);
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
