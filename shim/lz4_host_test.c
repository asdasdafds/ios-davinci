/* Host check of the shim's LZ4 payload decoder:
 *   cc -O1 -o out/lz4_host_test lz4_host_test.c && out/lz4_host_test payload.lz4 payload.bin */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.c"

/* Stubs for what loader.c pulls from the console/fdt. */
void con_hex2(const char *l, const char *a, uint32_t x, const char *b, uint32_t y) { printf("%s %s%#x %s%#x\n", l, a, x, b, y); }
void con_hex(const char *l, uint64_t x) { (void)l; (void)x; }
void con_str(const char *l, const char *s) { (void)l; (void)s; }
int fdt_chosen_u64(uintptr_t f, const char *p, uint64_t *o) { (void)f; (void)p; (void)o; return 0; }

static void *slurp(const char *path, long *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
	void *b = malloc(*n + 16);
	if (fread(b, 1, *n, f) != (size_t)*n) exit(2);
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	long zn, rn;
	uint8_t *z = slurp(argv[1], &zn), *r = slurp(argv[2], &rn);
	uint8_t *out = calloc(rn + 64, 1);
	if (!lz4_magic((const char *)z)) { puts("no SWEETLZ4 magic"); return 1; }
	const char *err = lz4_payload((uintptr_t)z, zn, (uintptr_t)out);
	if (err) { puts(err); return 1; }
	if (memcmp(out, r, rn)) { puts("MISMATCH"); return 1; }
	printf("OK: %ld -> %ld bytes identical\n", zn, rn);
	return 0;
}
