#include <stdio.h>
#include "runtime.h"

void start() {
	long p = long2val(2);
	long a = p;
	p = long2val(3);
	long f = (long)caml_alloc(val2long(a), val2long(p));
	printf("%ld\n", val2long(a));
	Field(f, val2long(p)) = long2val(0);
	long f1 = Field(f, val2long(p));
	printf("%ld\n", val2long(f1));
	long zero = long2val(0);
	long two_slots = long2val(2);
	long slot1 = long2val(1);
	long slot2 = long2val(2);
	long head = (long)caml_alloc(val2long(two_slots), val2long(zero));
	Field(head, val2long(slot1)) = long2val(1);
	Field(head, val2long(slot2)) = (long)caml_alloc(val2long(two_slots), val2long(zero));
	long head_val = Field(head, val2long(slot1));
	printf("%ld\n", val2long(head_val));
	long node1 = Field(head, val2long(slot2));
	Field(node1, val2long(slot1)) = long2val(2);
	Field(node1, val2long(slot2)) = (long)caml_alloc(val2long(two_slots), val2long(zero));
	long node2 = Field(node1, val2long(slot2));
	Field(node2, val2long(slot1)) = long2val(3);
	Field(node2, val2long(slot2)) = long2val(0);
	long node2_val = Field(node2, val2long(slot1));
	printf("%ld\n", val2long(node2_val));
}

int main() {
	init_heap();
	start();
	return 0;
}
