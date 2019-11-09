#include <pml.h>
#include <stdio.h>
#include <assert.h>

unsigned count = 0u;

void defer_cb(struct pml_defer* d) {
	printf("nested iteration %d\n", count);
	++count;
	struct pml* ml = pml_defer_get_pml(d);
	if(count == 2) {
		pml_defer_destroy(d);
	}

	// finish the current iteration
	pml_iterate(ml, false);

	// start a new iteration that should trigger the defer source again
	pml_iterate(ml, false);
}

int main() {
	struct pml* pml = pml_new();
	pml_defer_new(pml, defer_cb);

	pml_iterate(pml, true);
	pml_destroy(pml);
	assert(count == 2);
}

