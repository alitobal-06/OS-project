#include <stdio.h>      //if you don't use scanf/printf change this include
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

typedef short bool;
#define true 1
#define false 0

#define MMU_ACCESS_INVALID -1
#define MMU_ACCESS_HIT 0
#define MMU_ACCESS_PAGE_FAULT 1

#define SHKEY 300


///==============================
//don't mess with this variable//
int * shmaddr;                 //
//===============================



int getClk()
{
    return *shmaddr;
}


/*
 * All process call this function at the beginning to establish communication between them and the clock module.
 * Again, remember that the clock is only emulation!
*/
void initClk()
{
    int shmid = shmget(SHKEY, 4, 0444);
    while ((int)shmid == -1)
    {
        //Make sure that the clock exists
        printf("Wait! The clock not initialized yet!\n");
        sleep(1);
        shmid = shmget(SHKEY, 4, 0444);
    }
    shmaddr = (int *) shmat(shmid, (void *)0, 0);
}


void destroyClk(bool terminateAll)
{
    shmdt(shmaddr);
    if (terminateAll)
    {
        killpg(getpgrp(), SIGINT);
    }
}

struct msgbuff
{
    long mtype;
    int id;
    int arrival;
    int runtime;
    int priority;
    int base;
    int limit;
};

/* Phase 2 MMU interface used by the RR scheduler. */
void mmu_init(void);
int mmu_start_process(int processId, int pageCount, int diskBase);
int mmu_access(int processId, int virtualAddress, char operation);
int mmu_handle_page_fault(int processId, int virtualAddress, char operation, int loadTime);
void mmu_clear_reference_bits(void);
void mmu_finish_process(int processId);
void mmu_log_completion(int processId, int virtualAddress, int time);