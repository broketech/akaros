/* Copyright (c) 2016 Google Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details. */

#pragma once

#include <parlib/uthread.h>
#include <vmm/vmm.h>
#include <sys/queue.h>

__BEGIN_DECLS

/* Three types of threads.  Guests are actual guest VMs.  Controllers are
 * threads that are paired to guests and handles their exits.  Guests and
 * controllers are 1:1 (via *buddy).  Task threads are for the VMM itself, such
 * as a console thread. */

#define VMM_THREAD_GUEST		1
#define VMM_THREAD_CTLR			2
#define VMM_THREAD_TASK			3

#define VMM_GTH_FL_CTLR			0x1

#define VMM_THR_STACKSIZE		8192

struct guest_thread;
struct ctlr_thread;
struct task_thread;

struct guest_thread {
	struct uthread				uthread;
	struct ctlr_thread			*buddy;
	int							flags;
};

struct ctlr_thread {
	struct uthread				uthread;
	struct guest_thread			*buddy;
	size_t						stacksize;
	void						*stacktop;
};

struct task_thread {
	struct uthread				uthread;
	void						(*func)(void *);
	void						*arg;
	size_t						stacksize;
	void						*stacktop;
	bool						is_spinner;
};

struct vmm_thread {
	union {
		struct guest_thread		guest;
		struct ctlr_thread		ctlr;
		struct task_thread		task;
	};
	int							type;
	TAILQ_ENTRY(vmm_thread)		tq_next;
};

TAILQ_HEAD(vmm_thread_tq, vmm_thread);

/* Initialize a VMM for a collection of guest cores.  Returns an array of
 * guest_threads * on success, 0 on failure.  Do not free() the array.
 *
 * XXX: consider doing all the gpci shit internally.
 * 		if we did, would they even need that array?
 * 			need to set fields in the ctx
 * XXX: what flags?  vmm flags?  some of our own, to control the 2LS? */
struct guest_thread **vmm_init(int nr_gpcs, struct vmm_gpcore_init *gpcis,
                               int flags);
/* Starts a guest thread/core. */
void start_guest_thread(struct guest_thread *gth);
/* Start and run a task thread. */
int vmm_run_task(void (*func)(void *), void *arg);



/* 

minor shit
---------------------------------
arch indep setters for pc, sp, etc?

join on task threads?  what does thread0 do?  if it returns, the program ends
	sleep forever on a double locked mtx.
	how does the process end?  who calls exit?
		can have some vmexits trigger it.
		maybe when the OS powers off?

helpers for injecting traps into remote GPCs
	e.g. handle send_ipi exit

signals?
	these are intra-process, per thread.  doesn't seem necessary.
		hope there's not some weird glibc shit, like needing to send SIGBUS 

lib init, mcp init  (who decides if we're an MCP?)
	goes back to where do we customize this.  in the app/program or not.

is this header used by all vmms?  (probably).
	all 2LSs use the same sched.c too?
		with diffs in the actual vmm code? 
			(so they might want control over sched ops?)
			this is a generic 2LS question, like with kweb
				where do you put the ultra customizations? 
				this is a little diff, since we have a user/vmm library
				unlike kweb, where it was all in the app
	or rather, there's one VMM.  slight customization in apps.
		all that ACPI and bootup shit will move into the lib
		same with the vm exit handling?

major shit
---------------------------------
blocked syscall mgmt stuff

preemption handling...
	timer irqs, multiplexing, knowing when we get more cores to stop multiplexing
	general scheduling plan
		what if they want to spin in their task threads?
		what if a ctlr thread (or possibly a guest thread) blocks?
			yield vcore for now, try to get it later?
				hard to ask for a particular vcore (kernel change?)
					(say we have holes at 3 and 5.  we want 5.  we get 3. 
						change_to 5?  (stuff could be preempted in btw, so we're
						never sure what we have))
		threads exit?

	have an array of guest threads. 
		can have a VC be responsible for each.
			or multiplex

have_enough_vc
  	nr_task_cores = nr_nonblocked_spinning_tasks +
  	                (nonblocked_tasks ? 1 : 0)
	num_vcores >= nr_guest_cores + nr_task_cores

		do we only need/want one vcore for tasks?

	tempted to cache this, and update on preemption
		but the nr of runnable/active might change
		(e.g. ctlr thread might block and unblock)
			maybe control caching with an invalidate flag
			that gets triggered on preempt, runnable

need to handle changes in have_enough_vc
	we thought we had enough, then either due to an unblock or a
	preempt, we now want more (thread_runnable)
		we can put in a request, but what if we don't get it?
			maybe a long-lived timer/event?
		general problem for all 2LS?
		even if we immediately got it, we could get preempted.
		but that would at least tell us 
	run "decide" on runnable (which covers preempt) and entry
		possible caching

		XXX when a thread becomes runnable, our desired_nr changes.  we need to recheck and possible start another timer.
			and every time we do this, technically everyone should!


	maybe we could have a kernel service that sends an event if you
	DONT have amt wanted.  can sign up for the event.  ksched can
	do it on the timer tick.  (so no alarm/cancel alarm)
		- we can do this, though it depends on the ksched polling all MCPSs
			- would like to not assume this.  we could have a ksched that only
			responds to freed cores.
		- could have this be a response to the poke_ call
			or maybe even the ret value from poke?
				no - poke just makes sure someone is looking
					or we change how poke works
			but we could have the ksched send an event every time it failed to
			max out a proc's alloc
				- use a CEQ or bitmsg
				- might need this for short handlers too.  we want a new core,
				but if we don't get it, we need to interrupt one of our other
				cores.  we didn't get a preempt event, since we didn't lose
				anything.  but we did fail to get something.
				
	
	there are two purposes to the alarm
		- 1 give us a chance to see if we have the right nr cores
			- this was so we could toggle our behavior
			- wanted to ask for the cores and then assume that we'd get them
		- 2 if we don't have the right number, then run the multiplexer
			- we only need one of these.  we need N of the muxers.
		- if we don't have #1, we have to assume we won't get them.
			- as soon as we get 1 event for the muxer, we can turn it off
				- just per core

	* XXX some sort of affinity?  like a particular vcore just does a single
	 * guest core?
	 * 		if we say that VC X should do syscalls, then we always do tasks on
	 * 		VC X, and never do guests (o/w we get exits).
	 * 			this affects where we send syscall completions
	 * 				(assuming we don't poll!)
	 * 		if we pick this particular vcore, then we need to handle it going
	 * 		offline.  could update the desired vc in the (indir) evq->vcore
	 * 			so we need to check if there is a task vc or not?
	 * 				and what about spinners?  do they use their local vc?
	 * 				and guests?
	 * 		is there a distinction btw guests and spins?  both are dedicated
	 * 		spinning threads
	 * 			not really a distinction for the sched, though other scheds
	 * 			might
	// XXX might not be the best data structure.  we'd like to match the
	// topology of the guest to the underlying host
	// 		could look in the array to check the status of the particular GPC
	// 		(i.e. GPC x -> PC x).  though that will always suck since we don't
	// 		have PC 0.
	// want to know the vcore where a particular guest_thread is running


handling preemption
- when we get preempted, we probably need to force ourselves into sched_entry at
some point, so that we can readjust the tick (turn it on)
	- does this need to happen across all vcores?
		- say vc 1 is preempted.  vc 2 got the message and turned on its tick.
		vc 3+ didn't find out, and will just keep on spinning.
			- so we'll need to turn on their ticks.
				- pvc alarm style would work, but now we race a lot
				- easier to just broadcast __notify them all.
					-^^^^^^ THIS

	thread_paused callback:
		 this also doesn't tell us we lost a core.  just that a thread was paused.

		 if we just had to notice sched_entry, then everyone would be racing to
		 broadcast.
		
		 things would be simpler if we had a callback that said "you lost core X.
		 we'll take care of most things for parlib."
		 		even saying this might be tricky.  when we do a change_to, we're
		 		preempting ourselves.  but we might not go away permanently.
		 			(could branch in handle_preempt based on whether we change_to
		
		 		so it's more of a "you might lose vcore X, check your shit"

	 * 
*/

__END_DECLS
