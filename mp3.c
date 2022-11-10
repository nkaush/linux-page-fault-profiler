#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include "mp3_given.h"

#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/mm.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Neil Kaushikkar");
MODULE_DESCRIPTION("CS-423 MP3");

#define DEBUG 1
#define FILENAME "status"
#define DIRECTORY "mp3"
#define PREFIX "[MP3] "
#define CHR_DEV_NAME "mp3-chr-dev"
#define MP3_WQ_NAME "mp3-worker"

#define BUFFER_PAGE_SIZE 4096
#define BUFFER_NUM_PAGES 128
#define BUFFER_SIZE (BUFFER_PAGE_SIZE * BUFFER_NUM_PAGES)
#define METRIC_COLLECTION_INTERVAL 50
#define CHR_DEV_MAJOR_DEV_NUM 423
#define CHR_DEV_MINOR_DEV_NUM 0
#define CHR_DEV_COUNT 1
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

struct mp3_sample {
    size_t a;
    size_t b;
    size_t c;
    size_t d;
};

#define BUFFER_SAMPLE_SIZE sizeof(struct mp3_sample)
#define BUFFER_MAX_SAMPLES (BUFFER_SIZE / BUFFER_SAMPLE_SIZE)

static DEFINE_SPINLOCK(rp_lock);

// This list keeps track of all the processes to schedule
static struct list_head task_list_head = LIST_HEAD_INIT(task_list_head);
static size_t task_list_size = 0;

// A slab allocator cache to speed up allocation of the extended task_structs
static struct kmem_cache *mp3_pcb_cache;

static struct cdev mp3_cdev;
static dev_t mp3_cdev_id;
static struct mp3_sample *memory_buffer;
static size_t num_samples = 0;

static struct page * mp3_cdev_nopage_callback(struct vm_area_struct *vma, unsigned long address, int write_access);
static vm_fault_t mp3_cdev_fault_callback(struct vm_fault *vmf);

static const struct vm_operations_struct mp3_vm_ops = { 
    // .fault = mp3_cdev_fault_callback,
    .nopage = mp3_cdev_nopage_callback
};

static int mp3_cdev_open_callback(struct inode *inode, struct file *file);
static int mp3_cdev_mmap_callback(struct file *file, struct vm_area_struct *vm_area);
static int mp3_cdev_release_callback(struct inode *inode, struct file *filp);

static const struct file_operations mp3_cdev_fops = {
    .owner   = THIS_MODULE,
    .open    = mp3_cdev_open_callback,
    .mmap    = mp3_cdev_mmap_callback,
    .release = mp3_cdev_release_callback
};

static void collect_page_faults(struct work_struct *work);
static DECLARE_DELAYED_WORK(metric_collection_work, collect_page_faults);
static struct workqueue_struct *workqueue;

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
    size_t to_copy = 0, lock_flags = 0;
    ssize_t copied = 0;

    char *kernel_buf = (char *) kzalloc(count, GFP_KERNEL);
   
    // Go through each entry of the list and read + format the pid and cpu use
    spin_lock_irqsave(&rp_lock, lock_flags);
    list_for_each_entry(entry, &task_list_head, list) {
        // if we have written more than can be copied to the user buffer, stop
        if (to_copy >= count) { break; }

        to_copy += 
            snprintf(kernel_buf + to_copy, count - to_copy, "%d\n", entry->pid);
    }
    spin_unlock_irqrestore(&rp_lock, lock_flags);

    copied += simple_read_from_buffer(buffer, count, off, kernel_buf, to_copy);
    kfree(kernel_buf);

    return copied;
}

static ssize_t mp3_proc_write_callback(struct file *file, const char __user *buffer, size_t count, loff_t *off) {    
    ssize_t copied = 0;
    size_t kernel_buf_size = count + 1, lock_flags = 0;
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

        spin_lock_irqsave(&rp_lock, lock_flags);
        list_add(&pcb->list, task_list_head.next);
        ++task_list_size;

        if ( task_list_size == 1 ) {
            queue_delayed_work(workqueue, &metric_collection_work, msecs_to_jiffies(METRIC_COLLECTION_INTERVAL));
        }
        spin_unlock_irqrestore(&rp_lock, lock_flags);
    } else if ( command == 'U' ) { // TRY TO DE-REGISTER PROCESS
        FMT("Attempting to deregister pid %d\n", pid);

        spin_lock_irqsave(&rp_lock, lock_flags);
        pcb = find_mp3_pcb_by_pid(pid);
        if ( pcb ) {
            list_del(&pcb->list);
            --task_list_size;
            FMT("Deregister pid %d", pid);
            if ( task_list_size == 0 ) {
                cancel_delayed_work_sync(&metric_collection_work);
            }
        } else {
            FMT("Unable to deregister pid %d", pid);
        }
        spin_unlock_irqrestore(&rp_lock, lock_flags);

        kmem_cache_free(mp3_pcb_cache, pcb);
    }

    kfree(kernel_buf);
    return copied;
}

static void collect_page_faults(struct work_struct* work) {
    size_t min_flt, maj_flt, utime, stime, total_min_flt, total_maj_flt, total_time;
    struct mp3_sample *sample = memory_buffer + num_samples;
    size_t current_jiffies = jiffies, lock_flags = 0;
    struct mp3_pcb *pcb, *tmp;

    spin_lock_irqsave(&rp_lock, lock_flags);
    list_for_each_entry_safe(pcb, tmp, &task_list_head, list) {
        if ( get_cpu_use(pcb->pid, &min_flt, &maj_flt, &utime, &stime) == -1 ) {
            list_del(&pcb->list);
        } else {
            total_min_flt += min_flt;
            total_maj_flt += maj_flt;
            total_time += utime + stime;
        }
    };
    spin_unlock_irqrestore(&rp_lock, lock_flags);

    // TODO write to buffer...
    sample->a = current_jiffies;
    sample->b = total_min_flt;
    sample->c = total_maj_flt;
    sample->d = total_time;
    
    ++num_samples;
}

static vm_fault_t mp3_cdev_fault_callback(struct vm_fault *vmf) {
    struct vm_area_struct *vma = vmf->vma;
    size_t pfn, size = BUFFER_PAGE_SIZE;
    int ret;

    FMT("fault handler: pgoff: %zu + buffer: %p --> %p", vmf->pgoff, memory_buffer, memory_buffer + (vmf->pgoff << PAGE_SHIFT))

    pfn = vmalloc_to_pfn(memory_buffer + (vmf->pgoff << PAGE_SHIFT));
    ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);

    return 0;
}

static struct page * mp3_cdev_nopage_callback(struct vm_area_struct *vma, unsigned long address, int write_access) {
    struct page *pageptr;

    unsigned long physaddr = address - vma->vm_start + VMA_OFFSET(vma);
    FMT("mp3_cdev_nopage_callback: address: %zu, physaddress: %zu", address, physaddr);

    pageptr = vmalloc_to_page((void*) physaddr);
    get_page(pageptr);
    return pageptr;
}

static int mp3_cdev_open_callback(struct inode *inode, struct file *file) {
    LOG("Character device opened!")
    return 0; 
}

static int mp3_cdev_mmap_callback(struct file *file, struct vm_area_struct *vma) {
    FMT("Character device mmaped: start: %p --> end: %p", memory_buffer, memory_buffer + BUFFER_SIZE)
    vma->vm_ops = &mp3_vm_ops;
    vma->vm_start = (long unsigned int) memory_buffer;
    vma->vm_end = (long unsigned int) memory_buffer + BUFFER_SIZE;
    
    return 0;
}

static int mp3_cdev_release_callback(struct inode *inode, struct file *filp) {
    LOG("Character device released!")
    return 0;
}

// mp1_init - Called when module is loaded
int __init mp3_init(void) {
    // size_t i;
    int s;

    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE LOADING\n");
    #endif

    workqueue = create_singlethread_workqueue(MP3_WQ_NAME);

    // Setup proc fs entry
    proc_dir = proc_mkdir(DIRECTORY, NULL);
    proc_create(FILENAME, 0666, proc_dir, &mp3_file_ops);

    // Set up SLAB allocator cache
    mp3_pcb_cache = KMEM_CACHE(mp3_pcb, SLAB_PANIC);

    memory_buffer = vzalloc(BUFFER_SIZE);
    memset(memory_buffer, 0xff, BUFFER_SIZE);
    // for(i = 0; i < BUFFER_NUM_PAGES * BUFFER_PAGE_SIZE; i += BUFFER_PAGE_SIZE) {
    //     SetPageReserved(vmalloc_to_page(vmalloc_to_page(memory_buffer + i)));
    // }

    mp3_cdev_id = MKDEV(CHR_DEV_MAJOR_DEV_NUM, CHR_DEV_MINOR_DEV_NUM);
    s = register_chrdev_region(mp3_cdev_id, CHR_DEV_COUNT, CHR_DEV_NAME);
    FMT("register_chrdev_region returned %d", s)
    if ( s ) {
        FMT("register_chrdev_region returned status %d", s);
        return 1;
    }

    cdev_init(&mp3_cdev, &mp3_cdev_fops);
    s = cdev_add(&mp3_cdev, mp3_cdev_id, CHR_DEV_COUNT);
    FMT("cdev_add returned %d", s)

    // s = register_chrdev(CHR_DEV_MAJOR_DEV_NUM, CHR_DEV_NAME, &mp3_cdev_fops);

    printk(KERN_ALERT "MP3 MODULE LOADED\n");
    return 0;
}

// mp1_exit - Called when module is unloaded
void __exit mp3_exit(void) {
    struct mp3_pcb *entry, *tmp;
    size_t lock_flags = 0;
    // size_t i;

    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
    #endif
    
    // Remove proc fs entry
    LOG("Removing proc fs entry");
    remove_proc_entry(FILENAME, proc_dir);
    remove_proc_entry(DIRECTORY, NULL);

    // Remove kmem cache
    LOG("Removing kmem cache");
    kmem_cache_destroy(mp3_pcb_cache);

    // Clear workqueue
    if ( workqueue ) {
        cancel_delayed_work_sync(&metric_collection_work);
        destroy_workqueue(workqueue);
    }

    // delete ll nodes
    LOG("Removing LL nodes");
    spin_lock_irqsave(&rp_lock, lock_flags);
    list_for_each_entry_safe(entry, tmp, &task_list_head, list) {
        FMT("removing process with pid %d\n", entry->pid);
        list_del(&entry->list);
        kmem_cache_free(mp3_pcb_cache, entry);
    };
    spin_unlock_irqrestore(&rp_lock, lock_flags);

    // Free up virtual memory space
    LOG("Free virtual memory");
    // for(i = 0; i < BUFFER_NUM_PAGES * BUFFER_PAGE_SIZE; i += BUFFER_PAGE_SIZE) {
    //     ClearPageReserved(vmalloc_to_page(memory_buffer + i));
    // }
    vfree(memory_buffer);

    // Unregister character device
    LOG("Unregister character device");
    cdev_del(&mp3_cdev);
    unregister_chrdev_region(mp3_cdev_id, CHR_DEV_COUNT);
    
    printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);
