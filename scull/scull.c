#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <asm-generic/bug.h>
#include <linux/kernel.h>
#include <linux/compiler.h>

#define SCULL_QUANTUM 6UL
#define SCULL_QSET 4UL

//ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
//ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
//int (*open) (struct inode *, struct file *);
//int (*release) (struct inode *, struct file *);

struct scull_qset
{
	void **data;
	struct scull_qset *next;
};

struct scull_dev
{
	struct scull_qset *data;   /* Pointer to first quantum set */
	size_t quantum;            /* the current quantum size */
	size_t qset;               /* the current array size */
	unsigned long size;        /* amount of data stored here */
	// unsigned int access_key;   /* used by sculluid and scullpriv */
	struct mutex lock;      /* mutual exlusion semaphore */
	struct cdev cdev;          /* Char device structure */
};

struct scull_dev *sdev;
struct proc_dir_entry *pentry;

static dev_t scull_major = 0; /* if set to 0, it will be allocated dynamically */
static dev_t scull_minor = 0;

int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *next, *dptr;
	int qset = dev->qset;
	int i;

	BUG_ON(!dev);

	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				kfree(dptr->data[i]);
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
	}

	dev->size = 0;
	dev->quantum = SCULL_QUANTUM;
	dev->qset = SCULL_QSET;
	dev->data = NULL;
	return 0;
}

static struct scull_qset* scull_follow(struct scull_dev *dev, size_t item)
{
	struct scull_qset *dptr = dev->data;

	while(dev && item > 0) {
		dptr = dptr->next;
		--item;
	}

	return dptr;
}

static ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = sdev;
	struct scull_qset *dptr;
	size_t quantum = dev->quantum, qset = dev->qset;
	size_t item_size = quantum * qset;
	size_t item, s_pos, q_pos, rest, retval;

        if(mutex_lock_interruptible(&dev->lock))
          return -ERESTARTSYS;

	item = *f_pos / item_size;
	rest = *f_pos % item_size;

	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item);

	printk(KERN_DEBUG "dptr %p item %lu s_pos %lu q_pos %lu\n",
			dptr, item, s_pos, q_pos);

	if (dptr == NULL || !dptr->data || !dptr->data[s_pos]) {
		printk(KERN_DEBUG "EOF\n");
		retval = 0;
		goto out;
	}

	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

out:
	mutex_unlock(&dev->lock);
	return retval;
}

static struct scull_qset* scull_add_qset(struct scull_dev *dev)
{
	struct scull_qset **qset_parents_ptr = &dev->data;
	struct scull_qset *qset = *qset_parents_ptr;

	while (qset) {
		qset_parents_ptr = &qset->next;
		qset = qset->next;
	}

	*qset_parents_ptr = kzalloc(sizeof(struct scull_qset), GFP_KERNEL);
	return *qset_parents_ptr;
}

static ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = sdev;
	struct scull_qset *dptr;
	size_t quantum = dev->quantum, qset = dev->qset;
	size_t item_size = quantum * qset;
	size_t qset_free_space;
	size_t item, s_pos, q_pos, rest, retval;

	if(mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	item = *f_pos / item_size;
	rest = *f_pos % item_size;

	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item);

	if (dptr == NULL) {
		dptr = scull_add_qset(dev);
		if (dptr == NULL) {
			retval = -ENOMEM;
			goto out;
		}
	}

	if (!dptr->data) {
		dptr->data = kzalloc(qset * sizeof(char *), GFP_KERNEL);
		if (!dptr->data) {
			retval = -ENOMEM;
			goto out;
		}
	}

	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kzalloc(quantum, GFP_KERNEL);
		if (!dptr->data[s_pos]) {
			retval = -ENOMEM;
			goto out;
		}
	}

	qset_free_space = quantum - q_pos;
	if (count > qset_free_space)
		count = qset_free_space;

	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto out;
	}

	if (count == qset_free_space && !dptr->data)
		dptr->data = kzalloc(qset * sizeof(char *), GFP_KERNEL);

	*f_pos += count;
	retval = count;

out:
	mutex_unlock(&dev->lock);
	return retval;
}

static int get_dev(void)
{
	dev_t dev;


	if (scull_major) {
		dev = MKDEV(scull_major, scull_minor);
		return register_chrdev_region(dev, 1 /* nr of devs */, "scull");
	}

	scull_major = MAJOR(dev);
	return alloc_chrdev_region(&dev, scull_minor, 1 /* nr of devs */, "scull");
}

int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	return 0;
}

int scull_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static struct file_operations fops =
{
	.owner = THIS_MODULE,
	.read = scull_read,
	.write = scull_write,
	.open = scull_open,
	.release = scull_release
};

static int scull_init(void)
{
	printk(KERN_INFO "Loading scull\n");

	if (get_dev()) {
		printk(KERN_ERR "Failed to obtain dev major\n");
		return -1;
	}

	sdev = (struct scull_dev*) kmalloc(sizeof(struct scull_dev), GFP_KERNEL);

	if (!sdev) {
		printk(KERN_ERR "Failed to allocate storage for scull dev struct\n");
		return -ENOMEM;
	}

	sdev->data = NULL;
	sdev->quantum = SCULL_QUANTUM;
	sdev->qset = SCULL_QSET;
        mutex_init(&sdev->lock);

	cdev_init(&sdev->cdev, &fops);

	if (unlikely(cdev_add(&sdev->cdev, scull_minor, 1 /* nr of dev numbers */))) {
		printk(KERN_ERR "Failed to add cdev\n");
		return -1;
	}

	pentry = proc_create("scullmem", 0777, NULL, &fops);

	if (unlikely(!pentry))
		printk(KERN_ERR "Failed to create proc entry\n"); // continue even if failed

	return 0;
}

static void scull_exit(void)
{
	printk(KERN_INFO "Removing scull\n");

	if (!sdev)
		return;

	scull_trim(sdev);
	cdev_del(&sdev->cdev);

	if (pentry)
		proc_remove(pentry);

	kfree(sdev);
}

module_init(scull_init);
module_exit(scull_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");
