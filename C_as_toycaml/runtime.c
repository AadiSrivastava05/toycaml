#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "runtime.h"
#include "mmtk-bindings/include/mmtk.h"

__thread long **root_stack[HEAP_SIZE];
__thread long stack_idx;

__thread long current_frame_stack_sz[HEAP_SIZE];
__thread long current_frame = 0;

atomic_long num_threads;

atomic_long num_stopped;

/* Thread-local mutator context */
__thread MMTk_Mutator mutator;

void *thread_entry_point(void *func_ptr)
{
    mutator = mmtk_bind_mutator(NULL);

    void (*user_function)(void) = (void (*)(void))func_ptr;
    user_function();

    return NULL;
}

pthread_t domain_spawn(void *function)
{
    pthread_t pid;

    atomic_fetch_add(&num_threads, 1);

    if (pthread_create(&pid, NULL, thread_entry_point, function) != 0)
    {
        atomic_fetch_sub(&num_threads, 1);
        perror("Failed to spawn");
        exit(1);
    }

    return pid;
}

void domain_join(pthread_t pid)
{

    pthread_join(pid, NULL);
    atomic_fetch_sub(&num_threads, 1);

}

long *get_stack_ptr()
{
    long *sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp)); // might not be portable to all ISAs
    return sp;
}

void init_heap()
{
    atomic_init(&num_threads, 1);
    atomic_init(&num_stopped, 0);

    mmtk_init(HEAP_SIZE * sizeof(long), "immix");
    mutator = mmtk_bind_mutator(NULL);
}

long *caml_alloc(long len, long tag)
{
    int offset = 0;
    int semantics = 0; /* 0 = AllocationSemantics::Default */

    poll_for_gc();

    long *result = (long *)mmtk_alloc(mutator, (len + 1) * sizeof(long), sizeof(long), 0, semantics);

    mmtk_post_alloc(mutator, result, len * sizeof(long), tag, semantics);
    for (int i = 1; i <= len; i++)
    {
        Field(result, i) = 1;
    }

    return result;
}

void make_static_root(long **ptr_to_var)
{
    mmtk_register_global_root(ptr_to_var);
    return;
}

void make_root(long **ptr_to_var)
{
    root_stack[stack_idx++] = ptr_to_var;
    current_frame_stack_sz[current_frame]++;
    return;
}

void toycaml_new_frame()
{
    current_frame++;
    current_frame_stack_sz[current_frame] = 0;
}

void toycaml_return_handler()
{
    stack_idx -= (current_frame_stack_sz[current_frame]);
    current_frame--;
    poll_for_gc();
}

void poll_for_gc(){
    if (wants_to_stop())
    {
        atomic_fetch_add(&num_stopped, 1);
        while (wants_to_stop())
        {
            if (atomic_load(&num_stopped) == atomic_load(&num_threads))
            {
                world_has_stopped();
            }
        }
        atomic_fetch_sub(&num_stopped, 1);
    }
}
