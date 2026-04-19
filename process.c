#include "headers.h"
// #include <stdlib.h>

/* Modify this file as needed*/
int remainingtime;

int main(int argc, char * argv[])
{
    // Checks if remaining time was passed as argument
    if (argc < 2)
    {
        return 1;
    }

    initClk();
    
    remainingtime = atoi(argv[1]);
    
    int lastClk = getClk();

    while (remainingtime > 0)
    {
        while(getClk() == lastClk);
        lastClk = getClk();
        remainingtime--;
    }

    kill(getppid(), SIGUSR1); // signal parent that process has finished
    destroyClk(false);
    
    return 0;
}
