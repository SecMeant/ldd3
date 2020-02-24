#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>

#define AUTHOR		"Patryk Wlazłyń"
#define DESCRIPTION	"Driver for novation mk2 launchpad";
#define VERSION		"0.1";

#define USB_MK2_VENDOR_ID	0x1235
#define USB_MK2_PRODUCT_ID	0x0069

#define USB_MK2_MINOR_BASE	8
#define MK2_MAX_TRANSFER	128

#define WRITES_IN_FLIGHT	8

// 407 = header + packet * 80 + footer = 6 + 5 * 80 + 1
#define USB_MK2_MAX_OUT_LEN	((size_t) 407)

#define MK2_SYSEX_PACKET_SIZE	3
#define MK2_STUFFED_PACKET_SIZE	4
#define MK2_SYSEX_SIZE_ROUND_UP	2

#define MK2_SYSEX_MOREDATA	0x04
#define MK2_SYSEX_DATAEND1	0x05
#define MK2_SYSEX_DATAEND2	0x06
#define MK2_SYSEX_DATAEND3	0x07

static struct usb_driver mk2_driver;

struct mk2_read_buffer
{
	unsigned char	*data;
	size_t		size;
	size_t		filled;
	size_t		copied;
};

void *mk2_read_buffer_alloc(mk2_read_buffer *read_buffer, size_t size)
{
	read_buffer.data = kmalloc(size, GFP_KERNEL);
	read_buffer.size = size;
	read_buffer.filled = 0;
	read_buffer.copied = 0;

	return read_buffer.data;
}

struct mk2_read_endp
{
	struct mk2_read_buffer	buffer;
	struct urb		*urb;
	struct mutex		io_mutex;
	wait_queue_head_t	wait_queue;
	spinlock_t 		err_lock;
	int 			errors;
	__u8			address;
	bool			requested_read;
};

struct mk2_write_endp
{
	struct usb_anchor	submitted;
	struct semaphore	limit_sem;
	struct mutex		io_mutex;
	spinlock_t		err_lock;
	int errors;
	__u8			address;
};

struct mk2_state
{
	unsigned long
		disconnected 	: 1,
		suspended 	: 1;
}

struct mk2dev
{
	struct usb_device	*udev;
	struct usb_interface	*interface;
	struct kref		kref;
	struct mk2_read_endp	read_endp;
	struct mk2_write_endp	write_endp;
	struct mk2_state	state;
};

//struct mk2dev
//{
//	struct usb_device	*udev;
//	struct usb_interface	*interface;
//	struct semaphore	limit_sem;
//	struct usb_anchor	submitted;
//	struct urb		*bulk_in_urb;
//	unsigned char		*bulk_in_buffer;
//	size_t			bulk_in_size;
//	size_t			bulk_in_filled;
//	size_t			bulk_in_copied;
//	__u8			bulk_in_endpointAddr;
//	__u8			bulk_out_endpointAddr;
//	int			rerrors;
//	int			werrors;
//	bool			ongoing_read;
//	spinlock_t		rerr_lock;
//	spinlock_t		werr_lock;
//	struct kref		kref;
//	struct mutex		read_mutex;
//	struct mutex		write_mutex;
//	unsigned long		disconnected : 1;
//	wait_queue_head_t	bulk_in_wait;
//};

static const struct usb_device_id mk2_idtable[] = {
	{ USB_DEVICE(USB_MK2_VENDOR_ID, USB_MK2_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE (usb, mk2_idtable);

static void mk2_delete(struct kref *kref)
{
	struct mk2dev *dev = container_of(kref, struct mk2dev, kref);

	usb_free_urb(dev->read_endp.urb);
	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	kfree(dev->read_endp.buffer.data);
	kfree(dev);
}

static int mk2_open(struct inode *inode, struct file *file)
{
	struct mk2dev *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&mk2_driver, subminor);
	if (unlikely(!interface)) {
		pr_err("%s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (unlikely(!dev)) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;
	
	kref_get(&dev->kref);

	file->private_data = dev;

exit:
	return retval;
}

static int mk2_release(struct inode *inode, struct file *file)
{
	struct mk2dev *dev;

	dev = file->private_data;

	if (unlikely(!dev))
		return -ENODEV;
	
	usb_autopm_put_interface(dev->interface);
	kref_put(&dev->kref, mk2_delete);
	return 0;
}

static void mk2_write_bulk_callback(struct urb *urb)
{
	struct mk2dev *dev;
	unsigned long flags;

	dev = urb->context;

	if (urb->status) {
		if (!(urb->status == -ENOENT ||
			urb->status == -ECONNRESET ||
			urb->status == -ESHUTDOWN ))
				dev_err(&dev->interface->dev,
					"%s - nonzero write bulk status received: %d\n",
					__func__, urb->status);

		spin_lock_irqsave(&dev->werr_lock, flags);
		dev->werrors = urb->status;
		spin_unlock_irqrestore(&dev->werr_lock, flags);
	}

	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	up(&dev->limit_sem);
}

static void stuff_buffer(char *buf, size_t stuffed_size, const char __user *user_buffer, size_t count)
{
	size_t blk, rem, oi = 0, ii = 0;

	blk = count / 3;
	rem = count % 3;

	while (blk > 0) {
		buf[oi+0] = MK2_SYSEX_MOREDATA;
		buf[oi+1] = user_buffer[ii+0];
		buf[oi+2] = user_buffer[ii+1];
		buf[oi+3] = user_buffer[ii+2];
		
		oi += 4;
		ii += 3;
		--blk;
	}

	switch (rem) {
		case 0:
			buf[oi-4] = MK2_SYSEX_DATAEND3;
			break;
		case 1:
			buf[oi+0] = MK2_SYSEX_DATAEND1;
			buf[oi+1] = user_buffer[ii+0];
			buf[oi+2] = 0;
			buf[oi+3] = 0;
			break;
		case 2:
			buf[oi+0] = MK2_SYSEX_DATAEND2;
			buf[oi+1] = user_buffer[ii+0];
			buf[oi+2] = user_buffer[ii+1];
			buf[oi+3] = 0;
			break;
		default:
			break;
	}

	print_hex_dump(KERN_DEBUG, "mk2 write (raw): ", DUMP_PREFIX_ADDRESS,
			16, 1, user_buffer, count, true);

	print_hex_dump(KERN_DEBUG, "mk2 write: ", DUMP_PREFIX_ADDRESS,
			16, 1, buf, stuffed_size, true);

}

static ssize_t mk2_write(struct file *filp, const char __user *user_buffer, size_t count, loff_t *ppos)
{
	struct mk2dev *dev;
	struct mk2_write_endp endpoint;
	struct urb *urb = NULL;
	char *buf = NULL;
	ssize_t stuffed_size, retval = 0;

	if (count == 0)
		goto exit;

	count = min(count, USB_MK2_MAX_OUT_LEN);

	/* Each packet in sysex message must be padded to max width ie. 4.
	 * Thats why we round data size up first.
	 * */
	stuffed_size  = count + MK2_SYSEX_SIZE_ROUND_UP;

	/* Calculate number of packets */
	stuffed_size /= MK2_SYSEX_PACKET_SIZE;

	/* Get total data size with stuffed bytes between packets */
	stuffed_size *= MK2_STUFFED_PACKET_SIZE;


	dev = filp->private_data;
	endpoint = dev->write_endp;

	if (!(filp->f_flags & O_NONBLOCK)) {
		if (down_interruptible(&endpoint->limit_sem)) {
			retval = -ERESTARTSYS;
			goto exit;
		}
	} else {
		if (down_trylock(&endpoint->limit_sem)) {
			retval = -EAGAIN;
			goto exit;
		}
	}

	spin_lock_irq(&endpoint->err_lock);
	retval = endpoint->errors;
	if (retval < 0) {
		endpoint->errors = 0;
		retval = (retval == -EPIPE) ? retval : -EIO;
	}
	spin_unlock_irq(&dev->werr_lock);
	if (retval < 0)
		goto error;
	
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, stuffed_size, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}

	if (unlikely(!access_ok(user_buffer, count))) {
		retval = -EINVAL;
		goto error;
	}

	stuff_buffer(buf, stuffed_size, user_buffer, count);

	mutex_lock(&dev->io_mutex);
	if (unlikely(dev->disconnected)) {
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			  buf, stuffed_size, mk2_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &dev->submitted);

	retval = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (retval) {
		dev_err(&dev->interface->dev,
			"%s - failed to submit write urb, error %lu\n",
			__func__, retval);
		goto error_unanchor;
	}

	usb_free_urb(urb);

	return stuffed_size;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	if (urb) {
		usb_free_coherent(dev->udev, stuffed_size, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	up(&dev->limit_sem);

exit:
	return retval;
}

static void mk2_read_bulk_callback(struct urb *urb)
{
	struct mk2dev *dev;
	unsigned long irqstate;

	dev = urb->context;

	spin_lock_irqsave(&dev->read_endp.err_lock, irqstate);

	if (urb->status) {
		if (!(	urb->status == -ENOENT ||
			urb->status == -ECONNRESET ||
			urb->status == -ESHUTDOWN))
				dev_err(&dev->interface->dev,
					"%s - nonzero write bulk status received: %d\n",
					__func__, urb->status);

		dev->read_endp.errors = urb->status;
	} else {
		dev->read_endp.buffer.filled = urb->actual_length;
	}
	dev->read_endp.requested_read = 0;
	spin_unlock_irqrestore(&dev->read_endp.err_lock, irqstate);
	wake_up_interruptible(&dev->read_endp.wait_queue);
}

static int mk2_request_read(struct mk2_read_endp *endpoint, size_t count)
{
	struct mk2dev *dev;
	int retval;

	dev = container_of(endpoint, struct mk2dev, read_endp);

	usb_fill_bulk_urb(endpoint->urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev, endpoint->address),
			endpoint->buffer.data,
			min(endpoiint.buffer.size, count),
			mk2_read_bulk_callback,
			dev);

	spin_lock_irq(&endpoint->err_lock);
	endpoint->requested_read = 1;
	spin_unlock_irq(&endpoint->err_lock);

	endpoint->buffer.filled = 0;
	endpoint->buffer.copied = 0;

	retval = usb_submit_urb(endpoint->urb, GFP_KERNEL);
	if (retval < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, retval);

		retval = (retval == -ENOMEM) ? retval : -EIO;

		spin_lock_irq(&endpoint->err_lock);
		dev->requested_read= 0;
		spin_unlock_irq(&endpoint->err_lock);
	}

	return retval;
}

static ssize_t mk2_read(struct file *filp, char __user *user_buffer, size_t count, loff_t *ppos)
{
	struct mk2_read_endp *endpoint;
	int retval;
	size_t leftover;
	bool requested_read, disconnected, suspended;

	endpoint = &((struct mk2dev*) filp->private_data)->read_endp;

	if (!count)
		return -EINVAL;

	retval = mutex_lock_interruptible(&endpoint.io_mutex);
	if (retval < 0)
		return retval;
	
	if (dev->disconnected) {
		retval = -ENODEV;
		goto exit;
	}

retry:
	spin_lock_irq(&endpoint.err_lock);
	requested_read = endpoint->requested_read;
	spin_unlock_irq(&endpoint.err_lock);

	if (requested_read) {
		if (filp->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			goto exit;
		}

		retval = wait_event_interruptible(endpoint->wait_queue,
						(!endpoint->requested_read));
						// not needed parenthesis ?
		if (retval < 0)
			goto exit;
	}

	// At this point no read is requested.
	// One was never started or returned.
	// So check for errors.
	retval = endpoint.errors;
	if (retval < 0) {
		dev->errors = 0;
		retval = (retval == -EPIPE) ? retval : -EIO;
		goto exit;
	}

	// If data was read from device but not copied to user last time
	// its still in the buffer, return leftover to the user.
	// Otherwise immediately start read_request and try again.
	leftover = endpoint->buffer.filled - endpoint->buffer.copied;
	if (leftover) {
		size_t n = min(leftover, count);

		if (copy_to_user(user_buffer,
				 dev->bulk_in_buffer + dev->bulk_in_copied, n))
			retval = -EFAULT;
		else
			retval = chunk;

		endpoint->buffer.copied += n;

		if (leftover < count)
			mk2_request_read(endpoint, count - n);
	} else {
		retval = mk2_request_read(endpoint, count);
		if (retval < 0)
			goto exit;
		else
			goto retry;
	}

exit:
	mutex_unlock(&dev->io_mutex);
	return retval;
}

static const struct file_operations mk2_fops = {
	.owner   =	THIS_MODULE,
	.read    =	mk2_read,
	.write   =	mk2_write,
	.open    =	mk2_open,
	.release =	mk2_release,
	.llseek  =	noop_llseek,
};

static struct usb_class_driver mk2_class = {
	.name =		"mk2-%d",
	.fops =		&mk2_fops,
	.minor_base = 	USB_MK2_MINOR_BASE,
};

static int mk2_probe(struct usb_interface *interface,
		     const struct usb_device_id *id)
{
	struct mk2dev *dev;
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	int retval;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	// Initialize base kernel structures
	kref_init(&dev->kref);

	// Initialize read endpoints kernel structures
	mutex_init(&dev->read_endp.io_mutex);
	init_waitqueue_head(&dev->read_endp.wait_queue_head_t);
	spin_lock_init(&dev->read_endp.err_lock);

	// Initialize write endpoints kernel structures
	init_usb_anchor(&dev->write_endp.submitted);
	sema_init(&dev->write_endp.limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&dev->write_endp.io_mutex);
	spin_lock_init(&dev->write_endp.io_mutex);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);

	retval = usb_find_common_endpoints(interface->cur_altsetting,
					   &bulk_in, &bulk_out, NULL, NULL);
	if (retval) {
		dev_err(&interface->dev,
			"Could not find both bulk-in and bulk-out endpoints\n");
		goto error;
	}

	dev->read_endp.address = bulk_in->bEndpointAddress;
	if (!mk2_read_buffer_alloc(&dev->read_endp.buffer, usb_endpoint_maxp(bulk_in))) {
		retval = -ENOMEM;
		goto error;
	}
	dev->read_endp.urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->read_endp.urb) {
		retval = -ENOMEM;
		goto error;
	}

	dev->write_endp.address = bulk_out->bEndpointAddress;

	usb_set_intfdata(interface, dev);

	retval = usb_register_dev(interface, &mk2_class);
	if (retval) {
		dev_err(&interface->dev,
			"Not able to get minor for this device.\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	dev_info(&interface->dev,
		"USB MK2 device now attached to mk2-%d",
		interface->minor);
	return 0;

error:
	kref_put(&dev->kref, mk2_delete);
	return retval;
}

static void mk2_disconnect(struct usb_interface *interface)
{
	struct mk2dev *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	usb_deregister_dev(interface, &mk2_class);

	mutex_lock(&dev->read_endp.io_mutex);
	mutex_lock(&dev->write_endp.io_mutex);
	dev->disconnected = 1;
	mutex_unlock(&dev->write_endp.io_mutex);
	mutex_unlock(&dev->read_endp.io_mutex);

	usb_kill_urb(dev->read_endp.urb);
	usb_kill_anchored_urbs(&dev->write_endp.submitted);

	kref_put(&dev->kref, mk2_delete);
	dev_info(&interface->dev, "USB mk2 #%d now disconnceted", minor);
}

static struct usb_driver mk2_driver = {
	.name =	"mk2",
	.probe = mk2_probe,
	.disconnect = mk2_disconnect,
	.id_table = mk2_idtable,
	.supports_autosuspend = 1,
};
module_usb_driver(mk2_driver);

MODULE_AUTHOR(AUTHOR);
MODULE_DESCRIPTION(DESCRIPTION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL v2");
