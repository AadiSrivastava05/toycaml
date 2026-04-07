#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "runtime.h"

__thread long **root_stack[ROOT_STACK_SIZE];
__thread long stack_idx;

__thread long current_frame_stack_sz[ROOT_STACK_SIZE];
__thread long current_frame = 0;

__thread long* gc_retval = NULL;

STW_State stw_state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .gc_off = PTHREAD_COND_INITIALIZER,
    .world_stopped = PTHREAD_COND_INITIALIZER,
    .resume_world = PTHREAD_COND_INITIALIZER,
    .num_threads = 1, // Main thread starts as 1
    .num_stopped = 0,
    .gc_requested = false,
    .world_has_stopped = false
};

/* Thread-local mutator context */
__thread MMTk_Mutator mutator;

void *thread_entry_point(void *func_ptr)
{
    // poll_for_gc();
    mutator = mmtk_bind_mutator((void*)(uintptr_t)pthread_self());
    // poll_for_gc();

    void (*user_function)(void) = (void (*)(void))func_ptr;
    user_function();
    // poll_for_gc();

    mmtk_destroy_mutator(mutator);

    pthread_mutex_lock(&stw_state.lock);
    stw_state.num_threads--;
    if (stw_state.gc_requested && stw_state.num_stopped == stw_state.num_threads) {
        stw_state.world_has_stopped = true;
        pthread_cond_broadcast(&stw_state.world_stopped);
    }
    pthread_mutex_unlock(&stw_state.lock);
    // poll_for_gc();

    return NULL;
}

pthread_t domain_spawn(void *function)
{
    pthread_t pid;

    poll_for_gc();
    pthread_mutex_lock(&stw_state.lock);
    stw_state.num_threads++;
    pthread_mutex_unlock(&stw_state.lock);
    
    if (pthread_create(&pid, NULL, thread_entry_point, function) != 0)
    {
        pthread_mutex_lock(&stw_state.lock);
        stw_state.num_threads--;
        pthread_mutex_unlock(&stw_state.lock);
        perror("Failed to spawn");
        exit(1);
    }

    poll_for_gc();

    return pid;
}

void domain_join(pthread_t pid)
{
    
    poll_for_gc();
    pthread_mutex_lock(&stw_state.lock);
    stw_state.num_threads--; // because sleeping thread will break stop the world logic if included in total threads
    if (stw_state.gc_requested && stw_state.num_stopped == stw_state.num_threads) {
        stw_state.world_has_stopped = true; 
        pthread_cond_broadcast(&stw_state.world_stopped);
    }
    pthread_mutex_unlock(&stw_state.lock);
    
    pthread_join(pid, NULL);
    pthread_mutex_lock(&stw_state.lock);
    stw_state.num_threads++;
    pthread_mutex_unlock(&stw_state.lock);
    
    poll_for_gc();


}

long *get_stack_ptr()
{
    long *sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp)); // might not be portable to all ISAs
    return sp;
}

void init_heap()
{
    mmtk_init(HEAP_SIZE * sizeof(long), "immix");
    mutator = mmtk_bind_mutator(NULL);
}

long *caml_alloc(long len, long tag)
{
    if(len <= 0){
        fprintf(stderr, "FATAL: Attempting to allocate object with zero or negative length.\n");
        exit(1);
    }
    
    int offset = 0;
    int semantics = 0; /* 0 = AllocationSemantics::Default */

    poll_for_gc();
    
    long *result = (long *)mmtk_alloc(mutator, (len + 1) * sizeof(long), sizeof(long), 0, semantics); // extra slot for header

    mmtk_post_alloc(mutator, result, (len+1) * sizeof(long), tag, semantics);
    long *obj_body = result + 1;
    for (int i = 0; i < len; i++) {
        obj_body[i] = 1; 
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

void make_return_root(long **ptr_to_var)
{
    gc_retval = *ptr_to_var;
    return;
}

void toycaml_new_frame()
{
    current_frame++;
    current_frame_stack_sz[current_frame] = 0;
}

void toycaml_return_handler()
{
    toycaml_return_with_val(NULL);
}

long* toycaml_return_with_val(long* val) {
    stack_idx -= (current_frame_stack_sz[current_frame]);
    current_frame--;
    poll_for_gc();
    long* ret = gc_retval;
    gc_retval = NULL;
    return ret ? ret : val;
}

void poll_for_gc(){
    pthread_mutex_lock(&stw_state.lock);
    if(stw_state.gc_requested){
        // printf("\n[GC] GC Requested. I am stopping\n");
        stw_state.num_stopped++;
        
        if(stw_state.num_stopped == stw_state.num_threads){
            stw_state.world_has_stopped = true;
            pthread_cond_broadcast(&stw_state.world_stopped);
        }
        
        while(stw_state.gc_requested){
            pthread_cond_wait(&stw_state.gc_off, &stw_state.lock);
        }
        
        // printf("\n[GC] GC Ended. I am waiting to resume\n");
        stw_state.num_stopped--;
        if(stw_state.num_stopped == 0){
            // printf("\n[GC] GC Ended. And I am the last to leave, so everyone can resume now.\n");
            stw_state.world_has_stopped = false;
            pthread_cond_broadcast(&stw_state.resume_world);
        }
        else{
            while(stw_state.world_has_stopped){
                pthread_cond_wait(&stw_state.resume_world, &stw_state.lock);
            }
            // printf("\n[GC] World can resume. I am now resuming\n");
        }
        // printf("\n[GC] Now I resumed.\n");
    }
    pthread_mutex_unlock(&stw_state.lock);
}
