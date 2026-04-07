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

extern STW_State stw_state;

value* to_heap;
value* from_heap;
value heap_sz;
_Atomic(value) cur_heap_ptr;

void* static_roots[MAX_STATIC_ROOTS];
atomic_int static_root_count = 0;


typedef struct {
    long ***root_stack_ptr; // Pointer to the thread's root_stack
    long *stack_idx_ptr;    // Pointer to the thread's stack_idx
    long **gc_retval_ptr;   // Pointer to the thread's return value
    atomic_bool is_active;  // if we can scan the stack or not
    _Atomic(value) page_cursor;
    _Atomic(value) page_end;
    _Atomic(value) page_limit;
} ThreadRegistry;

ThreadRegistry registry[MAX_THREADS];
atomic_int registered_threads = 0; // id for mutators taken from counter

void* semi_space_collection(void*);

static void clamp_all_active_pages(void) {
    int total_threads = atomic_load(&registered_threads);
    for (int i = 0; i < total_threads; i++) {
        if (!atomic_load(&registry[i].is_active)) continue;
        value cursor = atomic_load(&registry[i].page_cursor);
        atomic_store(&registry[i].page_limit, cursor);
    }
}

static void reset_all_thread_pages(void) {
    int total_threads = atomic_load(&registered_threads);
    for (int i = 0; i < total_threads; i++) {
        atomic_store(&registry[i].page_cursor, 0);
        atomic_store(&registry[i].page_end, 0);
        atomic_store(&registry[i].page_limit, 0);
    }
}

static bool reserve_thread_page(int id, value request_bytes) {
    value preferred = PAGE_BYTES;
    if (request_bytes > preferred) preferred = request_bytes;

    while (1) {
        value old_ptr = atomic_load(&cur_heap_ptr);
        if (old_ptr >= heap_sz) return false;

        value remaining = heap_sz - old_ptr;
        if (remaining < request_bytes) return false;

        value chunk = preferred;
        if (chunk > remaining) chunk = remaining;

        value new_ptr = old_ptr + chunk;
        if (atomic_compare_exchange_weak(&cur_heap_ptr, &old_ptr, new_ptr)) {
            atomic_store(&registry[id].page_cursor, old_ptr);
            atomic_store(&registry[id].page_end, old_ptr + chunk);
            atomic_store(&registry[id].page_limit, old_ptr + chunk);
            return true;
        }
    }
}

static void* try_alloc_from_thread_page(int id, value request_bytes) {
    value cursor = atomic_load(&registry[id].page_cursor);
    value end = atomic_load(&registry[id].page_end);
    value limit = atomic_load(&registry[id].page_limit);

    if (cursor > end || end > limit) return NULL;
    if (request_bytes > (end - cursor)) return NULL;
    if (request_bytes > (limit - cursor)) return NULL;

    atomic_store(&registry[id].page_cursor, cursor + request_bytes);
    return (void*)((uintptr_t)from_heap + (uintptr_t)cursor);
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

    atomic_store(&cur_heap_ptr, 0);
}

// Request MMTk to create a new mutator for the given `tls` thread
MMTk_Mutator mmtk_bind_mutator(void* tls) {
    int id = atomic_fetch_add(&registered_threads, 1);
    if (id >= MAX_THREADS) {
        fprintf(stderr, "FATAL: Exceeded MAX_THREADS\n");
        exit(1);
    }
        
    // We need to capture the addresses of the __thread variables
    extern __thread long **root_stack[ROOT_STACK_SIZE];
    extern __thread long stack_idx;
    extern __thread long* gc_retval;
    
    registry[id].gc_retval_ptr = &gc_retval;
    registry[id].root_stack_ptr = (long***)root_stack;
    registry[id].stack_idx_ptr = &stack_idx;
    atomic_store(&registry[id].is_active, true);
    atomic_store(&registry[id].page_cursor, 0);
    atomic_store(&registry[id].page_end, 0);
    atomic_store(&registry[id].page_limit, 0);
    
    return (MMTk_Mutator)(intptr_t)id; 
}

// Reclaim mutator that is no longer needed
void mmtk_destroy_mutator(MMTk_Mutator mutator) {
    int id = (int)(intptr_t)mutator;
    atomic_store(&registry[id].is_active, false);
    atomic_store(&registry[id].page_cursor, 0);
    atomic_store(&registry[id].page_end, 0);
    atomic_store(&registry[id].page_limit, 0);
}


// Allocate memory for an object
void* mmtk_alloc(MMTk_Mutator mutator, size_t size, size_t align, size_t offset, int allocator) {
    int id = (int)(intptr_t)mutator;
    value request_bytes = (value)size;

    void* fast_ptr = try_alloc_from_thread_page(id, request_bytes);
    if (fast_ptr != NULL) return fast_ptr;

    if (reserve_thread_page(id, request_bytes)) {
        fast_ptr = try_alloc_from_thread_page(id, request_bytes);
        if (fast_ptr != NULL) return fast_ptr;
    }

    pthread_mutex_lock(&stw_state.lock);
    if(stw_state.gc_requested){
        stw_state.num_stopped++;

        if(stw_state.num_stopped == stw_state.num_threads){
            stw_state.world_has_stopped = true;
            pthread_cond_broadcast(&stw_state.world_stopped);
        }

        while(stw_state.gc_requested){
            pthread_cond_wait(&stw_state.gc_off, &stw_state.lock);
        }

        stw_state.num_stopped--;
        if(stw_state.num_stopped == 0){
            stw_state.world_has_stopped = false;
            pthread_cond_broadcast(&stw_state.resume_world);
        }
        else{
            while(stw_state.world_has_stopped){
                pthread_cond_wait(&stw_state.resume_world, &stw_state.lock);
            }
        }

        if (reserve_thread_page(id, request_bytes)) {
            fast_ptr = try_alloc_from_thread_page(id, request_bytes);
            if (fast_ptr != NULL) {
                pthread_mutex_unlock(&stw_state.lock);
                return fast_ptr;
            }
        }
    }

    if (!reserve_thread_page(id, request_bytes)) {
        // printf("\n[GC] I am requesting GC.\n");
        fprintf(stderr, "[GC] Allocation failed, requesting collection\n");
        stw_state.num_stopped++;
        stw_state.gc_requested = true;
        stw_state.world_has_stopped = false;
        clamp_all_active_pages();
        
        if(stw_state.num_stopped == stw_state.num_threads){
            stw_state.world_has_stopped = true;
        }
        
        // printf("\n[GC] I requested GC, now I will wait for world to stop.\n");
        while(!stw_state.world_has_stopped){
            pthread_cond_wait(&stw_state.world_stopped, &stw_state.lock);
        }
        
        // printf("\n[GC] I requested GC, now world has stopped. I will initiate GC thread.\n");
        pthread_t pid;
        if (pthread_create(&pid, NULL, (semi_space_collection), NULL) != 0){
            perror("Failed to spawn gc thread");
            exit(1);
        }
        
        
        // printf("\n[GC] I requested GC, initiated GC thread, now will wait for GC thread to join.\n");
        pthread_join(pid, NULL);
        // printf("\n[GC] I requested GC, initiated GC thread, GC thread ran and completed. I can now wake up the threads to get into resuming state.\n");
        stw_state.gc_requested = false;
        pthread_cond_broadcast(&stw_state.gc_off);
        
        // printf("\n[GC] I requested GC, now I will wait for resuming.\n");
        stw_state.num_stopped--;
        if(stw_state.num_stopped == 0){
            stw_state.world_has_stopped = false;
            pthread_cond_broadcast(&stw_state.resume_world);
        }
        else{
            while(stw_state.world_has_stopped){
                pthread_cond_wait(&stw_state.resume_world, &stw_state.lock);
            }
        }
        // printf("\n[GC] I requested GC, and I resumed.\n");
        
        // check if we survived OOM
        if (!reserve_thread_page(id, request_bytes)) {
            stw_state.num_threads--;
            pthread_mutex_unlock(&stw_state.lock);
            fprintf(stderr, "FATAL: Out of Memory after GC while allocating.\n");
            exit(1); 
        }
        
        
    }
    
    fast_ptr = try_alloc_from_thread_page(id, request_bytes);
    if (fast_ptr == NULL) {
        stw_state.num_threads--;
        pthread_mutex_unlock(&stw_state.lock);
        fprintf(stderr, "FATAL: Out of Memory after GC while allocating.\n");
        exit(1); 
    }

    pthread_mutex_unlock(&stw_state.lock);
    return fast_ptr;
}

// perform post-allocation hooks or actions such as initializing object metadata
void mmtk_post_alloc(MMTk_Mutator mutator,
                            void* refer,
                            int bytes,
                            int tag,
                            int allocator)
{
    value* header_ptr = (value*)refer;
    // store total words (including header)
    long total_words = bytes / sizeof(value);
    *header_ptr = (total_words << 10) | (tag & 0x3FF); 
}

// return the current amount of used memory in bytes
size_t mmtk_used_bytes(){
    return (size_t)atomic_load(&cur_heap_ptr);
}

// return the current amount of free memory in bytes
size_t mmtk_free_bytes(){
    value used = atomic_load(&cur_heap_ptr);
    return (size_t)(heap_sz - used);
}

// return the current amount of total memory in bytes
size_t mmtk_total_bytes(){
    return (heap_sz);
}

_Atomic(value*) free_ptr_atomic = 0;

value copy(value v){
    // is it a pointer?
    if(!IS_HEAP_PTR(v)) return v;

    // is it actually IN the from_heap?
    if(v < (value)from_heap || v >= (value)from_heap + heap_sz) return v;

    value* full_v = ((value*)v - 1);
    
    // casting the header location to an atomic pointer
    _Atomic(value)* atomic_header_ptr = (_Atomic(value)*)full_v;
    _Atomic(value)* atomic_slot_ptr = (_Atomic(value)*)(full_v+1);

    // fprintf(stderr, "[GC] Copying object at: %zu\n", (size_t)full_v);
    // fprintf(stderr, "[GC] Copying object header: %zu\n", (size_t)atomic_load(atomic_header_ptr));

    while(1){
        value header = atomic_load(atomic_header_ptr);
        if (header == 0) {
            // already forwarded
            return atomic_load(atomic_slot_ptr);
        }
        else if(header == -1) {
            // some other thread is copying this object, we can spin and wait
            continue;
        }
        else {
            // attempt to claim this object by setting header to -1
            if(atomic_compare_exchange_weak(atomic_header_ptr, &header, -1)) {
                value tot_words = header >> 10;
                size_t bytes_sz = tot_words * sizeof(value);

                // copy to the actual free_ptr location
                // value* new_v = (value*)atomic_fetch_add(&free_ptr_atomic, tot_words);
                value* old_free = atomic_load(&free_ptr_atomic);
                value* new_free;
                do {
                    new_free = old_free + tot_words;
                } while (!atomic_compare_exchange_weak(&free_ptr_atomic, &old_free, new_free));

                value* new_v = old_free;
                if ((uintptr_t)(new_v + tot_words) > ((uintptr_t)to_heap + heap_sz)) {
                    fprintf(stderr, "FATAL: Out of Memory during GC while GC it self????.\n");
                    exit(1);
                }
                memcpy(new_v, full_v, bytes_sz);
                new_v[0] = header; // copy header as well because we changed it to -1 to claim, we need to restore it for the new location
                atomic_store(atomic_slot_ptr, (value)(new_v + 1)); // store the to-space pointer in the old location
                atomic_store(atomic_header_ptr, 0); // mark as forwarded with valid to-space pointer
                return (value)(new_v + 1);
            }
        }
    }
}


atomic_int picked_static_roots = 0;
atomic_int picked_local_roots = 0;
_Atomic(value*) picked_scan_ptr = 0;


void* run_semi_space(void*){
    // static/global root scans
    int i = atomic_fetch_add(&picked_static_roots, 1);
    int total_static_roots = atomic_load(&static_root_count);
    while(i < total_static_roots){
        value* root = (value*)static_roots[i];
        *(root) = copy(*root);
        i = atomic_fetch_add(&picked_static_roots, 1);
    }

    // local root scans
    int total_threads = atomic_load(&registered_threads);
    i = atomic_fetch_add(&picked_local_roots, 1);
    while(i < total_threads){
        if (!atomic_load(&registry[i].is_active)) {
            i = atomic_fetch_add(&picked_local_roots, 1);
            continue; // skipping dead threads
        }
        long** retval_ptr = registry[i].gc_retval_ptr;
        if (*retval_ptr != NULL) {
            *retval_ptr = (long*)copy((value)*retval_ptr);
        }

        long*** thread_stack = registry[i].root_stack_ptr; 
        
        long idx = *(registry[i].stack_idx_ptr);
        for (long j = 0; j < idx; j++) {
            // thread_stack[j] is a long** (a pointer to your local variable)
            *thread_stack[j] = (long*)copy((value)*thread_stack[j]);
        }
        i = atomic_fetch_add(&picked_local_roots, 1);
    }

    return NULL;

}

void* semi_space_collection(void*){
    printf("\n[GC] Collection Started. Used bytes: %zu\n", (size_t)atomic_load(&cur_heap_ptr));

    atomic_store(&picked_static_roots, 0);
    atomic_store(&picked_local_roots, 0);
    // picked_static_roots = 0;
    // picked_local_roots = 0;
    // picked_scan_ptr = to_heap;
    atomic_store(&free_ptr_atomic, (value*)to_heap);
    // free_ptr_atomic = to_heap;

    // fprintf(stderr, "[GC] The freeptr is at: %zu\n", (size_t)atomic_load(&free_ptr_atomic));


    pthread_t pid1, pid2, pid3;
    if (pthread_create(&pid1, NULL, (run_semi_space), NULL) != 0){
        perror("Failed to spawn gc thread");
        exit(1);
    }
    if (pthread_create(&pid2, NULL, (run_semi_space), NULL) != 0){
        perror("Failed to spawn gc thread");
        exit(1);
    }
    if (pthread_create(&pid3, NULL, (run_semi_space), NULL) != 0){
        perror("Failed to spawn gc thread");
        exit(1);
    }

    pthread_join(pid1, NULL);
    pthread_join(pid2, NULL);
    pthread_join(pid3, NULL);

    // fprintf(stderr, "[GC] Root copying phase completed\n");

    
    // bfs over active ptrs from roots
    value* scan_ptr = to_heap;
    // fprintf(stderr, "[GC] Starting graph scanning phase\nThe freeptr is at: %zu\nThe scan ptr is at: %zu\n", (size_t)atomic_load(&free_ptr_atomic), (size_t)scan_ptr);
    
    while (scan_ptr < (value*)atomic_load(&free_ptr_atomic)) {
        // printf("\n[GC] Scanning object at: %zu\n", (size_t)scan_ptr);
        value* obj = scan_ptr;
        long total_words = obj[0] >> 10;
        // printf("[GC] Object has %zu words\n", total_words);
        
        for (long j = 1; j < total_words; j++) {
            if (IS_HEAP_PTR(obj[j])) {
                obj[j] = copy(obj[j]);
            }
        }
        scan_ptr += total_words;
    }

    // fprintf(stderr, "[GC] Graph scanning phase completed\n");


    value* temp = from_heap;
    from_heap = to_heap;
    to_heap = temp;

    
    value* free_ptr = (value*)(size_t)atomic_load(&free_ptr_atomic);

    atomic_store(&cur_heap_ptr, (value)((free_ptr - from_heap) * sizeof(value)));
    reset_all_thread_pages();
    printf("[GC] Collection Finished. Used bytes after compaction: %zu\n", (size_t)atomic_load(&cur_heap_ptr));

    return NULL;
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
