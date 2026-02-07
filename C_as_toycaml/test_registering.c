#include "registering.h"
#include <stdio.h>

// Initialize Global State
caml_domain_state my_domain_state = { NULL };
caml_domain_state *Caml_state = &my_domain_state;

// Helper function to inspect the stack frames and print registered roots
void debug_print_roots() {
    printf("\n Root debug test \n");
    struct caml__roots_block *frame = Caml_state->local_roots;
    int frame_count = 0;

    while (frame != NULL) {
        printf("Frame %d (addr: %p):\n", frame_count, (void*)frame);
        
        for (int i = 0; i < frame->ntables; i++) {
            printf("  Table %d:\n", i);
            value *table = frame->tables[i];
            for (int j = 0; j < frame->nitems; j++) {
                printf("    Root[%d] Address: %p | Value: %ld\n", j, (void*)&table[j], table[j]);
            }
        }
        frame = frame->next;
        frame_count++;
    }
    printf("Debug end\n");
}

value func_b(value arg) {
    CAMLparam1(arg);
    CAMLlocal2(x, y); // Registers 2 local variables

    x = 42; 
    y = 100;
    
    printf("\nInside func_b:\n");
    debug_print_roots(); // Should see roots for func_b, func_a, and main

    CAMLreturn(x);
}

value func_a(value arg) {
    CAMLparam1(arg);
    
    printf("\nInside func_a (before calling b):\n");
    debug_print_roots(); // Should see roots for func_a and main

    value res = func_b(arg);
    
    CAMLreturn(res);
}

int main() {
    // Top-level roots usually handled differently, but we use CAMLparam here for demo
    // Can have a separate mechanism for top-level roots if needed
    CAMLparam0(); 
    CAMLlocal1(top_val);

    top_val = 999;

    printf("Inside main:\n");
    func_a(top_val);

    CAMLreturn(0);
}