#include <stdio.h>
#include <stdlib.h>
#include "../runtime.h"

#define TAG_NODE 0
#define TAG_ARRAY 0
#define EMPTY (long*)NULL


long* mk() {
    toycaml_frame;
    long* arr = caml_alloc(128, TAG_ARRAY);
    for (int i = 0; i < 128; i++) {
        Field(arr, i) = long2val(0); 
    }
    make_return_root(&arr);
    toycaml_return(arr);
}

long* make(int d) {
    toycaml_frame;
    if (d == 0) {
        long* data = mk();
        make_root(&data);
        
        long* node = caml_alloc(3, TAG_NODE);
        Field(node, 0) = (long)EMPTY;
        Field(node, 1) = (long)data;
        Field(node, 2) = (long)EMPTY;
        make_return_root(&node);
        toycaml_return(node);
    } 
    else {
        long* left = make(d - 1);
        make_root(&left);
        long* data = mk();
        make_root(&data);
        long* right = make(d - 1);
        make_root(&right);
        
        long* node = caml_alloc(3, TAG_NODE);
        Field(node, 0) = (long)left;
        Field(node, 1) = (long)data;
        Field(node, 2) = (long)right;
        make_return_root(&node);
        toycaml_return(node);
    }
}

int check(long* tree) {
    if (tree == EMPTY) {
        return 0;
    }
    return 1 + check((long*)Field(tree, 0)) + check((long*)Field(tree, 2));
}

int g_min_depth = 4;
int g_max_depth;
int g_stretch_depth;

void worker_thread(void) {
    toycaml_frame;

    // Stretch Tree
    {
        toycaml_frame;
        long* stretch_tree = make(g_stretch_depth);
        make_root(&stretch_tree);
        
        int c = check(stretch_tree);
        printf("[Thread] stretch tree depth %i\t check: %i\n", g_stretch_depth, c);
        toycaml_return_handler(); 
    }

    // Long Lived Tree
    // Note: We use local make_root instead of make_static_root to avoid 
    // global array race conditions across multiple threads.
    long* long_lived_tree = make(g_max_depth);
    make_root(&long_lived_tree);

    // Loop Depths
    for (int d = g_min_depth; d <= g_max_depth; d += 2) {
        int niter = 1 << (g_max_depth - d + g_min_depth);
        int check_sum = 0;
        
        for (int i = 1; i <= niter; i++) {
            toycaml_frame;
            long* temp_tree = make(d);
            make_root(&temp_tree); 
            check_sum += check(temp_tree);
            toycaml_return_handler();
        }
        printf("[Thread] %i\t trees of depth %i\t check: %i\n", niter, d, check_sum);
    }

    printf("[Thread] long lived tree depth %i\t check: %i\n", g_max_depth, check(long_lived_tree));
    
    toycaml_return_handler();
}

int main(int argc, char** argv) {
    init_heap();
    
    // ./toycaml_gc <depth> <num_threads>
    int n = (argc > 1) ? atoi(argv[1]) : 10;
    int num_workers = (argc > 2) ? atoi(argv[2]) : 2; 

    g_max_depth = (n > g_min_depth + 2) ? n : g_min_depth + 2;
    g_stretch_depth = g_max_depth + 1;

    printf("Starting test with max_depth: %d, worker_threads: %d\n\n", g_max_depth, num_workers);

    pthread_t* threads = malloc(num_workers * sizeof(pthread_t));

    // Spawn all worker threads
    for (int i = 0; i < num_workers; i++) {
        threads[i] = domain_spawn(worker_thread);
    }

    // Join all worker threads
    for (int i = 0; i < num_workers; i++) {
        domain_join(threads[i]);
    }

    printf("\nAll threads finished successfully.\n");
    free(threads);
    return 0;
}