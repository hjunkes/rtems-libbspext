/* $Id: isrWrap.c,v 1.7 2008/10/25 20:22:40 strauman Exp $ */

/* Fix the broken IRQ API of RTEMS;
 * 
 * Author: Till Straumann <strauman@slac.stanford.edu>, 2003
 *
 * On some BSPs, RTEMS does not allow for passing an argument to
 * ISRs and ISRs can not be shared. This is especially annoying
 * on PCI where shared IRQs should be available.
 * 
 * Not having an ISR argument leads to subtle problems when
 * shared IRQs are involved: consider the case of a driver
 * supporting multiple, identical devices listening on different
 * interrupt lines with different priorities. The driver's
 * global ISR cannot just poll all devices for activity:
 *
 *   ISR() {
 *     for ( i=0; i<NDEVS; i++) {
 *         if (irq_pending(dev[i])) {
 *            handle_irq(dev[i]);
 *         }
 *     }
 *   }
 *
 * is NOT CORRECT and (unless the device provides atomical
 * 'test and clear' of the IRQ condition) contains a race
 * condition:
 *
 * Let's say device # 1 raises an IRQ on line 1
 * and just between the ISR() executing 'irq_pending()' and 'handle_irq()',
 * clearing the IRQ, device #2 raises another line with higher
 * priority. The preempting ISR then also finds #1's IRQ pending
 * and executes the 'handle_irq()' sequence!
 *
 * Acknowledgement of sponsorship
 * ------------------------------
 * This software was produced by
 *     the Stanford Linear Accelerator Center, Stanford University,
 * 	   under Contract DE-AC03-76SFO0515 with the Department of Energy.
 * 
 * Government disclaimer of liability
 * ----------------------------------
 * Neither the United States nor the United States Department of Energy,
 * nor any of their employees, makes any warranty, express or implied, or
 * assumes any legal liability or responsibility for the accuracy,
 * completeness, or usefulness of any data, apparatus, product, or process
 * disclosed, or represents that its use would not infringe privately owned
 * rights.
 * 
 * Stanford disclaimer of liability
 * --------------------------------
 * Stanford University makes no representations or warranties, express or
 * implied, nor assumes any liability for the use of this software.
 * 
 * Stanford disclaimer of copyright
 * --------------------------------
 * Stanford University, owner of the copyright, hereby disclaims its
 * copyright and all other rights in this software.  Hence, anyone may
 * freely use it for any purpose without restriction.  
 * 
 * Maintenance of notices
 * ----------------------
 * In the interest of clarity regarding the origin and status of this
 * SLAC software, this and all the preceding Stanford University notices
 * are to remain affixed to any copy or derivative of this software made
 * or distributed by the recipient and are to be affixed to any copy of
 * software made or distributed by the recipient that contains a copy or
 * derivative of this software.
 * 
 * ------------------ SLAC Software Notices, Set 4 OTT.002a, 2004 FEB 03
 */ 
#include <rtems.h>
#include <bsp.h>
//#include <rtems/system.h>
#include <bsp/irq.h>

#include "bspExt.h"

#include <assert.h>
#include <stdlib.h>


static void noop(const rtems_irq_connect_data *unused) {};
static int  noop1(const rtems_irq_connect_data *unused) { return 0;};

#if RTEMS_ISMINVERSION(4,6,99) && defined(BSP_SHARED_HANDLER_SUPPORT)
/* Finally have an ISR argument but the API still sucks */
int
bspExtInstallSharedISR(int irqLine, void (*isr)(void *), void * uarg, int flags)
{
rtems_irq_connect_data suck = {0};
	suck.name   = irqLine;
	suck.hdl    = isr;
	suck.handle = uarg;
	suck.on     = noop;
	suck.off    = noop;
	suck.isOn   = noop1;
	return ! ( ( BSPEXT_ISR_NONSHARED & flags ) ?
		BSP_install_rtems_irq_handler(&suck) :	
		BSP_install_rtems_shared_irq_handler(&suck) );
}

int
bspExtRemoveSharedISR(int irqLine, void (*isr)(void *), void *uarg)
{
rtems_irq_connect_data suck = {0};
	suck.name   = irqLine;
	suck.hdl    = isr;
	suck.handle = uarg;
	suck.on     = noop;
	suck.off    = noop;
	suck.isOn   = noop1;
	return ! BSP_remove_rtems_irq_handler(&suck);
}

#else

typedef volatile struct ISRRec_ {
	void  *uarg;
	void (*hdl)(void*);
	volatile struct ISRRec_ *next;
	int   flags;
} ISRRec, * volatile ISR;

typedef struct WrapRec_ {
	void	(* const wrapper)(void);
	volatile int	irqLine;
	ISR	anchor;
} WrapRec, *Wrap;

#define DECL_WRAPPER(num) static void wrap##num() { isrdispatch(wrappers+num); }
#define SLOTDECL(num) { wrap##num, 0, 0 }

#define NumberOf(arr) (sizeof(arr)/sizeof((arr)[0]))

static void isrdispatch(Wrap w) { register ISR i; for (i=w->anchor; i; i=i->next) i->hdl(i->uarg); }

static WrapRec wrappers[];

/* there should at least be enough wrappers for the 4 PCI interrupts; provide some more... */
DECL_WRAPPER(0)
DECL_WRAPPER(1)
DECL_WRAPPER(2)
DECL_WRAPPER(3)
DECL_WRAPPER(4)
DECL_WRAPPER(5)
DECL_WRAPPER(6)

static WrapRec wrappers[] = {
	SLOTDECL(0),
	SLOTDECL(1),
	SLOTDECL(2),
	SLOTDECL(3),
	SLOTDECL(4),
	SLOTDECL(5),
	SLOTDECL(6),
};

int
bspExtRemoveSharedISR(int irqLine, void (*isr)(void *), void *uarg)
{
Wrap w;
ISR	*preq;

	assert( irqLine >= 0 );

	bspExtLock();

		for ( w = wrappers; w < wrappers + NumberOf(wrappers); w++ )
			if ( w->irqLine == irqLine ) {
				for ( preq = &w->anchor; *preq; preq = &(*preq)->next ) {
					if ( (*preq)->hdl == isr && (*preq)->uarg == uarg ) {
							ISR found = *preq;
							/* atomic; there's no need to disable interrupts */
							*preq = (*preq)->next;
							if ( 0 == w->anchor) {
								/* was the last one; release the wrapper slot */
								rtems_irq_connect_data d;
								d.on   = d.off = noop;
								d.isOn = noop1;

								d.hdl  = (rtems_irq_hdl)w->wrapper;
								d.name = w->irqLine;
								assert ( BSP_remove_rtems_irq_handler(&d) );
								w->irqLine = -1;
							}
							bspExtUnlock();
							free((void*)found);
							return 0;
					}
				}
			}
	bspExtUnlock();
	return -1;
}

int
bspExtInstallSharedISR(int irqLine, void (*isr)(void *), void * uarg, int flags)
{
Wrap w;
Wrap avail=0;

ISR	req = malloc(sizeof(*req));

	assert( req );
	req->hdl   = isr;
	req->uarg  = uarg;
	req->flags = flags;

	assert( irqLine >= 0 );

	bspExtLock();
		for ( w=wrappers; w<wrappers+NumberOf(wrappers); w++ ) {
			if ( w->anchor ) {
				if ( w->irqLine == irqLine ) {
					if ( BSPEXT_ISR_NONSHARED & (flags | w->anchor->flags) ) {
						/* we either requested exclusive access or
						 * the already installed ISR has this flag set
						 */
						goto bailout;
					}
					req->next = w->anchor;
					/* atomic; there's no need to disable interrupts - even if an interrupt happens right now */
					w->anchor = req;
					req = 0;
					break;
				}
			} else {
				/* found a free slot, remember */
				avail = w;
			}
		}
		if ( req && avail ) {
			/* no wrapper installed yet for this line but a slot is available */
			rtems_irq_connect_data d;
			d.on   = d.off = noop;
			d.isOn = noop1;

			d.hdl  = (rtems_irq_hdl)avail->wrapper;
			d.name = irqLine;
			avail->irqLine = irqLine;

			if ( BSP_install_rtems_irq_handler(&d) ) {
				/* this should be safe; if an IRQ happens isrdispatch() simply will find anchor == 0 */
				req->next = avail->anchor;
				avail->anchor = req;
				req = 0;
			}
		}
bailout:
	bspExtUnlock();
	free((void*)req);
	return req != 0;
}

#endif
