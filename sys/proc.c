/*
 * This file has been modified as part of the FreeMiNT project. See
 * the file Changes.MH for details and dates.
 */

/*
 * Copyright 1990,1991,1992 Eric R. Smith.
 * Copyright 1992,1993,1994 Atari Corporation.
 * All rights reserved.
 */

/* routines for handling processes */

# include "proc.h"
# include "global.h"

# include "libkern/libkern.h"

# include "mint/asm.h"
# include "mint/credentials.h"
# include "mint/filedesc.h"
# include "mint/basepage.h"
# include "mint/resource.h"
# include "mint/signal.h"

# include "arch/context.h"	/* save_context, change_context */
# include "arch/kernel.h"
# include "arch/mprot.h"

# include "bios.h"
# include "dosfile.h"
# include "dosmem.h"
# include "fasttext.h"
# include "filesys.h"
# include "k_exit.h"
# include "kmemory.h"
# include "memory.h"
# include "proc_help.h"
# include "random.h"
# include "signal.h"
# include "time.h"
# include "timeout.h"
# include "random.h"
# include "util.h"
# include "xbios.h"

# include <osbind.h>


static void swap_in_curproc	(void);
static void do_wakeup_things	(short sr, int newslice, long cond);
INLINE void do_wake		(int que, long cond);
INLINE ulong gen_average	(ulong *sum, uchar *load_ptr, ulong max_size);


/*
 * We initialize proc_clock to a very large value so that we don't have
 * to worry about unexpected process switches while starting up
 */
ushort proc_clock = 0x7fff;


/* global process variables */
PROC *proclist = NULL;		/* list of all active processes */
PROC *curproc  = NULL;		/* current process		*/
PROC *rootproc = NULL;		/* pid 0 -- MiNT itself		*/
PROC *sys_q[NUM_QUEUES] =
{
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* default; actual value comes from mint.cnf */
short time_slice = 2;


/*
 * initialize the process table
 */
void
init_proc (void)
{
	static DTABUF dta;
	
	static struct proc	rootproc0;
	static struct memspace	mem0;
	static struct ucred	ucred0;
	static struct pcred	pcred0;
	static struct filedesc	fd0;
	static struct cwd	cwd0;
	static struct sigacts	sigacts0;
	static struct plimit	limits0;
	
	/* XXX */
	bzero (&rootproc0, sizeof (rootproc0));
	bzero (&mem0, sizeof (mem0));
	bzero (&ucred0, sizeof (ucred0));
	bzero (&pcred0, sizeof (pcred0));
	bzero (&fd0, sizeof (fd0));
	bzero (&cwd0, sizeof (cwd0));
	bzero (&sigacts0, sizeof (sigacts0));
	bzero (&limits0, sizeof (limits0));
	
	pcred0.ucr = &ucred0;			ucred0.links = 1;
	
	rootproc0.p_mem		= &mem0;	mem0.links = 1;
	rootproc0.p_cred	= &pcred0;	pcred0.links = 1;
	rootproc0.p_fd		= &fd0;		fd0.links = 1;
	rootproc0.p_cwd		= &cwd0;	cwd0.links = 1;
	rootproc0.p_sigacts	= &sigacts0;	sigacts0.links = 1;
//	rootproc0.p_limits	= &limits0;	limits0.links = 1;
	
	fd0.ofiles = fd0.dfiles;
	fd0.ofileflags = fd0.dfileflags;
	fd0.nfiles = NDFILE;
	
	DEBUG (("%lx, %lx, %lx, %lx, %lx, %lx, %lx",
		&rootproc0, &mem0, &pcred0, &ucred0, &fd0, &cwd0, &sigacts0));
	
	rootproc = curproc = &rootproc0;	rootproc0.links = 1;
	
	/* set the stack barrier */
	curproc->stack_magic = STACK_MAGIC;
	
	curproc->ppid = -1;		/* no parent */
	curproc->domain = DOM_TOS;	/* TOS domain */
	curproc->sysstack = (long) (curproc->stack + STKSIZE - 12);
	curproc->magic = CTXT_MAGIC;
	curproc->memflags = F_PROT_S;	/* default prot mode: super-only */
	
	((long *) curproc->sysstack)[1] = FRAME_MAGIC;
	((long *) curproc->sysstack)[2] = 0;
	((long *) curproc->sysstack)[3] = 0;
	
	curproc->dta = &dta;		/* looks ugly */
	curproc->base = _base;
	strcpy (curproc->name, "MiNT");
	
	/* get some memory */
	curproc->p_mem->num_reg = NUM_REGIONS;
	curproc->p_mem->mem = kmalloc (curproc->p_mem->num_reg * sizeof (MEMREGION *));
	curproc->p_mem->addr = kmalloc (curproc->p_mem->num_reg * sizeof (virtaddr));
	
	/* make sure kmalloc was successful */
	assert (curproc->p_mem->mem && curproc->p_mem->addr);
	
	/* make sure it's filled with zeros */
	bzero (curproc->p_mem->mem, curproc->p_mem->num_reg * sizeof (MEMREGION *));
	bzero (curproc->p_mem->addr, curproc->p_mem->num_reg * sizeof (virtaddr));
	
	/* get root and current directories for all drives */
	{
		FILESYS *fs;
		int i;
		
		for (i = 0; i < NUM_DRIVES; i++)
		{
			fcookie dir;
			
			fs = drives [i];
			if (fs && xfs_root (fs, i, &dir) == E_OK)
			{
				curproc->p_cwd->root[i] = dir;
				dup_cookie (&curproc->p_cwd->curdir[i], &dir);
			}
			else
			{
				curproc->p_cwd->root[i].fs = curproc->p_cwd->curdir[i].fs = 0;
				curproc->p_cwd->root[i].dev = curproc->p_cwd->curdir[i].dev = i;
			}
		}
	}
	
	init_page_table_ptr (curproc->p_mem);
	init_page_table (curproc, curproc->p_mem);
	
	/* Set the correct drive. The current directory we
	 * set later, after all file systems have been loaded.
	 */
	curproc->p_cwd->curdrv = Dgetdrv ();
	proclist = curproc;
	
	curproc->p_cwd->cmask = 0;
	
	/* some more protection against job control; unless these signals are
	 * re-activated by a shell that knows about job control, they'll have
	 * no effect
	 */
# if 1
	SIGACTION(curproc, SIGTTIN).sa_handler = SIG_IGN;
	SIGACTION(curproc, SIGTTOU).sa_handler = SIG_IGN;
	SIGACTION(curproc, SIGTSTP).sa_handler = SIG_IGN;
# else
	curproc->sighandle[SIGTTIN] =
	curproc->sighandle[SIGTTOU] =
	curproc->sighandle[SIGTSTP] = SIG_IGN;
# endif
	
	/* set up some more per-process variables */
	curproc->started = xtime;
	
	if (has_bconmap)
		/* init_xbios not happened yet */
		curproc->p_fd->bconmap = (int) Bconmap (-1);
	else
		curproc->p_fd->bconmap = 1;
	
	curproc->logbase = (void *) Logbase();
	curproc->criticerr = *((long _cdecl (**)(long)) 0x404L);
}

/* reset_priorities():
 * 
 * reset all process priorities to their base level
 * called once per second, so that cpu hogs can get _some_ time
 * slices :-).
 */
void
reset_priorities (void)
{
	PROC *p;
	
	for (p = proclist; p; p = p->gl_next)
	{
		if (p->slices >= 0)
		{
			p->curpri = p->pri;
			p->slices = SLICES (p->curpri);
		}
	}
}

/* run_next(p, slices):
 * 
 * schedule process "p" to run next, with "slices" initial time slices;
 * "p" does not actually start running until the next context switch
 */
void
run_next (PROC *p, int slices)
{
	register ushort sr;
	
	sr = spl7 ();
	
	p->slices = -slices;
	p->curpri = MAX_NICE;
	p->wait_q = READY_Q;
	p->q_next = sys_q[READY_Q];
	sys_q[READY_Q] = p;
	
	spl (sr);
}

/* fresh_slices(slices):
 * 
 * give the current process "slices" more slices in which to run
 */
void
fresh_slices (int slices)
{
	reset_priorities ();
	
	curproc->slices = 0;
	curproc->curpri = MAX_NICE + 1;
	proc_clock = time_slice + slices;
}

/*
 * add a process to a wait (or ready) queue.
 *
 * processes go onto a queue in first in-first out order
 */

void
add_q (int que, PROC *proc)
{
	PROC *q, **lastq;
	
	/* "proc" should not already be on a list */
	assert (proc->wait_q == 0);
	assert (proc->q_next == 0);
	
	lastq = &sys_q[que];
	q = *lastq;
	while (q)
	{
		lastq = &q->q_next;
		q = *lastq;
	}
	*lastq = proc;
	
	proc->wait_q = que;
	if (que != READY_Q && proc->slices >= 0)
	{
		proc->curpri = proc->pri;	/* reward the process */
		proc->slices = SLICES (proc->curpri);
	}
}

/*
 * remove a process from a queue
 */

void
rm_q (int que, PROC *proc)
{
	PROC *q;
	PROC *old = 0;
	
	assert (proc->wait_q == que);
	
	q = sys_q[que];
	while (q && q != proc)
	{
		old = q;
		q = q->q_next;
	}
	
	if (q == 0)
		FATAL ("rm_q: unable to remove process from queue");
	
	if (old)
		old->q_next = proc->q_next;
	else
		sys_q[que] = proc->q_next;
	
	proc->wait_q = 0;
	proc->q_next = 0;
}

/*
 * preempt(): called by the vbl routine and/or the trap handlers when
 * they detect that a process has exceeded its time slice and hasn't
 * yielded gracefully. For now, it just does sleep(READY_Q); later,
 * we might want to keep track of statistics or something.
 */

void _cdecl
preempt (void)
{
	if (bconbsiz)
	{
		bflush ();
	}
	else
	{
		/* punish the pre-empted process */
		if (curproc->curpri >= MIN_NICE)
			curproc->curpri -= 1;
	}
	
	sleep (READY_Q, curproc->wait_cond);
}

/*
 * swap_in_curproc(): for all memory regions of the current process swaps
 * in the contents of those regions that have been saved in a shadow region
 */

static void
swap_in_curproc (void)
{
	struct memspace *mem = curproc->p_mem;
	long txtsize = curproc->p_mem->txtsize;
	MEMREGION *m, *shdw, *save;
	int i;
	
	for (i = 0; i < mem->num_reg; i++)
	{
		m = mem->mem[i];
		if (m && m->save)
		{
			save = m->save;
			for (shdw = m->shadow; shdw->save; shdw = shdw->shadow)
				assert (shdw != m);
			
			assert (m->loc == shdw->loc);
			
			shdw->save = save;
			m->save = 0;
			if (i != 1 || txtsize == 0)
			{
				quickswap ((char *) m->loc, (char *) save->loc, m->len);
			}
			else
			{
				quickswap ((char *) m->loc, (char *) save->loc, 256);
				quickswap ((char *) m->loc + (txtsize+256), (char *) save->loc + 256, m->len - (txtsize+256));
			}
		}
	}
}

/*
 * sleep(que, cond): put the current process on the given queue, then switch
 * contexts. Before a new process runs, give it a fresh time slice. "cond"
 * is the condition for which the process is waiting, and is placed in
 * curproc->wait_cond
 */

INLINE void
do_wakeup_things (short sr, int newslice, long cond)
{
	/*
	 * check for stack underflow, just in case
	 */
	auto int foo;
	PROC *p;
	
	p = curproc;
	
	if ((sr & 0x700) < 0x500)
	{
		/* skip all this if int level is too high */
		if (p->pid && ((long) &foo) < (long) p->stack + ISTKSIZE + 512)
		{
			ALERT ("stack underflow");
			handle_sig (SIGBUS);
		}
		
		/* see if process' time limit has been exceeded */
		if (p->maxcpu)
		{
			if (p->maxcpu <= p->systime + p->usrtime)
			{
				DEBUG (("cpu limit exceeded"));
				raise (SIGXCPU);
			}
		}
		
		/* check for alarms and similar time out stuff */
		checkalarms ();
		
		if (p->sigpending && cond != (long) sys_pwaitpid)
			/* check for signals */
			check_sigs ();
	}
	
	if (newslice)
	{
		if (p->slices >= 0)
		{
			/* get a fresh time slice */
			proc_clock = time_slice;
		}
		else
		{
			/* slices set by run_next */
			proc_clock = time_slice - p->slices;
			p->curpri = p->pri;
		}
		
		p->slices = SLICES (p->curpri);
	}
}

static long sleepcond, iwakecond;

/*
 * sleep: returns 1 if no signals have happened since our last sleep, 0
 * if some have
 */

int _cdecl 
sleep (int _que, long cond)
{
	PROC *p;
	ushort sr;
	short que = _que & 0xff;
	ulong onsigs = curproc->nsigs;
	int newslice = 1;
	
	/* save condition, checkbttys may just wake() it right away ...
	 * note this assumes the condition will never be waked from interrupts
	 * or other than thru wake() before we really went to sleep, otherwise
	 * use the 0x100 bit like select
	 */
	sleepcond = cond;
	
	/* if there have been keyboard interrupts since our last sleep,
	 * check for special keys like CTRL-ALT-Fx
	 */
	sr = splhigh ();
	if ((sr & 0x700) < 0x500)
	{
		/* can't call checkkeys if sleep was called
		 * with interrupts off  -nox
		 */
		spl (sr);
		(void) checkbttys ();
		if (kintr)
		{
			(void) checkkeys ();
			kintr = 0;
		}
		
# ifdef DEV_RANDOM		
		/* Wake processes waiting for random bytes */
		checkrandom ();
# endif
		
		sr = splhigh ();
		if ((curproc->sigpending & ~(curproc->p_sigmask))
			&& curproc->pid && que != ZOMBIE_Q && que != TSR_Q)
		{
			spl (sr);
			check_sigs ();
			sleepcond = 0;	/* possibly handled a signal, return */
			sr = spl7 ();
		}
	}
	
	/* kay: If _que & 0x100 != 0 then take curproc->wait_cond != cond as
	 * an indicatation that the wakeup has already happend before we
	 * actually go to sleep and return immediatly.
	 */
	if ((que == READY_Q && !sys_q[READY_Q])
		|| ((sleepcond != cond || (iwakecond == cond && cond) || (_que & 0x100 && curproc->wait_cond != cond))
			&& (!sys_q[READY_Q] || (newslice = 0, proc_clock))))
	{
		/* we're just going to wake up again right away! */
		iwakecond = 0;
		spl (sr);
		do_wakeup_things (sr, newslice, cond);
		
		return (onsigs != curproc->nsigs);
	}
	
	/* unless our time slice has expired (proc_clock == 0) and other
	 * processes are ready...
	 */
	iwakecond = 0;
	if (!newslice)
		que = READY_Q;
	else
		curproc->wait_cond = cond;
	
	add_q (que, curproc);
	
	/* alright curproc is on que now... maybe there's an
	 * interrupt pending that will wakeselect or signal someone
	 */
	spl (sr);
	
	if (!sys_q[READY_Q])
	{
		/* hmm, no-one is ready to run. might be a deadlock, might not.
		 * first, try waking up any napping processes;
		 * if that doesn't work, run the root process,
		 * just so we have someone to charge time to.
		 */
		wake (SELECT_Q, (long) nap);
		
		sr = splhigh ();
		if (!sys_q[READY_Q])
		{
			p = rootproc;		/* pid 0 */
			rm_q (p->wait_q, p);
			add_q (READY_Q, p);
		}
		spl (sr);
	}
	
	/*
	 * Walk through the ready list, to find what process should run next.
	 * Lower priority processes don't get to run every time through this
	 * loop; if "p->slices" is positive, it's the number of times that
	 * they will have to miss a turn before getting to run again
	 *
	 * Loop structure:
	 *	while (we haven't picked anybody)
	 *	{
	 *		for (each process)
	 *		{
	 *			if (sleeping off a penalty)
	 *			{
	 *				decrement penalty counter
	 *			}
	 *			else
	 *			{
	 *				pick this one and break out of
	 *				both loops
	 *			}
	 *		}
	 *	}
	 */
	sr = splhigh ();
	p = 0;
	while (!p)
	{
		for (p = sys_q[READY_Q]; p; p = p->q_next)
		{
			if (p->slices > 0)
				p->slices--;
			else
				break;
		}
	}
	/* p is our victim */
	rm_q (READY_Q, p);
	spl (sr);
	
	if (save_context(&(curproc->ctxt[CURRENT])))
	{
		/*
		 * restore per-process variables here
		 */
# ifndef MULTITOS
# ifdef FASTTEXT
		if (!hardscroll)
			*((void **) 0x44eL) = curproc->logbase;
# endif
# endif
		swap_in_curproc ();
		do_wakeup_things (sr, 1, cond);
		
		return (onsigs != curproc->nsigs);
	}
	
	/*
	 * save per-process variables here
	 */
# ifndef MULTITOS
# ifdef FASTTEXT
	if (!hardscroll)
		curproc->logbase = *((void **) 0x44eL);
# endif
# endif
	
	curproc->ctxt[CURRENT].regs[0] = 1;
	curproc = p;
	
	proc_clock = time_slice;			/* fresh time */
	
	if ((p->ctxt[CURRENT].sr & 0x2000) == 0)	/* user mode? */
		leave_kernel ();
	
	assert (p->magic == CTXT_MAGIC);
	change_context (&(p->ctxt[CURRENT]));
	
	/* not reached */
	return 0;
}

/*
 * wake(que, cond): wake up all processes on the given queue that are waiting
 * for the indicated condition
 */

INLINE void
do_wake (int que, long cond)
{
	PROC *p;
top:
	for (p = sys_q[que]; p; )
	{
		PROC *q;
		register short s;
		
		s = spl7 ();
		
		/* check p is still on the right queue,
		 * maybe an interrupt just woke it...
		 */
		if (p->wait_q != que)
		{
			spl (s);
			goto top;
		}
		
		q = p;
		p = p->q_next;
		if (q->wait_cond == cond)
		{
			rm_q (que, q);
			add_q (READY_Q, q);
		}
		
		spl (s);
	}
}

void _cdecl 
wake (int que, long cond)
{
	if (que == READY_Q)
	{
		ALERT ("wake: why wake up ready processes??");
		return;
	}
	
	if (sleepcond == cond)
		sleepcond = 0;
	
	do_wake (que, cond);
}

/*
 * iwake(que, cond, pid): special version of wake() for IO interrupt
 * handlers and such.  the normal wake() would lose when its
 * interrupt goes off just before a process is calling sleep() on the
 * same condition (similar problem like with wakeselect...)
 *
 * use like this:
 *	static ipid = -1;
 *	static volatile sleepers = 0;	(optional, to save useless calls)
 *	...
 *	device_read(...)
 *	{
 *		ipid = curproc->pid;	(p_getpid() for device drivers...)
 *		while (++sleepers, (not ready for IO...)) {
 *			sleep(IO_Q, cond);
 *			if (--sleepers < 0)
 *				sleepers = 0;
 *		}
 *		if (--sleepers < 0)
 *			sleepers = 0;
 *		ipid = -1;
 *		...
 *	}
 *
 * and in the interrupt handler:
 *	if (sleepers > 0)
 *	{
 *		sleepers = 0;
 *		iwake (IO_Q, cond, ipid);
 *	}
 *
 * caller is responsible for not trying to wake READY_Q or other nonsense :)
 * and making sure the passed pid is always -1 when curproc is calling
 * sleep() for another than the waked que/condition.
 */

void _cdecl 
iwake (int que, long cond, short pid)
{
	if (pid >= 0)
	{
		register ushort s;
		
		s = spl7 ();
		
		if (iwakecond == cond)
		{
			spl (s);
			return;
		}
		
		if (curproc->pid == pid && !curproc->wait_q)
			iwakecond = cond;
		
		spl (s);
	}
	
	do_wake (que, cond);
}

/*
 * wakeselect(p): wake process p from a select() system call
 * may be called by an interrupt handler or whatever
 */

void _cdecl 
wakeselect (PROC *p)
{
	short s;
	
	s = spl7 ();
	
	if (p->wait_cond == (long) wakeselect
		|| p->wait_cond == (long) &select_coll)
	{
		p->wait_cond = 0;
	}
	
	if (p->wait_q == SELECT_Q)
	{
		rm_q (SELECT_Q, p);
		add_q (READY_Q, p);
	}
	
	spl (s);
}

/*
 * dump out information about processes
 */

/*
 * kludge alert! In order to get the right pid printed by FORCE, we use
 * curproc as the loop variable.
 *
 * I have changed this function so it is more useful to a user, less to
 * somebody debugging MiNT.  I haven't had any stack problems in MiNT
 * at all, so I consider all that stack info wasted space.  -- AKP
 */

# ifdef DEBUG_INFO
static const char *qstring[] =
{
	"run", "ready", "wait", "iowait", "zombie", "tsr", "stop", "select"
};

/* UNSAFE macro for qname, evaluates x 1, 2, or 3 times */
# define qname(x) ((x >= 0 && x < NUM_QUEUES) ? qstring[x] : "unkn")
# endif

ulong uptime = 0;
ulong avenrun[3] = { 0, 0, 0 };
ushort uptimetick = 200;	

static ushort number_running;

void
DUMPPROC (void)
{
# ifdef DEBUG_INFO
	PROC *p = curproc;

	FORCE ("Uptime: %ld seconds Loads: %ld %ld %ld Processes running: %d",
		uptime,
		(avenrun[0] * 100) / 2048 , (avenrun[1] * 100) / 2048, (avenrun[2] * 100 / 2048),
 		number_running);

	for (curproc = proclist; curproc; curproc = curproc->gl_next)
	{
		FORCE ("state %s PC: %lx BP: %lx",
			qname(curproc->wait_q),
			curproc->ctxt[SYSCALL].pc,
			curproc->base);
	}
	curproc = p;	/* restore the real curproc */
# endif
}

INLINE ulong
gen_average (ulong *sum, uchar *load_ptr, ulong max_size)
{
	register long old_load = (long) *load_ptr;
	register long new_load = number_running;
	
	*load_ptr = (uchar) new_load;
	
	*sum += (new_load - old_load) * LOAD_SCALE;
	
	return (*sum / max_size);
}

void
calc_load_average (void)
{
	static uchar one_min [SAMPS_PER_MIN];
	static uchar five_min [SAMPS_PER_5MIN];
	static uchar fifteen_min [SAMPS_PER_15MIN];
	
	static ushort one_min_ptr = 0;
	static ushort five_min_ptr = 0;
	static ushort fifteen_min_ptr = 0;
	
	static ulong sum1 = 0;
	static ulong sum5 = 0;
	static ulong sum15 = 0;
	
	register PROC *p;
	
# if 0	/* moved to intr.spp */
	uptime++;
	uptimetick += 200;
	
	if (uptime % 5) return;
# endif
	
	number_running = 0;
	
	for (p = proclist; p; p = p->gl_next)
	{
		if (p != rootproc)
		{
			if ((p->wait_q == CURPROC_Q) || (p->wait_q == READY_Q))
				number_running++;
		}
		
		/* Check the stack magic here, to ensure the system/interrupt
		 * stack hasn't grown too much. Most noticeably, NVDI 5's new
		 * bitmap conversion (vr_transfer_bits()) seems to eat _a lot_
		 * of supervisor stack, that's why the values in proc.h have
		 * been increased.
		 */
		if (p->stack_magic != STACK_MAGIC)
			FATAL ("proc %lx has invalid stack_magic %lx", (long) p, p->stack_magic);
	}
	
	if (one_min_ptr == SAMPS_PER_MIN)
		one_min_ptr = 0;
	
	avenrun [0] = gen_average (&sum1, &one_min [one_min_ptr++], SAMPS_PER_MIN);
	
	if (five_min_ptr == SAMPS_PER_5MIN)
		five_min_ptr = 0;
	
	avenrun [1] = gen_average (&sum5, &five_min [five_min_ptr++], SAMPS_PER_5MIN);
	
	if (fifteen_min_ptr == SAMPS_PER_15MIN)
		fifteen_min_ptr = 0;
	
	avenrun [2] = gen_average (&sum15, &fifteen_min [fifteen_min_ptr++], SAMPS_PER_15MIN);
}
