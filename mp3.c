#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include "mp3_given.h"

#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Neil Kaushikkar");
MODULE_DESCRIPTION("CS-423 MP3");

#define DEBUG 1
#define FILENAME "status"
#define DIRECTORY "mp3"
#define PREFIX "[MP3] "
#define BASE10 10

#define LOG(fmt)                                            \
    do {                                                    \
        printk("[MP3][%s:%d] "fmt"\n", __FILE__, __LINE__); \
    } while (0);

#define FMT(fmt, ...)                                                    \
    do {                                                                 \
        printk("[MP3][%s:%d] "fmt"\n", __FILE__, __LINE__, __VA_ARGS__); \
    } while (0);

struct mp3_pcb {
    struct list_head list;
    struct task_struct *linux_task;
    pid_t pid;
};

static DEFINE_SPINLOCK(rp_lock);

// This list keeps track of all the processes to schedule
static struct list_head task_list_head = LIST_HEAD_INIT(task_list_head);

// A slab allocator cache to speed up allocation of the extended task_structs
static struct kmem_cache *mp3_pcb_cache;

// This proc entry represents the directory mp3 in the procfs 
static struct proc_dir_entry *proc_dir;

static ssize_t mp3_proc_read_callback(struct file *file, char __user *buffer, size_t count, loff_t *off);

static ssize_t mp3_proc_write_callback(struct file *file, const char __user *buffer, size_t count, loff_t *off);

// This struct contains callbacks for operations on our procfs entry.
static const struct proc_ops mp3_file_ops = {
    .proc_read = mp3_proc_read_callback,
    .proc_write = mp3_proc_write_callback,
};

static struct mp3_pcb * find_mp3_pcb_by_pid(pid_t pid) {
    struct mp3_pcb *pcb;

    list_for_each_entry(pcb, &task_list_head, list) {
        if ( pcb->pid == pid ) {
            return pcb;
        }
    }

    return NULL;
}

static ssize_t mp3_proc_read_callback(struct file *file, char __user *buffer, size_t count, loff_t *off) {
    struct mp3_pcb *entry;
    size_t to_copy = 0;
    ssize_t copied = 0;

    char *kernel_buf = (char *) kzalloc(count, GFP_KERNEL);
   
    // Go through each entry of the list and read + format the pid and cpu use
    spin_lock(&rp_lock);
    list_for_each_entry(entry, &task_list_head, list) {
        // if we have written more than can be copied to the user buffer, stop
        if (to_copy >= count) { break; }

        to_copy += 
            snprintf(kernel_buf + to_copy, count - to_copy, "%d\n", entry->pid);
    }
    spin_unlock(&rp_lock);

    copied += simple_read_from_buffer(buffer, count, off, kernel_buf, to_copy);
    kfree(kernel_buf);

    return copied;
}

static ssize_t mp3_proc_write_callback(struct file *file, const char __user *buffer, size_t count, loff_t *off) {    
    ssize_t copied = 0;
    size_t kernel_buf_size = count + 1;
    char *kernel_buf = (char *) kzalloc(kernel_buf_size, GFP_KERNEL);
    struct task_struct *pid_task = NULL;
    struct mp3_pcb *pcb = NULL;
    char *pid_str, command;
    pid_t pid;

    // Copy the userspace memory into our kernel buffer
    copied += simple_write_to_buffer(kernel_buf, kernel_buf_size, off, buffer, count);
    command = *kernel_buf;
    pid_str = kernel_buf + 2; // skip past the command and comma

    if ( kstrtoint(pid_str, BASE10, &pid ) != 0 ) { // failed to parse pid
        FMT("Unable to parse pid [%s]", pid_str);
        kfree(kernel_buf);
        return copied;
    }
    
    if ( command == 'R' ) { // TRY TO REGISTER PROCESS
        pid_task = find_task_by_pid(pid);
        if ( !pid_task ) { 
            // Unable to find task, or parse period/processing time, so exit
            FMT("Unable to find process info [%d]", pid);
            kfree(kernel_buf);
            return count;
        }

        FMT("registering task with pid=%d", pid);

        pcb = kmem_cache_alloc(mp3_pcb_cache, GFP_KERNEL);
        pcb->linux_task = pid_task;
        pcb->pid = pid;

        spin_lock(&rp_lock);
        list_add(&pcb->list, task_list_head.next);
        spin_unlock(&rp_lock);
    } else if ( command == 'U' ) { // TRY TO DE-REGISTER PROCESS
        FMT("Attempting to deregister pid %d\n", pid);

        spin_lock(&rp_lock);
        pcb = find_mp3_pcb_by_pid(pid);
        if ( pcb ) {
            list_del(&pcb->list);
            FMT("Deregister pid %d\n", pid);
        } else {
            FMT("Unable to deregister pid %d\n", pid);
        }
        spin_unlock(&rp_lock);

        kmem_cache_free(mp3_pcb_cache, pcb);
    }

    kfree(kernel_buf);
    return copied;
}

// mp1_init - Called when module is loaded
int __init mp3_init(void) {
    struct mp3_pcb *entry, *tmp;

    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE LOADING\n");
    #endif

    // Setup proc fs entry
    proc_dir = proc_mkdir(DIRECTORY, NULL);
    proc_create(FILENAME, 0666, proc_dir, &mp3_file_ops);

    LOG("Removing LL nodes\n");
    spin_lock(&rp_lock);
    list_for_each_entry_safe(entry, tmp, &task_list_head, list) {
        FMT("removing process with pid %d\n", entry->pid);
        list_del(&entry->list);
        kmem_cache_free(mp3_pcb_cache, entry);
    };
    spin_unlock(&rp_lock);

    // Set up SLAB allocator cache
    mp3_pcb_cache = KMEM_CACHE(mp3_pcb, SLAB_PANIC);

    printk(KERN_ALERT "MP3 MODULE LOADED\n");
    return 0;   
}

// mp1_exit - Called when module is unloaded
void __exit mp3_exit(void) {
    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
    #endif
    
    LOG("Removing proc fs entry");
    remove_proc_entry(FILENAME, proc_dir);
    remove_proc_entry(DIRECTORY, NULL);

    LOG("Removing kmem cache");
    kmem_cache_destroy(mp3_pcb_cache);

    printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);
