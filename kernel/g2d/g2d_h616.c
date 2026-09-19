#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include "g2d_regs.h"

struct g2d_dev {
    struct device *dev;
    void __iomem *base;
    struct clk *clk;
    int irq;
    struct completion done;
    struct mutex lock;
};

static struct g2d_dev *g2d;

static inline u32 g2d_rd(struct g2d_dev *g, u32 off) { return readl(g->base + off); }
static inline void g2d_wr(struct g2d_dev *g, u32 off, u32 v) { writel(v, g->base + off); }

static void g2d_hw_init(struct g2d_dev *g)
{
    g2d_wr(g, G2D_AHB_RESET, 0x0);
    g2d_wr(g, G2D_AHB_RESET, 0x3);
    g2d_wr(g, G2D_SCLK_GATE, 0x3);
    g2d_wr(g, G2D_HCLK_GATE, 0x3);
}

static irqreturn_t g2d_irq(int irq, void *data)
{
    struct g2d_dev *g = data;
    u32 tmp = g2d_rd(g, G2D_MIXER_INT);
    if (tmp & 0x1) {
        g2d_wr(g, G2D_MIXER_INT, tmp);   /* ack by write-back */
        complete(&g->done);
        return IRQ_HANDLED;
    }
    return IRQ_NONE;
}

static int g2d_open(struct inode *inode, struct file *filp) { return 0; }
static int g2d_release(struct inode *inode, struct file *filp) { return 0; }

static long g2d_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    /* filled in Task 3/4 */
    return -ENOTTY;
}

static const struct file_operations g2d_fops = {
    .owner = THIS_MODULE, .open = g2d_open, .release = g2d_release,
    .unlocked_ioctl = g2d_ioctl,
};

static struct miscdevice g2d_misc = { .minor = MISC_DYNAMIC_MINOR, .name = "g2d", .fops = &g2d_fops };

static int g2d_probe(struct platform_device *pdev)
{
    struct g2d_dev *g;
    int ret;

    g = devm_kzalloc(&pdev->dev, sizeof(*g), GFP_KERNEL);
    if (!g) return -ENOMEM;
    g->dev = &pdev->dev;
    g->base = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(g->base)) return PTR_ERR(g->base);
    g->clk = devm_clk_get(&pdev->dev, "g2d");
    if (IS_ERR(g->clk)) return dev_err_probe(&pdev->dev, PTR_ERR(g->clk), "clk\n");
    ret = clk_prepare_enable(g->clk);
    if (ret) return ret;
    g->irq = platform_get_irq(pdev, 0);
    if (g->irq < 0) { clk_disable_unprepare(g->clk); return g->irq; }
    ret = devm_request_irq(&pdev->dev, g->irq, g2d_irq, IRQF_TRIGGER_HIGH, "g2d", g);
    if (ret) { clk_disable_unprepare(g->clk); return ret; }
    mutex_init(&g->lock);
    init_completion(&g->done);
    g2d_hw_init(g);
    ret = misc_register(&g2d_misc);
    if (ret) { clk_disable_unprepare(g->clk); return ret; }
    g2d = g;
    dev_info(&pdev->dev, "g2d ready\n");
    return 0;
}

static void g2d_remove(struct platform_device *pdev)
{
    misc_deregister(&g2d_misc);
}

static const struct of_device_id g2d_of[] = { { .compatible = "allwinner,sunxi-g2d" }, {} };
MODULE_DEVICE_TABLE(of, g2d_of);
static struct platform_driver g2d_driver = {
    .probe = g2d_probe, .remove = g2d_remove,
    .driver = { .name = "sunxi-g2d-h616", .of_match_table = g2d_of },
};
module_platform_driver(g2d_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Allwinner H616 G2D (prototype)");
