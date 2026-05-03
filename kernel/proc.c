#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}


struct proc*
getproc(int pid)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      release(&p->lock);
      return p;
    }
    release(&p->lock);
  }
  return 0;
}

// ============================================================================
// co_yield(pid, value)
//
// Coroutine-style direct process switch.
//
// LOCKING PROTOCOL:
// =================
// We always acquire BOTH locks (me + other) ordered by proc* address
// to prevent deadlock.  After that:
//
// CASE A — other is NOT parked (not sleeping on CO_CHAN):
//   We park ourselves (state=SLEEPING, chan=CO_CHAN, store value in
//   trapframe->a0), release BOTH locks, then re-acquire ONLY me->lock
//   and call sched().
//   sched() contract: noff==1, p->lock held, state!=RUNNING  → all met.
//   We sleep until some future CASE_B switcher sends us back.
//
// CASE B — other IS parked (SLEEPING on CO_CHAN):
//   We are the active switcher.  We must call swtch() with exactly
//   ONE lock held — other->lock — because other will resume from
//   sched() and sched() was entered with its own lock held.
//   So: we release me->lock BEFORE swtch, keeping other->lock.
//   After swtch returns (when someone switches back to US):
//     - me->lock is held (the future CASE_B switcher holds it per
//       this same protocol)
//     - noff == 1  ✓
//   We do a single release(&me->lock) to restore lock-free state for
//   the usertrapret() path.
//
// Value passing:
//   CASE A stores value in me->trapframe->a0 before parking.
//   CASE B reads it from other->trapframe->a0, then overwrites it
//   with the new value before swtch so other wakes up with the right
//   return value already in a0.
// ============================================================================
int
co_yield(int pid, int value)
{
  // Sentinel channel: unique kernel address, never used by sleep/wakeup.
  // A process parked in co_yield sleeps on this channel and is invisible
  // to the normal scheduler (which only runs RUNNABLE processes).
  void *const CO_CHAN = (void*)co_yield;

  struct proc *me = mycpu()->proc;

  printf("[CY] ENTER caller=%d target=%d value=%d\n",
         me->pid, pid, value);

  // ── validate arguments ────────────────────────────────────────────
  if (pid <= 0 || value < 0 || me->pid == pid) {
    printf("[CY] REJECT bad_args caller=%d pid=%d value=%d"
           " (pid<=0:%d val<0:%d self-yield:%d)\n",
           me->pid, pid, value,
           pid <= 0, value < 0, me->pid == pid);
    return -1;
  }

  // ── find target process ───────────────────────────────────────────
  struct proc *other = getproc(pid);
  if (other == 0) {
    printf("[CY] REJECT no_such_proc caller=%d pid=%d\n", me->pid, pid);
    return -1;
  }

  // ── acquire both locks in address order ───────────────────────────
  // This prevents deadlock when two processes co_yield to each other
  // simultaneously (impossible with NCPU=1, but correct regardless).
  struct proc *first  = (me < other) ? me    : other;
  struct proc *second = (me < other) ? other : me;
  acquire(&first->lock);
  acquire(&second->lock);
  // noff == 2 from here until we release one

  // ── proc table snapshot (taken while both locks held) ─────────────
  printf("[CY] === PROC TABLE (caller=%d noff=%d) ===\n",
         me->pid, mycpu()->noff);
  {
    static const char *snames[] = {
      [UNUSED]   = "UNUSED",
      [USED]     = "USED  ",
      [SLEEPING] = "SLEEP ",
      [RUNNABLE] = "RUNBLE",
      [RUNNING]  = "RUNNIN",
      [ZOMBIE]   = "ZOMBIE",
    };
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      if (p->state == UNUSED) continue;
      const char *sn = (p->state >= 0 && p->state < NELEM(snames)
                        && snames[p->state])
                       ? snames[p->state] : "???   ";
      printf("[CY]   pid=%d %s killed=%d chan_CO=%d name=%s%s\n",
             p->pid, sn, p->killed,
             p->chan == CO_CHAN,
             p->name,
             (p == me)    ? " <-- ME"     :
             (p == other) ? " <-- TARGET" : "");
    }
  }
  printf("[CY] === END PROC TABLE ===\n");

  // ── reject dead target ────────────────────────────────────────────
  if (other->state == ZOMBIE || other->state == UNUSED || other->killed) {
    release(&second->lock);
    release(&first->lock);
    printf("[CY] REJECT dead_target caller=%d target=%d"
           " state=%d killed=%d\n",
           me->pid, other->pid, other->state, other->killed);
    return -1;
  }

  struct cpu *c = mycpu();

  // ══════════════════════════════════════════════════════════════════
  // CASE B: other is parked on CO_CHAN → we are the active switcher
  // ══════════════════════════════════════════════════════════════════
  if (other->state == SLEEPING && other->chan == CO_CHAN) {
    printf("[CY] CASE_B caller=%d: target=%d is parked -> direct switch\n",
           me->pid, other->pid);

    // Read the value other stored when it parked (CASE_A wrote it into
    // other->trapframe->a0 before going to sleep)
    int got = (int)(uint32)other->trapframe->a0;
    printf("[CY] CASE_B caller=%d: read got=%d from target=%d trapframe\n",
           me->pid, got, other->pid);

    // Deliver our value to other: it will return this from co_yield
    other->trapframe->a0 = (uint64)(uint32)value;

    // Wake other: skip RUNNABLE entirely, go straight to RUNNING.
    // other will resume from inside sched(), which was entered with
    // other->lock held — that lock is still held right now (noff==2).
    other->chan  = 0;
    other->state = RUNNING;

    // Park ourselves
    me->chan  = CO_CHAN;
    me->state = SLEEPING;

    // The CPU now logically belongs to other
    c->proc = other;

    // ── CRITICAL: fix noff to 1 before swtch ──────────────────────
    // We hold first->lock and second->lock (noff==2).
    // We must release me->lock and keep other->lock.
    //
    // After swtch, whoever switches BACK to us will hold me->lock
    // (same protocol: CASE_B switcher keeps other->lock = our lock).
    // That is how me->lock gets re-acquired for the resume path.
    if (me == first) {
      // first == me, second == other  →  release first (me)
      printf("[CY] CASE_B caller=%d: release me=first->lock,"
             " keep other=second->lock (noff will be 1)\n", me->pid);
      release(&second->lock);
    } else {
      // first == other, second == me  →  release second (me)
      printf("[CY] CASE_B caller=%d: release me=second->lock,"
             " keep other=first->lock (noff will be 1)\n", me->pid);
      release(&first->lock);
    }
    // noff == 1, holding other->lock  ✓
    // other->state == RUNNING         ✓
    // me->state    == SLEEPING        ✓
    // c->proc      == other           ✓

    printf("[CY] CASE_B caller=%d: swtch -> pid=%d"
           " (delivering value=%d, we will get back got=%d later)\n",
           me->pid, other->pid, value, got);

    // intena belongs to this kernel thread, not the CPU — save/restore
    int intena = c->intena;
    swtch(&me->context, &other->context);
    // ── WE ARE BACK (someone CASE_B-switched to us) ───────────────
    // Invariant on re-entry:
    //   mycpu()->proc == me  (set by the CASE_B that switched to us)
    //   me->lock is held     (the CASE_B switcher kept our lock)
    //   noff == 1            ✓
    //   me->trapframe->a0    == value delivered to us by the switcher
    mycpu()->intena = intena;

    printf("[CY] CASE_B caller=%d: RESUMED"
           " trapframe_a0=%d killed=%d noff=%d\n",
           me->pid,
           (int)(uint32)me->trapframe->a0,
           me->killed,
           mycpu()->noff);

    int was_killed = me->killed;
    // Release me->lock — returns us to lock-free state for usertrapret
   // release(&me->lock);

    if (was_killed) {
      printf("[CY] CASE_B caller=%d: killed on resume -> return -1\n",
             me->pid);
      return -1;
    }
    printf("[CY] CASE_B caller=%d: RETURN got=%d\n", me->pid, got);
    return got;
  }

  // ══════════════════════════════════════════════════════════════════
  // CASE A: other is NOT parked → we park and wait
  // ══════════════════════════════════════════════════════════════════
  printf("[CY] CASE_A caller=%d: target=%d not parked (state=%d)"
         " -> parking self\n", me->pid, other->pid, other->state);

  // Store our value so a future CASE_B can read it from trapframe->a0
  me->trapframe->a0 = (uint64)(uint32)value;
  me->chan  = CO_CHAN;
  me->state = SLEEPING;

  printf("[CY] CASE_A caller=%d: stored value=%d in trapframe,"
         " state=SLEEPING chan=CO_CHAN\n", me->pid, value);

  // Release BOTH locks (noff -> 0)
  release(&second->lock);
  release(&first->lock);

  // Acquire ONLY me->lock, then call sched()
  // sched() requires: p->lock held, noff==1, state!=RUNNING  → all met
  acquire(&me->lock);
  printf("[CY] CASE_A caller=%d: acquired me->lock, calling sched()"
         " noff=%d\n", me->pid, mycpu()->noff);
  sched();

  // ── RESUMED from CASE_B direct switch ─────────────────────────────
  // Invariant on re-entry:
  //   me->lock held (noff==1)   — the CASE_B that woke us kept our lock
  //   me->trapframe->a0         — value delivered by CASE_B
  //   me->state == RUNNING      — set by CASE_B before swtch
  printf("[CY] CASE_A caller=%d: RESUMED"
         " trapframe_a0=%d killed=%d noff=%d\n",
         me->pid,
         (int)(uint32)me->trapframe->a0,
         me->killed,
         mycpu()->noff);

  int got        = (int)(uint32)me->trapframe->a0;
  int was_killed = me->killed;
  release(&me->lock);

  if (was_killed) {
    printf("[CY] CASE_A caller=%d: killed on resume -> return -1\n",
           me->pid);
    return -1;
  }
  printf("[CY] CASE_A caller=%d: RETURN got=%d\n", me->pid, got);
  return got;
}