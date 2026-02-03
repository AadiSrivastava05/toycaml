#ifndef MMTK_H
#define MMTK_H

#include <stddef.h>
#include <sys/types.h>

// The extern "C" is only required if the runtime
// implementation language is C++
// extern "C" {

// An arbitrary address
typedef void* Address;
// MmtkMutator should be an opaque pointer for the VM
typedef void* MmtkMutator;
// An opaque pointer to a VMThread
typedef void* VMThread;

/**
 * Initialize MMTk instance
 */
void mmtk_init();

/**
 * Set the heap size
 *
 * @param min minimum heap size
 * @param max maximum heap size
 */
void mmtk_set_heap_size(size_t min, size_t max);

// } // extern "C"

/**
 * Bind a mutator thread in MMTk
 *
 * @param tls pointer to mutator thread
 * @return an instance of an MMTk mutator
 */
MmtkMutator mmtk_bind_mutator(VMThread tls);
// requires a 1-to-1 mapping for storing the mutator info

/**
 * Allocate an object
 *
 * @param mutator the mutator instance that is requesting the allocation
 * @param size the size of the requested object
 * @param align the alignment requirement for the object
 * @param offset the allocation offset for the object
 * @param allocator the allocation semantics to use for the allocation
 * @return the address of the newly allocated object
 */
void *mmtk_alloc(MmtkMutator mutator, size_t size, size_t align,
        ssize_t offset, int allocator);

/**
 * Set relevant object metadata
 *
 * @param mutator the mutator instance that is requesting the allocation
 * @param object the ObjectReference address chosen by the VM binding
 * @param size the size of the allocated object
 * @param allocator the allocation semantics to use for the allocation
 */
void mmtk_post_alloc(MmtkMutator mutator, void* object, size_t size, int allocator);

#endif // MMTK_H
