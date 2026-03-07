#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include <pthread.h>

#define MAX_STATIC_ROOTS 1024
#define IS_HEAP_PTR(v) (((v) & 1) == 0 && (v) != 0)
#define MAX_THREADS 128

typedef void* MMTk_Mutator;

typedef intptr_t value;

value* to_heap;
value* from_heap;
value heap_sz;
atomic_size_t cur_heap_ptr;

pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

atomic_flag gc_thread_spawned = ATOMIC_FLAG_INIT;

void* static_roots[MAX_STATIC_ROOTS];
atomic_int static_root_count = 0;


typedef struct {
    long ***root_stack_ptr; // Pointer to the thread's root_stack
    long *stack_idx_ptr;    // Pointer to the thread's stack_idx
    atomic_bool is_active;  // if we can scan the stack or not
} ThreadRegistry;

ThreadRegistry registry[MAX_THREADS];
atomic_int registered_threads = 0; // id for mutators taken from counter

static atomic_bool gc_requested = false;
static atomic_bool world_is_ready = false;
static atomic_bool is_gc_active = false; 

void* semi_space_collection(void*);

void world_has_stopped(){
    atomic_store(&world_is_ready, true);
}

bool wants_to_stop(){
    return atomic_load(&gc_requested);
}

// Initialize an MMTk instance
void mmtk_init(uint32_t heap_size, char *plan){
    // ignoring plan, for now just semi space
    heap_sz = heap_size;

    from_heap = (value*)(malloc((heap_size)));
    to_heap = (value*)(malloc((heap_size)));

    if(from_heap==NULL || to_heap==NULL){
        printf("Heap init allocation failed.");
        exit(1);
    }

    cur_heap_ptr = 0;
}

// Request MMTk to create a new mutator for the given `tls` thread
MMTk_Mutator mmtk_bind_mutator(void* tls) {
    int id = atomic_fetch_add(&registered_threads, 1);
    
    // We need to capture the addresses of the __thread variables
    extern __thread long **root_stack[HEAP_SIZE];
    extern __thread long stack_idx;
    
    registry[id].root_stack_ptr = (long***)root_stack;
    registry[id].stack_idx_ptr = &stack_idx;
    atomic_store(&registry[id].is_active, true);
    
    return (MMTk_Mutator)(intptr_t)id; 
}

// Reclaim mutator that is no longer needed
void mmtk_destroy_mutator(MMTk_Mutator mutator) {
    int id = (int)(intptr_t)mutator;
    atomic_store(&registry[id].is_active, false);
}


// Allocate memory for an object
void* mmtk_alloc(MMTk_Mutator mutator, size_t size, size_t align, size_t offset, int allocator) {

    if (cur_heap_ptr + size > heap_sz) {
        
        while (atomic_load(&num_stopped) > 0);
        
        atomic_store(&gc_requested, true);
        
        if(!atomic_flag_test_and_set(&gc_thread_spawned)){
            pthread_t pid;
            if (pthread_create(&pid, NULL, (semi_space_collection), NULL) != 0){
                perror("Failed to spawn gc thread");
                exit(1);
            }
            poll_for_gc();
            pthread_join(pid, NULL);
            atomic_flag_clear(&gc_thread_spawned);
        }

        poll_for_gc();

        
        // check if we survived OOM
        if (cur_heap_ptr + size > heap_sz) {
            fprintf(stderr, "FATAL: Out of Memory after GC.\n");
            exit(1); 
        }
        
        
    }
    
    size_t old_ptr = atomic_fetch_add(&cur_heap_ptr, size);
    if (old_ptr > heap_sz) {
        atomic_fetch_add(&cur_heap_ptr, -size);
        fprintf(stderr, "FATAL: Out of Memory after GC.\n");
        exit(1); 
    }
    
    void* ptr = (void*)((uintptr_t)from_heap + old_ptr);
    return ptr;
}

// perform post-allocation hooks or actions such as initializing object metadata
void mmtk_post_alloc(MMTk_Mutator mutator,
                            void* refer,
                            int bytes,
                            int tag,
                            int allocator)
{
    value* header_ptr = (value*)refer;
    // store total words (including header and forwarding slot)
    long total_words = bytes / sizeof(value);
    *header_ptr = (total_words << 10) | (tag & 0x3FF); 
    
    *(header_ptr + 1) = 0;
}

// return the current amount of used memory in bytes
size_t mmtk_used_bytes(){
    return cur_heap_ptr;
}

// return the current amount of free memory in bytes
size_t mmtk_free_bytes(){
    return (heap_sz-cur_heap_ptr);
}

// return the current amount of total memory in bytes
size_t mmtk_total_bytes(){
    return (heap_sz);
}

value copy(value v, value** free_ptr_ref){
    // is it a pointer?
    if(!IS_HEAP_PTR(v)) return v;

    // is it actually IN the from_heap?
    if(v < (value)from_heap || v >= (value)from_heap + heap_sz) return v;

    value* full_v = (value*)v - 2;

    // already forwarded?
    if(full_v[1] != 0){
        return (value)((value*)full_v[1] + 2); 
    }

    value header = full_v[0];
    value tot_words = header >> 10;
    size_t bytes_sz = tot_words * sizeof(value);

    // copy to the actual free_ptr location
    value* new_v = *free_ptr_ref;
    memcpy(new_v, full_v, bytes_sz);
    
    // update the caller's free_ptr
    *free_ptr_ref += tot_words;

    // leave forwarding address
    full_v[1] = (value)new_v;

    return (value)(new_v + 2);
}

void* semi_space_collection(void*){
    while (!atomic_load(&world_is_ready)) {
            // Spinning
    }
    printf("\n[GC] Collection Started. Used bytes: %zu\n", cur_heap_ptr);
    value* free_ptr = to_heap;
    value* scan_ptr = to_heap;

    // static/global root scans
    for(int i = 0 ; i < static_root_count ; i++){
        value* root = (value*)static_roots[i];
        *(root) = copy(*root, &free_ptr);
    }
    
    // local root scans
    int total_threads = atomic_load(&registered_threads);
    for (int i = 0; i < total_threads; i++) {
        if (!atomic_load(&registry[i].is_active)) continue; // skipping dead threads

        long*** thread_stack = registry[i].root_stack_ptr; 
        
        long idx = *(registry[i].stack_idx_ptr);
        for (long j = 0; j < idx; j++) {
            // thread_stack[j] is a long** (a pointer to your local variable)
            *thread_stack[j] = (long*)copy((value)*thread_stack[j], &free_ptr);
        }
    }

    // bfs over active ptrs from roots
    while (scan_ptr < free_ptr) {
        value* obj = scan_ptr;
        long total_words = obj[0] >> 10;
        
        for (long i = 2; i < total_words; i++) {
            if (IS_HEAP_PTR(obj[i])) {
                obj[i] = copy(obj[i], &free_ptr);
            }
        }
        scan_ptr += total_words;
    }

    value* temp = from_heap;
    from_heap = to_heap;
    to_heap = temp;

    cur_heap_ptr = (free_ptr - from_heap) * sizeof(value);
    printf("[GC] Collection Finished. Used bytes after compaction: %zu\n", cur_heap_ptr);
    atomic_store(&gc_requested, false);
    atomic_store(&world_is_ready, false);
}

void mmtk_register_global_root(void *ref) {
    int idx = atomic_fetch_add(&static_root_count, 1);
    
    if (idx < MAX_STATIC_ROOTS) {
        static_roots[idx] = ref;
    } else {
        printf("ERROR: Exceeded MAX_STATIC_ROOTS (%d)\n", MAX_STATIC_ROOTS);
        exit(1);
    }
}
