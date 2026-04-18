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
    
    //TODO implement the scheduler :)
    //upon termination release the clock resources.
    
    destroyClk(true);
}
