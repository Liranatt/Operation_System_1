#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

uint64
sys_memsize(void)
{
  struct proc *p = myproc();
  return p->sz;
}

uint64
sys_co_yield(void)
{
  int target_pid;
  int value;
  struct proc *self;
  struct proc *target = 0;

  argint(0, &target_pid);
  argint(1, &value);

  self = myproc();
  if (target_pid <= 0)
    return -1;

  if (target_pid == self->pid)
    return -1;
  
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == target_pid && p->state != UNUSED && p->state != ZOMBIE) {
      if (killed(p)) {
        release(&p->lock);
        return -1;
      }

      target = p;
      break;
    }

    release(&p->lock);
  }

  if (target == 0)
    return -1;

  if (target->state == SLEEPING && target->chan == (void *)(uint64)self->pid) {
    target->trapframe->a0 = value;
    target->state = RUNNABLE;
    release(&target->lock);
  }
  else 
    release(&target->lock);

  extern struct spinlock wait_lock;

  acquire(&wait_lock);
  sleep((void *)(uint64)target_pid, &wait_lock);

  return self->trapframe->a0;
}
  
                
  

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

