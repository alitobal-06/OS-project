#include "headers.h"
#include <math.h>
#include <string.h>

#define READY 0
#define RUNNING 1
#define FINISHED 2

typedef enum
{
    ALGO_HPF,
    ALGO_RR
} SchedulerAlgo;

struct PCB
{
    int id;
    int arrival;
    int runtime;
    int waiting;
    int remaining;
    int priority;
    int started;
    int pid;
    int state;
    struct PCB *next;
};

static struct PCB *readyQueue = NULL;
static struct PCB *runningProcess = NULL;
static FILE *schedulerLog = NULL;
static volatile sig_atomic_t processFinishedFlag = 0;
static int generatorDone = 0;

static SchedulerAlgo schedulerAlgo = ALGO_HPF;
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
    printf("At time %d process %d started arr %d total %d remain %d wait %d\n",
           now,
           process->id,
           process->arrival,
           process->runtime,
           process->remaining,
           process->waiting);
    if (schedulerLog != NULL)
    {
        fprintf(schedulerLog, "At time %d process %d started arr %d total %d remain %d wait %d\n",
                now,
                process->id,
                process->arrival,
                process->runtime,
                process->remaining,
                process->waiting);
        fflush(schedulerLog);
    }
}

static void logStopped(struct PCB *process)
{
    int now = getClk();
    printf("At time %d process %d stopped arr %d total %d remain %d wait %d\n",
           now,
           process->id,
           process->arrival,
           process->runtime,
           process->remaining,
           process->waiting);
    if (schedulerLog != NULL)
    {
        fprintf(schedulerLog, "At time %d process %d stopped arr %d total %d remain %d wait %d\n",
                now,
                process->id,
                process->arrival,
                process->runtime,
                process->remaining,
                process->waiting);
        fflush(schedulerLog);
    }
}

static void logResumed(struct PCB *process)
{
    int now = getClk();
    printf("At time %d process %d resumed arr %d total %d remain %d wait %d\n",
           now,
           process->id,
           process->arrival,
           process->runtime,
           process->remaining,
           process->waiting);
    if (schedulerLog != NULL)
    {
        fprintf(schedulerLog, "At time %d process %d resumed arr %d total %d remain %d wait %d\n",
                now,
                process->id,
                process->arrival,
                process->runtime,
                process->remaining,
                process->waiting);
        fflush(schedulerLog);
    }
}

static void logFinished(struct PCB *process)
{
    int now = getClk();
    int ta = now - process->arrival;
    double wta = (double)ta / process->runtime;

    printf("At time %d process %d finished arr %d total %d remain %d wait %d TA %d WTA %.2f\n",
           now,
           process->id,
           process->arrival,
           process->runtime,
           process->remaining,
           process->waiting,
           ta,
           wta);
    if (schedulerLog != NULL)
    {
        fprintf(schedulerLog, "At time %d process %d finished arr %d total %d remain %d wait %d TA %d WTA %.2f\n",
                now,
                process->id,
                process->arrival,
                process->runtime,
                process->remaining,
                process->waiting,
                ta,
                wta);
        fflush(schedulerLog);
    }
}

static void insertByPriority(struct PCB **head, struct PCB *node)
{
    struct PCB *cur;

    if (*head == NULL || node->priority < (*head)->priority)
    {
        node->next = *head;
        *head = node;
        return;
    }

    cur = *head;
    while (cur->next != NULL && cur->next->priority < node->priority)
        cur = cur->next;

    node->next = cur->next;
    cur->next = node;
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
    node->waiting = 0;
    node->started = 0;
    node->pid = -1;
    node->state = READY;
    node->next = NULL;

    totalRuntime += node->runtime;

    if (schedulerAlgo == ALGO_HPF)
        insertByPriority(&readyQueue, node);
    else
        insertAtTail(&readyQueue, node);

    printf("Process %d inserted into ready queue at time %d\n", node->id, getClk());
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
    runningProcess->state = READY;
    logStopped(runningProcess);

    if (schedulerAlgo == ALGO_HPF)
        insertByPriority(&readyQueue, runningProcess);
    else
        insertAtTail(&readyQueue, runningProcess);

    runningProcess = NULL;
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
    if (schedulerAlgo == ALGO_RR)
        rrCounter++;
}

static void preemptIfNeeded(void)
{
    if (runningProcess == NULL || readyQueue == NULL)
        return;

    if (runningProcess->remaining <= 0)
        return;

    if (schedulerAlgo == ALGO_HPF)
    {
        if (readyQueue->priority < runningProcess->priority)
        {
            stopAndRequeueRunningProcess();
            beginContextSwitch(popHead(&readyQueue));
        }
        return;
    }

    if (schedulerAlgo == ALGO_RR && rrCounter >= rrQuantum)
    {
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

    if (strcmp(argv[1], "HPF") == 0)
    {
        schedulerAlgo = ALGO_HPF;
        rrQuantum = 0;
        return 1;
    }

    if (strcmp(argv[1], "RR") == 0)
    {
        schedulerAlgo = ALGO_RR;
        rrQuantum = atoi(argv[2]);
        if (rrQuantum <= 0)
        {
            perror("INVALID RR QUANTUM!");
            return 0;
        }
        return 1;
    }

    perror("INVALID SCHEDULING ALGORITHM!");
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
    signal(SIGUSR1, sigUSR1Handler);
    signal(SIGINT, clearSchedulerResources);

    msgq_id = openSchedulerIPC();
    if (msgq_id == -1)
        return 1;

    if (!openSchedulerLog())
        return 1;

    lastClk = getClk();

    while (!(generatorDone && readyQueue == NULL && runningProcess == NULL && !contextSwitchInProgress && pendingProcess == NULL))
    {
        drainIncomingProcesses(msgq_id);

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

