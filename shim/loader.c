/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

/*
 * davinci's ABL refuses `fastboot boot` images with a ramdisk of ~16 MiB+
 * ("Load Error"), so the payload may come LZ4-compressed:
 *   "SWEETLZ4" | u64 raw_size | LZ4 legacy stream (magic 0x184c2102, then
 *   [u32 block_size][block] ..., each block <= 8 MiB decompressed)
 * Byte accesses only: with the MMU off all memory is Device memory.
 */
static int lz4_magic(const char *m)
{
	static const char want[] = "SWEETLZ4";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static uint32_t rd_le32(const volatile uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One LZ4 block; returns bytes written or -1 on malformed input. */
static int64_t lz4_block(const volatile uint8_t *src, uint64_t n,
    volatile uint8_t *dst, uint64_t cap)
{
	const volatile uint8_t *ip = src, *iend = src + n;
	uint64_t o = 0;
	while (ip < iend) {
		uint8_t tok = *ip++;
		uint64_t lit = tok >> 4;
		if (lit == 15) {
			uint8_t b;
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if ((uint64_t)(iend - ip) < lit || o + lit > cap)
			return -1;
		for (uint64_t i = 0; i < lit; i++)
			dst[o++] = *ip++;
		if (ip >= iend)
			break;			/* last sequence: literals only */
		if (iend - ip < 2)
			return -1;
		uint64_t off = ip[0] | (uint64_t)ip[1] << 8;
		ip += 2;
		if (!off || off > o)
			return -1;
		uint64_t len = tok & 15;
		if (len == 15) {
			uint8_t b;
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				len += b;
			} while (b == 255);
		}
		len += 4;
		if (o + len > cap)
			return -1;
		for (uint64_t i = 0; i < len; i++, o++)
			dst[o] = dst[o - off];	/* overlapping copy is intended */
	}
	return (int64_t)o;
}

/* Decompress a SWEETLZ4 payload at src (ramdisk) to dst; 0 on success. */
static const char *lz4_payload(uintptr_t src, uint64_t src_len, uintptr_t dst)
{
	const volatile uint8_t *s = (const volatile uint8_t *)src;
	uint64_t raw = rd_le32(s + 8) | (uint64_t)rd_le32(s + 12) << 32;
	uint64_t pos = 16, out = 0;
	if (rd_le32(s + pos) != 0x184c2102)
		return "lz4: bad stream magic";
	pos += 4;
	while (pos + 4 <= src_len && out < raw) {
		uint32_t bs = rd_le32(s + pos);
		if (bs == 0x184c2102) {		/* concatenated legacy frames */
			pos += 4;
			continue;
		}
		pos += 4;
		if (pos + bs > src_len)
			return "lz4: truncated block";
		int64_t got = lz4_block(s + pos, bs, (volatile uint8_t *)dst + out, raw - out);
		if (got < 0)
			return "lz4: corrupt block";
		out += (uint64_t)got;
		pos += bs;
	}
	if (out != raw)
		return "lz4: size mismatch";
	con_hex2("LZ4", "raw=", raw, "dst=", dst);
	return 0;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	if (lz4_magic((const char *)rd_start)) {
		/* Decompress 64 MiB past the ramdisk (free lower-bank RAM ABL loaded into),
		 * then treat the result as the ramdisk. */
		uintptr_t dst = ((rd_end + 0xffffffUL) & ~0xffffffUL) + 0x4000000UL;
		const char *err = lz4_payload(rd_start, rd_end - rd_start, dst);
		if (err)
			return err;
		const volatile uint8_t *h = (const volatile uint8_t *)rd_start;
		uint64_t raw = rd_le32(h + 8) | (uint64_t)rd_le32(h + 12) << 32;
		rd_start = dst;
		rd_end = dst + raw;
	}
	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic))
		return "no SWEETXNU payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
