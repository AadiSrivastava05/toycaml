/* Registering locals and parameter macros */
#ifndef REGISTERING_H
#define REGISTERING_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t intnat;
typedef uintptr_t uintnat;
typedef intnat value;

/* Mocks to prevent unnecessary features for toycaml */

typedef struct caml__roots_block *roots_block_ptr;

typedef struct {
    roots_block_ptr local_roots;
} caml_domain_state;

// Global State Declaration
extern caml_domain_state *Caml_state;

// Helper Definitions
typedef uintptr_t mlsize_t; 
#define Val_unit ((value)1) 
#define CAMLunused_start 
#define CAMLunused_end 

typedef struct { 
    value data; 
} caml_result;
#define Result_unit ((caml_result){Val_unit})

// Mock State Check
static inline void* Caml_check_caml_state(void) {
    if (Caml_state == NULL) {
        printf("Caml_state is NULL! Runtime not initialized.\n");
        abort();
    }
    return (void*)0; 
}

/* End Mocks */


struct caml__roots_block {
  struct caml__roots_block *next;
  intnat ntables;
  intnat nitems;
  value *tables [5];
};

#define CAML_LOCAL_ROOTS (Caml_state->local_roots)

#define DO_CHECK_CAML_STATE 1

/* the Macros (Same as standard OCaml) */

#define CAMLparam0() \
  struct caml__roots_block** caml_local_roots_ptr = \
    (DO_CHECK_CAML_STATE ? Caml_check_caml_state() : (void)0, \
     &CAML_LOCAL_ROOTS); \
  struct caml__roots_block *caml__frame = *caml_local_roots_ptr

#define CAMLparam1(x) \
  CAMLparam0 (); \
  CAMLxparam1 (x)

#define CAMLparam2(x, y) \
  CAMLparam0 (); \
  CAMLxparam2 (x, y)

#define CAMLparam3(x, y, z) \
  CAMLparam0 (); \
  CAMLxparam3 (x, y, z)

#define CAMLparam4(x, y, z, t) \
  CAMLparam0 (); \
  CAMLxparam4 (x, y, z, t)

#define CAMLparam5(x, y, z, t, u) \
  CAMLparam0 (); \
  CAMLxparam5 (x, y, z, t, u)

#define CAMLparamN(x, size) \
  CAMLparam0 (); \
  CAMLxparamN (x, (size))

#define CAMLxparam1(x) \
  struct caml__roots_block caml__roots_##x; \
  CAMLunused_start int caml__dummy_##x = ( \
    (caml__roots_##x.next = *caml_local_roots_ptr), \
    (*caml_local_roots_ptr = &caml__roots_##x), \
    (caml__roots_##x.nitems = 1), \
    (caml__roots_##x.ntables = 1), \
    (caml__roots_##x.tables [0] = &x), \
    0) \
   CAMLunused_end

#define CAMLxparam2(x, y) \
  struct caml__roots_block caml__roots_##x; \
  CAMLunused_start int caml__dummy_##x = ( \
    (caml__roots_##x.next = *caml_local_roots_ptr), \
    (*caml_local_roots_ptr = &caml__roots_##x), \
    (caml__roots_##x.nitems = 1), \
    (caml__roots_##x.ntables = 2), \
    (caml__roots_##x.tables [0] = &x), \
    (caml__roots_##x.tables [1] = &y), \
    0) \
   CAMLunused_end

#define CAMLxparam3(x, y, z) \
  struct caml__roots_block caml__roots_##x; \
  CAMLunused_start int caml__dummy_##x = ( \
    (caml__roots_##x.next = *caml_local_roots_ptr), \
    (*caml_local_roots_ptr = &caml__roots_##x), \
    (caml__roots_##x.nitems = 1), \
    (caml__roots_##x.ntables = 3), \
    (caml__roots_##x.tables [0] = &x), \
    (caml__roots_##x.tables [1] = &y), \
    (caml__roots_##x.tables [2] = &z), \
    0) \
  CAMLunused_end

#define CAMLxparam4(x, y, z, t) \
  struct caml__roots_block caml__roots_##x; \
  CAMLunused_start int caml__dummy_##x = ( \
    (caml__roots_##x.next = *caml_local_roots_ptr), \
    (*caml_local_roots_ptr = &caml__roots_##x), \
    (caml__roots_##x.nitems = 1), \
    (caml__roots_##x.ntables = 4), \
    (caml__roots_##x.tables [0] = &x), \
    (caml__roots_##x.tables [1] = &y), \
    (caml__roots_##x.tables [2] = &z), \
    (caml__roots_##x.tables [3] = &t), \
    0) \
  CAMLunused_end

#define CAMLxparam5(x, y, z, t, u) \
  struct caml__roots_block caml__roots_##x; \
  CAMLunused_start int caml__dummy_##x = ( \
    (caml__roots_##x.next = *caml_local_roots_ptr), \
    (*caml_local_roots_ptr = &caml__roots_##x), \
    (caml__roots_##x.nitems = 1), \
    (caml__roots_##x.ntables = 5), \
    (caml__roots_##x.tables [0] = &x), \
    (caml__roots_##x.tables [1] = &y), \
    (caml__roots_##x.tables [2] = &z), \
    (caml__roots_##x.tables [3] = &t), \
    (caml__roots_##x.tables [4] = &u), \
    0) \
  CAMLunused_end

#define CAMLxparamN(x, size) \
  struct caml__roots_block caml__roots_##x; \
  CAMLunused_start int caml__dummy_##x = ( \
    (caml__roots_##x.next = *caml_local_roots_ptr), \
    (*caml_local_roots_ptr = &caml__roots_##x), \
    (caml__roots_##x.nitems = (size)), \
    (caml__roots_##x.ntables = 1), \
    (caml__roots_##x.tables[0] = &(x[0])), \
    0) \
  CAMLunused_end

#define CAMLxparamresult(x) \
  struct caml__roots_block caml__roots_##x; \
  CAMLunused_start int caml__dummy_##x = ( \
    (caml__roots_##x.next = *caml_local_roots_ptr), \
    (*caml_local_roots_ptr = &caml__roots_##x), \
    (caml__roots_##x.nitems = 1), \
    (caml__roots_##x.ntables = 1), \
    (caml__roots_##x.tables [0] = &(x.data)), \
    0) \
   CAMLunused_end

#define CAMLlocal1(x) \
  value x = Val_unit; \
  CAMLxparam1 (x)

#define CAMLlocal2(x, y) \
  value x = Val_unit, y = Val_unit; \
  CAMLxparam2 (x, y)

#define CAMLlocal3(x, y, z) \
  value x = Val_unit, y = Val_unit, z = Val_unit; \
  CAMLxparam3 (x, y, z)

#define CAMLlocal4(x, y, z, t) \
  value x = Val_unit, y = Val_unit, z = Val_unit, t = Val_unit; \
  CAMLxparam4 (x, y, z, t)

#define CAMLlocal5(x, y, z, t, u) \
  value x = Val_unit, y = Val_unit, z = Val_unit, t = Val_unit, u = Val_unit; \
  CAMLxparam5 (x, y, z, t, u)

#define CAMLlocalN(x, size) \
  value x [(size)]; \
  int caml__i_##x; \
  CAMLxparamN (x, (size)); \
  for (caml__i_##x = 0; caml__i_##x < size; caml__i_##x ++) { \
    x[caml__i_##x] = Val_unit; \
  }

#define CAMLlocalresult(res) \
  caml_result res = Result_unit; \
  CAMLxparamresult (res)

#define CAMLdrop do{              \
  *caml_local_roots_ptr = caml__frame; \
}while (0)

#define CAMLreturn0 do{ \
  CAMLdrop; \
  return; \
}while (0)

#define CAMLreturnT(type, result) do{ \
  type caml__temp_result = (result); \
  CAMLdrop; \
  return (caml__temp_result); \
}while(0)

#define CAMLreturn(result) CAMLreturnT(value, result)

#define CAMLnoreturn ((void) caml__frame)


#ifdef __cplusplus
}
#endif

#endif /* REGISTERING_H */