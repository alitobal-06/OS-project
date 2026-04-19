#include "headers.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int msgq_id;
void clearResources(int);

int main(int argc, char * argv[])
{
    signal(SIGINT, clearResources);

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

    sleep(1);

    int schedulerId = fork();
    if (schedulerId == -1)
    {
        perror("ERROR CREATING SCHEDULER!");
        return 1;
    }
    else if (schedulerId == 0)
    {
        execl("./scheduler.out", "scheduler.out", NULL);
        perror("ERROR STARTING SCHEDULER!");
        return 1;
    }

    initClk();
    key_t key = ftok("keyfile", 65);
    msgq_id = msgget(key, IPC_CREAT | 0666);

    FILE* pFile = fopen("processes.txt", "r");
    if (!pFile)
    {
        perror("ERROR OPENING FILE!");
        return 1;
    }

    char* line;
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

    destroyClk(true);
}

void clearResources(int signum)
{
    msgctl(msgq_id, IPC_RMID, NULL);
    destroyClk(true);
    exit(0);
}
