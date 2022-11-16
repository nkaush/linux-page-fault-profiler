# Page Fault Profiler Linux Kernel Module

## Usage:

An application that wishes to use this page fault profiler must implement the
following callbacks to interact with the kernel module: 
* Application registration - Notify the module to monitor this application
* Application deregistration - Notify the module to remove the application from the module

## Example C Code

### Application Registration

```C++
char* buf = NULL;
FILE* f = fopen("/proc/mp3/status", "w");

asprintf(&buf, "R,%d", getpid());
fwrite(buf, strlen(buf), 1, f);
fclose(f); free(buf);
```

### Verifying that the Application Was Registered
```C++
int is_registered = 0; // initially false...
char *lineptr = NULL, *colon = NULL;
FILE *f = fopen("/proc/mp3/status", "r");
size_t buf_size = 0;

while ( getline(&lineptr, &buf_size, f) != EOF ) {
    lineptr[strlen(lineptr) - 1] = '\0';
    if ( atoi(lineptr) == getpid() ) {
        is_registered = 1; // this process is registered!
        break;
    }
}

free(lineptr); fclose(f);
// continue...
```

### Application Deregistration

```C++
char* buf = NULL;
FILE* f = fopen("/proc/mp3/status", "w");
asprintf(&buf, "U,%d", getpid());
fwrite(buf, strlen(buf), 1, f);
fclose(f); free(buf);
```

## Implementation

This module uses an entry in the proc filesystem to allow applications in the userspace to interact with it. Userspace applications can register and deregister using the protocol specified above as the module uses a single write callback to handle all kinds of interactions. Userspace applications can verify that they are registered by checking whether its pid is contained in the list of pids copied on a read to the proc filesystem entry. The module uses a read callback to support this. It also keeps track of all processes registered at any given instant with an internal linked list.

The module uses an internal delayed workqueue to periodically collect minor and major page faults and CPU utilization for all registered processes. Every 50 milliseconds, the module will sum the statistics for all registered processes and write them to a memory buffer. This memory buffer is exposed with a character device. Userspace processes can access this memory buffer by `mmap`ing the character device. This kernel module supports reads and writes to the character device, and thus the buffer, by implementing a custom page fault handler. When an access to a page in the character device generates a fault, the kernel module callback function will copy the memory in the buffer corresponding to the page requested to the userspace virtual memory. 

## Analysis

This data was collected on a VM with 3GB RAM and 2 CPU cores. 

### Case Study 1

![](case_study_1_work_1_2.png)
![](case_study_1_work_3_4.png)
![](extra/case_study_1_work_1_2_3_4.png)

### Case Study 2

Below are the plots of the CPU utilization and total time for completion for 1, 5, 11, 16, and 21 processes. There is a roughly exponential growth in CPU utilization as the number of concurrent processes increases. We observe an exponential growth since the kernel must take time to swap out pages as each process requests to access some memory. The CPU spends more overhead swapping pages in addition to handling the syscalls such for each process. There are not many swaps needed for 1, 5, and 11 processes since the memory required for these numbers of concurrent processes is below/nearing the memory limit imposed on the VM. The CPU must perform many more swaps for 16 and 21 processes since running that many passes the RAM limit imposed on the VM. 

![](case_study_2_work_5.png)

We observe a minimal increase in the runtime for 16 processes and a large increase in the runtime of 21 concurrent processes. My VM begins to max out its RAM usage with 16 and 21 processes, so the CPU must perform many more page swaps from disk to RAM and vice versa. 

![](extra/case_study_2_runtime.png)