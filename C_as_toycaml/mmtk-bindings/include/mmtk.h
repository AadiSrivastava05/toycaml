// This is an example of native API for the single instance MMTk.

// Note: the mmtk core does not directly provide this API. However, it provides
// a similar multi-instance Rust API.  A VM binding should write their own C
// header file (possibly based on this example with their own extension and
// modification), and expose the Rust API based on their native API.

#ifndef MMTK_H
#define MMTK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* MMTk_Mutator;

extern void world_has_stopped();

extern bool wants_to_stop();

// Initialize an MMTk instance
extern void mmtk_init(uint32_t heap_size, char *plan);

// Request MMTk to create a new mutator for the given `tls` thread
extern MMTk_Mutator mmtk_bind_mutator(void* tls);

// Reclaim mutator that is no longer needed
extern void mmtk_destroy_mutator(MMTk_Mutator mutator);

// Allocate memory for an object
extern void* mmtk_alloc(MMTk_Mutator mutator,
                        size_t size,
                        size_t align,
                        size_t offset,
                        int allocator);

// Perform post-allocation hooks or actions such as initializing object metadata
extern void mmtk_post_alloc(MMTk_Mutator mutator,
                            void* refer,
                            int bytes,
                            int tag,
                            int allocator);

// Run the main loop for a GC worker. Does not return
extern void mmtk_start_worker(void* tls, void* worker);

// Initialize MMTk scheduler and GC workers
extern void mmtk_initialize_collection(void* tls);

// Return the current amount of used memory in bytes
extern size_t mmtk_used_bytes();

// Return the current amount of free memory in bytes
extern size_t mmtk_free_bytes();

// Return the current amount of total memory in bytes
extern size_t mmtk_total_bytes();

// Return if the object pointed to by `object` is live
extern bool mmtk_is_live_object(void* object);

// Return if object pointed to by `object` will never move
extern bool mmtk_will_never_move(void* object);

// Return if the address pointed to by `addr` is in memory that is mapped by MMTk
extern bool mmtk_is_mapped_address(void* addr);

// Request MMTk to trigger a GC. Note that this may not actually trigger a GC
extern int mmtk_handle_user_collection_request(void* tls);

// Add a reference to the list of weak references
extern void mmtk_add_weak_candidate(void* ref);

// Add a reference to the list of soft references
extern void mmtk_add_soft_candidate(void* ref);

// Generic hook to allow benchmarks to be harnessed
extern void mmtk_harness_begin(void* tls);

// Generic hook to allow benchmarks to be harnessed
extern void mmtk_harness_end();

extern void mmtk_register_global_root(void *ref);
#ifdef __cplusplus
}
#endif

#endif  // MMTK_H
