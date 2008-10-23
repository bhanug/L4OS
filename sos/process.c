#include <clock/clock.h>
#include <sos/sos.h>
#include <string.h>

#include "constants.h"
#include "frames.h"
#include "l4.h"
#include "libsos.h"
#include "list.h"
#include "process.h"
#include "region.h"
#include "syscall.h"

#define STDOUT_FN "console"
#define PS_PLACEHOLDER (Process *) (0x00000001)

#define WAIT_ANYBODY (-1)
#define WAIT_NOBODY (-2)

#define verbose 1

// The equivalent of a PCB
struct Process_t {
	process_t     info;
	Pagetable    *pagetable;
	List         *regions;
	void         *sp;
	void         *ip;
	timestamp_t   startedAt;
	// 1st level, fds, allows for redirection of files and dup2
	fildes_t      fds[PROCESS_MAX_FDS];
	// 2nd level, open files
	VFile         files[PROCESS_MAX_FILES];
	pid_t         waitingOn;
};

// Array of all PCBs
static Process *sosProcs[MAX_ADDRSPACES];

// The next pid to allocate
static pid_t nextPid;

// All pids below this value have already been allocated by libsos
// as threadids, and all over SOS it has been assumed that there is
// a 1:1 mapping between pid and tid (and sid incidentally), so just
// never allocate a pid below this value.
static pid_t tidOffset;

Process *process_lookup(pid_t key) {
	if (key < 0 || key >= MAX_ADDRSPACES) {
		dprintf(0, "!!! process_lookup(%d): outside range!\n", key);
	}

//	// for multithreaded SOS kernel
//	while(sosProcs[key] == NULL ) {
//		key--;
//	}

	// for standard single threaded user apps
	return sosProcs[key];
}

static Process *processAlloc(void) {
	Process *p = (Process*) malloc(sizeof(Process));

	p->info.pid = NIL_PID;   // decide later
	p->info.size = 0;  // fill in as we go
	p->info.stime = 0; // decide later
	p->info.ctime = 0; // don't ever need
	p->info.command[0] = '\0';
	p->info.ps_type = PS_TYPE_PROCESS;
	p->info.ipc_accept = PS_IPC_ALL;

	p->pagetable = pagetable_init();
	p->regions = list_empty();
	p->sp = NULL;
	p->ip = NULL;
	vfiles_init(p->fds, p->files);
	p->waitingOn = WAIT_NOBODY;

	process_set_state(p, PS_STATE_START);

	return p;
}

Process *process_init(process_type_t type) {
	// On the first process initialisation set the offset to whatever it is
	// and disable libsos from allocation any more threads
	if (tidOffset == 0) {
		tidOffset = (pid_t) L4_ThreadNo(sos_peek_new_tid());
		sos_get_new_tid_disable();
		nextPid = tidOffset;
	}

	// Do the normal process initialisation
	Process *p = processAlloc();
	p->info.ps_type = type;
	return p;
}

void process_add_region(Process *p, Region *r) {
	list_push(p->regions, r);
}

void process_set_ip(Process *p, void *ip) {
	p->ip = ip;
}

void process_set_sp(Process *p, void *sp) {
	// Set the stack pointer to something else -
	// Only possible AFTER process_prepare and
	// BEFORE process_run.  Moving the sp is
	// necessary if a process is to run without
	// the use of virtual memory, although it
	// will then need to manage its memory
	// manually.  
	p->sp = sp;
}

void process_set_name(Process *p, const char *name) {
	strncpy(p->info.command, name, MAX_FILE_NAME);
	L4_KDB_SetThreadName(process_get_tid(p), name);
}

void process_set_state(Process *p, process_state_t state) {
	p->info.state = state;
}

void process_set_ipcfilt(Process *p, process_ipcfilt_t ipc_filt) {
	p->info.ipc_accept = ipc_filt;
}

process_state_t process_get_state(Process *p) {
	return p->info.state;
}

process_ipcfilt_t process_get_ipcfilt(Process *p) {
	return p->info.ipc_accept;
}

static void *regionFindHighest(void *contents, void *data) {
	Region *r = (Region*) contents;
	int top = region_get_base(r) + region_get_size(r);

	if (top > (int) data) {
		return (void*) top;
	} else {
		return data;
	}
}

static void addBuiltinRegions(Process *p) {
	uintptr_t base = (uintptr_t) list_reduce(p->regions, regionFindHighest, 0);

	if (base == 0) {
		// No regions yet!  This must be some kind of special process
		// creation (i.e. the pager) that needs to be in physical memory,
		// so just let it do its stuff and set the sp manually, etc.
		return;
	} else {
		dprintf(2, "Base is at %p\n", (void*) base);
	}

	base = round_up(base, PAGESIZE);

	// Size of the region is zero for now, expand as we get syscalls
	list_push(p->regions, region_alloc(REGION_HEAP,
				base, 0, REGION_READ | REGION_WRITE, 0));

	// Put the stack half way up the address space - at the top
	// causes pagefaults, so halfway up seems like a nice compromise
	base = (((unsigned int) -1) >> 1) & PAGEALIGN;

	list_push(p->regions, region_alloc(REGION_STACK,
				base - ONE_MEG, ONE_MEG, REGION_READ | REGION_WRITE, 0));

	// Some times, 3 words are popped unvoluntarily so may as well just
	// always allow for this
	p->sp = (void*) (base - (3 * sizeof(L4_Word_t)));
}

static void regionsDump(void *contents, void *data) {
	Region *r = (Region*) contents;

	printf("*** region %p -> %p (%p) rights=%d\n",
			(void*) region_get_base(r),
			(void*) (region_get_base(r) + region_get_size(r)),
			(void*) r, region_get_rights(r));
}

void process_dump(Process *p) {
	printf("*** %s on %d at %p\n", __FUNCTION__, p->info.pid, p);
	printf("*** pagetable: %p\n", p->pagetable);
	list_iterate(p->regions, regionsDump, NULL);
	printf("*** sp: %p\n", p->sp);
	printf("*** ip: %p\n", p->ip);
}

static pid_t getNextPid(void) {
	pid_t oldPid = nextPid;
	int firstIteration = 1;

	while (sosProcs[nextPid] != NULL) {
		// Detect loop, no more pids!
		if (!firstIteration && oldPid == nextPid) {
			dprintf(0, "!!! getNextPid: none left\n");
			break;
		}

		nextPid++;

		// Gone past the end, loop around
		if (nextPid >= MAX_ADDRSPACES) {
			nextPid = tidOffset;
		}

		firstIteration = 0;
	}

	if (sosProcs[nextPid] != NULL) {
		return NIL_PID;
	} else {
		return nextPid;
	}
}

pid_t reserve_pid(void) {
	pid_t pid = getNextPid();
	dprintf(3, "reserving pid: %d\n", pid);
	if (pid != NIL_PID) {
		sosProcs[pid] = PS_PLACEHOLDER;
	} 
	return pid;
}

void process_prepare(Process *p) {
	// Register with the collection of PCBs
	if (p->info.pid == NIL_PID) {
		p->info.pid = getNextPid();
	}
	sosProcs[p->info.pid] = p;

	if (p->info.ps_type != PS_TYPE_ROOTTHREAD) {
		addBuiltinRegions(p);
		vfs_open(process_get_pid(p), STDOUT_FN, FM_WRITE, FM_UNLIMITED_RW, FM_UNLIMITED_RW);
	}
}

L4_Word_t process_append_region(Process *p, size_t size, int rights) {
	L4_Word_t base = (L4_Word_t) list_reduce(p->regions, regionFindHighest, 0);
	base = round_up(base, PAGESIZE);
	list_push(p->regions, region_alloc(REGION_OTHER, base, size, rights, 0));
	process_dump(p);
	return base;
}

/* Uses a default stack size of 1 page, have to perform below steps manually if a larger
 * stack is needed.
 */
Process *process_run_rootthread(const char *name, void *ip, int timestamp) {
	Process *p = process_init(PS_TYPE_ROOTTHREAD);
	process_prepare(p);
	process_set_name(p, name);
	process_set_ip(p, ip);
	process_set_sp(p, (void *)
			(frame_alloc(FA_STACK) + PAGESIZE - sizeof(L4_Word_t)));
	process_run(p, timestamp);
	return p;
}

L4_ThreadId_t process_run(Process *p, int timestamp) {
	L4_ThreadId_t tid;
	if (verbose > 2) process_dump(p);

	if (timestamp == YES_TIMESTAMP) {
		p->startedAt = time_stamp();
	}

	if (p->info.ps_type == PS_TYPE_ROOTTHREAD) {
		tid = sos_thread_new(process_get_tid(p), p->ip, p->sp);
	} else if (pager_is_active()) {
		tid = sos_task_new(p->info.pid, pager_get_tid(), p->ip, p->sp);
	} else {
		tid = sos_task_new(p->info.pid, L4_Pager(), p->ip, p->sp);
	}

	dprintf(1, "*** %s: running process %ld\n", __FUNCTION__, L4_ThreadNo(tid));
	L4_KDB_SetThreadName(process_get_tid(p), p->info.command);
	assert(L4_ThreadNo(tid) == p->info.pid);

	process_set_state(p, PS_STATE_ALIVE);

	return tid;
}

pid_t process_get_pid(Process *p) {
	if (p == NULL) return NIL_PID;
	return p->info.pid;
}

L4_ThreadId_t process_get_tid(Process *p) {
	if (p == NULL) return L4_nilthread;
	return L4_GlobalId(p->info.pid, 1);
}

L4_SpaceId_t process_get_sid(Process *p) {
	if (p == NULL) return L4_nilspace;
	return L4_SpaceId(process_get_pid(p));
}

Pagetable *process_get_pagetable(Process *p) {
	if (p == NULL) return NULL;
	return p->pagetable;
}

List *process_get_regions(Process *p) {
	if (p == NULL) return NULL;
	return p->regions;
}

static void wakeAll(pid_t wakeFor, pid_t wakeFrom) {
	for (pid_t i = tidOffset; i < MAX_ADDRSPACES; i++) {
		if ((sosProcs[i] != NULL) && (sosProcs[i]->waitingOn == wakeFor)) {
			sosProcs[i]->waitingOn = WAIT_NOBODY;
			process_set_state(sosProcs[i], PS_STATE_ALIVE);
			syscall_reply(process_get_tid(sosProcs[i]), wakeFrom);
		}
	}
}

void process_wake_all(pid_t pid) {
	wakeAll(pid, pid);
	wakeAll(WAIT_ANYBODY, pid);
}

void process_close_files(Process *p) {
	VFile *pfiles = process_get_ofiles(p);

	if (pfiles == NULL) {
		dprintf(0, "!!! process_close_files: %d has NULL open file table\n",
				process_get_pid(p));
	} else {
		for (int fd = 0; fd < PROCESS_MAX_FILES; fd++) {
			if (vfs_isopen(&(p->files[fd]))) {
				vfs_flush(process_get_pid(p), fd);
				vfs_close(process_get_pid(p), fd);
			}
		}
	}
}

L4_Word_t thread_kill(L4_ThreadId_t tid) {
	return L4_ThreadControl(tid, L4_nilspace,
				L4_nilthread, L4_nilthread, L4_nilthread, 0, NULL);
}

int process_kill(Process *p) {
	assert(p != NULL);

	if (process_get_pid(p) > tidOffset && p->info.ps_type != PS_TYPE_ROOTTHREAD) {
		// Isn't a kernel-allocated process, and isn't the pager.  Also we
		// don't want anybody killing threads (and they shouldn't be visible)
		please(thread_kill(process_get_tid(p)));

		// Delete address space
		please(L4_SpaceControl(process_get_sid(p), L4_SpaceCtrl_delete,
				L4_rootclist, L4_Nilpage, 0, NULL));

		// Incidentally, we reuse the caps and cap list so no need to free

		// Reuse if it's now the lowest pid
		if (process_get_pid(p) < nextPid) {
			nextPid = process_get_pid(p);
		}

		return 0;
	} else {
		// Invalid process
		return (-1);
	}
}

void process_remove(Process *p) {
	sosProcs[process_get_pid(p)] = NULL;
}

int process_write_status(process_t *dest, int n) {
	int count = 0;

	// Everything else has size dynamically updated, so just
	// write them all out
	for (int i = 0; i < MAX_ADDRSPACES && count < n; i++) {
		if (sosProcs[i] != NULL && sosProcs[i]->info.ps_type != PS_TYPE_ROOTTHREAD) {
			sosProcs[i]->info.stime = (time_stamp() - sosProcs[i]->startedAt);
			*dest = sosProcs[i]->info;
			dest++;
			count++;
		}
	}

	return count;
}

process_t *process_get_info(Process *p) {
	if (p == NULL) return NULL;
	p->info.stime = (time_stamp() - p->startedAt);
	return &p->info;
}

VFile *process_get_ofiles(Process *p) {
	if (p == NULL) return NULL;
	return p->files;
}

fildes_t *process_get_fds(Process *p) {
	if (p == NULL) return NULL;
	return p->fds;
}

void process_wait_any(Process *waiter) {
	waiter->waitingOn = WAIT_ANYBODY;
	process_set_state(waiter, PS_STATE_WAIT);
}

void process_wait_for(Process *waitFor, Process *waiter) {
	waiter->waitingOn = process_get_pid(waitFor);
	process_set_state(waiter, PS_STATE_WAIT);
}

void process_add_rootserver(void) {
	Process *rootserver = processAlloc();
	rootserver->startedAt = time_stamp();

	process_get_info(rootserver)->pid = L4_rootserverno;
	process_set_name(rootserver, "sos");
	process_set_state(rootserver, PS_STATE_ALIVE);

	assert(rootserver->info.pid == L4_rootserverno);
	sosProcs[L4_rootserverno] = rootserver;

	if (verbose > 2) process_dump(rootserver);
}

