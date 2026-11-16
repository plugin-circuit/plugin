#include "rt.h"

#define WASM_EXPORT(name) __attribute__((export_name(name)))

void *memset(void *dst, int c, size_t n) {
	unsigned char *d = (unsigned char *)dst;
	while (n--) *d++ = (unsigned char)c;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--) *d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	if (d < s) {
		while (n--) *d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--) *--d = *--s;
	}
	return dst;
}

int strcmp(const char *a, const char *b) {
	while (*a && *a == *b) ++a, ++b;
	return (unsigned char)*a - (unsigned char)*b;
}

size_t rt_strlen(const char *s) {
	size_t n = 0;
	while (s[n]) ++n;
	return n;
}

void rt_strncpy(char *dst, const char *src, size_t cap) {
	if (!cap) return;
	size_t i = 0;
	for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
	dst[i] = 0;
}

// ---- allocator: 16-byte aligned blocks with an 16-byte header, first-fit free list, memory.grow on demand
extern unsigned char __heap_base;
static uintptr_t heapTop = 0, heapEnd = 0;
typedef struct Block { size_t size; struct Block *next; size_t pad[2]; } Block; // header, 16 bytes on wasm32
static Block *freeList = 0;

WASM_EXPORT("malloc") void *malloc(size_t size) {
	size = (size + 15) & ~(size_t)15;
	Block **link = &freeList;
	for (Block *b = freeList; b; link = &b->next, b = b->next) {
		if (b->size >= size) {
			*link = b->next;
			return b + 1;
		}
	}
	if (!heapTop) {
		heapTop = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15;
		heapEnd = __builtin_wasm_memory_size(0) * 65536u;
	}
	uintptr_t need = heapTop + sizeof(Block) + size;
	if (need > heapEnd) {
		size_t pages = (need - heapEnd + 65535u) / 65536u;
		if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return 0;
		heapEnd += pages * 65536u;
	}
	Block *b = (Block *)heapTop;
	b->size = size;
	heapTop = need;
	return b + 1;
}

WASM_EXPORT("free") void free(void *ptr) {
	if (!ptr) return;
	Block *b = (Block *)ptr - 1;
	b->next = freeList;
	freeList = b;
}

// ---- math
float rt_sin2pi(float phase) {
	// reduce to [-0.5, 0.5) turns, then a 7th-order odd polynomial on x = 2*pi*t (|error| < 1e-4 in [-pi, pi]
	// after the symmetric fold to [-pi/2, pi/2])
	float t = phase - (float)(int)phase;
	if (t >= 0.5f) t -= 1.0f;
	if (t < -0.5f) t += 1.0f;
	if (t > 0.25f) t = 0.5f - t;
	if (t < -0.25f) t = -0.5f - t;
	float x = t * 6.28318530718f;
	float x2 = x * x;
	return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * -0.00019840874f)));
}

float rt_pow2(float x) {
	int i = (int)x;
	if (x < 0 && (float)i != x) --i;
	float f = x - (float)i; // [0, 1)
	// 2^f on [0,1): 4th-order polynomial
	float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * 0.00961813f)));
	if (i > 60) i = 60;
	if (i < -60) return 0.0f;
	float s = 1.0f;
	if (i > 0) while (i--) s *= 2.0f;
	else while (i++) s *= 0.5f;
	return p * s;
}

float rt_db_to_gain(float db) { return rt_pow2(db * 0.16609640474f); } // 10^(db/20) = 2^(db * log2(10)/20)

void rt_format_fixed1(char *dst, size_t cap, double value, const char *suffix) {
	char buf[64];
	size_t n = 0;
	if (value < 0) { buf[n++] = '-'; value = -value; }
	long tenths = (long)(value * 10.0 + 0.5);
	long whole = tenths / 10, frac = tenths % 10;
	char digits[24];
	int d = 0;
	do { digits[d++] = (char)('0' + whole % 10); whole /= 10; } while (whole && d < 20);
	while (d) buf[n++] = digits[--d];
	buf[n++] = '.';
	buf[n++] = (char)('0' + frac);
	for (const char *s = suffix; *s && n < sizeof(buf) - 1; ++s) buf[n++] = *s;
	buf[n] = 0;
	if (n == 2 + 1 && buf[0] == '-' && buf[1] == '0' && frac == 0) { /* "-0.0" never happens with this rounding */ }
	rt_strncpy(dst, buf, cap);
}

int rt_parse_number(const char *s, double *out) {
	while (*s == ' ') ++s;
	double sign = 1, v = 0, scale = 0;
	int digits = 0;
	if (*s == '-') { sign = -1; ++s; } else if (*s == '+') ++s;
	for (; *s; ++s) {
		if (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s - '0');
			if (scale) scale *= 10;
			++digits;
		} else if (*s == '.' && !scale) {
			scale = 1;
		} else {
			break;
		}
	}
	if (!digits) return 0;
	*out = sign * (scale ? v / scale : v);
	return 1;
}
