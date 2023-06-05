#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>

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

#include <linux/pid.h>
#include <linux/sched.h>

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
    size_t data[4];
};

#define BUFFER_SAMPLE_SIZE sizeof(struct mp3_sample)
#define BUFFER_MAX_SAMPLES (BUFFER_SIZE / BUFFER_SAMPLE_SIZE)

static DEFINE_SPINLOCK(rp_lock);

static ktime_t wallclock_time;

// This list keeps track of all the processes to schedule
static struct list_head task_list_head = LIST_HEAD_INIT(task_list_head);
static size_t task_list_size = 0;

// A slab allocator cache to speed up allocation of the extended task_structs
static struct kmem_cache *mp3_pcb_cache;

static struct cdev mp3_cdev;
static dev_t mp3_cdev_id;
static struct mp3_sample *memory_buffer;
static size_t num_samples = 0;

static vm_fault_t mp3_cdev_fault_callback(struct vm_fault *vmf);

static const struct vm_operations_struct mp3_vm_ops = { 
    .fault = mp3_cdev_fault_callback
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

struct task_struct* find_task_by_pid(unsigned int nr) {
    struct task_struct* task = NULL;
    rcu_read_lock();
    task=pid_task(find_vpid(nr), PIDTYPE_PID);
    rcu_read_unlock();
    if (task == NULL)
        printk(KERN_INFO "find_task_by_pid: couldnt find pid %d\n", nr);
    return task;
}

// THIS FUNCTION RETURNS 0 IF THE PID IS VALID. IT ALSO RETURNS THE
// PROCESS CPU TIME IN JIFFIES AND MAJOR AND MINOR PAGE FAULT COUNTS
// SINCE THE LAST INVOCATION OF THE FUNCTION FOR THE SPECIFIED PID.
// OTHERWISE IT RETURNS -1
int get_cpu_use(int pid, unsigned long *min_flt, unsigned long *maj_flt, unsigned long *utime, unsigned long *stime) {
        int ret = -1;
        struct task_struct* task;
        rcu_read_lock();
        task=find_task_by_pid(pid);
        if (task!=NULL) {
                *min_flt=task->min_flt;
                *maj_flt=task->maj_flt;
                *utime=task->utime;
                *stime=task->stime;

                task->maj_flt = 0;
                task->min_flt = 0;
                task->utime = 0;
                task->stime = 0;
                ret = 0;
        }
        rcu_read_unlock();
        return ret;
}

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
            FMT("delay is %zu jiffies", msecs_to_jiffies(METRIC_COLLECTION_INTERVAL))
            // queue_delayed_work(workqueue, &metric_collection_work, 50);
            queue_delayed_work(workqueue, &metric_collection_work, msecs_to_jiffies(METRIC_COLLECTION_INTERVAL));
            wallclock_time = ktime_get();
        }
        spin_unlock_irqrestore(&rp_lock, lock_flags);
    } else if ( command == 'U' ) { // TRY TO DE-REGISTER PROCESS
        FMT("Attempting to deregister pid %d", pid);

        spin_lock_irqsave(&rp_lock, lock_flags);
        pcb = find_mp3_pcb_by_pid(pid);
        if ( pcb ) {
            list_del(&pcb->list);
            --task_list_size;
            FMT("Deregister pid %d", pid);
            if ( task_list_size == 0 ) {
                cancel_delayed_work_sync(&metric_collection_work);
            }
            
            kmem_cache_free(mp3_pcb_cache, pcb);
        } else {
            FMT("Unable to deregister pid %d", pid);
        }
        spin_unlock_irqrestore(&rp_lock, lock_flags);
    }

    kfree(kernel_buf);
    return copied;
}

static void collect_page_faults(struct work_struct* work) {
    size_t min_flt, maj_flt, utime, stime, total_min_flt = 0, total_maj_flt = 0, total_time = 0;
    size_t current_jiffies = jiffies, lock_flags = 0, time_usage;
    ktime_t old_wallclock_time = wallclock_time, diff;
    struct mp3_pcb *pcb, *tmp;
    struct mp3_sample *sample;

    if ( num_samples == BUFFER_MAX_SAMPLES ) {
        FMT("memmove(%p, %p, %zu)", memory_buffer, memory_buffer + 1, BUFFER_SIZE - BUFFER_SAMPLE_SIZE)
        memmove(memory_buffer, memory_buffer + 1, BUFFER_SIZE - BUFFER_SAMPLE_SIZE);
        sample = memory_buffer + num_samples - 1;
    } else {
        sample = memory_buffer + num_samples;
        ++num_samples;
    }

    spin_lock_irqsave(&rp_lock, lock_flags);
    list_for_each_entry_safe(pcb, tmp, &task_list_head, list) {
        min_flt = -1;
        maj_flt = -1;
        utime = -1;
        stime = -1;
        if ( get_cpu_use(pcb->pid, &min_flt, &maj_flt, &utime, &stime) != -1 ) {
            total_min_flt += min_flt;
            total_maj_flt += maj_flt;
            total_time += utime + stime;
        }
    };
    spin_unlock_irqrestore(&rp_lock, lock_flags);

    wallclock_time = ktime_get();
    diff = wallclock_time - old_wallclock_time;
    time_usage = (total_time * 100ULL) / diff;

    FMT("%zu, %zu, %zu, %zu (num_samples %zu/%zu)", current_jiffies, total_min_flt, total_maj_flt, time_usage, num_samples, BUFFER_MAX_SAMPLES);
    sample->data[0] = current_jiffies;
    sample->data[1] = total_min_flt;
    sample->data[2] = total_maj_flt;
    sample->data[3] = time_usage;
    
    // queue_delayed_work(workqueue, &metric_collection_work, 50);
    queue_delayed_work(workqueue, &metric_collection_work, msecs_to_jiffies(METRIC_COLLECTION_INTERVAL));
}

static vm_fault_t mp3_cdev_fault_callback(struct vm_fault *vmf) {
    FMT("fault handler: pgoff: %zu + buffer: %p --> %p", vmf->pgoff, memory_buffer, memory_buffer + (vmf->pgoff << PAGE_SHIFT))

    vmf->page = vmalloc_to_page(((void*)memory_buffer) + (vmf->pgoff << PAGE_SHIFT));
    get_page(vmf->page);

    return 0;
}

static int mp3_cdev_open_callback(struct inode *inode, struct file *file) {
    LOG("Character device opened!")
    return 0; 
}

static int mp3_cdev_mmap_callback(struct file *file, struct vm_area_struct *vma) {
    FMT("Character device mmaped: start: %p --> end: %p", memory_buffer, memory_buffer + BUFFER_SIZE)
    vma->vm_ops = &mp3_vm_ops;
    
    return 0;
}

static int mp3_cdev_release_callback(struct inode *inode, struct file *filp) {
    LOG("Character device released!")
    return 0;
}

// mp1_init - Called when module is loaded
int __init mp3_init(void) {
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

    printk(KERN_ALERT "MP3 MODULE LOADED\n");
    return 0;
}

// mp1_exit - Called when module is unloaded
void __exit mp3_exit(void) {
    struct mp3_pcb *entry, *tmp;
    size_t lock_flags = 0;

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
        FMT("removing process with pid %d", entry->pid);
        list_del(&entry->list);
        kmem_cache_free(mp3_pcb_cache, entry);
    };
    spin_unlock_irqrestore(&rp_lock, lock_flags);

    // Free up virtual memory space
    LOG("Free virtual memory");
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
