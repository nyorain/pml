#include <pml.h>
#include <stdio.h>
#include <assert.h>

unsigned count = 0u;

void defer_cb(struct pml_defer* d) {
	printf("nested iteration %d\n", count);
	++count;
	if(count < 10) {
		// finish the current iteration
		pml_iterate(pml_defer_get_pml(d), true);

		// start a new iteration that should trigger the defer source again
		pml_iterate(pml_defer_get_pml(d), true);
	}
}

int main() {
	struct pml* pml = pml_new();
	pml_defer_new(pml, defer_cb);

	pml_iterate(pml, true);
	pml_destroy(pml);
	assert(count == 10);
}
