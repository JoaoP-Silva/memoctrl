#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>

#include "mmu.h"
#include "pager.h"

/* --- Data structures definitions --- */

/* PAGE TABLE */

// An entry in the page table
struct table_entry{
    intptr_t page_number;
    bool in_mem;
    bool wrote;
    int frame;
    int prot;
    int disk_block;
    pid_t pid;
    struct table_entry* next;
};

/* A table containing information about pages in the memory */
struct page_table {
    // Page list
    struct table_entry* head;
    struct table_entry* tail;
    // Mutex for sync
    pthread_mutex_t mutex;
    // Ptr to perform the second change algoritm
    struct table_entry* ptr;
};

/* PROCESS_LIST */

// List of pages in a process
struct p_pages_node{
    // Disk block previously reserved for the page
    int disc_block;
    // Variable to check whether the zero-fill was made
    bool used;
    // Mapped entry in the page table
    struct table_entry* entry;
    // Entry right before
    struct table_entry* previous_entry;
    // Next process page
    struct p_pages_node* next;
};

struct p_pages {
    struct p_pages_node* head;
    struct p_pages_node* tail;
};

// List of processes with declared pages
struct plist_node {
    // Process id
    pid_t pid;
    // Number of allocated pages
    int n_pages;
    // Process pages
    struct p_pages* p_pages;
    // Next process
    struct plist_node* next;
};

/* A list that references the page table by processes*/
struct plist {
    unsigned int num_process;
    // Processes list
    struct plist_node* head;
    struct plist_node* tail;
    // Mutex for sync
    pthread_mutex_t mutex;
};


/* struct to manage frames */
struct frames{
    int total_frames;
    int free_frames;
    pthread_mutex_t mutex;
    // Array to track used frames. 1 represents an used frame and 0 a free frame.
    bool* arr;
};

/* struct to manage blocks */
struct blocks{
    int total_blocks;
    int free_blocks;
    pthread_mutex_t mutex;
    // Array to track used disk blocks. 1 represents an used block and 0 a free block.
    bool* arr;
};

// Initialization
struct page_table page_table;
struct plist plist;
struct frames frames;
struct blocks blocks;

/* Utils methods*/
int getFreeFrame()
{
    int res = -1;
    for(int i = 0; i < frames.total_frames; i ++) if(!frames.arr[i]) 
    {
        res = i;
        break;
    }
    return res;
}

int getFreeBlock()
{
    int res = -1;
    for(int i = 0; i < blocks.total_blocks; i ++)if(!blocks.arr[i])
    {
        res = i;
        break;
    }
    return res; 
}

void allocateDiskBlock(int i)
{
    if(!blocks.arr[i])
    {
        blocks.arr[i] = 1;
        blocks.free_blocks--;
    }else printf("Disk block already reserved to another page.\n");   
}

void allocateFrame(int i)
{
    if(!frames.arr[i])
    {
        frames.arr[i] = 1;
        frames.free_frames--;
    }else printf("Mem frame already reserved\n");
}

void freeDiskBlock(int i)
{
    if(blocks.arr[i])
    {
        blocks.arr[i] = 0;
        blocks.free_blocks++;
    }  
}

void freeMemoryFrame(int i)
{
    if(frames.arr[i])
    {
        frames.arr[i] = 0;
        frames.free_frames++;
    }  
}

intptr_t getVAddr(int pageNumber)
{
    return UVM_BASEADDR + pageNumber * ((intptr_t)0x1000);
}

// Clock (second chance) algorithm (run this method with the mutex locked)
struct table_entry* getUnusedPage()
{
    // Initialize page_table.ptr
    if(page_table.ptr == NULL)page_table.ptr = page_table.head;
    while(page_table.ptr->prot != PROT_NONE)
    {
        // Only consider pages in memory
        if(page_table.ptr->in_mem == 1)
        {
            if(page_table.ptr == NULL)page_table.ptr = page_table.head;
            // Change page protection
            page_table.ptr->prot = PROT_NONE;
            pid_t pid = page_table.ptr->pid;
            intptr_t addr = getVAddr(page_table.ptr->page_number);
            mmu_chprot(pid, (void*)addr, PROT_NONE);
            // Go to next page
        }
        page_table.ptr = page_table.ptr->next;
        if(page_table.ptr == NULL)page_table.ptr = page_table.head;
    }
    struct table_entry* dead_entry = page_table.ptr;
    page_table.ptr = page_table.ptr->next;
    return dead_entry;
}

/* External functions */

void pager_init(int nframes, int nblocks){   
    frames.total_frames = nframes;
    frames.free_frames = nframes;
    blocks.total_blocks = nblocks;
    blocks.free_blocks = nblocks;
    page_table.head = NULL;
    page_table.tail = NULL;
    page_table.ptr = NULL;
    pthread_mutex_init(&page_table.mutex, NULL);

    plist.head = NULL;
    plist.tail = NULL;
    plist.num_process = 0;
    pthread_mutex_init(&plist.mutex, NULL);
    frames.arr = (bool*)malloc(nframes * sizeof(bool));
    blocks.arr = (bool*)malloc(nblocks * sizeof(bool));
    pthread_mutex_init(&frames.mutex, NULL);
    pthread_mutex_init(&blocks.mutex, NULL);
}

void pager_create(pid_t pid){
    pthread_mutex_lock(&plist.mutex);
    struct plist_node* h = plist.head;
    struct plist_node* new = (struct plist_node*)malloc(sizeof(struct plist_node));
    new->n_pages = 0;
    new->next = NULL;
    new->p_pages = NULL;
    new->pid = pid;

    if(h == NULL)
    {
        plist.head = new;
        plist.tail = new;
    }
    else
    {
        plist.tail->next = new;
        plist.tail = new;
    }
    plist.num_process++;
    pthread_mutex_unlock(&plist.mutex);
}


void *pager_extend(pid_t pid){
    // Allocate block to new page
    int block;
    pthread_mutex_lock(&blocks.mutex);
    if(blocks.free_blocks) block = getFreeBlock();
    else return NULL;
    allocateDiskBlock(block);
    pthread_mutex_unlock(&blocks.mutex);
    
    pthread_mutex_lock(&plist.mutex);
    // Locate process in process list
    struct plist_node* currProcess = plist.head; 
    for(int i = 0; i < plist.num_process; i++, currProcess = currProcess->next) if(currProcess->pid == pid) break;    
    currProcess->n_pages++;
    int pageNumber = currProcess->n_pages - 1;

    //Check if the process page list was already declared
    if(currProcess->p_pages == NULL)currProcess->p_pages = (struct p_pages*)malloc(sizeof(struct p_pages));
    struct p_pages* my_proc_pages = currProcess->p_pages;
    
    //Set new page to add to the process page list
    struct p_pages_node* new_page = (struct p_pages_node*)malloc(sizeof(struct p_pages_node));
    new_page->disc_block = block;
    new_page->entry = NULL;
    new_page->next = NULL;
    // Check whether its the first page of the process
    if(pageNumber == 0)
    {
        my_proc_pages->head = new_page;
        my_proc_pages->tail = my_proc_pages->head;
    }
    else
    {
        struct p_pages_node* old_tail = my_proc_pages->tail;
        old_tail->next = new_page;
        my_proc_pages->tail = new_page;
    }
    pthread_mutex_unlock(&plist.mutex);
    return (void*)getVAddr(pageNumber);
}

void pager_fault(pid_t pid, void *addr){
    pthread_mutex_lock(&plist.mutex);
    // Locate process in process list
    struct plist_node* currProcess = plist.head; 
    for(int i = 0; i < plist.num_process; i++, currProcess = currProcess->next) if(currProcess->pid == pid) break;
    // Get page number from the virtual address
    intptr_t page_number = ((intptr_t)addr - UVM_BASEADDR) /  0x1000;
    struct p_pages_node* currPage = currProcess->p_pages->head;
    // Go to the right page
    for(int i = 0; i < page_number; i++, currPage = currPage->next);

    //If the zero-fill wasnt perfomed yet, alocate memory for the page and change permissions
    int used = currPage->used;
    pthread_mutex_lock(&page_table.mutex);
    // First use memory
    if(!used)
    {
        pthread_mutex_lock(&frames.mutex);
        int frame = getFreeFrame();
        if(frame != -1)
        {
            allocateFrame(frame);
        }
        // All frames in use, perform second chance algorithm
        else
        {
            // Unsuded page go to disk
            struct table_entry* dead_entry = getUnusedPage();
            frame = dead_entry->frame;
            int block = dead_entry->disk_block;
            dead_entry->in_mem = 0;
            void* _addr =  (void*)getVAddr(dead_entry->page_number); 
            mmu_nonresident(pid, _addr);
            if(dead_entry->wrote)mmu_disk_write(frame, block);
        }
        struct table_entry* new_entry = (struct table_entry*)malloc(sizeof(struct table_entry));
        currPage->entry = new_entry;
        new_entry->frame = frame;
        new_entry->disk_block = currPage->disc_block;
        new_entry->next = NULL;
        new_entry->page_number = page_number;
        new_entry->prot = PROT_READ;
        new_entry->pid = pid;
        new_entry->in_mem = 1;
        new_entry->wrote = 0;
        // Append entry to the table
        if(page_table.head == NULL )
        {
            page_table.head = new_entry;
            page_table.tail = page_table.head;
            currPage->previous_entry = NULL;
        }
        else
        {
            struct table_entry* old_tail = page_table.tail;
            old_tail->next = new_entry;
            page_table.tail = new_entry;
            currPage->previous_entry = old_tail;
        }
        mmu_zero_fill(frame);
        currPage->used = 1;
        void* _addr = (void*)((intptr_t)addr);
        mmu_resident(pid, _addr, frame, PROT_READ);
        pthread_mutex_unlock(&frames.mutex);
    }
    // Memory already used
    else
    {
        int in_mem = currPage->entry->in_mem;
        if(in_mem)
        {
            currPage->entry->wrote = 1;
            currPage->entry->prot = PROT_READ | PROT_WRITE;
            void* _addr = (void*)((intptr_t)addr);
            mmu_chprot(pid, _addr, PROT_READ | PROT_WRITE);
        }
        else
        {
            // Unsuded page go to disk
            struct table_entry* currEntry = currPage->entry;
            struct table_entry* dead_entry = getUnusedPage();
            int frame = dead_entry->frame;
            int dead_block = dead_entry->disk_block;
            dead_entry->in_mem = 0;
            void* _addr =  (void*)getVAddr(dead_entry->page_number); 
            mmu_nonresident(pid, _addr);
            // Check whether the swapped pages wrote on memory
            if(dead_entry->wrote) mmu_disk_write(frame, dead_block);
            if(currEntry->wrote)mmu_disk_read(currEntry->disk_block, frame);
            else mmu_zero_fill(frame);
            // Update curr entry status
            currEntry->frame = frame;
            currEntry->in_mem = 1;
            currEntry->prot = 1;
            // Set mmu_resident parameters
            frame = currEntry->frame;
            int prot = currEntry->prot;
            _addr = (void*)getVAddr(currEntry->page_number);
            intptr_t pid = currEntry->pid;
            mmu_resident(pid, _addr, frame, prot);
        }
    }
    pthread_mutex_unlock(&plist.mutex);
    pthread_mutex_unlock(&page_table.mutex);
}

int pager_syslog(pid_t pid, void *addr, size_t len){
    pthread_mutex_lock(&page_table.mutex);
    pthread_mutex_lock(&plist.mutex);
    struct plist_node* currProcess = plist.head;
    for(int i = 0; i < plist.num_process; i++, currProcess = currProcess->next) if(currProcess->pid == pid) break;    
    
    int pageNumber = currProcess->n_pages;
    intptr_t rem_bytes = 0;
    if(len * 8 > 0x1000)rem_bytes = (intptr_t)(len * 8) % 0x1000;
    intptr_t offset_bytes = ((intptr_t)addr - UVM_BASEADDR) % 0x1000; 
    intptr_t offset_pages = ((intptr_t)addr - UVM_BASEADDR) / ((intptr_t)0x1000);
    intptr_t num_pages_write = len / 4;

	if (pageNumber == 0) {
        pthread_mutex_unlock(&page_table.mutex);
		pthread_mutex_unlock(&plist.mutex);
		return -1;
	}
    else {
        intptr_t end_addr_req  =  (intptr_t)addr + (intptr_t)len * 8;
        intptr_t end_addr_real  =  UVM_BASEADDR + (intptr_t)(0x1000 * (intptr_t)pageNumber);

        if(end_addr_req > end_addr_real || (intptr_t)addr < UVM_BASEADDR) {
            pthread_mutex_unlock(&page_table.mutex);
            pthread_mutex_unlock(&plist.mutex);
            return -1;
        }
        // else if(len > 4096) {
        //     pthread_mutex_unlock(&page_table.mutex);
        //     pthread_mutex_unlock(&plist.mutex);
        //     return -1;
        // }
    }
    char* buf = (char *)malloc(len * sizeof(char));
    struct p_pages_node* currPage = currProcess->p_pages->head; 
    // Go to initial page to write
    for(int i = 0; i < offset_pages; i++, currPage = currPage->next);
    int frame = currPage->entry->frame;
    for(intptr_t i = 0; i < num_pages_write; i++, currPage = currPage->next)
    {
        frame = currPage->entry->frame;
        // If is the first frame, add offset bytes
        if(i == 0)for(int j = offset_bytes; j < len && j < 0x1000; j++)buf[j] = pmem[frame + j];
        // In middle frames, get the full page
        else for(int j = 0; j < len && j < 0x1000; j++)buf[j] = pmem[frame + j];
    }
    // get the remaining bytes
    if(rem_bytes)
    {
        frame = currPage->entry->frame;
        for(int i = 0; i < rem_bytes; i++)buf[i] = pmem[frame + i];
    }
    // Print buffer
    for(int i = 0; i < len; i++) printf("%02x", (unsigned)buf[i]);
    printf("\n");
    pthread_mutex_unlock(&page_table.mutex);
    pthread_mutex_unlock(&plist.mutex);
    return 0;
    
}

void pager_destroy(pid_t pid){
    pthread_mutex_lock(&plist.mutex);
    if (plist.num_process == 0) goto end;
    // Locate process in process list and mantain the proces right before
    struct plist_node* currProcess = plist.head;
    struct plist_node* prevProcess = NULL;
    for(int i = 0; i < plist.num_process; i++)
    {
        if(currProcess->pid == pid) break;
        prevProcess=currProcess;
        currProcess = currProcess->next;
        
    }
    // Whether the process has allocated pages, remove it
    if(currProcess->p_pages != NULL)
    {
        struct p_pages_node* currPage = currProcess->p_pages->head;
        // Remove all nodes except head
        while(currPage->next != NULL)
        {
            struct p_pages_node* toRemove = currPage->next;
            currPage->next = toRemove->next;
            // Remove from page table
            pthread_mutex_lock(&page_table.mutex);
            struct table_entry* prev = toRemove->previous_entry;
            struct table_entry* curr = toRemove->entry;
            if(curr != NULL)
            {
                struct table_entry* next = toRemove->entry->next;
                int frame = curr->frame;
                int block = curr->disk_block;
                // If you are removing the head of the page table, update head
                if(prev == NULL) page_table.head = next;
                else prev->next = next;
                free(curr);
                pthread_mutex_unlock(&page_table.mutex);
                // Remove from process page table
                free(toRemove);
                // Free resources
                pthread_mutex_lock(&blocks.mutex);
                freeDiskBlock(block);
                pthread_mutex_unlock(&blocks.mutex);
                pthread_mutex_lock(&frames.mutex);
                freeMemoryFrame(frame);
                pthread_mutex_unlock(&frames.mutex);
            }
            else
            {
                int block = currPage->disc_block;
                pthread_mutex_lock(&blocks.mutex);
                freeDiskBlock(block);
                pthread_mutex_unlock(&blocks.mutex);
            }
        }
    
        // Remove head node
        struct p_pages_node* toRemove = currPage;
        struct table_entry* prev = toRemove->previous_entry;
        struct table_entry* curr = toRemove->entry;
        // Check if the page realy was in the page table
        if(curr != NULL)
        {
            int frame = curr->frame;
            int block = curr->disk_block;
            struct table_entry* next = toRemove->entry->next;
            // If you are removing the head of the page table, update head
            if(prev == NULL) page_table.head = next;
            else prev->next = next;
            free(curr);
            pthread_mutex_unlock(&page_table.mutex);
            // Remove from process page table
            free(toRemove);
            // Free resources
            pthread_mutex_lock(&blocks.mutex);
            freeDiskBlock(block);
            pthread_mutex_unlock(&blocks.mutex);
            pthread_mutex_lock(&frames.mutex);
            freeMemoryFrame(frame);
        }
        // If the page wasnt in the page table, only the block was reserved
        else
        {
            int block = currPage->disc_block;
            pthread_mutex_lock(&blocks.mutex);
            freeDiskBlock(block);
            pthread_mutex_unlock(&blocks.mutex);
        }
    }
    // Remove the process block from the process list
    if(prevProcess == NULL) plist.head = currProcess->next;
    else prevProcess->next = currProcess->next;
    free(currProcess->p_pages);
    free(currProcess);
    plist.num_process--;
    end:
    pthread_mutex_unlock(&plist.mutex);
    // Whether has no more processes, dealocate resources
    if(plist.num_process == 0)
    {
        free(frames.arr);
        free(blocks.arr);
    }
}