// test/compiler/harness/stdlib.h
#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

void exit(int status);
void abort(void);
int abs(int j);
int atoi(const char *nptr);
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int rand(void);
void srand(unsigned int seed);
#define RAND_MAX 0x7fffffff

#endif
