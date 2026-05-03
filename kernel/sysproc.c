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

uint64 sys_co_yield(void) {
    int target_pid, val;
    struct proc *p = myproc();

    if(argint(0, &target_pid) < 0 || argint(1, &val) < 0) {
        printf("[CY] PID %d: argint failed\n", p->pid);
        return -1;
    }

    printf("[CY] PID %d: sys_co_yield called (target=%d, val=%d)\n", p->pid, target_pid, val);
    
    uint64 ret = co_yield(target_pid, val);
    
    printf("[CY] PID %d: sys_co_yield returning value %d\n", p->pid, (int)ret);
    return ret;
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

