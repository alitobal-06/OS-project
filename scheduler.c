#include "headers.h"

struct PCB {
    int id;
    int arrival;
    int runtime;
    int remaining;
    int priority;
    int pid;
    int state;
};

int main(int argc, char * argv[])
{
    initClk();

    key_t key = ftok("keyfile", 65);
    int msgq_id = msgget(key, IPC_CREAT | 0666);

    while (1)
    {
        struct msgbuff msg;

        if (msgrcv(msgq_id, &msg, sizeof(msg) - sizeof(long), 0, IPC_NOWAIT) != -1)
        {
            printf("Received process %d at time %d\n", msg.id, getClk());
        }
    }
    
    //TODO implement the scheduler :)
    //upon termination release the clock resources.
    
    destroyClk(true);
}
