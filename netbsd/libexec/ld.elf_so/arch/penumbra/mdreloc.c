/*	$NetBSD$	*/

/*
 * Penumbra machine-dependent dynamic linker relocations.
 *
 * RELA format (explicit addends).  Relocation types match
 * LLVM ELFRelocs/Penumbra.def and sys/arch/penumbra/include/elf_machdep.h.
 */

#include <sys/cdefs.h>
#ifndef lint
__RCSID("$NetBSD$");
#endif

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/tls.h>

#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "rtld.h"

void _rtld_bind_start(void);
void _rtld_relocate_nonplt_self(Elf_Dyn *, Elf_Addr);
void *_rtld_bind(const Obj_Entry *, Elf_Word);

void
_rtld_setup_pltgot(const Obj_Entry *obj)
{
	obj->pltgot[0] = (Elf_Addr)&_rtld_bind_start;
	obj->pltgot[1] = (Elf_Addr)obj;
}

/*
 * Self-relocate the dynamic linker before any C globals work.
 * Only R_PENUMBRA_RELATIVE needs handling — everything else
 * would require symbol lookup, which isn't available yet.
 */
void
_rtld_relocate_nonplt_self(Elf_Dyn *dynp, Elf_Addr relocbase)
{
	const Elf_Rela *rela = NULL, *relalim;
	Elf_Addr relasz = 0;

	for (; dynp->d_tag != DT_NULL; dynp++) {
		switch (dynp->d_tag) {
		case DT_RELA:
			rela = (const Elf_Rela *)(relocbase + dynp->d_un.d_ptr);
			break;
		case DT_RELASZ:
			relasz = dynp->d_un.d_val;
			break;
		}
	}

	relalim = (const Elf_Rela *)((uintptr_t)rela + relasz);
	for (; rela < relalim; rela++) {
		Elf_Word r_type = ELF_R_TYPE(rela->r_info);
		Elf_Addr *where = (Elf_Addr *)(relocbase + rela->r_offset);

		switch (r_type) {
		case R_TYPE(RELATIVE): {
			Elf_Addr val = relocbase + rela->r_addend;
			*where = val;
			rdbg(("RELATIVE/L(%p) -> %p in <self>",
			    where, (void *)val));
			break;
		}

		case R_TYPE(NONE):
			break;

		default:
			abort();
		}
	}
}

int
_rtld_relocate_nonplt_objects(Obj_Entry *obj)
{
	const Elf_Rela *rela;
	const Elf_Sym *def = NULL;
	const Obj_Entry *defobj = NULL;
	unsigned long last_symnum = ULONG_MAX;

	for (rela = obj->rela; rela < obj->relalim; rela++) {
		Elf_Addr * const where =
		    (Elf_Addr *)(obj->relocbase + rela->r_offset);
		const Elf_Word r_type = ELF_R_TYPE(rela->r_info);
		unsigned long symnum;

		switch (r_type) {
		case R_TYPESZ(ADDR):
		case R_TYPE(GLOB_DAT):
		case R_TYPESZ(TLS_DTPMOD):
		case R_TYPESZ(TLS_DTPOFF):
		case R_TYPE(TLS_TPOFF32):
			symnum = ELF_R_SYM(rela->r_info);
			if (last_symnum != symnum) {
				last_symnum = symnum;
				def = _rtld_find_symdef(symnum, obj, &defobj,
				    false);
				if (def == NULL)
					return -1;
			}
			break;
		default:
			break;
		}

		switch (r_type) {
		case R_TYPE(NONE):
			break;

		case R_TYPE(RELATIVE): {
			Elf_Addr val = (Elf_Addr)obj->relocbase +
			    rela->r_addend;
			rdbg(("RELATIVE(%p) -> %p in %s",
			    where, (void *)val, obj->path));
			*where = val;
			break;
		}

		case R_TYPESZ(ADDR):
		case R_TYPE(GLOB_DAT): {
			Elf_Addr val = (Elf_Addr)(defobj->relocbase +
			    def->st_value) + rela->r_addend;
			rdbg(("ADDR/GLOB_DAT %s in %s -> %p in %s",
			    obj->strtab +
			    obj->symtab[ELF_R_SYM(rela->r_info)].st_name,
			    obj->path, (void *)val, defobj->path));
			*where = val;
			break;
		}

		case R_TYPESZ(TLS_DTPMOD): {
			Elf_Addr val = (Elf_Addr)defobj->tlsindex;
			rdbg(("TLS_DTPMOD %s in %s -> %p in %s",
			    obj->strtab +
			    obj->symtab[ELF_R_SYM(rela->r_info)].st_name,
			    obj->path, (void *)val, defobj->path));
			*where = val;
			break;
		}

		case R_TYPESZ(TLS_DTPOFF): {
			if (!defobj->tls_static &&
			    _rtld_tls_offset_allocate(__UNCONST(defobj)))
				return -1;
			Elf_Addr val = (Elf_Addr)def->st_value -
			    TLS_DTV_OFFSET;
			rdbg(("TLS_DTPOFF %s in %s -> %p in %s",
			    obj->strtab +
			    obj->symtab[ELF_R_SYM(rela->r_info)].st_name,
			    obj->path, (void *)val, defobj->path));
			*where = val;
			break;
		}

		case R_TYPE(TLS_TPOFF32): {
			if (!defobj->tls_static &&
			    _rtld_tls_offset_allocate(__UNCONST(defobj)))
				return -1;
			Elf_Addr val = (Elf_Addr)(def->st_value +
			    defobj->tlsoffset);
			rdbg(("TLS_TPOFF32 %s in %s -> %p in %s",
			    obj->strtab +
			    obj->symtab[ELF_R_SYM(rela->r_info)].st_name,
			    obj->path, (void *)val, defobj->path));
			*where = val;
			break;
		}

		case R_TYPE(COPY):
			/* Handled by _rtld_do_copy_relocations. */
			break;

		default:
			rdbg(("sym = %lu, type = %lu, offset = %p, "
			    "addend = %p, contents = %p",
			    (u_long)ELF_R_SYM(rela->r_info),
			    (u_long)ELF_R_TYPE(rela->r_info),
			    (void *)rela->r_offset,
			    (void *)rela->r_addend,
			    (void *)*where));
			_rtld_error("%s: unsupported relocation type %ld "
			    "in non-PLT relocations",
			    obj->path, (u_long)r_type);
			return -1;
		}
	}

	return 0;
}

int
_rtld_relocate_plt_lazy(Obj_Entry *obj)
{
	/* Eager binding only for now — no lazy PLT stubs. */
	return 0;
}

static int
_rtld_relocate_plt_object(const Obj_Entry *obj, const Elf_Rela *rela,
    Elf_Addr *tp)
{
	const Obj_Entry *defobj;
	Elf_Addr new_value;

	assert(ELF_R_TYPE(rela->r_info) == R_TYPE(JUMP_SLOT));

	const Elf_Sym *def = _rtld_find_plt_symdef(ELF_R_SYM(rela->r_info),
	    obj, &defobj, tp != NULL);
	if (__predict_false(def == NULL))
		return -1;
	if (__predict_false(def == &_rtld_sym_zero)) {
		/* Undefined weak symbol — leave GOT entry as 0. */
		return 0;
	}

	if (ELF_ST_TYPE(def->st_info) == STT_GNU_IFUNC) {
		if (tp == NULL)
			return 0;
		new_value = _rtld_resolve_ifunc(defobj, def);
	} else {
		new_value = (Elf_Addr)(defobj->relocbase + def->st_value);
	}
	rdbg(("bind now/fixup in %s -> new=%p",
	    defobj->strtab + def->st_name, (void *)new_value));
	*(Elf_Addr *)(obj->relocbase + rela->r_offset) = new_value;

	if (tp)
		*tp = new_value;
	return 0;
}

void *
_rtld_bind(const Obj_Entry *obj, Elf_Word reloff)
{
	const Elf_Rela *pltrel = (const Elf_Rela *)(obj->pltrel + reloff);
	Elf_Addr new_value;
	int err;

	_rtld_shared_enter();
	err = _rtld_relocate_plt_object(obj, pltrel, &new_value);
	if (err)
		_rtld_die();
	_rtld_shared_exit();

	return (caddr_t)new_value;
}

int
_rtld_relocate_plt_objects(const Obj_Entry *obj)
{

	for (const Elf_Rela *rela = obj->pltrela;
	    rela < obj->pltrelalim; rela++) {
		if (_rtld_relocate_plt_object(obj, rela, NULL) < 0)
			return -1;
	}

	return 0;
}
