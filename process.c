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
    int nowClk;

    while (remainingtime > 0)
    {
        nowClk = getClk();
        if (nowClk == lastClk)
            continue;

        if ((nowClk - lastClk) > 1)
        {
            // Process was paused; resync to current clock without consuming CPU time.
            lastClk = nowClk;
            continue;
        }

        lastClk = nowClk;
        remainingtime--;
    }

    kill(getppid(), SIGUSR1); // signal parent that process has finished
    destroyClk(false);
    
    return 0;
}
