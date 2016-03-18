#include <stdio.h>
#include <assert.h>
#include <parlib/vcore.h>
#include <parlib/parlib.h>
#include <parlib/mcs.h>
#include <parlib/uthread.h>

mcs_barrier_t b;

void do_work_son(int vcoreid)
{
	int pcoreid = sys_getpcoreid();
	int pid = sys_getpid();
	printf("Hello! My Process ID: %d My VCoreID: %d My CPU: %d\n", pid, vcoreid,
	       pcoreid);
	mcs_barrier_wait(&b,vcoreid);
}

void vcore_entry()
{
	assert(vcore_id() > 0);
	do_work_son(vcore_id());
}

int main(int argc, char** argv)
{
	assert(vcore_id() == 0);
	mcs_barrier_init(&b,max_vcores());
	vcore_request_total(max_vcores());
	do_work_son(0);
	return 0;
}
