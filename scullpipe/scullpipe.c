#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>

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
