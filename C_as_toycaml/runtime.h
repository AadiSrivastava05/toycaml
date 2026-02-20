#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "mmtk-bindings/include/mmtk.h"

#define HEAP_SIZE 8192
#define MIN_ALIGNMENT 2

/* OCaml-style field access and tagging */
#define Field(ptr, offset) ((long *)ptr)[offset]
#define long2val(x) ((x << 1) + 1)
#define val2long(x) (x >> 1)

/* Safepoint and return handling */
#define toycaml_return(x)           \
    do {                            \
        toycaml_return_handler();   \
        return (x);                 \
    } while (0)
#define toycaml_frame toycaml_new_frame()

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

/* Adds frame specific roots */
void make_root(long** ptr);

/* Frame handler */
void toycaml_new_frame();

/* Poll for GC */
void poll_for_gc();
