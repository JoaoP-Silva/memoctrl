#include "uvm.h"

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>

#include "log.h"

#include "mmu.h"
#include "mmuproto.h"

/* --- Data structures definitions --- */

/* PAGE TABLE */

// An entry in the page table
struct table_entry{
    int page_number;
    int frame;
    int prot;
    int disk_block;
    struct table_entry* next;
};

/* A table containing information about pages in the memory */
struct page_table {
    // Page list
    struct table_entry* head;
    struct table_entry* tail;
    // Mutex for sync
    pthread_mutex_t mutex;
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
    // Next process page
    struct p_pages_node* next;
};

struct p_pages {
    struct p_pages_node* head;
    struct p_pages_node* tail;
};

// List of processes with pages in memory
struct plist_node {
    // Process id
    pid_t pid;
    // Number of allocated pages
    int n_pages;
    // Process pages
    struct p_pages* p_pages;
    // Next process
    struct plist* next;
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

/* External functions */

void pager_init(int nframes, int nblocks){   
    frames.total_frames = nframes;
    frames.free_frames = nframes;
    blocks.total_blocks = nblocks;
    blocks.free_blocks = nblocks;
    page_table.head = NULL;
    page_table.tail = NULL;
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
    if(blocks.free_blocks) block = getFreeBlock();
    else return NULL;
    allocateDiskBlock(block);

    pthread_mutex_lock(&plist.mutex);
    // Locate process in process list
    struct plist_node* currProcess = plist.head; 
    for(int i = 0; i < plist.num_process; i++, currProcess = currProcess->next) if(currProcess->pid == pid) break;    
    currProcess->n_pages++;
    int pageNumber = currProcess->n_pages - 1;
    //Set new page to add to the process page list
    struct p_pages* my_proc_pages = currProcess->p_pages;
    struct p_pages_node* new_page = (struct p_pages_node*)malloc(sizeof(struct p_pages_node));
    new_page->disc_block = block;
    new_page->entry = NULL;
    new_page->next = NULL;
    // Check whether its the first page of the process
    if(pageNumber == 1)
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
    for(int i = 0; i < (int)page_number; i++, currPage = currPage->next);

    //If the zero-fill wasnt perfomed yet, alocate memory for the page and change permissions
    if(!currPage->used)
    {
        pthread_mutex_lock(&frames.mutex);
        int frame = getFreeFrame;
        if(frame != -1)
        {
            allocateFrame(frame);
            pthread_mutex_unlock(&frames.mutex);
        }
        // All frames in use
        else
        {

        }
        struct table_entry* new_entry = (struct table_entry*)malloc(sizeof(struct table_entry));
        currPage->entry = new_entry;
        new_entry->frame = frame;
        new_entry->disk_block = currPage->disc_block;
        new_entry->next = NULL;
        new_entry->page_number = (int)page_number;
        new_entry->prot = PROT_NONE;
        // Append entry to the table
        pthread_mutex_lock(&page_table.mutex);
        if(page_table.head == NULL )
        {
            page_table.head = new_entry;
            page_table.tail = page_table.head;
        }
        else
        {
            struct table_entry* old_tail = page_table.tail;
            old_tail->next = new_entry;
            page_table.tail = new_entry;
        }
        pthread_mutex_unlock(&page_table.mutex);
        pthread_mutex_unlock(&plist.mutex);
        mmu_zero_fill(frame);
        void* _addr = (void*)((intptr_t)addr - UVM_BASEADDR);
        mmu_resident(pid, _addr, frame, PROT_READ);
    }
    // If protection equals PROT_READ or PROT_NOME, the fault is from requiring writing access
    pthread_mutex_lock(&page_table.mutex);
    if(currPage->used && (currPage->entry->prot == PROT_READ || currPage->entry->prot == PROT_NONE))
    {
        currPage->entry->prot = PROT_READ | PROT_WRITE;
        void* _addr = (void*)((intptr_t)addr - UVM_BASEADDR);
        mmu_chprot(pid, _addr, PROT_READ | PROT_WRITE);
    }
}

int pager_syslog(pid_t pid, void *addr, size_t len){}

void pager_destroy(pid_t pid){}

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

intptr_t getVAddr(int pageNumber)
{
    return UVM_BASEADDR + pageNumber * ((intptr_t)0x1000);
}