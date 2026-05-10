#include "headers.h"
#include <math.h>
#include <string.h>

#define READY 0
#define RUNNING 1
#define FINISHED 2
#define BLOCKED 3

#define INVALID_FRAME_NUMBER -1

typedef enum
{
    ALGO_RR
} SchedulerAlgo;

#define MAX_REQUESTS_PER_PROCESS 100

struct MemoryRequest {
    int time;
    int address;
    char operation;
};
struct PCB
{
    int id;
    int arrival;
    int runtime;
    int waiting;
    int remaining;
    int priority;
    int base;
    int limit;
    int pageTableFrame;
    int consumedCpuTime;
    int blockedUntil;
    int started;
    int pid;
    int state;
    /* Phase 2: Person 3 - Memory Request Tracking */
    struct MemoryRequest requests[MAX_REQUESTS_PER_PROCESS];
    int requestCount;
    int nextRequestIndex;
    struct PCB *next;
};

static struct PCB *readyQueue = NULL;
static struct PCB *blockedQueue = NULL;
static struct PCB *runningProcess = NULL;
static FILE *schedulerLog = NULL;
static volatile sig_atomic_t processFinishedFlag = 0;
static int generatorDone = 0;

static SchedulerAlgo schedulerAlgo = ALGO_RR;
static int rrQuantum = 0;
static int rrCounter = 0;
static int lastDispatchClk = -1;
static int contextSwitchInProgress = 0;
static int contextSwitchStartClk = -1;
static struct PCB *pendingProcess = NULL;

static int totalRuntime = 0;
static int totalWaiting = 0;
static int firstStartTime = -1;
static int lastFinishTime = 0;
static int finishedCount = 0;
static double sumWTA = 0.0;
static double sumWTA2 = 0.0;
static int kQuantumsToClear = 0;
static int quantumsSinceLastClear = 0;

static void writePerformanceFile(void)
{
    FILE *perfFile = fopen("scheduler.perf", "w");
    double cpuUtilization = 0.0;
    double avgWTA = 0.0;
    double avgWaiting = 0.0;
    double stdWTA = 0.0;

    if (perfFile == NULL)
    {
        perror("ERROR OPENING scheduler.perf");
        return;
    }

    if (firstStartTime != -1 && lastFinishTime > firstStartTime)
        cpuUtilization = ((double)totalRuntime / (lastFinishTime - firstStartTime)) * 100.0;

    if (finishedCount > 0)
    {
        double variance;

        avgWTA = sumWTA / finishedCount;
        avgWaiting = (double)totalWaiting / finishedCount;
        variance = (sumWTA2 / finishedCount) - (avgWTA * avgWTA);
        if (variance < 0.0)
            variance = 0.0;
        stdWTA = sqrt(variance);
    }

    fprintf(perfFile, "CPU utilization = %.0f%%\n", cpuUtilization);
    fprintf(perfFile, "Avg WTA = %.2f\n", avgWTA);
    fprintf(perfFile, "Avg Waiting = %.0f\n", avgWaiting);
    fprintf(perfFile, "Std WTA = %.2f\n", stdWTA);
    fclose(perfFile);
}

static void logStarted(struct PCB *process)
{
    int now = getClk();
    // printf("At time %d process %d started arr %d total %d remain %d wait %d\n",
    //        now,
    //        process->id,
    //        process->arrival,
    //        process->runtime,
    //        process->remaining,
    //        process->waiting);
    // if (schedulerLog != NULL)
    // {
    //     fprintf(schedulerLog, "At time %d process %d started arr %d total %d remain %d wait %d\n",
    //             now,
    //             process->id,
    //             process->arrival,
    //             process->runtime,
    //             process->remaining,
    //             process->waiting);
    //     fflush(schedulerLog);
    // }
}

static void logStopped(struct PCB *process)
{
    int now = getClk();
    // printf("At time %d process %d stopped arr %d total %d remain %d wait %d\n",
    //        now,
    //        process->id,
    //        process->arrival,
    //        process->runtime,
    //        process->remaining,
    //        process->waiting);
    // if (schedulerLog != NULL)
    // {
    //     fprintf(schedulerLog, "At time %d process %d stopped arr %d total %d remain %d wait %d\n",
    //             now,
    //             process->id,
    //             process->arrival,
    //             process->runtime,
    //             process->remaining,
    //             process->waiting);
    //     fflush(schedulerLog);
    // }
}

static void logResumed(struct PCB *process)
{
    int now = getClk();
    // printf("At time %d process %d resumed arr %d total %d remain %d wait %d\n",
    //        now,
    //        process->id,
    //        process->arrival,
    //        process->runtime,
    //        process->remaining,
    //        process->waiting);
    // if (schedulerLog != NULL)
    // {
    //     fprintf(schedulerLog, "At time %d process %d resumed arr %d total %d remain %d wait %d\n",
    //             now,
    //             process->id,
    //             process->arrival,
    //             process->runtime,
    //             process->remaining,
    //             process->waiting);
    //     fflush(schedulerLog);
    // }
}

static void logFinished(struct PCB *process)
{
    int now = getClk();
    int ta = now - process->arrival;
    double wta = (double)ta / process->runtime;

    // printf("At time %d process %d finished arr %d total %d remain %d wait %d TA %d WTA %.2f\n",
    //        now,
    //        process->id,
    //        process->arrival,
    //        process->runtime,
    //        process->remaining,
    //        process->waiting,
    //        ta,
    //        wta);
    // if (schedulerLog != NULL)
    // {
    //     // fprintf(schedulerLog, "At time %d process %d finished arr %d total %d remain %d wait %d TA %d WTA %.2f\n",
    //     //         now,
    //     //         process->id,
    //     //         process->arrival,
    //     //         process->runtime,
    //     //         process->remaining,
    //     //         process->waiting,
    //     //         ta,
    //     //         wta);
    //     // fflush(schedulerLog);
    // }
}

static void insertAtTail(struct PCB **head, struct PCB *node)
{
    struct PCB *cur;

    node->next = NULL;
    if (*head == NULL)
    {
        *head = node;
        return;
    }

    cur = *head;
    while (cur->next != NULL)
        cur = cur->next;
    cur->next = node;
}

static struct PCB *popHead(struct PCB **head)
{
    struct PCB *node;

    if (*head == NULL)
        return NULL;
    node = *head;
    *head = (*head)->next;
    node->next = NULL;
    return node;
}

static void incrementReadyWaiting(void)
{
    struct PCB *cur = readyQueue;
    while (cur != NULL)
    {
        cur->waiting++;
        cur = cur->next;
    }
}

static void enqueueReadyProcess(struct PCB *process)
{
    if (process == NULL)
        return;

    process->state = READY;
    insertAtTail(&readyQueue, process);
}

static void loadProcessRequests(struct PCB *process) {
    char filename[64];
    sprintf(filename, "requests_%d.txt", process->id);
    
    FILE *reqFile = fopen(filename, "r");
    process->requestCount = 0;
    process->nextRequestIndex = 0;

    if (reqFile == NULL) {
        printf("No request file found for process %d (%s)\n", process->id, filename);
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), reqFile)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        int time;
        char binAddr[32];
        char op;

        if (sscanf(line, "%d %31s %c", &time, binAddr, &op) == 3) {
            int idx = process->requestCount;
            if (idx >= MAX_REQUESTS_PER_PROCESS) {
                printf("Warning: Process %d exceeded max requests limit.\n", process->id);
                break;
            }
            
            process->requests[idx].time = time;
            // Convert binary string to integer
            process->requests[idx].address = (int)strtol(binAddr, NULL, 2);
            process->requests[idx].operation = op;
            
            process->requestCount++;
        }
    }
    fclose(reqFile);
}
static void createAndEnqueueProcess(struct msgbuff *msg)
{
    struct PCB *node = (struct PCB *)malloc(sizeof(struct PCB));
    if (node == NULL)
    {
        perror("ERROR ALLOCATING PCB!");
        return;
    }

    node->id = msg->id;
    node->arrival = msg->arrival;
    node->runtime = msg->runtime;
    node->remaining = msg->runtime;
    node->priority = msg->priority;
    node->base = msg->base;
    node->limit = msg->limit;
    node->pageTableFrame = INVALID_FRAME_NUMBER;
    node->consumedCpuTime = 0;
    node->blockedUntil = -1;
    node->waiting = 0;
    node->started = 0;
    node->pid = -1;
    node->state = READY;
    node->next = NULL;
   loadProcessRequests(node);

    totalRuntime += node->runtime;
 
    enqueueReadyProcess(node);

    // printf("Process %d inserted into ready queue at time %d base %d limit %d\n",
    //        node->id,
    //        getClk(),
    //        node->base,
    //        node->limit);
}

static void drainIncomingProcesses(int msgq_id)
{
    struct msgbuff msg;
    while (msgrcv(msgq_id, &msg, sizeof(msg) - sizeof(long), 0, IPC_NOWAIT) != -1)
    {
        if (msg.id == -1)
        {
            generatorDone = 1;
            continue;
        }
        createAndEnqueueProcess(&msg);
    }
}

static void startOrResumeRunningProcess(void)
{
    char remStr[16];
    int pid;
    int now;

    if (runningProcess == NULL)
        return;

    now = getClk();

    lastDispatchClk = now;
    rrCounter = 0;

    if (firstStartTime == -1)
        firstStartTime = now;

    if (runningProcess->started == 0)
    {
        /*
         * Phase 2 memory setup: each process owns a page table frame and
         * starts with virtual page 0 loaded. The MMU module owns the details.
         */
        runningProcess->pageTableFrame = mmu_start_process(runningProcess->id,
                                                           runningProcess->limit,
                                                           runningProcess->base);
        if (runningProcess->pageTableFrame == INVALID_FRAME_NUMBER)
        {
            perror("ERROR STARTING PROCESS MEMORY!");
            runningProcess->state = FINISHED;
            free(runningProcess);
            runningProcess = NULL;
            return;
        }

        runningProcess->started = 1;
        sprintf(remStr, "%d", runningProcess->remaining);
        pid = fork();
        if (pid == 0)
        {
            execl("./process.out", "process.out", remStr, NULL);
            perror("ERROR STARTING PROCESS!");
            exit(1);
        }
        runningProcess->pid = pid;
        runningProcess->state = RUNNING;
        logStarted(runningProcess);
    }
    else
    {
        runningProcess->state = RUNNING;
        kill(runningProcess->pid, SIGCONT);
        logResumed(runningProcess);
    }
}

static void beginContextSwitch(struct PCB *next)
{
    if (next == NULL)
        return;

    contextSwitchInProgress = 1;
    contextSwitchStartClk = getClk();
    pendingProcess = next;
}

static void completeContextSwitchIfReady(void)
{
    if (!contextSwitchInProgress)
        return;

    if ((getClk() - contextSwitchStartClk) < 1)
        return;

    contextSwitchInProgress = 0;
    contextSwitchStartClk = -1;
    runningProcess = pendingProcess;
    pendingProcess = NULL;
    if (runningProcess != NULL)
        startOrResumeRunningProcess();
}

static void dispatchIfIdle(void)
{
    if (runningProcess != NULL || readyQueue == NULL || contextSwitchInProgress)
        return;

    if (firstStartTime == -1)
    {
        runningProcess = popHead(&readyQueue);
        startOrResumeRunningProcess();
        return;
    }

    beginContextSwitch(popHead(&readyQueue));
}

static void stopAndRequeueRunningProcess(void)
{
    if (runningProcess == NULL)
        return;

    kill(runningProcess->pid, SIGSTOP);
    logStopped(runningProcess);

    enqueueReadyProcess(runningProcess);

    runningProcess = NULL;
}

/*
 * Phase 2 page-fault scheduler path.
 * Person 3 can call this after a request causes mmu_access() to report a
 * page fault. The faulting process leaves the CPU and returns after diskDelay.
 */
static void blockRunningProcessForPageFault(int diskDelay)
{
    if (runningProcess == NULL)
        return;

    if (runningProcess->pid > 0)
        kill(runningProcess->pid, SIGSTOP);

    runningProcess->state = BLOCKED;
    runningProcess->blockedUntil = getClk() + diskDelay;
    rrCounter = 0;
    insertAtTail(&blockedQueue, runningProcess);
    runningProcess = NULL;

    if (!contextSwitchInProgress && readyQueue != NULL)
        beginContextSwitch(popHead(&readyQueue));
}

static void releaseUnblockedProcesses(void)
{
    struct PCB *cur = blockedQueue;
    struct PCB *prev = NULL;
    int now = getClk();

    while (cur != NULL)
    {
        struct PCB *next = cur->next;

        if (cur->blockedUntil <= now)
        {
            if (prev == NULL)
                blockedQueue = next;
            else
                prev->next = next;

            cur->next = NULL;
            cur->blockedUntil = -1;

            /* Phase 2: Person 3 - Log the completion exactly when it wakes up! */
            // The address that caused the fault is the one right behind our current index
            int faultingAddress = cur->requests[cur->nextRequestIndex - 1].address;
            mmu_log_completion(cur->id, faultingAddress, now);
            enqueueReadyProcess(cur);
        }
        else
        {
            prev = cur;
        }

        cur = next;
    }
}

/*
 * Runs one memory request for the currently running process.
 * Request parsing belongs to Person 3; once a request is due, call this helper.
 */
static void handleRunningMemoryAccess(int virtualAddress, char operation)
{
    int accessResult;
    int diskDelay;

    if (runningProcess == NULL)
        return;

    accessResult = mmu_access(runningProcess->id, virtualAddress, operation);
    if (accessResult == MMU_ACCESS_HIT)
        return;

    if (accessResult != MMU_ACCESS_PAGE_FAULT)
        return;

    diskDelay = mmu_handle_page_fault(runningProcess->id,
                                      virtualAddress,
                                      operation,
                                      getClk());
    if (diskDelay > 0)
        blockRunningProcessForPageFault(diskDelay);
}

static void handleFinishedProcess(void)
{
    int now;
    int ta;
    double wta;

    if (runningProcess == NULL)
        return;

    now = getClk();
    runningProcess->state = FINISHED;
    runningProcess->remaining = 0;

    ta = now - runningProcess->arrival;
    wta = (double)ta / runningProcess->runtime;

    finishedCount++;
    totalWaiting += runningProcess->waiting;
    sumWTA += wta;
    sumWTA2 += (wta * wta);
    lastFinishTime = now;

    logFinished(runningProcess);
    mmu_finish_process(runningProcess->id);
    free(runningProcess);
    runningProcess = NULL;
    rrCounter = 0;

    if (!contextSwitchInProgress && readyQueue != NULL)
        beginContextSwitch(popHead(&readyQueue));
}

static void accountOneClockTick(void)
{
    int now = getClk();

    incrementReadyWaiting();

    if (runningProcess == NULL)
        return;

    if (now == lastDispatchClk)
        return;

    runningProcess->remaining--;
    runningProcess->consumedCpuTime++;
    rrCounter++;

    /*
     * Phase 2 integration point for Person 3:
     * check requests whose relative time == consumedCpuTime here, then pass
     * each due request to handleRunningMemoryAccess(address, operation).
     */
    /* Phase 2 integration point for Person 3 */
    while (runningProcess != NULL && runningProcess->nextRequestIndex < runningProcess->requestCount)
    {
        struct MemoryRequest *req = &runningProcess->requests[runningProcess->nextRequestIndex];
        
        if (req->time == runningProcess->consumedCpuTime)
        {
            // SAVE A SAFE REFERENCE before we do the memory access
            struct PCB *activeProc = runningProcess; 
            
            handleRunningMemoryAccess(req->address, req->operation);
            
            // Increment using our safe reference, so it doesn't crash if runningProcess became NULL
            activeProc->nextRequestIndex++; 
            
            // If the process faulted, runningProcess is now NULL. Break out safely.
            if (runningProcess == NULL) {
                return; 
            }
        }
        else if (req->time > runningProcess->consumedCpuTime)
        {
            break; 
        }
        else
        {
            runningProcess->nextRequestIndex++;
        }
    }
}

static void preemptIfNeeded(void)
{
    if (runningProcess == NULL || readyQueue == NULL)
        return;

    if (runningProcess->remaining <= 0)
        return;

  if (rrCounter >= rrQuantum)
    {
        /* Phase 2: Person 3 - K-Quantum R-Bit Reset */
        quantumsSinceLastClear++;
        if (quantumsSinceLastClear >= kQuantumsToClear) {
            mmu_clear_reference_bits();
            quantumsSinceLastClear = 0;
        }

        stopAndRequeueRunningProcess();
        beginContextSwitch(popHead(&readyQueue));
    }
}

static int parseSchedulerArgs(int argc, char *argv[])
{
    if (argc < 3)
    {
        perror("INVALID SCHEDULER ARGUMENTS!");
        return 0;
    }

    if (strcmp(argv[1], "RR") == 0)
    {
        schedulerAlgo = ALGO_RR;
        rrQuantum = atoi(argv[2]);
        kQuantumsToClear = atoi(argv[3]);
        if (rrQuantum <= 0)
        {
            perror("INVALID RR QUANTUM!");
            return 0;
        }
        return 1;
    }

    perror("PHASE 2 SUPPORTS RR ONLY!");
    return 0;
}

static int openSchedulerIPC(void)
{
    key_t key = ftok("keyfile", 65);
    int msgq_id;

    if (key == -1)
    {
        perror("ERROR GENERATING KEY!");
        return -1;
    }

    msgq_id = msgget(key, IPC_CREAT | 0666);
    if (msgq_id == -1)
    {
        perror("ERROR CREATING/OPENING MESSAGE QUEUE!");
        return -1;
    }
    return msgq_id;
}

static int openSchedulerLog(void)
{
    schedulerLog = fopen("scheduler.log", "w");
    if (schedulerLog == NULL)
    {
        perror("ERROR OPENING scheduler.log");
        return 0;
    }
    return 1;
}

static void sigUSR1Handler(int signum)
{
    (void)signum;
    processFinishedFlag = 1;
}

static void clearSchedulerResources(int signum)
{
    (void)signum;
    writePerformanceFile();
    if (schedulerLog != NULL)
        fclose(schedulerLog);
    schedulerLog = NULL;
    destroyClk(false);
    exit(0);
}

int main(int argc, char *argv[])
{
    int msgq_id;
    int lastClk;

    if (!parseSchedulerArgs(argc, argv))
        return 1;

    initClk();
    mmu_init();
    signal(SIGUSR1, sigUSR1Handler);
    signal(SIGINT, clearSchedulerResources);

    msgq_id = openSchedulerIPC();
    if (msgq_id == -1)
        return 1;

    if (!openSchedulerLog())
        return 1;

    lastClk = getClk();

    while (!(generatorDone && readyQueue == NULL && blockedQueue == NULL && runningProcess == NULL && !contextSwitchInProgress && pendingProcess == NULL))
    {
        drainIncomingProcesses(msgq_id);
        releaseUnblockedProcesses();

        if (processFinishedFlag)
        {
            processFinishedFlag = 0;
            handleFinishedProcess();
        }

        completeContextSwitchIfReady();
        dispatchIfIdle();

        if (getClk() != lastClk)
        {
            lastClk = getClk();
            releaseUnblockedProcesses();
            completeContextSwitchIfReady();
            accountOneClockTick();

            if (processFinishedFlag)
            {
                processFinishedFlag = 0;
                handleFinishedProcess();
            }

            preemptIfNeeded();
            completeContextSwitchIfReady();
            dispatchIfIdle();
        }
        else
        {
            usleep(1000);
        }
    }

    writePerformanceFile();
    if (schedulerLog != NULL)
        fclose(schedulerLog);
    schedulerLog = NULL;
    destroyClk(true);
    return 0;
}
