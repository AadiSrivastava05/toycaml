#include<stdio.h>
#include<stdlib.h>

#define HEAP_SIZE 1024

#define Field(ptr, offset) ((long*)ptr)[offset]
#define toycaml_return(x) toycaml_return_handler();return(x)
#define toycaml_frame toycaml_new_frame()

#define long2val(x) ((x<<1)+1)
#define val2long(x) (x>>1)


void init_heap();

long* caml_alloc(long len, long tag);

void make_static_root(long** ptr);

void make_root(long** ptr);

void toycaml_return_handler();

void toycaml_new_frame();
