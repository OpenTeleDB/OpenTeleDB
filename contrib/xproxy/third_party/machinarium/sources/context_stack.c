
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * machinarium.
 *
 * cooperative multitasking engine.
 */

#include <stdlib.h>
#include <machinarium.h>
#include <machinarium_private.h>

#ifdef HAVE_VALGRIND
#include <valgrind/valgrind.h>
#endif

int mm_contextstack_create(mm_contextstack_t *stack, size_t size,
			   size_t size_guard)
{
	static int pagesize = 0;
	char *base;

	if (pagesize == 0)
		pagesize = sysconf(_SC_PAGE_SIZE);
#if 0
	base = mmap(0, size_guard + size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return -1;
#else
	base = aligned_alloc(pagesize, size_guard + size);
	if (base == NULL)
		return -1;
#endif
	mprotect(base, size_guard, PROT_NONE);
	base += size_guard;
	stack->pointer = base;
	stack->size = size;
	stack->size_guard = size_guard;
#ifdef HAVE_VALGRIND
	stack->valgrind_stack = VALGRIND_STACK_REGISTER(
		stack->pointer, stack->pointer + stack->size);
#endif
	return 0;
}

void mm_contextstack_free(mm_contextstack_t *stack)
{
	if (stack->pointer == NULL)
		return;
#ifdef HAVE_VALGRIND
	VALGRIND_STACK_DEREGISTER(stack->valgrind_stack);
#endif
	char *base = stack->pointer - stack->size_guard;
#if 0
	munmap(base, stack->size_guard + stack->size);
#else
	mprotect(base, stack->size_guard, PROT_READ | PROT_WRITE);
	free(base);
#endif
}
