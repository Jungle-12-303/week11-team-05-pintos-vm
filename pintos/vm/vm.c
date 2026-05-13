/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "kernel/hash.h"

/* <<<<<<<<<<<<<<[HELIX]-------------- */
#include "threads/mmu.h"
/* --------------[HELIX]>>>>>>>>>>>>>> */

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
	struct frame *frame = malloc(sizeof(frame));
	if (frame == NULL){
		PANIC("와.. 여기서 할당 안되면 어째해야하노");
	}
	
	frame->kva = palloc_get_page(PAL_USER);
	if (frame->kva == NULL){
		free(frame);
	}
	/* --------------[HELIX]>>>>>>>>>>>>>> */

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
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
	struct page *page;

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	/* TODO: Validate the fault */
	if (addr == NULL || is_kernel_vaddr(addr)){
		return false;
	}
	/* TODO: Your code goes here */
	page = spt_find_page (spt, addr);
	/* --------------[HELIX]>>>>>>>>>>>>>> */

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

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)){
		return false;
	}
	/* --------------[HELIX]>>>>>>>>>>>>>> */

	
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
