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
value cur_heap_ptr;

pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;


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

void semi_space_collection(void);

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
    pthread_mutex_lock(&alloc_lock);

    if (cur_heap_ptr + size > heap_sz) {
        // Elect a GC Leader. The first thread to flip this to 'true' wins.
        if (!atomic_exchange(&is_gc_active, true)) {
            
            // GC LEADER
            pthread_mutex_unlock(&alloc_lock); 

            atomic_store(&gc_requested, true); 

            // the leader must count itself as stopped, 
            // otherwise num_stopped will never equal num_threads!
            atomic_fetch_add(&num_stopped, 1);

            if (atomic_load(&num_stopped) == atomic_load(&num_threads)) {
                world_has_stopped();
            }

            // wait for all mutators (followers) to arrive at the safepoint
            while (!atomic_load(&world_is_ready)) {
                // Spinning
            }

            // Run the actual garbage collection
            semi_space_collection();

            // Release the mutators
            atomic_store(&world_is_ready, false);
            atomic_store(&gc_requested, false);
            
            // Leader removes itself from the stopped count
            atomic_fetch_sub(&num_stopped, 1);
            
            // Open for future GCs
            atomic_store(&is_gc_active, false);
            
            pthread_mutex_lock(&alloc_lock);

            // check if we survived OOM
            if (cur_heap_ptr + size > heap_sz) {
                pthread_mutex_unlock(&alloc_lock);
                fprintf(stderr, "FATAL: Out of Memory after GC (Leader).\n");
                exit(1); 
            }

        } else {
            // Someone else is already running the GC
            pthread_mutex_unlock(&alloc_lock);
            
            // Wait to ensure the leader has flipped gc_requested to true
            while (!atomic_load(&gc_requested));
            
            // Join the safepoint and sleep exactly like normal threads
            poll_for_gc();
            
            // Wake up and re-acquire lock to attempt allocation again
            pthread_mutex_lock(&alloc_lock);
            
            // Check if the leader managed to free enough space for me
            if (cur_heap_ptr + size > heap_sz) {
                pthread_mutex_unlock(&alloc_lock);
                fprintf(stderr, "FATAL: Out of Memory after GC (Follower).\n");
                exit(1);
            }
        }
    }
    
    // Actual bump-pointer allocation (Guaranteed space at this line)
    void* ptr = (void*)((uintptr_t)from_heap + cur_heap_ptr);
    cur_heap_ptr += size;
    
    pthread_mutex_unlock(&alloc_lock);
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

void semi_space_collection(){
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
