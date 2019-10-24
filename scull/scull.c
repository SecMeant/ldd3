#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>

#define SCULL_INT_BUFFER_SIZE 128UL

//ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
//ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
//int (*open) (struct inode *, struct file *);
//int (*release) (struct inode *, struct file *);

static char *int_buffer = NULL;
static struct proc_dir_entry *scull_proc_entry = NULL;

static size_t scull_min(size_t a, size_t b)
{
  return a < b ? a : b;
}

static ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
  size_t to_read, copied, retval;

  if (*f_pos >= SCULL_INT_BUFFER_SIZE) {
    printk(KERN_DEBUG "scull reached EOF\n");
    return 0;
  }

  to_read = scull_min(SCULL_INT_BUFFER_SIZE - *f_pos, count);

  printk(KERN_DEBUG "Attempting to read %lu\n", to_read);

  copied = copy_to_user(buf, int_buffer, to_read);
  if(copied)
    printk(KERN_DEBUG "Scull failed to copy %lu bytes\n", copied);

  printk(KERN_DEBUG "Read %lu\n", copied);

  retval = to_read - copied;
  *f_pos += retval;
  return retval;
}

static ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
  size_t to_write, copied;

  if (*f_pos >= SCULL_INT_BUFFER_SIZE)
    return 0;

  to_write = scull_min(SCULL_INT_BUFFER_SIZE - *f_pos, count);

  copied = copy_from_user(int_buffer + *f_pos, buf, to_write);
  if(copied)
    printk(KERN_DEBUG "Scull failed to write %lu bytes\n", copied);

  *f_pos += copied;
  return to_write - copied;
}

static struct file_operations fops =
{
.owner = THIS_MODULE,
.read = scull_read,
.write = scull_write,
};

static int scull_init(void)
{
  size_t i;

  printk(KERN_DEBUG "scull init\n");

  int_buffer = kmalloc(SCULL_INT_BUFFER_SIZE, GFP_KERNEL);

  for(i = 0; i < SCULL_INT_BUFFER_SIZE; ++i)
    int_buffer[i] = '\0';

  scull_proc_entry = proc_create("scullmem", 0777, NULL, &fops);

  return 0;
}

static void scull_exit(void)
{
  printk(KERN_DEBUG "scull exit\n");

  if (scull_proc_entry)
    proc_remove(scull_proc_entry);

  if (int_buffer)
    kfree(int_buffer);
}

module_init(scull_init);
module_exit(scull_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");
