#include "headers.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

int msgq_id;
void clearResources(int);

int main(int argc, char * argv[])
{
    signal(SIGINT, clearResources);

    /* Phase 2 is RR-only, so startup asks only for the RR quantum. */
    int quantum = 0;
    int k_quantums = 0;
    printf("Enter RR quantum: ");
    scanf("%d", &quantum);
    printf("Enter K (number of quantums to clear R bits): "); // Add this
    scanf("%d", &k_quantums);
    char quantumStr[10];
    char kStr[10];
    sprintf(quantumStr, "%d", quantum);
    sprintf(kStr, "%d", k_quantums);

    FILE *keyFile = fopen("keyfile", "a");
    if (keyFile != NULL)
        fclose(keyFile);

    key_t key = ftok("keyfile", 65);
    if (key == -1)
    {
        perror("ERROR GENERATING KEY!");
        return 1;
    }

    msgq_id = msgget(key, IPC_CREAT | 0666);
    if (msgq_id == -1)
    {
        perror("ERROR CREATING MESSAGE QUEUE!");
        return 1;
    }
    
    
    int clkId = fork();
    if (clkId == -1)
    {
        perror("ERROR CREATING CLOCK!");
        return 1;
    }
    else if (clkId == 0)
    {
        execl("./clk.out", "clk.out", NULL);
        perror("ERROR STARTING CLOCK!");
        return 1;
    }

    while (shmget(SHKEY, 4, 0444) == -1)
        usleep(10000);

    int schedulerId = fork();
    if (schedulerId == -1)
    {
        perror("ERROR CREATING SCHEDULER!");
        return 1;
    }
    else if (schedulerId == 0)
    {
        execl("./scheduler.out", "scheduler.out", "RR", quantumStr, kStr, NULL);
        perror("ERROR STARTING SCHEDULER!");
        return 1;
    }

    initClk();
    
    FILE* pFile = fopen("processes.txt", "r");
    if (!pFile)
    {
        perror("ERROR OPENING FILE!");
        return 1;
    }

    char line[100];
    while(fgets(line, sizeof(line), pFile))
    {
        if (line[0] == '#')
            continue;
        
        struct msgbuff msg;
        /*
         * Phase 2 process format:
         * id, arrival, runtime, priority, disk base page, virtual page limit.
         */
        if (sscanf(line, "%d\t%d\t%d\t%d\t%d\t%d",
                   &msg.id,
                   &msg.arrival,
                   &msg.runtime,
                   &msg.priority,
                   &msg.base,
                   &msg.limit) != 6)
        {
            printf("Invalid process line skipped: %s", line);
            continue;
        }

        msg.mtype = 1;

        while(getClk() < msg.arrival);

        msgsnd(msgq_id, &msg, sizeof(msg) - sizeof(long), !IPC_NOWAIT);
        // printf("Sent process %d at time %d\n", msg.id, getClk());
    }

    fclose(pFile);

    {
        struct msgbuff doneMsg;
        doneMsg.mtype = 1;
        doneMsg.id = -1;
        doneMsg.arrival = 0;
        doneMsg.runtime = 0;
        doneMsg.priority = 0;
        doneMsg.base = 0;
        doneMsg.limit = 0;
        msgsnd(msgq_id, &doneMsg, sizeof(doneMsg) - sizeof(long), !IPC_NOWAIT);
    }

    waitpid(schedulerId, NULL, 0);
    destroyClk(false);
    return 0;
}

void clearResources(int signum)
{
    msgctl(msgq_id, IPC_RMID, NULL);
    destroyClk(true);
    exit(0);
}
