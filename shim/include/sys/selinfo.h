/* XeGfx: FreeBSD's selinfo.h; select/poll wakeups go to xg_fd.c, because XNU exports selrecord/selwakeup under the
 * same names with a different ABI */
#include_next <sys/selinfo.h>
#ifndef _XG_SELINFO_H_
#define _XG_SELINFO_H_
struct thread;
void xg_fd_selrecord(struct thread *td, struct selinfo *sip);
void xg_fd_selwakeup(struct selinfo *sip);
#define selrecord(td, sip)	xg_fd_selrecord(td, sip)
#define selwakeup(sip)		xg_fd_selwakeup(sip)
#define selwakeuppri(sip, pri)	xg_fd_selwakeup(sip)
#endif
