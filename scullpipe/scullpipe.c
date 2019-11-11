#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/compiler_attributes.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>

#define SCULLP_BUF_SIZE 512

static dev_t scullpipe_major = 0;
static dev_t scullpipe_minor = 0;

static int scullpipe_open (struct inode *, struct file *);
static int scullpipe_release (struct inode *, struct file *);
static ssize_t scullpipe_read (struct file *, char __user *, size_t, loff_t *);
static ssize_t  scullpipe_write (struct file *, const char __user *, size_t, loff_t *);

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
	.release = scullpipe_release,
	.read = scullpipe_read,
	.write = scullpipe_write,
};

static int scullpipe_open (struct inode *inode, struct file *filp)
{
	filp->private_data = (void*) &sdev;
	printk(KERN_DEBUG "Scullpipe open\n");

	return 0;
}

static int scullpipe_release (struct inode *inode, struct file *filp)
{
	printk(KERN_DEBUG "Scullpipe release");
	return 0;
}

static __always_inline size_t readavail(const struct scullpipe_dev *sdev)
{
	if (sdev->wp < sdev->rp)
		return (sdev->bb - sdev->rp) + SCULLP_BUF_SIZE; // I hope this prevents overflow to happen
	return sdev->wp - sdev->rp;
}

static __always_inline size_t scullmin(size_t a, size_t b)
{
	a = b < a ? b : a;
	return a;
}

static bool __always_inline cbempty(const struct scullpipe_dev *sdev)
{
	return sdev->rp == sdev->wp;
}

static ssize_t  scullpipe_read (struct file *filp, char __user *to, size_t count, loff_t *off)
{
	struct scullpipe_dev *sdev = (struct scullpipe_dev *) filp->private_data;

	if (down_interruptible(&sdev->wsem))
		return -ERESTARTSYS;

	while (sdev->rp == sdev->wp) {
		up(&sdev->wsem);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(sdev->rq, (!cbempty(sdev))))
			return -ERESTARTSYS;

		if (down_interruptible(&sdev->wsem))
			return -ERESTARTSYS;
	}

	// There is data to read and semaphore is aquired
	
	count = scullmin(count, readavail(sdev));

	if (copy_to_user(to, sdev->rp, count)) {
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

static bool __always_inline cbfull(const struct scullpipe_dev *sdev)
{
	char * next = sdev->wp + 1;

	if (next >= sdev->bb + SCULLP_BUF_SIZE)
		next -= SCULLP_BUF_SIZE;

	return next == sdev->rp;
}

static size_t __always_inline spacefree(const struct scullpipe_dev *sdev)
{
	if (sdev->rp > sdev->wp)
		return sdev->rp - sdev->wp;

	return (sdev->bb - sdev->wp) + SCULLP_BUF_SIZE;
}

static ssize_t scullpipe_write(struct file *filp, const char __user *from, size_t count, loff_t *off)
{
	struct scullpipe_dev *sdev = (struct scullpipe_dev *) filp->private_data;

	if (down_interruptible(&sdev->wsem))
		return -ERESTARTSYS;
	
	while (cbfull(sdev)) {
		up(&sdev->wsem);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(sdev->wq, (!cbfull(sdev)) ))
			return -ERESTARTSYS;

		if (down_interruptible(&sdev->wsem))
			return -ERESTARTSYS;
	}

	// There is space and semaphore is aquired
	
	count = scullmin(count, spacefree(sdev));

	if (copy_from_user(sdev->wp, from, count)) {
		up(&sdev->wsem);
		return -EFAULT;
	}

	sdev->wp += count;

	if (sdev->wp == sdev->bb + SCULLP_BUF_SIZE)
		sdev->wp = sdev->bb;
	
	up(&sdev->wsem);

	wake_up_interruptible(&sdev->rq);

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

	init_waitqueue_head(&sdev->wq);
	init_waitqueue_head(&sdev->rq);

	cdev_init(&sdev->cdev, &fops);

	ret = cdev_add(&sdev->cdev, dev, 1);
	if (unlikely(ret)) {
		printk(KERN_DEBUG "Failed to obtain char dev major\n");
		kfree(sdev->bb);
		kfree(sdev);
		return ret;
	}

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
