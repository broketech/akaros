/* Copyright (c) 2016 Google Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Vcore timer ticks. */

#include <parlib/vcore.h>
#include <parlib/assert.h>
#include <parlib/tsc-compat.h>
#include <parlib/arch/bitmask.h>
#include <benchutil/alarm.h>
#include <benchutil/vcore_tick.h>

/* TODO: if we use some other form of per-vcore memory, we can also have a
 * per-vcore init function that we run before the VC spools up, removing the
 * need to check for PREINIT. */

enum {
	VC_TICK_PREINIT = 0,
	VC_TICK_ENABLED = 1,
	VC_TICK_DISABLED = 2,
};

struct vcore_tick {
	int							state;
	int							ctl_fd;
	int							timer_fd;
	uint64_t					next_deadline;
	uint64_t					period_ticks;
	struct event_queue			*ev_q;
};

static __thread struct vcore_tick __vc_tick;

static void vcore_tick_init(struct vcore_tick *vc_tick)
{
	int ret;

	ret = devalarm_get_fds(&vc_tick->ctl_fd, &vc_tick->timer_fd, 0);
	assert(!ret);
	/* We want an IPI and a bit set in the bitmask.  But no wakeups, in case
	 * we're offline. */
	vc_tick->ev_q = get_eventq(EV_MBOX_BITMAP);
	assert(vc_tick->ev_q);
	vc_tick->ev_q->ev_flags = EVENT_IPI;
	vc_tick->ev_q->ev_vcore = vcore_id();
	ret = devalarm_set_evq(vc_tick->ctl_fd, vc_tick->ev_q);
	assert(!ret);
	vc_tick->state = VC_TICK_DISABLED;
}

static void __vcore_tick_start(struct vcore_tick *vc_tick, uint64_t from_now)
{
	int ret;

	ret = devalarm_set_time(vc_tick->timer_fd, read_tsc() + from_now);
	assert(!ret);
}

/* Starts a timer tick for this vcore, in virtual time (time the vcore is
 * actually online).  You can call this repeatedly, even if the timer is already
 * on.  You also can update the period of an already-running tick. */
void vcore_tick_enable(uint64_t period_usec)
{
	struct vcore_tick *vc_tick = &__vc_tick;

	if (vc_tick->state == VC_TICK_PREINIT)
		vcore_tick_init(vc_tick);

	vc_tick->period_ticks = usec2tsc(period_usec);
	if (vc_tick->state == VC_TICK_DISABLED) {
		vc_tick->next_deadline = vcore_account_total_ticks(vcore_id()) +
		                         vc_tick->period_ticks;
		__vcore_tick_start(vc_tick, vc_tick->period_ticks);
		vc_tick->state = VC_TICK_ENABLED;
	}
}

/* Disables the timer tick.  You can call this repeatedly.  It is possible that
 * you will still have a timer tick pending after this returns. */
void vcore_tick_disable(void)
{
	struct vcore_tick *vc_tick = &__vc_tick;
	int ret;

	if (vc_tick->state == VC_TICK_PREINIT)
		vcore_tick_init(vc_tick);

	if (vc_tick->state == VC_TICK_ENABLED) {
		ret = devalarm_disable(__vc_tick.ctl_fd);
		assert(!ret);
		vc_tick->state = VC_TICK_DISABLED;
	}
}

/* Polls the vcore timer tick.  Returns TRUE if the time has expired, false
 * otherwise.  Either way, it will ensure that the underlying alarm is still
 * turned on.  This will not catch if multiple ticks *should* have gone off
 * since the last poll.  In that case, we'll just coalesce those into one tick.
 * */
bool vcore_tick_poll(void)
{
	struct vcore_tick *vc_tick = &__vc_tick;
	struct evbitmap *evbm;
	bool ret;
	uint64_t from_now, virtual_now;

	if (vc_tick->state == VC_TICK_PREINIT)
		vcore_tick_init(vc_tick);

	evbm = &vc_tick->ev_q->ev_mbox->evbm;
	if (!GET_BITMASK_BIT(evbm->bitmap, EV_ALARM)) {
		/* It might be possible that the virtual time has passed, but the alarm
		 * hasn't arrived yet.
		 *
		 * We assume that if the bit is not set and the tick is enabled that
		 * the kernel still has an alarm set for us.  It is possible for the bit
		 * to be set more than expected (disable an alarm, but fail to cancel
		 * the alarm before it goes off, then enable it, and then we'll have the
		 * bit set before the alarm expired).  However, it is not possible that
		 * the bit is clear and there is no alarm pending at this point.  This
		 * is because the only time we clear the bit is below, and then right
		 * after that we set an alarm. (The bit is also clear at init time, and
		 * we start the alarm when we enable the tick).
		 *
		 * Anyway, the alarm should be arriving shortly.  In this case, as in
		 * the case where the bit gets set right after we check, we missed
		 * polling for the event.  The kernel will still __notify us, setting
		 * notif_pending, and we'll notice the next time we attempt to leave
		 * vcore context. */
		return FALSE;
	}
	/* Don't care about clobbering neighboring bits (non-atomic op) */
	CLR_BITMASK_BIT(evbm->bitmap, EV_ALARM);
	/* As mentioned above, it is possible to still have an active alarm in the
	 * kernel.  We can still set a new time for the alarm, and it will just
	 * update the kernel's awaiter.  And if that alarm has fired, then we'll
	 * just have a spurious setting of the bit.  This does not affect our return
	 * value, which is based on virtual time, not alarm resets. */
	virtual_now = vcore_account_total_ticks(vcore_id());
	if (vc_tick->next_deadline <= virtual_now) {
		ret = TRUE;
		vc_tick->next_deadline += vc_tick->period_ticks;
		/* It's possible that we've fallen multiple ticks behind virtual now.
		 * In that case, we'll just jump ahead a bit */
		if (virtual_now >= vc_tick->next_deadline)
			vc_tick->next_deadline = virtual_now + vc_tick->period_ticks;
		from_now = vc_tick->next_deadline - virtual_now;
	} else {
		ret = FALSE;
		from_now = virtual_now - vc_tick->next_deadline;
	}
	__vcore_tick_start(vc_tick, from_now);
	return ret;
}
