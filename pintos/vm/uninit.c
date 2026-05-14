/* uninit.c: Implementation of uninitialized page.
 *
 * All of the pages are born as uninit page. When the first page fault occurs,
 * the handler chain calls uninit_initialize (page->operations.swap_in).
 * The uninit_initialize function transmutes the page into the specific page
 * object (anon, file, page_cache), by initializing the page object,and calls
 * initialization callback that passed from vm_alloc_page_with_initializer
 * function.
 * */

#include "vm/vm.h"
#include "vm/uninit.h"

static bool uninit_initialize (struct page *page, void *kva);
static void uninit_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations uninit_ops = {
	.swap_in = uninit_initialize,
	.swap_out = NULL,
	.destroy = uninit_destroy,
	.type = VM_UNINIT,
};

/* DO NOT MODIFY this function */
void
uninit_new (struct page *page, void *va, vm_initializer *init,
            enum vm_type type, void *aux,
            bool (*initializer) (struct page *, enum vm_type, void *)) {
	ASSERT (page != NULL);

	*page = (struct page) {
		.operations = &uninit_ops,
		.va = va,
		.frame = NULL, /* no frame for now */
		.uninit = (struct uninit_page) {
		        .init = init,
		        .type = type,
		        .aux = aux,
		        .page_initializer = initializer,
		}
	};
}

/* Initalize the page on first fault */
static bool
uninit_initialize (struct page *page, void *kva) {
	// 현재는 타입이 uninit이니까 type 자체를 uninit_page로 가져왔고,
	// 그 값은 page->uninit에 있는거 그대로 가져오기
	struct uninit_page *uninit = &page->uninit;
	/* Fetch first, page_initialize may overwrite the values */
	// 처음으로 실행할 init 함수.
	// 내 기준으로 말하면 lazy_load_segment가 됨.
	vm_initializer *init = uninit->init;
	// args
	void *aux = uninit->aux;

	// uninit->page_initializer에서 실행될 때
	// page와 aux값을 날려버릴 수도 있지 않을까?
	// 필요하다면 순서를 바꿔야 한다고 함

	// anon_initializer()
	// file_backed_initializer()

	/* TODO: You may need to fix this function. */
	return uninit->page_initializer (page, uninit->type, kva) &&
	       (init ? init (page, aux) : true);
}

/* Free the resources hold by uninit_page. Although most of pages are transmuted
 * to other page objects, it is possible to have uninit pages when the process
 * exit, which are never referenced during the execution.
 * PAGE will be freed by the caller. */
/* uninit_page가 보유한 리소스를 해제합니다. 대부분의 페이지는 다른 페이지 객체로 변환되지만,
 * 프로세스가 종료될 때 실행 중에 참조되지 않는 미초기 페이지가 남아 있을 수 있습니다.
 * PAGE는 호출자에 의해 해제됩니다. */
static void
uninit_destroy (struct page *page) {
	struct uninit_page *uninit UNUSED = &page->uninit;
	/* TODO: Fill this function.
	 * TODO: If you don't have anything to do, just return. */
}
