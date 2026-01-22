#include<stdio.h>
#include<stdlib.h>

#define Field(ptr, offset) ((long*)ptr)[offset]

#define long2val(x) ((x<<1)+1)
#define val2long(x) (x>>1)

long* heap_ptr;
long* limit_ptr;

long* root_stack[1024];
long stack_idx;

void init_heap(){
    heap_ptr = (long*)malloc(1024*(sizeof(long)));
    limit_ptr = heap_ptr + 1024;

    stack_idx = 0;
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

    root_stack[stack_idx++] = result;
    return result;
}
