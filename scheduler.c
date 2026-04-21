#include "headers.h"
#include <string.h>
#include <math.h>

#define READY 0
#define RUNNING 1
#define FINISHED 2
#define CONTEXT_SWITCH_OVERHEAD 1

struct PCB {
    int id;
    int arrival;
    int runtime;
    int waiting;
    int remaining;
    int priority;
    int started; // Flag to handle the preemption in HPF to prevent reforking of process
    int pid;
    int state;
    struct PCB *next;  // Pointer to the next PCB in the queue
};

// global variables
struct PCB *readyQueue = NULL;   
struct PCB *runningProcess = NULL; 
FILE *schedulerLog = NULL;
int contextSwitchInProgress = 0;
int contextSwitchStartTime = -1;
struct PCB *pendingProcess = NULL;
int generatorDone = 0;
volatile sig_atomic_t processFinishedFlag = 0;
int totalRuntime = 0;
int totalWaiting = 0;
int firstStartTime = -1;
int lastFinishTime = 0;
int finishedCount = 0;
double sumWTA = 0.0;
double sumWTA2 = 0.0;

void writePerformanceFile()
{
    FILE *perfFile = fopen("scheduler.perf", "w");
    if (perfFile == NULL)
    {
        perror("ERROR OPENING scheduler.perf");
        return;
    }

    double cpuUtilization = 0.0;
    double avgWTA = 0.0;
    double avgWaiting = 0.0;
    double stdWTA = 0.0;

    if (firstStartTime != -1 && lastFinishTime > firstStartTime)
    {
        cpuUtilization = ((double)totalRuntime / (lastFinishTime - firstStartTime)) * 100.0;
    }

    if (finishedCount > 0)
    {
        avgWTA = sumWTA / finishedCount;
        avgWaiting = (double)totalWaiting / finishedCount;

        double variance = (sumWTA2 / finishedCount) - (avgWTA * avgWTA);
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

void printStarted(struct PCB *process)
{
    int clkNow = getClk();

    printf("At time %d process %d started arr %d total %d remain %d wait %d\n",
        clkNow,
        process->id,
        process->arrival,
        process->runtime,
        process->remaining,
        process->waiting);

    if (schedulerLog != NULL)
    {
        fprintf(schedulerLog, "At time %d process %d started arr %d total %d remain %d wait %d\n",
            clkNow,
            process->id,
            process->arrival,
            process->runtime,
            process->remaining,
            process->waiting);
        fflush(schedulerLog);
    }
}

void printStopped(struct PCB *process)
{
    int clkNow = getClk();

    printf("At time %d process %d stopped arr %d total %d remain %d wait %d\n",
        clkNow,
        process->id,
        process->arrival,
        process->runtime,
        process->remaining,
        process->waiting);

    if (schedulerLog != NULL)
    {
        fprintf(schedulerLog, "At time %d process %d stopped arr %d total %d remain %d wait %d\n",
            clkNow,
            process->id,
            process->arrival,
            process->runtime,
            process->remaining,
            process->waiting);
        fflush(schedulerLog);
    }
}

void printFinished(struct PCB *process)
{
    int clkNow = getClk();
    int ta = clkNow - process->arrival;
    float wta = (float)ta / process->runtime;

    printf("At time %d process %d finished arr %d total %d remain %d wait %d TA %d WTA %.2f\n",
        clkNow,
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
            clkNow,
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

void printContinued(struct PCB *process)
{
    int clkNow = getClk();

    printf("At time %d process %d resumed arr %d total %d remain %d wait %d\n",
        clkNow,
        process->id,
        process->arrival,
        process->runtime,
        process->remaining,
        process->waiting);

    if (schedulerLog != NULL)
    {
        fprintf(schedulerLog, "At time %d process %d resumed arr %d total %d remain %d wait %d\n",
            clkNow,
            process->id,
            process->arrival,
            process->runtime,
            process->remaining,
            process->waiting);
        fflush(schedulerLog);
    }
}



void insertByPriority(struct PCB **head, struct PCB *newNode) {
    
    if(*head == NULL || newNode->priority < (*head)->priority)
    {
         newNode->next = *head;
         *head = newNode;
            return;
    }

    struct PCB *current = *head;
    while(current->next != NULL && current->next->priority < newNode->priority)
    {        current = current->next;
    }
    newNode->next = current->next;
    current->next = newNode;
}


void inertAtTail(struct PCB **head, struct PCB *newNode)
{
    newNode->next = NULL;
    
    if (*head == NULL)
    {
        *head = newNode;
        return;
    }
    
    struct PCB *current = *head;
    while (current->next != NULL)
    {
        current = current->next;
    }
    current->next = newNode;
}

struct PCB *removeHead(struct PCB **head)
{
    
    if (*head == NULL)
        return NULL;
    struct PCB *temp = *head;   
    *head = (*head)->next;      
    temp->next = NULL;         
    return temp;                
}

void startContextSwitch(struct PCB *nextProcess)
{
    if (nextProcess == NULL)
        return;

    contextSwitchInProgress = 1;
    contextSwitchStartTime = getClk();
    pendingProcess = nextProcess;
}

void handleProcessFinish()
{
    int ta;
    double wta;
    int clkNow;

    if (runningProcess == NULL)
        return;

    clkNow = getClk();

    runningProcess->state = FINISHED;
    runningProcess->remaining = 0;

    ta = clkNow - runningProcess->arrival;
    wta = (double)ta / runningProcess->runtime;

    finishedCount++;
    totalWaiting += runningProcess->waiting;
    sumWTA += wta;
    sumWTA2 += (wta * wta);
    lastFinishTime = clkNow;

    printFinished(runningProcess);

    free(runningProcess);
    runningProcess = NULL;
}

// process finished

void sigUSR1Handler(int signum)
{
    processFinishedFlag = 1;
}

void clearSchedulerResources(int signum)
{
    writePerformanceFile();

    if (schedulerLog != NULL)
    {
        fclose(schedulerLog);
        schedulerLog = NULL;
    }
    destroyClk(false);
    exit(0);
}



// run a process
void runProcess()
{
    if (runningProcess->started == 0)
    {
        int clkNow = getClk();

        if (firstStartTime == -1)
            firstStartTime = clkNow;

        runningProcess->waiting = clkNow - runningProcess->arrival;

        // first time → fork it
        runningProcess->started = 1;
        char remainingStr[10];
        sprintf(remainingStr, "%d", runningProcess->remaining);

        int pid = fork();
        if (pid == 0)
        {
            execl("./process.out", "process.out", remainingStr, NULL);
            perror("ERROR STARTING PROCESS!");
            exit(1);
        }
        runningProcess->pid = pid;
        printStarted(runningProcess);
    }
    else
    {
        int clkNow = getClk();

        runningProcess->waiting = clkNow - runningProcess->arrival -
                                 (runningProcess->runtime - runningProcess->remaining);

        // was stopped before → resume it
        kill(runningProcess->pid, SIGCONT);
        printContinued(runningProcess);
    }
}



int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        perror("INVALID SCHEDULER ARGUMENTS!");
        return 1;
    }

  
    char *algo = argv[1];
    int quantum = atoi(argv[2]);

    if (strcmp(algo, "HPF") != 0 && strcmp(algo, "RR") != 0)
    {
        perror("INVALID SCHEDULING ALGORITHM!");
        return 1;
    }

    if (strcmp(algo, "RR") == 0 && quantum <= 0)
    {
        perror("INVALID RR QUANTUM!");
        return 1;
    }

    initClk();
    signal(SIGUSR1, sigUSR1Handler);
    signal(SIGINT, clearSchedulerResources);

    
    key_t key = ftok("keyfile", 65);
    if (key == -1)
    {
        perror("ERROR GENERATING KEY!");
        return 1;
    }

    int msgq_id = msgget(key, IPC_CREAT | 0666);
    if (msgq_id == -1)
    {
        perror("ERROR CREATING/OPENING MESSAGE QUEUE!");
        return 1;
    }

    schedulerLog = fopen("scheduler.log", "w");
    if (schedulerLog == NULL)
    {
        perror("ERROR OPENING scheduler.log");
        return 1;
    }

   
    int lastClk = getClk();
    int quantumCounter = 0;
    while (!(generatorDone && readyQueue == NULL && runningProcess == NULL && !contextSwitchInProgress))
    {
        int dispatchedNow = 0;

        // Always drain incoming messages so arrivals are inserted in the same clock tick.
        struct msgbuff msg;
        while (msgrcv(msgq_id, &msg, sizeof(msg) - sizeof(long), 0, IPC_NOWAIT) != -1)
        {
            if (msg.id == -1)
            {
                generatorDone = 1;
                continue;
            }

            totalRuntime += msg.runtime;

            // create a new PCB for this process
            struct PCB *newProcess = (struct PCB *)malloc(sizeof(struct PCB));
            if (newProcess == NULL)
            {
                perror("ERROR ALLOCATING PCB!");
                continue;
            }
            newProcess->id = msg.id;
            newProcess->arrival = msg.arrival;
            newProcess->runtime = msg.runtime;
            newProcess->remaining = msg.runtime;
            newProcess->priority = msg.priority;
            newProcess->waiting = 0;
            newProcess->started = 0;
            newProcess->state = READY;
            newProcess->next = NULL;

            if (strcmp(algo, "HPF") == 0)
                insertByPriority(&readyQueue, newProcess);
            else
                inertAtTail(&readyQueue, newProcess);

            printf("Process %d inserted into ready queue at time %d\n", newProcess->id, getClk());
        }

        if (processFinishedFlag)
        {
            struct PCB *nextProcess = NULL;

            processFinishedFlag = 0;
            handleProcessFinish();
            quantumCounter = 0;

            // A switch to another ready process after finish still costs overhead.
            if (!contextSwitchInProgress && runningProcess == NULL && readyQueue != NULL)
            {
                nextProcess = removeHead(&readyQueue);
                startContextSwitch(nextProcess);
            }
        }

        // If CPU is idle and no context switch is in progress, dispatch immediately
        // so a process arriving at tick t can start at tick t.
        if (!contextSwitchInProgress && runningProcess == NULL && readyQueue != NULL)
        {
            runningProcess = removeHead(&readyQueue);
            runningProcess->state = RUNNING;
            quantumCounter = 0;
            runProcess();
            dispatchedNow = 1;
        }

        if ((getClk() == lastClk && !contextSwitchInProgress) || dispatchedNow)
            continue;

        lastClk = getClk();

            // BLOCK 2: handle context-switch overhead first
        if (contextSwitchInProgress)
        {
            if ((getClk() - contextSwitchStartTime) >= CONTEXT_SWITCH_OVERHEAD)
            {
                contextSwitchInProgress = 0;
                contextSwitchStartTime = -1;

                if (pendingProcess != NULL)
                {
                    runningProcess = pendingProcess;
                    pendingProcess = NULL;
                    runningProcess->state = RUNNING;
                    quantumCounter = 0;
                    runProcess();
                }
            }
            continue;
        }

        // BLOCK 3: update remaining time of running process once per clock tick.
        if (runningProcess != NULL)
            runningProcess->remaining--;

        // A finished process should not be preempted/stopped again.
        if (runningProcess != NULL && runningProcess->remaining <= 0)
            continue;
            
        // BLOCK 4: decide who runs
        if (runningProcess == NULL && readyQueue != NULL)
        {
            // nothing running → pick from ready queue
            runningProcess = removeHead(&readyQueue);
            runningProcess->state = RUNNING;
            quantumCounter = 0;
            runProcess();
        }
        else if (runningProcess != NULL && readyQueue != NULL)
        {
            if (strcmp(algo, "HPF") == 0)
            {
                // preempt if higher priority process arrived
                if (readyQueue->priority < runningProcess->priority)
                {
                    struct PCB *nextProcess;

                    // stop current
                    kill(runningProcess->pid, SIGSTOP);
                    runningProcess->state = READY;
                    printStopped(runningProcess);

                    // put back in ready queue
                    insertByPriority(&readyQueue, runningProcess);

                    // context switch to higher priority one
                    nextProcess = removeHead(&readyQueue);
                    runningProcess = NULL;
                    startContextSwitch(nextProcess);
                }
            }
            else if (strcmp(algo, "RR") == 0)
            {
                quantumCounter++;
                if (quantumCounter >= quantum)
                {
                    struct PCB *nextProcess;

                    // quantum expired → stop current
                    kill(runningProcess->pid, SIGSTOP);
                    runningProcess->state = READY;
                    printStopped(runningProcess);

                    // put back at tail for RR
                    inertAtTail(&readyQueue, runningProcess);

                    // context switch to next RR process
                    nextProcess = removeHead(&readyQueue);
                    runningProcess = NULL;
                    startContextSwitch(nextProcess);
                }
            }
        }
    }

    writePerformanceFile();
    if (schedulerLog != NULL)
    {
        fclose(schedulerLog);
        schedulerLog = NULL;
    }
    destroyClk(true);
}

