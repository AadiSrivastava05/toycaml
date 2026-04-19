#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "runtime.h"
#include <pthread.h>

#define MAX_STATIC_ROOTS 1024
#define IS_HEAP_PTR(v) (((v) & 1) == 0 && (v) != 0)
#define MAX_THREADS 128
#ifndef NUM_GC_THREADS
#define NUM_GC_THREADS 4
#endif

typedef void* MMTk_Mutator;
typedef intptr_t value;

/* Bit 8: COPYING only while installing into to-space (cleared when copy finishes; no swap).
   Bit 9: scan responsibility — physical idle vs busy swap each collection so survivors’
   “done” bit pattern becomes next cycle’s idle without a second heap pass. */
#define GC_HDR_COPYING   ((value)(1 << 8))
#define GC_SCAN_RESP_BIT ((value)(1 << 9))
#define GC_HDR_PHYS_MASK (GC_HDR_COPYING | GC_SCAN_RESP_BIT)

static value gc_scan_idle_phys = 0;
static value gc_scan_busy_phys = GC_SCAN_RESP_BIT;

static void gc_swap_scan_resp_meaning_after_collection(void) {
    value t = gc_scan_idle_phys;
    gc_scan_idle_phys = gc_scan_busy_phys;
    gc_scan_busy_phys = t;
}

static bool gc_scan_resp_is_busy(value h) {
    return ((h ^ gc_scan_idle_phys) & GC_SCAN_RESP_BIT) != 0;
}

extern STW_State stw_state;

value* to_heap;
value* from_heap;
value heap_sz;
_Atomic(value) cur_heap_ptr;

void* static_roots[MAX_STATIC_ROOTS];
atomic_int static_root_count = 0;

typedef struct {
    long ***root_stack_ptr;
    long *stack_idx_ptr;   
    long **gc_retval_ptr;  
    atomic_bool is_active; 
    _Atomic(value) page_cursor;
    _Atomic(value) page_end;
    _Atomic(value) page_limit;
} ThreadRegistry;

ThreadRegistry registry[MAX_THREADS];
atomic_int registered_threads = 0;

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

void mmtk_init(uint32_t heap_size, char *plan){
    heap_sz = heap_size;
    from_heap = (value*)(malloc((heap_size)));
    to_heap = (value*)(malloc((heap_size)));

    if(from_heap==NULL || to_heap==NULL){
        printf("Heap init allocation failed.");
        exit(1);
    }
    atomic_store(&cur_heap_ptr, 0);
}

MMTk_Mutator mmtk_bind_mutator(void* tls) {
    int id = atomic_fetch_add(&registered_threads, 1);
    if (id >= MAX_THREADS) {
        fprintf(stderr, "FATAL: Exceeded MAX_THREADS\n");
        exit(1);
    }
        
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

void mmtk_destroy_mutator(MMTk_Mutator mutator) {
    int id = (int)(intptr_t)mutator;
    atomic_store(&registry[id].is_active, false);
    atomic_store(&registry[id].page_cursor, 0);
    atomic_store(&registry[id].page_end, 0);
    atomic_store(&registry[id].page_limit, 0);
}

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
        fprintf(stderr, "[GC] Allocation failed, requesting collection\n");
        stw_state.num_stopped++;
        stw_state.gc_requested = true;
        stw_state.world_has_stopped = false;
        clamp_all_active_pages();
        
        if(stw_state.num_stopped == stw_state.num_threads){
            stw_state.world_has_stopped = true;
        }
        
        while(!stw_state.world_has_stopped){
            pthread_cond_wait(&stw_state.world_stopped, &stw_state.lock);
        }
        
        pthread_t pid;
        if (pthread_create(&pid, NULL, (semi_space_collection), NULL) != 0){
            perror("Failed to spawn gc thread");
            exit(1);
        }
        
        pthread_join(pid, NULL);
        stw_state.gc_requested = false;
        pthread_cond_broadcast(&stw_state.gc_off);
        
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

void mmtk_post_alloc(MMTk_Mutator mutator, void* refer, int bytes, int tag, int allocator) {
    value* header_ptr = (value*)refer;
    long total_words = bytes / sizeof(value);
    *header_ptr = (total_words << 10) | (tag & 0xFF) | (gc_scan_idle_phys & GC_SCAN_RESP_BIT);
}

size_t mmtk_used_bytes(){
    return (size_t)atomic_load(&cur_heap_ptr);
}

size_t mmtk_free_bytes(){
    value used = atomic_load(&cur_heap_ptr);
    return (size_t)(heap_sz - used);
}

size_t mmtk_total_bytes(){
    return (heap_sz);
}

static _Atomic(intptr_t) free_ptr_atomic;

// Thread-local state for GC workers
typedef struct {
    value** stack;
    size_t stack_top;
    size_t stack_capacity;
    value gclab_cursor;
    value gclab_limit;
} GCWorkerState;

void push_worker_stack(GCWorkerState* worker, value* obj) {
    if (worker->stack_top >= worker->stack_capacity) {
        worker->stack_capacity *= 2;
        worker->stack = realloc(worker->stack, worker->stack_capacity * sizeof(value*));
        if (!worker->stack) {
            fprintf(stderr, "FATAL: OOM resizing GC worker stack.\n");
            exit(1);
        }
    }
    worker->stack[worker->stack_top++] = obj;
}

value copy(value v, GCWorkerState* worker){
    if(!IS_HEAP_PTR(v)) return v;
    if(v < (value)from_heap || v >= (value)from_heap + heap_sz) return v;

    value* full_v = ((value*)v - 1);
    _Atomic(value)* atomic_header_ptr = (_Atomic(value)*)full_v;
    _Atomic(value)* atomic_slot_ptr = (_Atomic(value)*)(full_v+1);

    while(1){
        value header = atomic_load_explicit(atomic_header_ptr, memory_order_acquire);
        
        if (header == 0) {
            return atomic_load_explicit(atomic_slot_ptr, memory_order_relaxed);
        }
        else if(header == -1) {
            #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause(); 
            #endif
            continue;
        }
        else {
            if(atomic_compare_exchange_weak_explicit(atomic_header_ptr, &header, -1, 
                                                     memory_order_acquire, memory_order_relaxed)) {
                
                value header_clean = header & ~GC_HDR_PHYS_MASK;
                value tot_words = header_clean >> 10;
                size_t bytes_sz = tot_words * sizeof(value);

                // --- GCLAB Allocation ---
                if (worker->gclab_cursor + tot_words > worker->gclab_limit) {
                    value req = 4096; // Request a 4KB chunk of words at a time
                    if (tot_words > req) req = tot_words;
                    worker->gclab_cursor = atomic_fetch_add(&free_ptr_atomic, req);
                    worker->gclab_limit = worker->gclab_cursor + req;
                    
                    if ((uintptr_t)(to_heap + worker->gclab_limit) > ((uintptr_t)to_heap + heap_sz)) {
                        fprintf(stderr, "FATAL: Out of Memory during GC while allocating in to_space.\n");
                        exit(1);
                    }
                }
                
                value* new_v = to_heap + worker->gclab_cursor;
                worker->gclab_cursor += tot_words;

                _Atomic(value)* new_hdr = (_Atomic(value)*)new_v;
                
                atomic_store_explicit(new_hdr, header_clean | GC_HDR_COPYING, memory_order_relaxed);
                if (bytes_sz > sizeof(value)) {
                    memcpy(new_v + 1, full_v + 1, bytes_sz - sizeof(value));
                }
                atomic_store_explicit(atomic_slot_ptr, (value)(new_v + 1), memory_order_relaxed);
                
                atomic_store_explicit(new_hdr,
                    (header_clean & ~GC_SCAN_RESP_BIT) | (gc_scan_idle_phys & GC_SCAN_RESP_BIT),
                    memory_order_release);
                    
                atomic_store_explicit(atomic_header_ptr, 0, memory_order_release);
                
                // Immediately push the newly copied object to our local DFS stack
                push_worker_stack(worker, (value*)(new_v + 1));
                
                return (value)(new_v + 1);
            }
        }
    }
}

void* run_semi_space(void* arg){
    int w = (int)(intptr_t)arg;
    
    // Initialize Local DFS Stack and GCLAB bounds
    GCWorkerState worker;
    worker.stack_capacity = 16384; 
    worker.stack = malloc(worker.stack_capacity * sizeof(value*));
    worker.stack_top = 0;
    worker.gclab_cursor = 0;
    worker.gclab_limit = 0;

    int total_static_roots = atomic_load(&static_root_count);
    int s0 = (w * total_static_roots) / NUM_GC_THREADS;
    int s1 = ((w + 1) * total_static_roots) / NUM_GC_THREADS;
    for (int i = s0; i < s1; i++) {
        value* root = (value*)static_roots[i];
        *(root) = copy(*root, &worker);
    }

    int total_threads = atomic_load(&registered_threads);
    int t0 = (w * total_threads) / NUM_GC_THREADS;
    int t1 = ((w + 1) * total_threads) / NUM_GC_THREADS;
    for (int i = t0; i < t1; i++) {
        if (!atomic_load(&registry[i].is_active)) continue;
        long** retval_ptr = registry[i].gc_retval_ptr;
        if (*retval_ptr != NULL) {
            *retval_ptr = (long*)copy((value)*retval_ptr, &worker);
        }

        long*** thread_stack = registry[i].root_stack_ptr;
        long idx = *(registry[i].stack_idx_ptr);
        for (long j = 0; j < idx; j++) {
            *thread_stack[j] = (long*)copy((value)*thread_stack[j], &worker);
        }
    }

    // DFS Scanning Loop
    while (worker.stack_top > 0) {
        value* obj = worker.stack[--worker.stack_top];
        value* full_obj = obj - 1; 
        
        value h = *full_obj; 
        long tot = (long)((h & ~GC_HDR_PHYS_MASK) >> 10);
        
        // obj points directly to the data fields (index 0 is the first data word)
        for (long j = 0; j < tot - 1; j++) {
            if (IS_HEAP_PTR(obj[j])) {
                obj[j] = copy(obj[j], &worker);
            }
        }
    }

    free(worker.stack);
    return NULL;
}

void* semi_space_collection(void*){
    printf("\n[GC] Collection Started. Used bytes: %zu\n", (size_t)atomic_load(&cur_heap_ptr));

    atomic_store(&free_ptr_atomic, 0);

    pthread_t gc_workers[NUM_GC_THREADS];
    for (int w = 0; w < NUM_GC_THREADS; w++) {
        if (pthread_create(&gc_workers[w], NULL, run_semi_space, (void*)(intptr_t)w) != 0) {
            perror("Failed to spawn gc thread");
            exit(1);
        }
    }
    
    for (int w = 0; w < NUM_GC_THREADS; w++) {
        pthread_join(gc_workers[w], NULL);
    }

    value* temp = from_heap;
    from_heap = to_heap;
    to_heap = temp;

    value* free_ptr = from_heap + atomic_load(&free_ptr_atomic);

    atomic_store(&cur_heap_ptr, (value)((free_ptr - from_heap) * sizeof(value)));
    reset_all_thread_pages();
    printf("[GC] Collection Finished. Used bytes after compaction: %zu\n", (size_t)atomic_load(&cur_heap_ptr));

    gc_swap_scan_resp_meaning_after_collection();
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