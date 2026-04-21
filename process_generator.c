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

    
    // Get scheduling algorithm and quantum if needed
    char algo[10];
    int quantum = 0;
    printf("Choose scheduling algorithm:\n");
    printf("1. HPF\n");
    printf("2. RR\n");
    printf("Enter algorithm (HPF/RR): ");
    scanf("%9s", algo);

    if (strcmp(algo, "1") == 0)
        strcpy(algo, "HPF");
    else if (strcmp(algo, "2") == 0)
        strcpy(algo, "RR");
    
    if (strcmp(algo, "RR") == 0)
    {
        printf("Enter quantum: ");
        scanf("%d", &quantum);
    }
    
    char quantumStr[10];
    sprintf(quantumStr, "%d", quantum);

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
    
    
    int schedulerId = fork();
    if (schedulerId == -1)
    {
        perror("ERROR CREATING SCHEDULER!");
        return 1;
    }
    else if (schedulerId == 0)
    {
        execl("./scheduler.out", "scheduler.out", algo, quantumStr, NULL);
        perror("ERROR STARTING SCHEDULER!");
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
        sscanf(line, "%d\t%d\t%d\t%d",
        &msg.id,
        &msg.arrival,
        &msg.runtime,
        &msg.priority);

        msg.mtype = 1;

        while(getClk() < msg.arrival);

        msgsnd(msgq_id, &msg, sizeof(msg) - sizeof(long), !IPC_NOWAIT);
        printf("Sent process %d at time %d\n", msg.id, getClk());
    }

    fclose(pFile);

    {
        struct msgbuff doneMsg;
        doneMsg.mtype = 1;
        doneMsg.id = -1;
        doneMsg.arrival = 0;
        doneMsg.runtime = 0;
        doneMsg.priority = 0;
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
