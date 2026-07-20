// csstress - #446 scheduler/context-switch stress reproducer.
//
// A single binary registered under SEVERAL service names. Services whose
// name starts with "spin" are auto-flagged migratable by the kernel
// (proc_create_user), so they run on the Application Processors; the others
// run on the BSP. Every worker hammers yield() in a tight loop, which drives
// a full sched_schedule() + context_switch() per iteration. With workers on
// BOTH cores pulling from the shared ready queue, this maximizes the rate of
// the enqueue-prev / release-BKL / context_switch window that #446 suspects.
//
// No GUI, no disk. Prints a periodic heartbeat to stdout (console -> serial)
// so liveness and switch-count progress are visible on the serial log; the
// real signal is the kernel's own [SCHED] instrumentation and any GPF.
#include "syscall.h"

static void sputs(const char *s){ int n=0; while(s[n]) n++; sys_write(1, s, (unsigned long)n); }
static void sputu(unsigned long v){
    char t[24]; int j=0;
    if(!v){ sys_write(1,"0",1); return; }
    while(v){ t[j++]=(char)('0'+(v%10)); v/=10; }
    char b[24]; int i=0; while(j) b[i++]=t[--j];
    sys_write(1,b,(unsigned long)i);
}

int main(void){
    volatile unsigned long acc = 1;
    unsigned long iter = 0;
    int pid = sys_getpid();
    sputs("[cs start pid"); sputu((unsigned long)pid); sputs("]\n");
    for(;;){
        // A little integer compute between switches (keeps a worker from being
        // a pure yield no-op; also dirties registers so a bad restore shows).
        for(int k=0;k<30;k++){
            acc = acc*6364136223846793005UL + 1442695040888963407UL;
            acc ^= (unsigned long)k + iter;
        }
        yield();
        if(((++iter) & 0x3FFFFUL) == 0){
            sputs("[cs pid"); sputu((unsigned long)pid);
            sputs(" it="); sputu(iter);
            sputs(" acc="); sputu(acc & 0xFFFFUL); sputs("]\n");
        }
    }
    return 0;
}
