#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/compiler_attributes.h>
#include <linux/semaphore.h>

#define SCULLP_BUF_SIZE 512

static dev_t scullpipe_major = 0;
static dev_t scullpipe_minor = 0;

int scullpipe_open (struct inode *, struct file *);
int scullpipe_release (struct inode *, struct file *);
int scullpipe_read (struct file *, char __user *, size_t, loff_t *);
int scullpipe_write (struct file *, const char __user *, size_t, loff_t *);

struct scullpipe_dev {
	struct cdev cdev;
	char *bb, *wp, *rp;
	struct semaphore wsem;
	wait_queue_head_t rq, wq;
};

struct scullpipe_dev *sdev;

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = scullpipe_open,
	.release = scullpipe_release
};

int scullpipe_open (struct inode *inode, struct file *filp)
{
	filp->private_data = (void*) &sdev;
	printk(KERN_DEBUG "Scullpipe open\n");

	return 0;
}

int scullpipe_release (struct inode *inode, struct file *filp)
{
	printk(KERN_DEBUG "Scullpipe release");
	return 0;
}

static __always_inline size_t readavail(const scullpipe_dev *sdev)
{
	if (sdev->wp < sdev->rp)
		return (sdev->bb - sdev->rp) + SCULLP_BUF_SIZE; // I hope this prevents overflow to happen
	return sdev->wr - sdev->rp;
}

static __always_inline size_t scullmin(size_t a, size_t b)
{
	a = b < a ? b : a;
	return a;
}

int scullpipe_read (struct file *filp, char __user *to, size_t count, loff_t off)
{
	struct scullpipe_dev *sdev = (struct scullpipe_dev *) filp->private_data;

	if (down_interruptible(&sdev->wsem))
		return -ERESTARTSYS;

	while (sdev->rp == sdev->wp) {
		up(&sdev->wsem);

		if (sdev->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(sdev->wsem, (sdev->rp != sdev->wp)))
			return -ERESTARTSYS;

		if (down_interruptible(&sdev->wsem))
			return -ERESTARTSYS;
	}

	// There is data to read and semaphore is aquired
	
	count = scullmin(count, readavail());

	if (copy_to_user(to, dev->rp, count)) {
		up (&sdev->wsem);
		return -EFAULT;
	}

	sdev->rp += count;

	if (sdev->rp == sdev->bb + SCULLP_BUF_SIZE)
		sdev->rp = sdev->bb;

	up(&sdev->wsem);

	wake_up_interruptible(&sdev->wq);

	return count;
}

static int scullpipe_init(void)
{
	int ret;
	dev_t dev;

	printk(KERN_DEBUG "scullpipe init\n");

	if (scullpipe_major) {
		dev = MKDEV(scullpipe_major, scullpipe_minor);
		ret = register_chrdev_region(dev, 1, "scullpipe");
	} else {
		ret = alloc_chrdev_region(&dev, scullpipe_minor, 1, "scullpipe");
	}

	if (unlikely(ret)) {
		printk(KERN_DEBUG "Failed to allocate char dev num\n");
		return ret;
	}

	sdev = (struct scullpipe_dev*) kmalloc(sizeof(struct scullpipe_dev), GFP_KERNEL);

	if (unlikely(!sdev)) {
		printk(KERN_DEBUG "Failed to allocate memory\n");
		return -ENOMEM;
	}

	sdev->bb = kmalloc(SCULLP_BUF_SIZE, GFP_KERNEL);

	if (!sdev->bb) {
		printk(KERN_DEBUG "Failed to allocate mem for int buf\n");
		kfree(sdev);
		return -ENOMEM;
	}

	sdev->wp = sdev->bb;
	sdev->rp = sdev->bb;

	cdev_init(&sdev->cdev, &fops);

	ret = cdev_add(&sdev->cdev, dev, 1);
	if (unlikely(ret)) {
		printk(KERN_DEBUG "Failed to obtain char dev major\n");
		kfree(sdev->bb);
		kfree(sdev);
		return ret;
	}

	init_waitqueuea_head(sdev->wq);
	init_waitqueuea_head(sdev->rq);

	return 0;
}

static void scullpipe_exit(void)
{
	printk(KERN_DEBUG "scullpipe exit\n");
	kfree(sdev->bb);
	kfree(sdev);
}

module_init(scullpipe_init);
module_exit(scullpipe_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");
