/*
Authors: Raymon Lee, Estevan Pickett
Course: CSC 452
phase4.c

***
Adding work here to avoid merge conflicts
***

 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <usloss.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase3_usermode.h>
#include <phase4.h>
#include <usyscall.h>
#include "phase3_kernelInterfaces.h"

// Functions
static int ClockDriver(void *);
static int TermDriver(void *);
static int TermReader(void *);
static int TermWriter(void *);
static void sleepReal(USLOSS_Sysargs *);
static void termReadReal(USLOSS_Sysargs *);
static void termWriteReal(USLOSS_Sysargs *);

// Constants
#define TERM_UNITS 4  // Number of terminal units 

// Data structures
typedef struct Sleep_Process {
    int pid;           // Process ID
    int wake_time;     // Time to wake up (in clock ticks)
    struct Sleep_Process *next;
} Sleep_Process;

typedef struct {
    int mbox_id;       // Mailbox for this terminal's device driver
    int mutex;         // Mutex for synchronizing access to the terminal
    int reader_mutex;  // Mutex for synchronizing multiple readers
    int writer_mutex;  // Mutex for synchronizing multiple writers
    
    // Terminal input line buffers
    char buffer[10][MAXLINE + 1];   // might need to add 1 for null terminator if needed ??? 
    int head;                       // Index of next buffer to read
    int tail;                       // Index of next buffer to fill
    int chars_in_current_line;      // Number of characters in current line buffer
    int lines_in_buffer;            // Number of complete lines in buffer
} Term_Info;

// Global variables
static int running;                         // Indicates if drivers are running
static Sleep_Process *sleep_queue = NULL;   // List of sleeping processes
static int clock_driver_pid;                // PID of the clock driver
static int term_driver_pid[TERM_UNITS];     // PIDs of the terminal drivers
static Term_Info term_info[TERM_UNITS];     // Information about each terminal
static int clock_mailbox;                   // Mailbox for synchronizing with the clock
static int term_mailbox[TERM_UNITS];        // Mailboxes for synchronizing with terminals

// Initialize phase 4 data structures and start device drivers
void phase4_init(void) {
    int i, j;
    USLOSS_Console("phase4_init: started\n");
    
    // Initialize terminal data structures
    for (i = 0; i < TERM_UNITS; i++) {
        term_info[i].mbox_id = MboxCreate(0, 0);  // Synchronization mailbox
        term_info[i].mutex = MboxCreate(1, 0);    // Mutex with 1 slot
        term_info[i].reader_mutex = MboxCreate(1, 0); // Reader mutex
        term_info[i].writer_mutex = MboxCreate(1, 0); // Writer mutex
        
        // Initialize line buffer data
        term_info[i].head = 0;
        term_info[i].tail = 0;
        term_info[i].chars_in_current_line = 0;
        term_info[i].lines_in_buffer = 0;
        
        // Clear all buffers
        for (j = 0; j < 10; j++) {
            memset(term_info[i].buffer[j], 0, MAXLINE + 1);
        }
        
        // Create mailbox for this terminal
        term_mailbox[i] = MboxCreate(0, 0);
    }
    
    // Create mailbox for the clock driver
    clock_mailbox = MboxCreate(0, 0);
    
    // Register system call handlers
    systemCallVec[SYS_SLEEP] = sleepReal;
    systemCallVec[SYS_TERMREAD] = termReadReal;
    systemCallVec[SYS_TERMWRITE] = termWriteReal;

    running = 1;
    
    USLOSS_Console("phase4_init: done\n");
}

// Start the device driver processes
void phase4_start_service_processes(void) {
    int i, pid;
    
    // Start the clock driver
    pid = spork("Clock Driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    if (pid < 0) {
        USLOSS_Console("phase4_start_service_processes: can't create clock driver\n");
        USLOSS_Halt(1);
    }
    clock_driver_pid = pid;
    
    // Start the terminal drivers
    for (i = 0; i < TERM_UNITS; i++) {
        pid = spork("Term Driver", TermDriver, (void *)(long)i, USLOSS_MIN_STACK, 2);
        if (pid < 0) {
            USLOSS_Console("phase4_start_service_processes: can't create term driver %d\n", i);
            USLOSS_Halt(1);
        }
        term_driver_pid[i] = pid;
    }
}

// Clock device driver process. Waits for clock interrupts and wakes up processes that have finished sleeping.
static int ClockDriver(void *arg) {
    int status;
    int current_time;
    Sleep_Process *proc, *prev, *next;
    
    // Infinite loop handling clock interrupts
    while (running) {
        // Wait for clock interrupt
        waitDevice(USLOSS_CLOCK_DEV, 0, &status);
        
        // Check for processes to wake up
        if (sleep_queue != NULL) {
            current_time = currentTime();
            
            // Check the queue for processes to wake up
            proc = sleep_queue;
            prev = NULL;
            
            while (proc != NULL) {
                // Save the next pointer before potentially waking this process
                next = proc->next;
                
                if (proc->wake_time <= current_time) {
                    // Wake up this process
                    unblockProc(proc->pid);
                    
                    // Remove from the queue
                    if (prev == NULL) {
                        sleep_queue = proc->next;
                    } else {
                        prev->next = proc->next;
                    }      
                    // Free the structure
                    free(proc);
                } else {
                    // This process stays in the queue
                    prev = proc;
                }          
                proc = next;
            }
        }
    }
    return 0;
}

// Handler for Sleep system call
static void sleepReal(USLOSS_Sysargs *args) {
    int seconds = (int)(long)args->arg1;
    Sleep_Process *proc, *temp, *prev;
    int current_time, wake_time;
    
    // Validate arguments
    if (seconds < 0) {
        args->arg4 = (void *)(long)-1;
        return;
    }
    
    // If seconds == 0, just return immediately
    if (seconds == 0) {
        args->arg4 = (void *)(long)0;
        return;
    }
    
    // Get current time and calculate wake time
    current_time = currentTime();
    wake_time = current_time + (seconds * 1000000);  // Convert to microseconds
    
    // Create a sleep process structure
    proc = (Sleep_Process *)malloc(sizeof(Sleep_Process));
    if (proc == NULL) {
        USLOSS_Console("sleepReal: out of memory\n");
        USLOSS_Halt(1);
    }
    proc->pid = getpid();
    proc->wake_time = wake_time;
    proc->next = NULL;
    
    // Add to the sleep queue in order of wake time
    if (sleep_queue == NULL || sleep_queue->wake_time > wake_time) {
        // Insert at head
        proc->next = sleep_queue;
        sleep_queue = proc;
    } else {
        // Find the right spot in the queue
        temp = sleep_queue;
        prev = NULL;

        while (temp != NULL && temp->wake_time <= wake_time) {
            prev = temp;
            temp = temp->next;
        }
        // Insert after prev
        proc->next = temp;
        prev->next = proc;
    }
    // Block until the clock driver wakes us up
    blockMe();
    args->arg4 = (void *)(long)0;
}

// Terminal device driver process. Waits for terminal interrupts and handles input/output.
static int TermDriver(void *arg) {
    int unit = (int)(long)arg;
    int status;
    int result;
    char ch;
    int i;
    
    // Let parent know we running
    MboxSend(term_mailbox[unit], NULL, 0);
    
    while (running) {
        // Wait for terminal interrupt
        waitDevice(USLOSS_TERM_DEV, unit, &status);
        
        // Check if we received input (biti 5)
        if (USLOSS_TERM_STAT_RECV(status) == USLOSS_DEV_BUSY) {
        }
    }
    return 0;
}

// Handler for TermRead system call
static void termReadReal(USLOSS_Sysargs *args) {
}

// Handler for TermWrite system call
static void termWriteReal(USLOSS_Sysargs *args) {
    
}