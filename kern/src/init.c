/* See COPYRIGHT for copyright information. */

#ifdef CONFIG_BSD_ON_CORE0
#error "Yeah, it's not possible to build ROS with BSD on Core 0, sorry......"
#else

#include <arch/arch.h>
#include <arch/topology.h>
#include <arch/console.h>
#include <multiboot.h>
#include <stab.h>
#include <smp.h>

#include <time.h>
#include <atomic.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <monitor.h>
#include <pmap.h>
#include <process.h>
#include <trap.h>
#include <syscall.h>
#include <kclock.h>
#include <manager.h>
#include <testing.h>
#include <kmalloc.h>
#include <hashtable.h>
#include <radix.h>
#include <mm.h>
#include <frontend.h>
#include <ex_table.h>
#include <percpu.h>

#include <arch/init.h>
#include <bitmask.h>
#include <slab.h>
#include <kfs.h>
#include <vfs.h>
#include <devfs.h>
#include <blockdev.h>
#include <ext2fs.h>
#include <kthread.h>
#include <console.h>
#include <linker_func.h>
#include <ip.h>
#include <acpi.h>
#include <coreboot_tables.h>

#define MAX_BOOT_CMDLINE_SIZE 4096

#define ASSIGN_PTRVAL(prm, top, val)			\
	do {										\
		if (prm && (prm < top)) {				\
			*prm = val;							\
			prm++;								\
		}										\
	} while (0)

int booting = 1;
struct proc_global_info __proc_global_info;
struct sysinfo_t sysinfo;
static char boot_cmdline[MAX_BOOT_CMDLINE_SIZE];

static void run_linker_funcs(void);
static int run_init_script(void);

const char *get_boot_option(const char *base, const char *option, char *param,
							size_t max_param)
{
	size_t optlen = strlen(option);
	char *ptop = param + max_param - 1;
	const char *opt, *arg;

	if (!base)
		base = boot_cmdline;
	for (;;) {
		opt = strstr(base, option);
		if (!opt)
			return NULL;
		if (((opt == base) || (opt[-1] == ' ')) &&
			((opt[optlen] == 0) || (opt[optlen] == '=') ||
			 (opt[optlen] == ' ')))
			break;
		base = opt + optlen;
	}
	arg = opt + optlen;
	if (*arg == '=') {
		arg++;
		if (*arg == '\'') {
			arg++;
			for (; *arg; arg++) {
				if (*arg == '\\')
					arg++;
				else if (*arg == '\'')
					break;
				ASSIGN_PTRVAL(param, ptop, *arg);
			}
		} else {
			for (; *arg && (*arg != ' '); arg++)
				ASSIGN_PTRVAL(param, ptop, *arg);
		}
	}
	ASSIGN_PTRVAL(param, ptop, 0);

	return arg;
}

static void extract_multiboot_cmdline(struct multiboot_info *mbi)
{
	if (mbi && (mbi->flags & MULTIBOOT_INFO_CMDLINE) && mbi->cmdline) {
		const char *cmdln = (const char *) KADDR(mbi->cmdline);

		/* We need to copy the command line in a permanent buffer, since the
		 * multiboot memory where it is currently residing will be part of the
		 * free boot memory later on in the boot process.
		 */
		strlcpy(boot_cmdline, cmdln, sizeof(boot_cmdline));
	}
}

void kernel_init(multiboot_info_t *mboot_info)
{
	extern char __start_bss[], __stop_bss[];

	memset(__start_bss, 0, __stop_bss - __start_bss);
	/* mboot_info is a physical address.  while some arches currently have the
	 * lower memory mapped, everyone should have it mapped at kernbase by now.
	 * also, it might be in 'free' memory, so once we start dynamically using
	 * memory, we may clobber it. */
	multiboot_kaddr = (struct multiboot_info*)((physaddr_t)mboot_info
                                               + KERNBASE);
	extract_multiboot_cmdline(multiboot_kaddr);

	cons_init();
	print_cpuinfo();

	printk("Boot Command Line: '%s'\n", boot_cmdline);

	exception_table_init();
	cache_init();					// Determine systems's cache properties
	pmem_init(multiboot_kaddr);
	kmem_cache_init();              // Sets up slab allocator
	kmalloc_init();
	hashtable_init();
	radix_init();
	cache_color_alloc_init();       // Inits data structs
	colored_page_alloc_init();      // Allocates colors for agnostic processes
	acpiinit();
	topology_init();
	percpu_init();
	kthread_init();					/* might need to tweak when this happens */
	vmr_init();
	file_init();
	page_check();
	idt_init();
	kernel_msg_init();
	timer_init();
	vfs_init();
	devfs_init();
	train_timing();
	kb_buf_init(&cons_buf);
	arch_init();
	block_init();
	enable_irq();
	run_linker_funcs();
	/* reset/init devtab after linker funcs 3 and 4.  these run NIC and medium
	 * pre-inits, which need to happen before devether. */
	devtabreset();
	devtabinit();

#ifdef CONFIG_EXT2FS
	mount_fs(&ext2_fs_type, "/dev/ramdisk", "/mnt", 0);
#endif /* CONFIG_EXT2FS */
#ifdef CONFIG_ETH_AUDIO
	eth_audio_init();
#endif /* CONFIG_ETH_AUDIO */
	get_coreboot_info(&sysinfo);
	booting = 0;

#ifdef CONFIG_RUN_INIT_SCRIPT
	if (run_init_script()) {
		printk("Configured to run init script, but no script specified!\n");
		manager();
	}
#else
	manager();
#endif
}

#ifdef CONFIG_RUN_INIT_SCRIPT
static int run_init_script(void)
{
	/* If we have an init script path specified */
	if (strlen(CONFIG_INIT_SCRIPT_PATH_AND_ARGS) != 0) {
		int vargs = 0;
		char *sptr = &CONFIG_INIT_SCRIPT_PATH_AND_ARGS[0];

		/* Figure out how many arguments there are, by finding the spaces */
		/* TODO: consider rewriting this stuff with parsecmd */
		while (*sptr != '\0') {
			if (*(sptr++) != ' ') {
				vargs++;
				while ((*sptr != ' ') && (*sptr != '\0'))
					sptr++;
			}
		}

		/* Initialize l_argv with its first three arguments, but allocate space
		 * for all arguments as calculated above */
		int static_args = 3;
		int total_args = vargs + static_args;
		char *l_argv[total_args];
		l_argv[0] = "";
		l_argv[1] = "busybox";
		l_argv[2] = "ash";

		/* Initialize l_argv with the rest of the arguments */
		int i = static_args;
		sptr = &CONFIG_INIT_SCRIPT_PATH_AND_ARGS[0];
		while (*sptr != '\0') {
			if (*sptr != ' ') {
				l_argv[i++] = sptr;
				while ((*sptr != ' ') && (*sptr != '\0'))
					sptr++;
				if (*sptr == '\0')
					break;
				*sptr = '\0';
			}
			sptr++;
		}

		/* Run the script with its arguments */
		mon_bin_run(total_args, l_argv, NULL);
	}
	return -1;
}
#endif

/*
 * Panic is called on unresolvable fatal errors.
 * It prints "panic: mesg", and then enters the kernel monitor.
 */
void _panic(const char *file, int line, const char *fmt,...)
{
	va_list ap;
	struct per_cpu_info *pcpui;
	/* We're panicing, possibly in a place that can't handle the lock checker */
	pcpui = &per_cpu_info[core_id_early()];
	pcpui->__lock_checking_enabled--;
	va_start(ap, fmt);
	printk("kernel panic at %s:%d, from core %d: ", file, line,
	       core_id_early());
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);

dead:
	monitor(NULL);
	/* We could consider turning the lock checker back on here, but things are
	 * probably a mess anyways, and with it on we would probably lock up right
	 * away when we idle. */
	//pcpui->__lock_checking_enabled++;
	smp_idle();
}

/* like panic, but don't */
void _warn(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	va_start(ap, fmt);
	printk("kernel warning at %s:%d, from core %d: ", file, line,
	       core_id_early());
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);
}

static void run_links(linker_func_t *linkstart, linker_func_t *linkend)
{
	/* Unlike with devtab, our linker sections for the function pointers are
	 * 8 byte aligned (4 on 32 bit) (done by the linker/compiler), so we don't
	 * have to worry about that.  */
	printd("linkstart %p, linkend %p\n", linkstart, linkend);
	for (int i = 0; &linkstart[i] < linkend; i++) {
		printd("i %d, linkfunc %p\n", i, linkstart[i]);
		linkstart[i]();
	}
}

static void run_linker_funcs(void)
{
	run_links(__linkerfunc1start, __linkerfunc1end);
	run_links(__linkerfunc2start, __linkerfunc2end);
	run_links(__linkerfunc3start, __linkerfunc3end);
	run_links(__linkerfunc4start, __linkerfunc4end);
}

/* You need to reference PROVIDE symbols somewhere, or they won't be included.
 * Only really a problem for debugging. */
void debug_linker_tables(void)
{
	extern struct dev __devtabstart[];
	extern struct dev __devtabend[];
	printk("devtab %p %p\nlink1 %p %p\nlink2 %p %p\nlink3 %p %p\nlink4 %p %p\n",
	       __devtabstart,
	       __devtabend,
		   __linkerfunc1start,
		   __linkerfunc1end,
		   __linkerfunc2start,
		   __linkerfunc2end,
		   __linkerfunc3start,
		   __linkerfunc3end,
		   __linkerfunc4start,
		   __linkerfunc4end);
}

#endif //Everything For Free
