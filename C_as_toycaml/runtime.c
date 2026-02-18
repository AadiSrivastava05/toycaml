#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "mmtk-bindings/include/mmtk.h"
#include "runtime.h"

long **root_stack[HEAP_SIZE];
long stack_idx;

long current_frame_stack_sz[HEAP_SIZE];
long current_frame;

long num_threads;
pthread_mutex_t num_threads_lock;

long num_stopped;

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

    pthread_mutex_lock(&num_threads_lock);

    if (pthread_create(&pid, NULL, thread_entry_point, function) != 0)
    {
        perror("Failed to spawn");
        exit(1);
    }
    num_threads += 1;

    pthread_mutex_unlock(&num_threads_lock);

    return pid;
}

void domain_join(pthread_t pid)
{

    pthread_mutex_lock(&num_threads_lock);
    pthread_join(pid, NULL);
    num_threads -= 1;
    pthread_mutex_unlock(&num_threads_lock);
}

long *get_stack_ptr()
{
    long *sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp)); // might not be portable to all ISAs
    return sp;
}

void init_heap()
{
    pthread_mutex_init(&num_threads_lock, NULL);
    num_threads = 1;
    num_stopped = 0;

    mmtk_init(HEAP_SIZE * sizeof(long), "immix");
    mutator = mmtk_bind_mutator(NULL);
}

long *caml_alloc(long len, long tag)
{
    if (tag >= 256)
    {
        printf("Error in the tag.\n");
        exit(1);
    }

    int offset = 0;
    int semantics = 0; /* 0 = AllocationSemantics::Default */

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
    mmtk_make_static_root(ptr_to_var);
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
    if (wants_to_stop())
    {
        pthread_mutex_lock(&num_threads_lock);
        num_stopped++;
        pthread_mutex_unlock(&num_threads_lock);
        while (wants_to_stop())
        {
            pthread_mutex_lock(&num_threads_lock);
            if (num_stopped == num_threads)
            {
                world_has_stopped();
            }
            pthread_mutex_unlock(&num_threads_lock);
        }
        pthread_mutex_lock(&num_threads_lock);
        num_stopped--;
        pthread_mutex_unlock(&num_threads_lock);
    }
}
