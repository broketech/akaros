/* Copyright (c) 2016 Google Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details. */

#include <vmm/sched.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <assert.h>
#include <parlib/spinlock.h>
#include <benchutil/vcore_tick.h>

static struct spin_pdr_lock queue_lock = SPINPDR_INITIALIZER;
/* Runnable queues, broken up by thread type. */
static struct vmm_thread_tq rnbl_tasks = TAILQ_HEAD_INITIALIZER(rnbl_tasks);
static struct vmm_thread_tq rnbl_spins = TAILQ_HEAD_INITIALIZER(rnbl_spins);
static struct vmm_thread_tq rnbl_guests = TAILQ_HEAD_INITIALIZER(rnbl_guests);
/* Counts of *unblocked* threads.  Unblocked = Running + Runnable. */
static int nr_unblk_tasks;
static int nr_unblk_spins;
static int nr_unblk_guests;

static struct guest_thread **guest_threads;


static void vmm_sched_entry(void);
static void vmm_thread_runnable(struct uthread *uth);
static void vmm_thread_paused(struct uthread *uth);
static void vmm_thread_blockon_sysc(struct uthread *uth, void *sysc);
static void vmm_thread_has_blocked(struct uthread *uth, int flags);
static void vmm_thread_refl_fault(struct uthread *uth,
                                  struct user_context *ctx);

struct schedule_ops vmm_sched_ops = {
	.sched_entry = vmm_sched_entry,
	.thread_runnable = vmm_thread_runnable,
	.thread_paused = vmm_thread_paused,
	.thread_blockon_sysc = vmm_thread_blockon_sysc,
	.thread_has_blocked = vmm_thread_has_blocked,
	.thread_refl_fault = vmm_thread_refl_fault,
};

/* event handling? XXX */
static void vmm_handle_syscall(struct event_msg *ev_msg, unsigned int ev_type,
                               void *data);


/* Helpers */
static void enqueue_vmm_thread(struct vmm_thread *vth);
static struct vmm_thread *alloc_vmm_thread(int type);
static void *__alloc_stack(size_t stacksize);
static void __free_stack(void *stacktop, size_t stacksize);

static void __attribute__((constructor)) vmm_lib_init(void)
{
	struct task_thread *thread0;

	init_once_racy(return);
	uthread_lib_init();

	thread0 = (struct task_thread*)alloc_vmm_thread(VMM_THREAD_TASK);
	assert(thread0);
	thread0->stacksize = USTACK_NUM_PAGES * PGSIZE;
	thread0->stacktop = (void*)USTACKTOP;
	spin_pdr_lock(&queue_lock);
	nr_unblk_tasks++;
	spin_pdr_unlock(&queue_lock);

	// XXX syscall event handler  other init?  (we still need to be a 2LS for
	// task threads from startup, before they run vmm_init)

	uthread_2ls_init((struct uthread*)thread0, &vmm_sched_ops);
}

static void __attribute__((noreturn)) run_vmm_thread(struct vmm_thread *vth)
{
	struct guest_thread *gth;

	switch (vth->type) {
	case VMM_THREAD_GUEST:
		gth = (struct guest_thread*)vth;
		/* When scheduling a guest thread, the main part of the 2LS doesn't have
		 * to think about whether it is in the guest or in the ctlr. */
		if (gth->flags & VMM_GTH_FL_CTLR)
			run_uthread((struct uthread*)gth->buddy);
		else
			run_uthread((struct uthread*)gth);
		break;
	case VMM_THREAD_CTLR:
		panic("Shouldn't be able to directly sched a ctlr thread");
		break;
	case VMM_THREAD_TASK:
		run_uthread((struct uthread*)vth);
		break;
	default:
		panic("Bad thread type %d\n", vth->type);
	}
}

static int desired_nr_vcores(void)
{
	/* TODO: if we're being greedy, we want nr_total guest + spins + 1 */
	/* Lockless peak.  This is always an estimate. */
	return nr_unblk_guests + nr_unblk_spins + (nr_unblk_tasks ? 1 : 0);
}

static struct vmm_thread *__pop_first(struct vmm_thread_tq *tq)
{
	struct vmm_thread *vth;

	vth = TAILQ_FIRST(tq);
	if (vth)
		TAILQ_REMOVE(tq, vth, tq_next);
	return vth;
}

// XXX prob rename or break up
static struct vmm_thread *pick_a_thread(void)
{
	struct vmm_thread *vth = 0;

	spin_pdr_lock(&queue_lock);
	if (!vth)
		vth = __pop_first(&rnbl_guests);
	if (!vth)
		vth = __pop_first(&rnbl_spins);
	if (!vth)
		vth = __pop_first(&rnbl_tasks);
	spin_pdr_unlock(&queue_lock);
	return vth;
}

static struct vmm_thread *pick_a_thread_rr(void)
{
	struct vmm_thread *vth = 0;

	spin_pdr_lock(&queue_lock);
	if (!vth)
		vth = __pop_first(&rnbl_tasks);
	if (!vth)
		vth = __pop_first(&rnbl_guests);
	if (!vth)
		vth = __pop_first(&rnbl_spins);
	spin_pdr_unlock(&queue_lock);
	return vth;
}

static void yield_current_uth(void)
{
	struct vmm_thread *vth;

	if (!current_uthread)
		return;
	vth = (struct vmm_thread*)stop_current_uthread();
	enqueue_vmm_thread(vth);
}

static void __attribute__((noreturn)) vmm_sched_entry(void)
{
	struct vmm_thread *vth;
	int nr_vcores_wanted = desired_nr_vcores();

	if (nr_vcores_wanted <= num_vcores()) {
		vcore_tick_disable();
		if (current_uthread)
			run_current_uthread();
		// XXX do we want to alloc a task core?
		// 				if guests and spinners don't block, then for the most
		// 				part our last vcore will just do the task stuff.  but we
		// 				don't know who that is.
		// 					we might want multiple cores doing this (LL vcores)
		// 		lock, assign it.  make sure it is online?  when preempted, do we
		// 		say no one is it?  or does someone else have to take it on?
		// 			generic preemption thing.  we might want to say "i want this
		// 			VC running no matter what".  so the handler could say "fuck
		// 			it, turn me into them.  copyout_uthread, etc."  problem is
		// 			when there are more than one of these cores (deadlock).
		// 				so it's probably better to plan for having only 1 vc.
		// 		nice thing about the task core is we can direct syscall events
		// 		there.
		// 			though there are other syscalls in general...
		//
		// 		say we have a task core.  does that mean no one else does them?
		// 			is that task core allowed to pick up guests?
		// 				presumably no, since we have enough.
		//
		// 	k, task core is vc 0.  is it possible to have enough, but not have
		// 	vc 0?  (yes!  the kernel fills in from the bottom, but we could have
		// 	yielded.  and we expect task_core to yield a lot, but we won't
		// 	always be able to get it back)
		// 			if we dont' get it back, then we have less < nr_wanted
		// 		btw, if we try to yield the task core, then wake back up via
		// 		short handler, we need to have the short handler deal with us
		// 		not getting vcores
		// 			this goes back to having an event if you don't get the nr
		// 			vcores wanted after some time.
		//
		//
		// maybe: 
		// 		try for guests / spinners.
		// 		if none
		// 			now we'll do tasks.  usually there will just be one of us.
		// 			racy write to claim being the task core
		// 				what if there is another who is doing it?  should i only
		// 				do this if there are runnable tasks?
		// 					and if not?  (there might be a non-preempted task
		// 					core already)
		// 						direct to a short handler evq?
		// 							how do we swap back and forth?
		// 						do we keep spinning?  (greed vs nice)
		//
		// 				set the ev_q for all syscalls to me. (again, guests and
		// 				spinners have syscalls).
		// 					who cares about them?  they'll go offline as soon as
		// 					they block
		//
		// 		this is all for syscall notification, right?
		// 			if so, we notify based on the thread type?
		// 			
		// super greedy:
		// 	if enough.  never yield.  assign VCs to tasks.  they spin, waiting
		// 		for those to be runnable.  syscalls get sent to the assigned
		// 		cores.
		//
		// 		this style is harder to change on the fly to adapt to not having
		// 		enough cores.
		// 			want the runqueues anyway
		vth = pick_a_thread();
	} else {
		vcore_tick_enable(10000); // XXX #define / flag / attr
		vcore_request_total(nr_vcores_wanted);
		if (vcore_tick_poll()) {
			/* slightly less than ideal: we grab the lock twice */
			yield_current_uth();
		} else {
			if (current_uthread)
				run_current_uthread();
		}
		vth = pick_a_thread_rr();
	}
	if (!vth)
		vcore_yield_or_restart();
	run_vmm_thread(vth);
}

static void vmm_thread_runnable(struct uthread *uth)
{
	// XXX
	assert(0);
}

static void vmm_thread_paused(struct uthread *uth)
{
	// XXX
	assert(0);
}

static void vmm_thread_blockon_sysc(struct uthread *uth, void *sysc)
{
	// XXX
	assert(0);
}

static void vmm_thread_has_blocked(struct uthread *uth, int flags)
{
	// XXX
	assert(0);
}

static void vmm_thread_refl_fault(struct uthread *uth,
                                  struct user_context *ctx)
{
	// XXX
	assert(0);
}

static void destroy_guest_thread(struct guest_thread *gth)
{
	struct ctlr_thread *cth = gth->buddy;

	__free_stack(cth->stacktop, cth->stacksize);
	uthread_cleanup((struct uthread*)cth);
	free(cth);
	uthread_cleanup((struct uthread*)gth);
	free(gth);
}

static void __ctlr_entry(void)
{
	struct ctlr_thread *cth = (struct ctlr_thread*)current_uthread;
	struct guest_thread *gth = cth->buddy;

	// XXX
	assert(0);
	// set CTLR flag
	// 		actually, handle_refl vm fault should do that
	// handle_vmexit(gth);
	// restart gth, clear CTLR flag
}

static struct guest_thread *create_guest_thread(int gpcoreid)
{
	struct guest_thread *gth;
	struct ctlr_thread *cth;
	struct uth_thread_attr uth_attr = {.want_tls = FALSE};

	gth = (struct guest_thread*)alloc_vmm_thread(VMM_THREAD_GUEST);
	cth = (struct ctlr_thread*)alloc_vmm_thread(VMM_THREAD_CTLR);
	if (!gth || !cth) {
		free(gth);
		free(cth);
		return 0;
	}
	gth->buddy = cth;
	cth->buddy = gth;
	gth->flags = 0;
	cth->stacksize = VMM_THR_STACKSIZE;
	cth->stacktop = __alloc_stack(cth->stacksize);
	if (!cth->stacktop) {
		free(gth);
		free(cth);
		return 0;
	}
	gth->uthread.u_ctx.type = ROS_VM_CTX;
	gth->uthread.u_ctx.tf.vm_tf.tf_guest_pcoreid = gpcoreid;
	init_user_ctx(&cth->uthread.u_ctx, (uintptr_t)&__ctlr_entry,
	              (uintptr_t)(cth->stacktop));
	uthread_init((struct uthread*)gth, &uth_attr);
	uthread_init((struct uthread*)cth, &uth_attr);
	return gth;
}

struct guest_thread **vmm_init(int nr_gpcs, struct vmm_gpcore_init *gpcis,
                               int flags)
{
	struct guest_thread **gths;

	// XXX think about flags.  just pass through?  or some of our own
	if (ros_syscall(SYS_vmm_setup, nr_gpcs, gpcis, flags, 0, 0, 0) != nr_gpcs)
		return 0;
	gths = malloc(nr_gpcs * sizeof(struct guest_thread *));
	if (!gths)
		return 0;
	for (int i = 0; i < nr_gpcs; i++) {
		gths[i] = create_guest_thread(i);
		if (!gths[i]) {
			for (int j = 0; j < i; j++)
				destroy_guest_thread(gths[j]);
			free(gths);
			return 0;
		}
	}

	//uthread_mcp_init(); // XXX prob flag based
	//		XXX and need to save that flag somewhere
	guest_threads = gths;
	return gths;
}

void start_guest_thread(struct guest_thread *gth)
{
	spin_pdr_lock(&queue_lock);
	TAILQ_INSERT_TAIL(&rnbl_guests, (struct vmm_thread*)gth, tq_next);
	nr_unblk_guests++;
	spin_pdr_unlock(&queue_lock);
}

static void __tth_exit_cb(struct uthread *uthread, void *junk)
{
	struct task_thread *tth = (struct task_thread*)uthread;

	uthread_cleanup(uthread);
	__free_stack(tth->stacktop, tth->stacksize);
	free(tth);
}

static void __task_thread_run(void)
{
	struct task_thread *tth = (struct task_thread*)current_uthread;

	tth->func(tth->arg);
	uthread_yield(FALSE, __tth_exit_cb, 0);
}

int vmm_run_task(void (*func)(void *), void *arg)
{
	struct task_thread *tth;
	struct uth_thread_attr uth_attr = {.want_tls = FALSE};

	tth = (struct task_thread*)alloc_vmm_thread(VMM_THREAD_TASK);
	if (!tth)
		return -1;
	tth->stacksize = VMM_THR_STACKSIZE;
	tth->stacktop = __alloc_stack(tth->stacksize);
	if (!tth->stacktop) {
		free(tth);
		return -1;
	}
	tth->func = func;
	tth->arg = arg;
	init_user_ctx(&tth->uthread.u_ctx, (uintptr_t)&__task_thread_run,
	              (uintptr_t)(tth->stacktop));
	uthread_init((struct uthread*)tth, &uth_attr);

	/* TODO: assumes you want a task, not a spinning task */
	// XXX SPIN
	spin_pdr_lock(&queue_lock);
	TAILQ_INSERT_TAIL(&rnbl_tasks, (struct vmm_thread*)tth, tq_next);
	nr_unblk_tasks++;
	spin_pdr_unlock(&queue_lock);
	return 0;
}

static void enqueue_vmm_thread(struct vmm_thread *vth)
{
	struct guest_thread *gth;

	spin_pdr_lock(&queue_lock);
	switch (vth->type) {
	case VMM_THREAD_GUEST:
		TAILQ_INSERT_TAIL(&rnbl_guests, vth, tq_next);
		break;
	case VMM_THREAD_CTLR:
		gth = ((struct ctlr_thread*)vth)->buddy;
		assert(gth->flags & VMM_GTH_FL_CTLR);
		TAILQ_INSERT_TAIL(&rnbl_guests, (struct vmm_thread*)gth, tq_next);
		break;
	case VMM_THREAD_TASK:
		if (((struct task_thread*)vth)->is_spinner)
			TAILQ_INSERT_TAIL(&rnbl_spins, vth, tq_next);
		else
			TAILQ_INSERT_TAIL(&rnbl_tasks, vth, tq_next);
		break;
	}
	spin_pdr_unlock(&queue_lock);
}

static struct vmm_thread *alloc_vmm_thread(int type)
{
	struct vmm_thread *vth;
	int ret;

	ret = posix_memalign((void**)&vth, __alignof__(struct vmm_thread),
	                     sizeof(struct vmm_thread));
	if (ret)
		return 0;
	memset(vth, 0, sizeof(struct vmm_thread));
	vth->type = type;
	return vth;
}

static void __free_stack(void *stacktop, size_t stacksize)
{
	munmap(stacktop - stacksize, stacksize);
}

static void *__alloc_stack(size_t stacksize)
{
	int force_a_page_fault;
	void *stacktop;
	void *stackbot = mmap(0, stacksize, PROT_READ | PROT_WRITE | PROT_EXEC,
	                      MAP_ANONYMOUS, -1, 0);

	if (stackbot == MAP_FAILED)
		return 0;
	stacktop = stackbot + stacksize;
	/* Want the top of the stack populated, but not the rest of the stack;
	 * that'll grow on demand (up to stacksize, then will clobber memory). */
	force_a_page_fault = ACCESS_ONCE(*(int*)(stacktop - sizeof(int)));
	return stacktop;
}
