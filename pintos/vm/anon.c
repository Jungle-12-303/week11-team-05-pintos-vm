/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"

/* <<<<<<<<<<<<<<[HELIX]-------------- */
#include "threads/vaddr.h"
#include "lib/kernel/bitmap.h"

#define DISK_SECTOR_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)
#define NOT_SWAPPED ((size_t) - 1)

static struct bitmap *swap_bitmap;
/* --------------[HELIX]>>>>>>>>>>>>>> */

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
	if (swap_disk == NULL){
		PANIC("no swap disk");
	}
	size_t swap_bit_cnt = (disk_size(swap_disk) / DISK_SECTOR_PER_PAGE);

	swap_bitmap = bitmap_create(swap_bit_cnt);
	if (swap_bitmap == NULL){
		PANIC("no swap bitmap but swap disk");
	}
	/* --------------[HELIX]>>>>>>>>>>>>>> */
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
    anon_page->swap_slot = NOT_SWAPPED;
    return true;
	/* --------------[HELIX]>>>>>>>>>>>>>> */
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	if (anon_page->swap_slot == NOT_SWAPPED){
		memset(kva, 0, PGSIZE);
		return true;
	}
	
	size_t target_slot = anon_page->swap_slot;

	for (size_t i = 0; i < DISK_SECTOR_PER_PAGE; i++){
		disk_read(swap_disk, 
				  target_slot * DISK_SECTOR_PER_PAGE + i,
				  (uint8_t *)kva + i * DISK_SECTOR_PER_PAGE);
	}

	bitmap_reset(swap_bitmap, target_slot);
	anon_page->swap_slot = NOT_SWAPPED;
	/* --------------[HELIX]>>>>>>>>>>>>>> */
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	size_t target_slot = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
	if (target_slot = BITMAP_ERROR){
		return false;
	}
	
	void *kva = page->frame->kva;

	for (size_t i = 0; i < DISK_SECTOR_PER_PAGE; i++){
		disk_write(swap_disk, 
				  target_slot * DISK_SECTOR_PER_PAGE + i,
				  (uint8_t *)kva + i * DISK_SECTOR_PER_PAGE);
	}

	anon_page->swap_slot = target_slot;
	page->frame = NULL;

	return true;
	/* --------------[HELIX]>>>>>>>>>>>>>> */
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	/* <<<<<<<<<<<<<<[HELIX]-------------- */
	if (anon_page->swap_slot != NOT_SWAPPED){
		bitmap_reset(swap_bitmap, anon_page->swap_slot);
	}
	/* --------------[HELIX]>>>>>>>>>>>>>> */
}
