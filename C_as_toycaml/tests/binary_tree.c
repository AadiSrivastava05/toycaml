#include <stdio.h>
#include <stdlib.h>
#include "../runtime.h"

#define TAG_NODE 0
#define TAG_ARRAY 0
#define EMPTY (long*)NULL

long* mk() {
    toycaml_frame;
    
    // Allocate 128 fields
    long* arr = caml_alloc(128, TAG_ARRAY);
    
    for (int i = 0; i < 128; i++) {
        Field(arr, i) = long2val(0); 
    }

    toycaml_return(arr);
}

long* make(int d) {
    toycaml_frame;
    
    if (d == 0) {
        // Node (Empty, mk (), Empty)
        long* data = mk();
        make_root(&data);
        
        long* node = caml_alloc(3, TAG_NODE);
        Field(node, 0) = (long)EMPTY;
        Field(node, 1) = (long)data;
        Field(node, 2) = (long)EMPTY;
        
        toycaml_return(node);
    } 
    else {
        // have to register intermediate pointers as roots before 
        // calling functions that allocate (make or mk).
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
        
        toycaml_return(node);
    }
}

int check(long* tree) {
    if (tree == EMPTY) {
        return 0;
    }
    // Field 0: Left, Field 2: Right
    return 1 + check((long*)Field(tree, 0)) + check((long*)Field(tree, 2));
}

int main(int argc, char** argv) {
    init_heap();
    
    int min_depth = 4;
    int n = (argc > 1) ? atoi(argv[1]) : 10;
    int max_depth = (n > min_depth + 2) ? n : min_depth + 2;
    int stretch_depth = max_depth + 1;

    // --- Stretch Tree ---
    {
        toycaml_frame;
        long* stretch_tree = make(stretch_depth);
        make_root(&stretch_tree);
        
        int c = check(stretch_tree);
        printf("stretch tree of depth %i\t check: %i\n", stretch_depth, c);
        toycaml_return_handler(); 
    }

    // --- Long Lived Tree ---
    long* long_lived_tree = make(max_depth);
    make_static_root(&long_lived_tree);

    // --- Loop Depths ---
    for (int d = min_depth; d <= max_depth; d += 2) {
        int niter = 1 << (max_depth - d + min_depth);
        int check_sum = 0;
        
        for (int i = 1; i <= niter; i++) {
            toycaml_frame;
            long* temp_tree = make(d);
            make_root(&temp_tree); 
            check_sum += check(temp_tree);
            toycaml_return_handler();
        }
        printf("%i\t trees of depth %i\t check: %i\n", niter, d, check_sum);
    }

    printf("long lived tree of depth %i\t check: %i\n", max_depth, check(long_lived_tree));

    return 0;
}