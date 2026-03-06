#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "runtime.h"

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
    mutator = mutator = mmtk_bind_mutator((void*)(uintptr_t)pthread_self());

    void (*user_function)(void) = (void (*)(void))func_ptr;
    user_function();

    mmtk_destroy_mutator(mutator);

    atomic_fetch_sub(&num_threads, 1);

    if (wants_to_stop() && atomic_load(&num_stopped) == atomic_load(&num_threads)) {
        world_has_stopped();
    }

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
    
    atomic_fetch_sub(&num_threads, 1);
    if (wants_to_stop() && atomic_load(&num_stopped) == atomic_load(&num_threads)) {
        extern void world_has_stopped(void);
        world_has_stopped();
    }
    pthread_join(pid, NULL);
    atomic_fetch_add(&num_threads, 1); // because sleeping thread will break stop the world logic if included in total threads

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

    // long *result = (long *)mmtk_alloc(mutator, (len + 1) * sizeof(long), sizeof(long), 0, semantics);
    long *result = (long *)mmtk_alloc(mutator, (len + 2) * sizeof(long), sizeof(long), 0, semantics); // extra slots for header and forwarding pointer

    mmtk_post_alloc(mutator, result, (len+2) * sizeof(long), tag, semantics);
    long *obj_body = result + 2;
    for (int i = 0; i < len; i++) {
        obj_body[i] = 1; // Correctly fills fields 0 to len-1
    }
    return obj_body;
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
