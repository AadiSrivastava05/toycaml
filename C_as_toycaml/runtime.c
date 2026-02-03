#include<stdio.h>
#include<stdlib.h>
#include "mmtk-toycaml/toycaml/mmtk.h"

#define HEAP_SIZE 1024
#define MIN_ALIGNMENT 2 // since we are just dealing with pointers and integers, we only need one bit here like in Ocaml (hopefully)

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

MmtkMutator global_mutator;

long* get_stack_ptr(){
    long* sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp)); // might not be portable to all ISAs
    return sp;
}

void init_heap(){
    heap_ptr = (long*)malloc(HEAP_SIZE*(sizeof(long)));
    limit_ptr = heap_ptr + HEAP_SIZE;

    // global_mutator = mmtk_bind_mutator(NULL); // can be used as the thread starting function.

    stack_idx = 0;
    static_stack_idx = 0;

    current_frame = 0;
}

long* caml_alloc(long len, long tag){
    long* result;

    if(tag>=256){
        printf("Error in the tag.\n");
        exit(1);
    }

    if(heap_ptr + (len+1) < limit_ptr){
        *(heap_ptr) = ((len<<10) + (tag));

        result = heap_ptr+1;

        heap_ptr += (len+1);

        for(int i = 0 ; i<len ; i++){
            Field(result, i) = 1;
        }
    }
    else{
        printf("Could not allocate, heap will overflow!\n");
        exit(1);
    }

    // version for incorporating MMTk (will uncomment after Isfarul setsup the rust side)

    // size_t bytes_sz = (len + 1) * sizeof(long);

    // currently passing 0 for offset and 0 for default allocator
    // void* raw_mem = mmtk_alloc(global_mutator, bytes_sz, MIN_ALIGNMENT, 0, 0);

    // if (raw_mem == NULL) {
    //     printf("MMTk could not allocate memory!\n");
    //     exit(1);
    // }

    // adding the header part
    // long* header_ptr = (long*)raw_mem;
    // *header_ptr = ((len << 10) + tag);

    // ObjectReference starts after header (my understanding)
    // long* result = header_ptr + 1;

    // Now result has the "object"
    // mmtk_post_alloc(global_mutator, result, bytes_sz, 0);

    // for(int i = 0; i < len; i++) {
    //     Field(result, i) = 1; 
    // }


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
