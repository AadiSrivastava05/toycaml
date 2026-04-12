#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "mmtk-bindings/include/mmtk.h"

#define HEAP_SIZE (4096 * 1024 * 32)
#define MIN_ALIGNMENT 2
#define PAGE_BYTES 4096
#define ROOT_STACK_SIZE (1 << 20)

/* OCaml-style field access and tagging */
#define Field(ptr, offset) ((long *)ptr)[offset] 
#define long2val(x) ((x << 1) + 1)
#define val2long(x) (x >> 1)

/* return handling */
#define toycaml_return(x)           \
    return (typeof(x))toycaml_return_with_val((long*)(x))
#define toycaml_frame toycaml_new_frame()

// below are for semi_space_gc.c
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t gc_off;
    pthread_cond_t world_stopped;
    pthread_cond_t resume_world;
    long num_threads;
    long num_stopped;
    bool gc_requested;
    bool world_has_stopped;
} STW_State;

/* Internal thread wrapper to bind mutator */
void *thread_entry_point(void *func_ptr);

/* Spawns a domain and increments thread count */
pthread_t domain_spawn(void *function);

/* Joins a domain and decrements thread count */
void domain_join(pthread_t pid);

/* Captures current RSP */
long *get_stack_ptr();

/* Initializes MMTk and main mutator */
void init_heap();

/* Allocates object with tag and initializes fields */
long *caml_alloc(long len, long tag);

/* Registers a static pointer as a GC root */
void make_static_root(long **ptr_to_var);

/* Blocks thread if GC is requested (STW) */
void toycaml_return_handler();

/* Blocks thread if GC is requested, returns updated pointer if GC moved it */
long* toycaml_return_with_val(long* val);

/* Adds frame specific roots */
void make_root(long** ptr);

/* Adds return value root */
void make_return_root(long** ptr);

/* Frame handler */
void toycaml_new_frame();

/* Poll for GC */
void poll_for_gc();
