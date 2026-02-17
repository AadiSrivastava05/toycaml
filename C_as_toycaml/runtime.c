#include<stdio.h>
#include<stdlib.h>
#include "mmtk-bindings/include/mmtk.h"
#include "runtime.h"

#define Field(ptr, offset) ((long*)ptr)[offset]
#define toycaml_return(x) toycaml_return_handler();return(x)
#define toycaml_frame toycaml_new_frame()

#define long2val(x) ((x<<1)+1)
#define val2long(x) (x>>1)

long* heap_ptr;
long* limit_ptr;

long** root_stack[HEAP_SIZE];
long stack_idx;

long** static_root_stack[HEAP_SIZE];
long static_stack_idx;


long current_frame_stack_sz[HEAP_SIZE];
long current_frame;

MMTk_Mutator mutator;

long* get_stack_ptr(){
    long* sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp)); // might not be portable to all ISAs
    return sp;
}

void init_heap(){
    mmtk_init(HEAP_SIZE * sizeof(long), "immix");

    /* TODO(Aadi): Mutator counter */
    // Create a default mutator, for initial thread
    mutator = mmtk_bind_mutator(NULL);

    heap_ptr = (long*)malloc(HEAP_SIZE*(sizeof(long)));
    limit_ptr = heap_ptr + HEAP_SIZE;

    stack_idx = 0;
    static_stack_idx = 0;

    current_frame = 0;
}

long* caml_alloc(long len, long tag){
    if(tag>=256){
        printf("Error in the tag.\n");
        exit(1);
    }

    int offset = 0;
    int semantics = 0; /* 0 = AllocationSemantics::Default */

    long *result = (long *)mmtk_alloc(mutator, (len + 1) * sizeof(long), sizeof(long), 0, semantics);

    mmtk_post_alloc(mutator, result, len * sizeof(long), tag, semantics);
    for(int i = 1 ; i <= len; i++) {
        Field(result, i) = 1;
    }

    return result;
}

void make_static_root(long** ptr_to_var){
    static_root_stack[static_stack_idx++] = ptr_to_var;
    return;
}

void make_root(long** ptr_to_var){
    root_stack[stack_idx++] = ptr_to_var;
    current_frame_stack_sz[current_frame]++;
    return;
}

void toycaml_return_handler(){
    stack_idx -= (current_frame_stack_sz[current_frame]);
    current_frame--;
}

void toycaml_new_frame(){
    current_frame++;
    current_frame_stack_sz[current_frame] = 0;
}
