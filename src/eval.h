#include "utils/error.h"
#include "objects/objects.h"
Object *eval(const char *str, int len, Object *closure, Heap *heap, Error *error);