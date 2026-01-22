#include<stdio.h>
#include<stdlib.h>
int main(){
	long p = (1+(2<<1));
	long a = p;
	p = (1+(3<<1));
	long f = (long*)malloc(((a>>1)+1)*(sizeof(long)));
	*(long*)f = ((a>>1)<<10) + ((p>>1));
	printf("%ld\n",(a>>1));
	*((long*)f + (p>>1)) = (1+(0<<1));
	long f1 = *((long*)f + (p>>1));
	printf("%ld\n",(f1>>1));
	long zero = (1+(0<<1));
	long two_slots = (1+(2<<1));
	long slot1 = (1+(1<<1));
	long slot2 = (1+(2<<1));
	long head = (long*)malloc(((two_slots>>1)+1)*(sizeof(long)));
	*(long*)head = ((two_slots>>1)<<10) + ((zero>>1));
	*((long*)head + (slot1>>1)) = (1+(1<<1));
	*((long*)head + (slot2>>1)) = (long*)malloc(((two_slots>>1)+1)*(sizeof(long)));
	*(long*)*((long*)head + (slot2>>1)) = ((two_slots>>1)<<10) + ((zero>>1));
	long head_val = *((long*)head + (slot1>>1));
	printf("%ld\n",(head_val>>1));
	long node1 = *((long*)head + (slot2>>1));
	*((long*)node1 + (slot1>>1)) = (1+(2<<1));
	*((long*)node1 + (slot2>>1)) = (long*)malloc(((two_slots>>1)+1)*(sizeof(long)));
	*(long*)*((long*)node1 + (slot2>>1)) = ((two_slots>>1)<<10) + ((zero>>1));
	long node2 = *((long*)node1 + (slot2>>1));
	*((long*)node2 + (slot1>>1)) = (1+(3<<1));
	*((long*)node2 + (slot2>>1)) = (1+(0<<1));
	long node2_val = *((long*)node2 + (slot1>>1));
	printf("%ld\n",(node2_val>>1));
}
