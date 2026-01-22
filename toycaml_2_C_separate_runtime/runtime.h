#include<stdio.h>
#include<stdlib.h>

#define Field(ptr, offset) ((volatile long*)ptr)[offset]

#define long2val(x) ((x<<1)+1)
#define val2long(x) (x>>1)

void init_heap();

long* caml_alloc(long len, long tag);
