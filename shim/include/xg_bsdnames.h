/* <wtl> project: xegfx | file: shim/include/xg_bsdnames.h | author: watchthelight </wtl> */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Forced include for every FreeBSD/Linux unit. XNU exports these names too; inside one statically linked kext the
 * XNU-side objects would bind to our FreeBSD copies (I-094: _start registered an XNU oid with FreeBSD's
 * sysctl_register_oid and walked a NULL tree). tools/shadow_check.py fails the link if a new one appears.
 */
#ifndef _XG_BSDNAMES_H_
#define _XG_BSDNAMES_H_
#define bsearch xg_bsd_bsearch
#define eventhandler_deregister xg_bsd_eventhandler_deregister
#define eventhandler_find_list xg_bsd_eventhandler_find_list
#define eventhandler_prune_list xg_bsd_eventhandler_prune_list
#define eventhandler_register xg_bsd_eventhandler_register
#define hz xg_bsd_hz
#define kmem_free xg_bsd_kmem_free
#define knote xg_bsd_knote
#define log xg_bsd_log
#define pmap_copy_page xg_bsd_pmap_copy_page
#define pmap_zero_page xg_bsd_pmap_zero_page
#define sbuf_bcat xg_bsd_sbuf_bcat
#define sbuf_bcpy xg_bsd_sbuf_bcpy
#define sbuf_cat xg_bsd_sbuf_cat
#define sbuf_clear xg_bsd_sbuf_clear
#define sbuf_cpy xg_bsd_sbuf_cpy
#define sbuf_data xg_bsd_sbuf_data
#define sbuf_delete xg_bsd_sbuf_delete
#define sbuf_done xg_bsd_sbuf_done
#define sbuf_finish xg_bsd_sbuf_finish
#define sbuf_len xg_bsd_sbuf_len
#define sbuf_new xg_bsd_sbuf_new
#define sbuf_printf xg_bsd_sbuf_printf
#define sbuf_putc xg_bsd_sbuf_putc
#define sbuf_setpos xg_bsd_sbuf_setpos
#define sbuf_vprintf xg_bsd_sbuf_vprintf
#define suword64 xg_bsd_suword64
#define sysctl__children xg_bsd_sysctl__children
#define sysctl_handle_int xg_bsd_sysctl_handle_int
#define sysctl_handle_long xg_bsd_sysctl_handle_long
#define sysctl_handle_opaque xg_bsd_sysctl_handle_opaque
#define sysctl_handle_string xg_bsd_sysctl_handle_string
#define sysctl_register_oid xg_bsd_sysctl_register_oid
#define sysctl_unregister_oid xg_bsd_sysctl_unregister_oid
#define vm_page_free xg_bsd_vm_page_free
#define vm_page_remove xg_bsd_vm_page_remove
#define vm_page_unwire xg_bsd_vm_page_unwire
#define wakeup xg_bsd_wakeup
#define wakeup_one xg_bsd_wakeup_one
#define zfree xg_bsd_zfree
/* exported by older kernels only (macOS 11 to 15) */
#define allproc xg_bsd_allproc
#define kmem_alloc_contig xg_bsd_kmem_alloc_contig
#define proc0 xg_bsd_proc0
#define vm_page_replace xg_bsd_vm_page_replace
/* not shadows, a tap: FreeBSD/Linux console output also lands in the ring behind sysctl debug.xegfx_log */
#define printf(...) xg_printf(__VA_ARGS__)
#define vprintf(...) xg_vprintf(__VA_ARGS__)
int xg_printf(const char *, ...) __attribute__((__format__(__printf__, 1, 2)));
int xg_vprintf(const char *, __builtin_va_list) __attribute__((__format__(__printf__, 1, 0)));
/* the same formatter for strings, so Linux's %pV / %pe work everywhere drm formats text (N-226) */
#define snprintf(...) xg_snprintf(__VA_ARGS__)
#define vsnprintf(...) xg_vsnprintf(__VA_ARGS__)
int xg_snprintf(char *, __SIZE_TYPE__, const char *, ...);
int xg_vsnprintf(char *, __SIZE_TYPE__, const char *, __builtin_va_list);
#endif
