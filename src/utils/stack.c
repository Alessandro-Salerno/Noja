#include <stdint.h>
#include <stdlib.h>
#include "stack.h"
#include "defs.h"

struct xStack {
	unsigned int size, 
				 used;
	int 		 refs;
	void 		*body[];
};

_Bool Stack_IsReadOnlyCopy(Stack *s)
{
	return (uintptr_t) s & (uintptr_t) 1;
}

void *Stack_New(int size)
{
	if(size < 0)
		size = 1024;

	Stack *s = malloc(sizeof(Stack) + sizeof(void*) * size);

	if(s == NULL)
		return NULL;

	assert((intptr_t) s % 8 == 0);

	s->size = size;
	s->used = 0;
	s->refs = 1;
	return s;
}

static Stack *unmark(Stack *s)
{
	return (Stack*) ((intptr_t) s & ~ (intptr_t) 1);
}

void *Stack_Top(Stack *s, int n)
{
	assert(n <= 0);

	// Remove readonly bit.
	s = unmark(s);

	if(s->used == 0)
		return NULL;

	if((int) s->used + n - 1 < 0)
		return NULL;

	return s->body[s->used + n - 1];
}

_Bool Stack_Pop(Stack *s, unsigned int n)
{
	if(Stack_IsReadOnlyCopy(s))
		return 0;

	if(s->used < n)
		return 0;

	s->used -= n;
	return 1;
}

_Bool Stack_Push(Stack *s, void *item)
{
	assert(s != NULL);
	assert(item != NULL);

	if(Stack_IsReadOnlyCopy(s))
		return 0;

	if(s->used == s->size)
		return 0;

	s->body[s->used] = item;
	s->used += 1;
	return 1;
}

Stack *Stack_Copy(Stack *s, _Bool readonly)
{
	if(Stack_IsReadOnlyCopy(s))
		{
			// Reference is readonly,
			// so the copy must be
			// readonly.
			readonly = 1;

			// Remove readonly bit.
			s = unmark(s);
		}

	s->refs += 1;
	
	if(readonly)
		return (Stack*) ((uintptr_t) s | (intptr_t) 1);
	else
		return s;
}

void Stack_Free(Stack *s)
{
	// Remove readonly bit.
	s = unmark(s);

	s->refs -= 1;
	assert(s->refs >= 0);

	if(s->refs == 0)
		free(s);
}

unsigned int Stack_Size(Stack *s)
{
	// Remove readonly bit.
	s = unmark(s);

	return s->used;
}

unsigned int Stack_Capacity(Stack *s)
{
	// Remove readonly bit.
	s = unmark(s);

	return s->size;
}