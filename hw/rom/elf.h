/*
 * elf.h — Minimal ELF32 definitions for Penumbra boot ROM
 *
 * Only what the ROM needs to load a PIE ELF: file header, program
 * headers, and a handful of constants.  No section headers, no
 * symbol tables, no dynamic linking structures — the LOADER
 * handles its own relocations after the ROM loads the segments.
 */

#ifndef ELF_H
#define ELF_H

#include "penumbra.h"

/* ── ELF identification ─────────────────────────────────────────────── */

#define EI_NIDENT    16

#define ELFMAG0      0x7f
#define ELFMAG1      'E'
#define ELFMAG2      'L'
#define ELFMAG3      'F'

#define ELFCLASS32   1
#define ELFDATA2LSB  1      /* little-endian */

#define ET_EXEC      2      /* statically linked executable (fixed VA) */
#define ET_DYN       3      /* PIE / shared object */
#define EM_PENUMBRA  0xF0DA

/* ── ELF32 file header ──────────────────────────────────────────────── */

struct elf32_ehdr {
    unsigned char e_ident[EI_NIDENT];
    unsigned short e_type;       /* ET_DYN (PIE) or ET_EXEC (static) */
    unsigned short e_machine;    /* EM_PENUMBRA */
    uint32_t e_version;
    uint32_t e_entry;            /* entry point (virtual address) */
    uint32_t e_phoff;            /* program header table offset */
    uint32_t e_shoff;
    uint32_t e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;      /* number of program headers */
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
};

/* ── ELF32 program header ──────────────────────────────────────────── */

#define PT_NULL    0
#define PT_LOAD    1

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;     /* offset in file */
    uint32_t p_vaddr;      /* virtual address (relative to 0 for PIE) */
    uint32_t p_paddr;
    uint32_t p_filesz;     /* bytes in file */
    uint32_t p_memsz;      /* bytes in memory (>= filesz; difference is .bss) */
    uint32_t p_flags;
    uint32_t p_align;
};

#endif /* ELF_H */
