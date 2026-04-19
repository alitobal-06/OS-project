#include "headers.h"

#define READY 0
#define RUNNING 1
#define FINISHED 2

struct PCB {
    int id;
    int arrival;
    int runtime;
    int waiting;
    int remaining;
    int priority;
    int started; // flag tohandle the preemption in hpf 3lshan law bada2 may3mlsh fork tany y33ml SIGCONT
    int pid;
    int state;
    struct PCB *next;  // Pointer to the next PCB in the queue
};

// globasl variables
struct PCB *readyQueue = NULL;   
struct PCB *runningProcess = NULL; 



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

struct PCB *removeHead(struct PCB **head)
{
    
    if (*head == NULL)
        return NULL;
    struct PCB *temp = *head;   
    *head = (*head)->next;      
    temp->next = NULL;         
    return temp;                
}


// process finished

void sigUSR1Handler(int signum)
{
    if (runningProcess == NULL)
        return;
    runningProcess->state = FINISHED;


    printf("At time %d process %d finished arr %d total %d remain %d wait %d\n",
        getClk(),
        runningProcess->id,
        runningProcess->arrival,
        runningProcess->runtime,
        runningProcess->remaining,
        runningProcess->waiting);

    free(runningProcess);
    runningProcess = NULL;
}



// run a process
void runProcess(int currentTime, char *algo)
{
    if (runningProcess->started == 0)
    {
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
        printf("At time %d process %d started arr %d total %d remain %d wait %d\n",
            currentTime,
            runningProcess->id,
            runningProcess->arrival,
            runningProcess->runtime,
            runningProcess->remaining,
            runningProcess->waiting);
    }
    else
    {
        // was stopped before → resume it
        kill(runningProcess->pid, SIGCONT);
        printf("At time %d process %d resumed arr %d total %d remain %d wait %d\n",
            currentTime,
            runningProcess->id,
            runningProcess->arrival,
            runningProcess->runtime,
            runningProcess->remaining,
            runningProcess->waiting);
    }
}



int main(int argc, char *argv[])
{
  
    char *algo = argv[1];
    int quantum = atoi(argv[2]);
    initClk();
    signal(SIGUSR1, sigUSR1Handler);

    
    key_t key = ftok("keyfile", 65);
    int msgq_id = msgget(key, IPC_CREAT | 0666);

   
    int lastClk = -1;
    int quantumCounter = 0;
    while (1)
    {
        int currentTime = getClk();
        if (currentTime != lastClk)
        {
            lastClk = currentTime;

            // BLOCK 1: check message queue for new arrivals
            struct msgbuff msg;
            while (msgrcv(msgq_id, &msg, sizeof(msg) - sizeof(long), 0, IPC_NOWAIT) != -1)
            {
                // create a new PCB for this process
                struct PCB *newProcess = (struct PCB *)malloc(sizeof(struct PCB));
                newProcess->id = msg.id;
                newProcess->arrival = msg.arrival;
                newProcess->runtime = msg.runtime;
                newProcess->remaining = msg.runtime;
                newProcess->priority = msg.priority;
                newProcess->waiting = 0;
                newProcess->state = READY;
                newProcess->next = NULL;

                // insert into ready queue
                insertByPriority(&readyQueue, newProcess);
                printf("Process %d inserted into ready queue at time %d\n", newProcess->id, currentTime);
            }

            // BLOCK 2  
            // update waiting time for all processes in ready queue
            struct PCB *temp = readyQueue;
               while (temp != NULL)
                 {
                  temp->waiting++;
                   temp = temp->next;
                 }
            
                 // BLOCK 3: update remaining time of running process
            if (runningProcess != NULL)
            {
                runningProcess->remaining--;
            }
            
                        // BLOCK 4: decide who runs
            if (runningProcess == NULL && readyQueue != NULL)
            {
                // nothing running → pick from ready queue
                runningProcess = removeHead(&readyQueue);
                runningProcess->state = RUNNING;
                quantumCounter = 0;
                runProcess(currentTime, algo);
            }
            else if (runningProcess != NULL && readyQueue != NULL)
            {
                if (strcmp(algo, "HPF") == 0)
                {
                    // preempt if higher priority process arrived
                    if (readyQueue->priority < runningProcess->priority)
                    {
                        // stop current
                        kill(runningProcess->pid, SIGSTOP);
                        runningProcess->state = READY;
                        printf("At time %d process %d stopped arr %d total %d remain %d wait %d\n",
                            currentTime,
                            runningProcess->id,
                            runningProcess->arrival,
                            runningProcess->runtime,
                            runningProcess->remaining,
                            runningProcess->waiting);

                        // put back in ready queue
                        insertByPriority(&readyQueue, runningProcess);

                        // run higher priority one
                        runningProcess = removeHead(&readyQueue);
                        runningProcess->state = RUNNING;
                        runProcess(currentTime, algo);
                    }
                }
                else if (strcmp(algo, "RR") == 0)
                {
                    quantumCounter++;
                    if (quantumCounter >= quantum)
                    {
                        // quantum expired → stop current
                        kill(runningProcess->pid, SIGSTOP);
                        runningProcess->state = READY;
                        printf("At time %d process %d stopped arr %d total %d remain %d wait %d\n",
                            currentTime,
                            runningProcess->id,
                            runningProcess->arrival,
                            runningProcess->runtime,
                            runningProcess->remaining,
                            runningProcess->waiting);

                        // put back at tail for RR
                        // temporarily use insertByPriority
                        // we will add insertAtTail later
                        insertByPriority(&readyQueue, runningProcess);

                        // run next
                        runningProcess = removeHead(&readyQueue);
                        runningProcess->state = RUNNING;
                        quantumCounter = 0;
                        runProcess(currentTime, algo);
                    }
                }
            }

         
        }
    }

    destroyClk(true);
}

