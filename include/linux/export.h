/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_EXPORT_H
#define _LINUX_EXPORT_H

#include <linux/compiler.h>
#include <linux/linkage.h>
#include <linux/stringify.h>

/*
 * This comment block is used by fixdep. Please do not remove.
 *
 * When CONFIG_MODVERSIONS is changed from n to y, all source files having
 * EXPORT_SYMBOL variants must be re-compiled because genksyms is run as a
 * side effect of the *.o build rule.
 */

#ifndef __ASSEMBLY__
#if defined(MODULE) && !defined(CONFIG_INTEGRATE_MODULES)
extern struct module __this_module;
#define THIS_MODULE (&__this_module)
#else
#define THIS_MODULE ((struct module *)0)
#endif

#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
#include <linux/compiler.h>
/*
 * Emit the ksymtab entry as a pair of relative references: this reduces
 * the size by half on 64-bit architectures, and eliminates the need for
 * absolute relocations that require runtime processing on relocatable
 * kernels.
 */
#define __KSYMTAB_ENTRY(sym, sec)					\
	__ADDRESSABLE(sym)						\
	asm("	.section \"___ksymtab" sec "+" #sym "\", \"a\"	\n"	\
	    "	.balign	4					\n"	\
	    "__ksymtab_" #sym ":				\n"	\
	    "	.long	" #sym "- .				\n"	\
	    "	.long	__kstrtab_" #sym "- .			\n"	\
	    "	.long	__kstrtabns_" #sym "- .			\n"	\
	    "	.previous					\n")

struct kernel_symbol {
	int value_offset;
	int name_offset;
	int namespace_offset;
};
#else
#define __KSYMTAB_ENTRY(sym, sec)					\
	static const struct kernel_symbol __ksymtab_##sym		\
	__attribute__((section("___ksymtab" sec "+" #sym), used))	\
	__aligned(sizeof(void *))					\
	= { (unsigned long)&sym, __kstrtab_##sym, __kstrtabns_##sym }

struct kernel_symbol {
	unsigned long value;
	const char *name;
	const char *namespace;
};
#endif

#ifdef __GENKSYMS__

#define ___EXPORT_SYMBOL(sym, sec, ns)	__GENKSYMS_EXPORT_SYMBOL(sym)

#else

/*
 * For every exported symbol, do the following:
 *
 * - Put the name of the symbol and namespace (empty string "" for none) in
 *   __ksymtab_strings.
 * - Place a struct kernel_symbol entry in the __ksymtab section.
 *
 * note on .section use: we specify progbits since usage of the "M" (SHF_MERGE)
 * section flag requires it. Use '%progbits' instead of '@progbits' since the
 * former apparently works on all arches according to the binutils source.
 */
#define ___EXPORT_SYMBOL(sym, sec, ns)						\
	extern typeof(sym) sym;							\
	extern const char __kstrtab_##sym[];					\
	extern const char __kstrtabns_##sym[];					\
	asm("	.section \"__ksymtab_strings\",\"aMS\",%progbits,1	\n"	\
	    "__kstrtab_" #sym ":					\n"	\
	    "	.asciz 	\"" #sym "\"					\n"	\
	    "__kstrtabns_" #sym ":					\n"	\
	    "	.asciz 	\"" ns "\"					\n"	\
	    "	.previous						\n");	\
	__KSYMTAB_ENTRY(sym, sec)

#endif

#if !defined(CONFIG_MODULES) || defined(__DISABLE_EXPORTS) || \
    defined(CONFIG_INTEGRATE_MODULES)

/*
 * Allow symbol exports to be disabled completely so that C code may
 * be reused in other execution contexts such as the UEFI stub or the
 * decompressor.
 */
#define __EXPORT_SYMBOL(sym, license, ns)

#elif defined(__GENKSYMS__)

#define __EXPORT_SYMBOL(sym, license, ns)	__GENKSYMS_EXPORT_SYMBOL(sym)

#elif defined(__ASSEMBLY__)

#define __EXPORT_SYMBOL(sym, license, ns) \
	___EXPORT_SYMBOL(sym, license, ns)

#else

#ifdef CONFIG_GENDWARFKSYMS
/*
 * With CONFIG_GENDWARFKSYMS, ensure the compiler emits debugging
 * information for all exported symbols, including those defined in
 * different TUs, by adding a __gendwarfksyms_ptr_<symbol> pointer
 * that's discarded during the final link.
 */
#define __GENDWARFKSYMS_EXPORT(sym)				\
	static typeof(sym) *__gendwarfksyms_ptr_##sym __used	\
		__section(".discard.gendwarfksyms") = &sym;
#else
#define __GENDWARFKSYMS_EXPORT(sym)
#endif

#define __EXPORT_SYMBOL(sym, license, ns)			\
	extern typeof(sym) sym;					\
	__ADDRESSABLE(sym)					\
	__GENDWARFKSYMS_EXPORT(sym)				\
	asm(__stringify(___EXPORT_SYMBOL(sym, license, ns)))

#endif

#ifdef DEFAULT_SYMBOL_NAMESPACE
#define _EXPORT_SYMBOL(sym, license)	__EXPORT_SYMBOL(sym, license, DEFAULT_SYMBOL_NAMESPACE)
#else
#define _EXPORT_SYMBOL(sym, license)	__EXPORT_SYMBOL(sym, license, "")
#endif

#define EXPORT_SYMBOL(sym)		_EXPORT_SYMBOL(sym, "")
#define EXPORT_SYMBOL_GPL(sym)		_EXPORT_SYMBOL(sym, "GPL")
#define EXPORT_SYMBOL_NS(sym, ns)	__EXPORT_SYMBOL(sym, "", ns)
#define EXPORT_SYMBOL_NS_GPL(sym, ns)	__EXPORT_SYMBOL(sym, "GPL", ns)

#define EXPORT_SYMBOL_FOR_MODULES(sym, mods) __EXPORT_SYMBOL(sym, "GPL", "module:" mods)

#endif /* _LINUX_EXPORT_H */
