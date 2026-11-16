// Minimal freestanding runtime for the test WCLAPs: no libc, no WASI imports.
// memset/memcpy/strcmp (clang emits calls to the first two), a first-fit free-list malloc exported to hosts,
// a polynomial sine and tiny number formatting/parsing.
#pragma once
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int strcmp(const char *a, const char *b);
size_t rt_strlen(const char *s);
void rt_strncpy(char *dst, const char *src, size_t cap);

void *malloc(size_t size);
void free(void *ptr);

float rt_sin2pi(float phase); // sin(2*pi*phase), phase in [0, 1)
float rt_db_to_gain(float db);
float rt_pow2(float x);

// "%.1f<suffix>" into dst (cap includes the terminator)
void rt_format_fixed1(char *dst, size_t cap, double value, const char *suffix);
// parses an optional sign, digits and one decimal point; returns 0 if no digits
int rt_parse_number(const char *s, double *out);
