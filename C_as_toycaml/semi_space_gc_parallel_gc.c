#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include "runtime.h"
#include <pthread.h>

#define MAX_STATIC_ROOTS 1024
#define IS_HEAP_PTR(v) (((v) & 1) == 0 && (v) != 0)
#define MAX_THREADS 128
#define NUM_GC_THREADS 4

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
    *header_ptr = (total_words << 10) | (tag & 0xFF) | (gc_scan_idle_phys & GC_SCAN_RESP_BIT);
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

/* Word offset from to_heap during collection; use integer fetch_add (not _Atomic(value*) fetch_add). */
static _Atomic(intptr_t) free_ptr_atomic;
static atomic_int gc_copy_in_progress;

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
                value header_clean = header & ~GC_HDR_PHYS_MASK;
                value tot_words = header_clean >> 10;
                size_t bytes_sz = tot_words * sizeof(value);

                atomic_fetch_add(&gc_copy_in_progress, 1);
                intptr_t w = atomic_fetch_add(&free_ptr_atomic, (intptr_t)tot_words);
                value* new_v = to_heap + w;
                if ((uintptr_t)(new_v + tot_words) > ((uintptr_t)to_heap + heap_sz)) {
                    fprintf(stderr, "FATAL: Out of Memory during GC while GC it self????.\n");
                    exit(1);
                }
                _Atomic(value)* new_hdr = (_Atomic(value)*)new_v;
                atomic_store(new_hdr, header_clean | GC_HDR_COPYING);
                if (bytes_sz > sizeof(value)) {
                    memcpy(new_v + 1, full_v + 1, bytes_sz - sizeof(value));
                }
                atomic_store(atomic_slot_ptr, (value)(new_v + 1));
                atomic_store(new_hdr,
                    (header_clean & ~GC_SCAN_RESP_BIT) | (gc_scan_idle_phys & GC_SCAN_RESP_BIT));
                atomic_store(atomic_header_ptr, 0);
                atomic_fetch_add(&gc_copy_in_progress, -1);
                return (value)(new_v + 1);
            }
        }
    }
}

void* run_semi_space(void* arg){
    int w = (int)(intptr_t)arg;

    int total_static_roots = atomic_load(&static_root_count);
    int s0 = (w * total_static_roots) / NUM_GC_THREADS;
    int s1 = ((w + 1) * total_static_roots) / NUM_GC_THREADS;
    for (int i = s0; i < s1; i++) {
        value* root = (value*)static_roots[i];
        *(root) = copy(*root);
    }

    int total_threads = atomic_load(&registered_threads);
    int t0 = (w * total_threads) / NUM_GC_THREADS;
    int t1 = ((w + 1) * total_threads) / NUM_GC_THREADS;
    for (int i = t0; i < t1; i++) {
        if (!atomic_load(&registry[i].is_active)) continue;
        long** retval_ptr = registry[i].gc_retval_ptr;
        if (*retval_ptr != NULL) {
            *retval_ptr = (long*)copy((value)*retval_ptr);
        }

        long*** thread_stack = registry[i].root_stack_ptr;

        long idx = *(registry[i].stack_idx_ptr);
        for (long j = 0; j < idx; j++) {
            *thread_stack[j] = (long*)copy((value)*thread_stack[j]);
        }
    }

    return NULL;
}

void* cheney_bfs(void* arg) {
    (void)arg;
    value* scan = to_heap;

    for (;;) {
        value* f = to_heap + atomic_load(&free_ptr_atomic);
        if ((uintptr_t)scan >= (uintptr_t)f) {
            f = to_heap + atomic_load(&free_ptr_atomic);
            if ((uintptr_t)scan >= (uintptr_t)f
                && atomic_load(&gc_copy_in_progress) == 0)
                return NULL;
            continue;
        }

        _Atomic(value)* ah = (_Atomic(value)*)scan;
        value h = atomic_load(ah);
        value base = h & ~GC_HDR_PHYS_MASK;
        long tot = (long)(base >> 10);
        if (tot <= 0) {
            scan += 1;
            continue;
        }

        if ((h & GC_HDR_COPYING) || gc_scan_resp_is_busy(h)) {
            scan += tot;
            continue;
        }

        value want = (h & ~GC_SCAN_RESP_BIT) | (gc_scan_busy_phys & GC_SCAN_RESP_BIT);
        if (!atomic_compare_exchange_weak(ah, &h, want)) {
            long skip = (long)((h & ~GC_HDR_PHYS_MASK) >> 10);
            scan += skip > 0 ? skip : 1;
            continue;
        }

        for (long j = 1; j < tot; j++) {
            if (IS_HEAP_PTR(scan[j]))
                scan[j] = copy(scan[j]);
        }
        /* Header left at busy resp pattern so lagging workers skip; next cycle swap makes it idle. */
        scan += tot;
    }
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

    atomic_store(&gc_copy_in_progress, 0);

    pthread_t bfs_workers[NUM_GC_THREADS];
    for (int w = 0; w < NUM_GC_THREADS; w++) {
        if (pthread_create(&bfs_workers[w], NULL, cheney_bfs, (void*)(intptr_t)w) != 0) {
            perror("Failed to spawn cheney bfs thread");
            exit(1);
        }
    }
    for (int w = 0; w < NUM_GC_THREADS; w++) {
        pthread_join(bfs_workers[w], NULL);
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
