#include <linux/init.h>
#include <linux/module.h>

static int hm_init(void)
{
  printk(KERN_ALERT "hm init\n");
  return 0;
}

static void hm_exit(void)
{
  printk(KERN_ALERT "hm exit\n");
}

module_init(hm_init);
module_exit(hm_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("secmeant");
MODULE_DESCRIPTION("Test driver");
MODULE_VERSION("1.0");

