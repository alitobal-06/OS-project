#include <stdio.h>
int getClk(void);
/* Total simulated physical memory size from the Phase 2 document. */
#define RAM_SIZE 512

/* Each page/frame stores 16 bytes, so page number = address / 16. */
#define PAGE_SIZE 16

/* Physical memory has 512 / 16 = 32 frames. */
#define FRAME_COUNT (RAM_SIZE / PAGE_SIZE)

/* The system uses 10-bit addresses, so virtual memory can have 1024 / 16 = 64 pages. */
#define MAX_VIRTUAL_PAGES 64

/* Fixed-size table for tracking page tables during the simulation. */
#define MAX_TRACKED_PROCESSES 100

/* Sentinel values used when a process/page/frame is not assigned yet. */
#define INVALID_PROCESS_ID -1
#define INVALID_PAGE_NUMBER -1
#define INVALID_FRAME_NUMBER -1

/* Return codes for mmu_access. */
#define MMU_ACCESS_INVALID -1
#define MMU_ACCESS_HIT 0
#define MMU_ACCESS_PAGE_FAULT 1

/* One disk read costs 10 ticks. A modified victim will add another 10 ticks later. */
#define DISK_ACCESS_TIME 10

typedef struct Frame
{
    int frameNumber;
    int isFree;
    int isPageTable;
    int ownerProcessId;
    int virtualPageNumber;
    int referenced;
    int modified;
} Frame;

typedef struct PageTableEntry
{
    int present;
    int frameNumber;
    int referenced;
    int modified;
} PageTableEntry;

typedef struct ProcessPageTable
{
    int inUse;
    int processId;
    int pageCount;
    int diskBase;
    int pageTableFrame;
    PageTableEntry entries[MAX_VIRTUAL_PAGES];
} ProcessPageTable;

/* Function prototypes used by the MMU implementation in this file. */
void mmu_init(void);
int mmu_allocate_page_table(int processId, int pageCount);
int mmu_load_first_page(int processId, int diskBase);
int mmu_access(int processId, int virtualAddress, char operation);
int mmu_handle_page_fault(int processId, int virtualAddress, char operation, int loadTime);
int mmu_start_process(int processId, int pageCount, int diskBase);
void mmu_clear_reference_bits(void);
void mmu_finish_process(int processId);
ProcessPageTable *mmu_get_page_table(int processId);



/* One record for each physical frame in the simulated RAM. */
Frame frameTable[FRAME_COUNT];

/* Fixed-size storage for process page tables managed by the MMU. */
ProcessPageTable processPageTables[MAX_TRACKED_PROCESSES];

/*
 * Resets one physical frame to the initial free state.
 * This is used at startup and later can be reused when a process finishes.
 */
static void resetFrame(Frame *frame, int frameNumber)
{
    frame->frameNumber = frameNumber;
    frame->isFree = 1;
    frame->isPageTable = 0;
    frame->ownerProcessId = INVALID_PROCESS_ID;
    frame->virtualPageNumber = INVALID_PAGE_NUMBER;
    frame->referenced = 0;
    frame->modified = 0;
}

/*
 * Makes one page-table entry invalid.
 * At process creation, all virtual pages start as not present in RAM.
 */
static void resetPageTableEntry(PageTableEntry *entry)
{
    entry->present = 0;
    entry->frameNumber = INVALID_FRAME_NUMBER;
    entry->referenced = 0;
    entry->modified = 0;
}

/*
 * Clears a process page-table tracking slot.
 * This does not free physical frames by itself; it only resets MMU metadata.
 */
static void resetProcessPageTable(ProcessPageTable *pageTable)
{
    int i;

    pageTable->inUse = 0;
    pageTable->processId = INVALID_PROCESS_ID;
    pageTable->pageCount = 0;
    pageTable->diskBase = INVALID_PAGE_NUMBER;
    pageTable->pageTableFrame = INVALID_FRAME_NUMBER;

    for (i = 0; i < MAX_VIRTUAL_PAGES; i++)
        resetPageTableEntry(&pageTable->entries[i]);
}

/*
 * Finds the first free physical frame.
 * Requirement: the MMU must always search for a free frame before replacement.
 */
static int findFreeFrame(void)
{
    int i;

    for (i = 0; i < FRAME_COUNT; i++)
    {
        if (frameTable[i].isFree)
            return i;
    }

    return INVALID_FRAME_NUMBER;
}

/* Finds the internal page-table tracking slot for a process id. */
static int findPageTableSlot(int processId)
{
    int i;

    for (i = 0; i < MAX_TRACKED_PROCESSES; i++)
    {
        if (processPageTables[i].inUse && processPageTables[i].processId == processId)
            return i;
    }

    return -1;
}

/* Finds an unused process page-table tracking slot. */
static int findFreePageTableSlot(void)
{
    int i;

    for (i = 0; i < MAX_TRACKED_PROCESSES; i++)
    {
        if (!processPageTables[i].inUse)
            return i;
    }

    return -1;
}

/*
 * Step 9: choose a victim frame using NRU.
 *
 * NRU classes are:
 *   class 0: R = 0, M = 0
 *   class 1: R = 0, M = 1
 *   class 2: R = 1, M = 0
 *   class 3: R = 1, M = 1
 *
 * The project requires deterministic behavior:
 * scan physical frames in ascending order and choose the first frame
 * from the lowest non-empty NRU class.
 *
 * Free frames and page-table frames are skipped because only resident
 * data/code pages can be replaced.
 */
static int selectNruVictimFrame(void)
{
    int classIndex;
    int i;

    for (classIndex = 0; classIndex < 4; classIndex++)
    {
        for (i = 0; i < FRAME_COUNT; i++)
        {
            int r;
            int m;
            int classValue;

            if (frameTable[i].isFree || frameTable[i].isPageTable)
                continue;

            r = frameTable[i].referenced ? 1 : 0;
            m = frameTable[i].modified ? 1 : 0;
            classValue = (r << 1) | m;

            if (classValue == classIndex)
                return i;
        }
    }

    return INVALID_FRAME_NUMBER;
}

/*
 * Removes a resident data/code page from a frame.
 *
 * This is used when NRU chooses a victim. The frame table alone is not enough:
 * the old owner's page-table entry must also be invalidated, otherwise that
 * process would still think the evicted virtual page is resident.
 *
 * Return value:
 *   1 if the victim was modified and needs write-back
 *   0 if the victim was clean or the frame was not a valid data-page victim
 */
static int invalidateVictimFrame(int frameNumber)
{
    Frame *frame;
    ProcessPageTable *ownerTable;
    PageTableEntry *entry;
    int victimModified = 0;

    if (frameNumber < 0 || frameNumber >= FRAME_COUNT)
        return 0;

    frame = &frameTable[frameNumber];
    if (frame->isFree || frame->isPageTable)
        return 0;

    victimModified = frame->modified ? 1 : 0;

    ownerTable = mmu_get_page_table(frame->ownerProcessId);
    if (ownerTable != NULL &&
        frame->virtualPageNumber >= 0 &&
        frame->virtualPageNumber < MAX_VIRTUAL_PAGES)
    {
        entry = &ownerTable->entries[frame->virtualPageNumber];
        entry->present = 0;
        entry->frameNumber = INVALID_FRAME_NUMBER;
        entry->referenced = 0;
        entry->modified = 0;
    }

    resetFrame(frame, frameNumber);
    return victimModified;
}

/*
 * Gets a frame for a process page table.
 * Page-table frames are resident for the process lifetime and will not be NRU victims.
 */
static int getFrameForPageTable(void)
{
    int frameNumber = findFreeFrame();

    if (frameNumber != INVALID_FRAME_NUMBER)
        return frameNumber;

    frameNumber = selectNruVictimFrame();
    if (frameNumber != INVALID_FRAME_NUMBER)
        invalidateVictimFrame(frameNumber);

    return frameNumber;
}

/*
 * Gets a frame for a normal process data/code page.
 * If memory is full, NRU chooses a data-page victim.
 *
 * This helper only chooses the frame. The caller is responsible for logging
 * any replacement event and invalidating the victim before mapping new data.
 */
static int getFrameForDataPage(void)
{
    int frameNumber = findFreeFrame();

    if (frameNumber != INVALID_FRAME_NUMBER)
        return frameNumber;

    return selectNruVictimFrame();
}

/* Converts a numeric virtual address back to binary text for memory.log. */
static void virtualAddressToBinaryString(int virtualAddress, char *buffer, int bufferSize)
{
    int bitStarted = 0;
    int writeIndex = 0;
    int bit;

    if (bufferSize <= 0)
        return;

    if (virtualAddress == 0)
    {
        buffer[0] = '0';
        if (bufferSize > 1)
            buffer[1] = '\0';
        return;
    }

    for (bit = 9; bit >= 0 && writeIndex < bufferSize - 1; bit--)
    {
        if ((virtualAddress & (1 << bit)) != 0)
            bitStarted = 1;

        if (bitStarted)
            buffer[writeIndex++] = ((virtualAddress & (1 << bit)) != 0) ? '1' : '0';
    }

    buffer[writeIndex] = '\0';
}

/*
 * Marks one physical frame as holding a resident data/code page
 * and updates the matching page-table entry.
 */
static void mapDataPage(ProcessPageTable *pageTable, int processId, int virtualPageNumber, int frameNumber, char operation)
{
    PageTableEntry *entry = &pageTable->entries[virtualPageNumber];

    frameTable[frameNumber].isFree = 0;
    frameTable[frameNumber].isPageTable = 0;
    frameTable[frameNumber].ownerProcessId = processId;
    frameTable[frameNumber].virtualPageNumber = virtualPageNumber;
    frameTable[frameNumber].referenced = 1;
    frameTable[frameNumber].modified = (operation == 'w');

    entry->present = 1;
    entry->frameNumber = frameNumber;
    entry->referenced = 1;
    entry->modified = (operation == 'w');
}

/*
 * Step 4: initialize the memory system.
 * All physical frames start free, all process page-table slots start unused,
 * and memory.log starts empty for the new run.
 */
void mmu_init(void)
{
    FILE *memoryLog;
    int i;

    for (i = 0; i < FRAME_COUNT; i++)
        resetFrame(&frameTable[i], i);

    for (i = 0; i < MAX_TRACKED_PROCESSES; i++)
        resetProcessPageTable(&processPageTables[i]);

    memoryLog = fopen("memory.log", "w");
    if (memoryLog == NULL)
    {
        perror("ERROR OPENING memory.log");
        return;
    }

    fclose(memoryLog);
}

/*
 * Step 5: allocate one physical frame for a process page table.
 * pageCount is the process limit from processes.txt.
 * Returns the page-table frame number so the scheduler can store it in the PCB.
 */
int mmu_allocate_page_table(int processId, int pageCount)
{
    int slot;
    int frameNumber;
    int i;

    if (processId < 0 || pageCount <= 0 || pageCount > MAX_VIRTUAL_PAGES)
        return INVALID_FRAME_NUMBER;

    /* If the process already has a page table, return the existing frame. */
    slot = findPageTableSlot(processId);
    if (slot != -1)
        return processPageTables[slot].pageTableFrame;

    slot = findFreePageTableSlot();
    if (slot == -1)
        return INVALID_FRAME_NUMBER;

    frameNumber = getFrameForPageTable();
    if (frameNumber == INVALID_FRAME_NUMBER)
        return INVALID_FRAME_NUMBER;

    /* Register the process page table in MMU metadata. */
    processPageTables[slot].inUse = 1;
    processPageTables[slot].processId = processId;
    processPageTables[slot].pageCount = pageCount;
    processPageTables[slot].pageTableFrame = frameNumber;

    /* Initially, every virtual page is invalid/not present. */
    for (i = 0; i < MAX_VIRTUAL_PAGES; i++)
        resetPageTableEntry(&processPageTables[slot].entries[i]);

    /* Mark the chosen physical frame as a resident page-table frame. */
    frameTable[frameNumber].isFree = 0;
    frameTable[frameNumber].isPageTable = 1;
    frameTable[frameNumber].ownerProcessId = processId;
    frameTable[frameNumber].virtualPageNumber = INVALID_PAGE_NUMBER;
    frameTable[frameNumber].referenced = 0;
    frameTable[frameNumber].modified = 0;

    return frameNumber;
}

/*
 * Step 6: load the first virtual page of a process into RAM.
 * The Phase 2 document says page-table allocation and first-page loading
 * take no extra simulated time, so this function only updates MMU metadata.
 *
 * diskBase is the starting page number of this process image on disk.
 * For the first page, disk address = diskBase + 0.
 *
 * Returns the physical frame number that now contains virtual page 0,
 * or -1 if the process has no page table or no frame is available yet.
 */
int mmu_load_first_page(int processId, int diskBase)
{
    ProcessPageTable *pageTable;
    PageTableEntry *firstEntry;
    int frameNumber;
    int usedFreeFrame;

    pageTable = mmu_get_page_table(processId);
    if (pageTable == NULL || pageTable->pageCount <= 0)
        return INVALID_FRAME_NUMBER;

    pageTable->diskBase = diskBase;

    firstEntry = &pageTable->entries[0];
    if (firstEntry->present)
        return firstEntry->frameNumber;

    frameNumber = findFreeFrame();
    usedFreeFrame = (frameNumber != INVALID_FRAME_NUMBER);
    if (!usedFreeFrame)
        frameNumber = selectNruVictimFrame();

    if (frameNumber == INVALID_FRAME_NUMBER)
        return INVALID_FRAME_NUMBER;

    if (!usedFreeFrame)
        invalidateVictimFrame(frameNumber);

    frameTable[frameNumber].isFree = 0;
    frameTable[frameNumber].isPageTable = 0;
    frameTable[frameNumber].ownerProcessId = processId;
    frameTable[frameNumber].virtualPageNumber = 0;
    frameTable[frameNumber].referenced = 0;
    frameTable[frameNumber].modified = 0;

    firstEntry->present = 1;
    firstEntry->frameNumber = frameNumber;
    firstEntry->referenced = 0;
    firstEntry->modified = 0;

    return frameNumber;
}

/*
 * Step 7: handle one memory access request for a process.
 *
 * virtualAddress is the numeric value of the requested virtual address.
 * If the request file stores addresses in binary text, the parser should
 * convert that binary string to an integer before calling this function.
 *
 * operation should be:
 *   'r' for read
 *   'w' for write
 *
 * Return values:
 *   MMU_ACCESS_HIT        page is resident; R/M bits were updated
 *   MMU_ACCESS_PAGE_FAULT page is valid for the process but not resident
 *   MMU_ACCESS_INVALID    bad process, bad address, or bad operation
 */
int mmu_access(int processId, int virtualAddress, char operation)
{
    ProcessPageTable *pageTable;
    PageTableEntry *entry;
    Frame *frame;
    int virtualPageNumber;
    int frameNumber;

    if (virtualAddress < 0 || (operation != 'r' && operation != 'w'))
        return MMU_ACCESS_INVALID;

    pageTable = mmu_get_page_table(processId);
    if (pageTable == NULL)
        return MMU_ACCESS_INVALID;

    virtualPageNumber = virtualAddress / PAGE_SIZE;
    if (virtualPageNumber < 0 || virtualPageNumber >= pageTable->pageCount)
        return MMU_ACCESS_INVALID;

    entry = &pageTable->entries[virtualPageNumber];
    if (!entry->present)
        return MMU_ACCESS_PAGE_FAULT;

    frameNumber = entry->frameNumber;
    if (frameNumber < 0 || frameNumber >= FRAME_COUNT)
        return MMU_ACCESS_INVALID;

    frame = &frameTable[frameNumber];
    if (frame->isFree || frame->isPageTable || frame->ownerProcessId != processId)
        return MMU_ACCESS_INVALID;

    entry->referenced = 1;
    frame->referenced = 1;

    if (operation == 'w')
    {
        entry->modified = 1;
        frame->modified = 1;
    }

    return MMU_ACCESS_HIT;
}

/*
 * Step 8: handle a page fault for a valid process page.
 *
 * This function performs the MMU side of the fault:
 *   1. log the page fault
 *   2. allocate a free frame if one exists
 *   3. otherwise choose a data-page victim using NRU
 *   4. write back the victim if it is modified
 *   5. load the demanded page into the selected frame
 *   6. update the frame table and the process page table
 *   7. return how many ticks the disk operation should block the process
 */
int mmu_handle_page_fault(int processId, int virtualAddress, char operation, int loadTime)
{
    ProcessPageTable *pageTable;
    FILE *memoryLog;
    char virtualAddressText[16];
    int virtualPageNumber;
    int diskAddress;
    int frameNumber;
    int usedFreeFrame;
    int victimModified;
    int diskDelay;

    if (virtualAddress < 0 || (operation != 'r' && operation != 'w'))
        return MMU_ACCESS_INVALID;

    pageTable = mmu_get_page_table(processId);
    if (pageTable == NULL)
        return MMU_ACCESS_INVALID;

    virtualPageNumber = virtualAddress / PAGE_SIZE;
    if (virtualPageNumber < 0 || virtualPageNumber >= pageTable->pageCount)
        return MMU_ACCESS_INVALID;

    if (pageTable->entries[virtualPageNumber].present)
        return 0;

    virtualAddressToBinaryString(virtualAddress, virtualAddressText, sizeof(virtualAddressText));

    memoryLog = fopen("memory.log", "a");
    if (memoryLog == NULL)
    {
        perror("ERROR OPENING memory.log");
        return MMU_ACCESS_INVALID;
    }

    fprintf(memoryLog, "PageFault upon VA %s from process %d\n", virtualAddressText, processId);

    frameNumber = findFreeFrame();
    usedFreeFrame = (frameNumber != INVALID_FRAME_NUMBER);
    if (!usedFreeFrame)
        frameNumber = selectNruVictimFrame();

    if (frameNumber == INVALID_FRAME_NUMBER)
    {
        fclose(memoryLog);
        return MMU_ACCESS_INVALID;
    }

    if (usedFreeFrame)
    {
        fprintf(memoryLog, "Free Physical page %d allocated\n", frameNumber);
        victimModified = 0;
    }
    else
    {
        victimModified = invalidateVictimFrame(frameNumber);
        if (victimModified)
            fprintf(memoryLog, "Swapping out page %d to disk\n", frameNumber);
    }

    diskAddress = pageTable->diskBase + virtualPageNumber;
    mapDataPage(pageTable, processId, virtualPageNumber, frameNumber, operation);
    diskDelay = DISK_ACCESS_TIME;
    if (victimModified)
        diskDelay += DISK_ACCESS_TIME;
    // fprintf(memoryLog,
    //         "At time %d disk address %d for process %d is loaded into memory page %d.\n",
    //         loadTime + diskDelay,
    //         diskAddress,
    //         processId,
    //         frameNumber);

    fclose(memoryLog);

    return diskDelay;
}

/*
 * Step 10: clear R bits for all resident data pages.
 *
 * The scheduler should call this after every K Round Robin quantums.
 * Page-table frames are skipped because NRU only works on data/code pages.
 */
void mmu_clear_reference_bits(void)
{
    int i;

    for (i = 0; i < FRAME_COUNT; i++)
    {
        Frame *frame = &frameTable[i];
        ProcessPageTable *ownerTable;
        PageTableEntry *entry;

        if (frame->isFree || frame->isPageTable)
            continue;

        frame->referenced = 0;
        ownerTable = mmu_get_page_table(frame->ownerProcessId);
        if (ownerTable == NULL)
            continue;

        if (frame->virtualPageNumber < 0 || frame->virtualPageNumber >= MAX_VIRTUAL_PAGES)
            continue;

        entry = &ownerTable->entries[frame->virtualPageNumber];
        if (entry->present)
            entry->referenced = 0;
    }
}

/*
 * Step 11: free all frames and metadata for a finished process.
 *
 * This releases both:
 *   - the process page-table frame
 *   - all resident data/code frames owned by that process
 *
 * After this, later processes may reuse those physical frames.
 */
void mmu_finish_process(int processId)
{
    int i;
    ProcessPageTable *pageTable = mmu_get_page_table(processId);

    if (pageTable == NULL)
        return;

    for (i = 0; i < FRAME_COUNT; i++)
    {
        if (!frameTable[i].isFree && frameTable[i].ownerProcessId == processId)
            resetFrame(&frameTable[i], i);
    }

    resetProcessPageTable(pageTable);
}

/*
 * Convenience helper for scheduler integration.
 *
 * The scheduler can call this when a process starts for the first time.
 * It allocates the process page-table frame and loads virtual page 0.
 */
int mmu_start_process(int processId, int pageCount, int diskBase)
{
    int pageTableFrame = mmu_allocate_page_table(processId, pageCount);
    if (pageTableFrame == INVALID_FRAME_NUMBER)
        return INVALID_FRAME_NUMBER;

    int firstPageFrame = mmu_load_first_page(processId, diskBase);
    if (firstPageFrame == INVALID_FRAME_NUMBER)
        return INVALID_FRAME_NUMBER;

    // Person 3 Fix: Log the initial allocations to match TA sample
    FILE *memoryLog = fopen("memory.log", "a");
    if (memoryLog != NULL)
    {
        fprintf(memoryLog, "Free Physical page %d allocated\n", pageTableFrame);
        fprintf(memoryLog, "Free Physical page %d allocated\n", firstPageFrame);
        // Note: getClk() isn't directly in MMU.c, so we assume time matches arrival for now, 
        // or you can pass the clock time into this function if you want exact TA clock matching.
        fprintf(memoryLog, "At time %d disk address %d for process %d is loaded into memory page %d.\n",
                getClk(), diskBase, processId, firstPageFrame);
        fclose(memoryLog);
    }

    return pageTableFrame;
}
/*
 * Lets the scheduler or later MMU functions retrieve a process page table.
 * Returns NULL if mmu_allocate_page_table was not called for this process yet.
 */
ProcessPageTable *mmu_get_page_table(int processId)
{
    int slot = findPageTableSlot(processId);

    if (slot == -1)
        return NULL;

    return &processPageTables[slot];
}
void mmu_log_completion(int processId, int virtualAddress, int time) {
    ProcessPageTable *pageTable = mmu_get_page_table(processId);
    if (pageTable == NULL) return;

    int virtualPageNumber = virtualAddress / 16; // 16 is PAGE_SIZE
    int diskAddress = pageTable->diskBase + virtualPageNumber;
    int frameNumber = pageTable->entries[virtualPageNumber].frameNumber;

    FILE *memoryLog = fopen("memory.log", "a");
    if (memoryLog != NULL) {
        fprintf(memoryLog, "At time %d disk address %d for process %d is loaded into memory page %d.\n",
                time, diskAddress, processId, frameNumber);
        fclose(memoryLog);
    }
}
