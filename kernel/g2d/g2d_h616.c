// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/dma-direction.h>
#include "g2d_regs.h"

MODULE_IMPORT_NS("DMA_BUF");

/*
 * Private prototype ABI. Task 4 programs the hardware from the resolved
 * addresses; for now G2D_IOC_BLT only imports and validates the buffers.
 */
struct g2d_blt_req {
    __s32 src_fd;        /* NV12 dma-buf (physically contiguous / CMA) */
    __s32 dst_fd;        /* XRGB8888 dma-buf */
    __u32 width, height; /* e.g. 1920, 1080 */
    __u32 src_format;    /* G2D_FMT_NV12 (0x28) */
    __u32 dst_format;    /* G2D_FMT_XRGB8888 (0x04) */
    __u32 timeout_ms;
};

#define G2D_IOC_BLT   _IOWR('G', 0x55, struct g2d_blt_req)
/* Debug: returns G2D_SCLK_GATE in the low 16 bits of the ioctl result. */
#define G2D_IOC_PING  _IO('G', 0x01)

#define G2D_MAX_DIM   2048

/*
 * CCU G2D bus-gating/reset register (CCU base 0x03001000 + 0x63c); bit 16 is
 * G2D_RST (0 = asserted, 1 = deasserted).
 */
#define CCU_G2D_BGR_REG  0x63c

struct g2d_import {
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;
    dma_addr_t base;
    size_t size;
};

struct g2d_dev {
    struct device *dev;
    void __iomem *base;
    void __iomem *ccu;   /* CCU page, used to deassert the G2D reset */
    struct clk *clk;
    struct clk *clk_bus;
    int irq;
    struct completion done;
    struct mutex lock;
    int open_count;
    struct miscdevice misc;
};

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

static int g2d_open(struct inode *inode, struct file *filp)
{
    struct miscdevice *misc = filp->private_data;
    struct g2d_dev *g = container_of(misc, struct g2d_dev, misc);

    mutex_lock(&g->lock);
    g->open_count++;
    mutex_unlock(&g->lock);
    return 0;
}

static int g2d_release(struct inode *inode, struct file *filp)
{
    struct miscdevice *misc = filp->private_data;
    struct g2d_dev *g = container_of(misc, struct g2d_dev, misc);

    mutex_lock(&g->lock);
    g->open_count--;
    mutex_unlock(&g->lock);
    return 0;
}

/*
 * Import a dma-buf and resolve a single contiguous DMA span covering the whole
 * buffer. The G2D block has no IOMMU here, so only physically contiguous
 * (CMA/reserved) buffers are usable; anything scattered is rejected.
 */
static int g2d_import(struct device *dev, int fd, struct g2d_import *imp)
{
    struct sg_table *sgt;
    struct scatterlist *sg;
    dma_addr_t base = 0;
    size_t span = 0;
    unsigned int i;
    int ret;

    memset(imp, 0, sizeof(*imp));

    if (fd < 0)
        return -EINVAL;

    imp->dmabuf = dma_buf_get(fd);
    if (IS_ERR(imp->dmabuf)) {
        ret = PTR_ERR(imp->dmabuf);
        imp->dmabuf = NULL;
        return ret;
    }

    imp->attach = dma_buf_attach(imp->dmabuf, dev);
    if (IS_ERR(imp->attach)) {
        ret = PTR_ERR(imp->attach);
        imp->attach = NULL;
        goto err_put;
    }

    sgt = dma_buf_map_attachment(imp->attach, DMA_BIDIRECTIONAL);
    if (IS_ERR(sgt)) {
        ret = PTR_ERR(sgt);
        goto err_detach;
    }
    imp->sgt = sgt;

    for_each_sgtable_dma_sg(sgt, sg, i) {
        dma_addr_t addr = sg_dma_address(sg);
        size_t len = sg_dma_len(sg);

        if (!len)
            continue;
        if (!span) {
            base = addr;
            span = len;
            continue;
        }
        if (addr != base + span) {
            dev_err(dev, "g2d: fd %d is not physically contiguous\n", fd);
            ret = -EINVAL;
            goto err_unmap;
        }
        span += len;
    }

    if (!span || span < imp->dmabuf->size) {
        dev_err(dev, "g2d: fd %d contiguous span %zu < buffer %zu\n",
                fd, span, imp->dmabuf->size);
        ret = -EINVAL;
        goto err_unmap;
    }

    imp->base = base;
    imp->size = span;
    return 0;

err_unmap:
    dma_buf_unmap_attachment(imp->attach, imp->sgt, DMA_BIDIRECTIONAL);
    imp->sgt = NULL;
err_detach:
    dma_buf_detach(imp->dmabuf, imp->attach);
    imp->attach = NULL;
err_put:
    dma_buf_put(imp->dmabuf);
    imp->dmabuf = NULL;
    return ret;
}

static void g2d_release_import(struct g2d_import *imp)
{
    if (imp->sgt)
        dma_buf_unmap_attachment(imp->attach, imp->sgt, DMA_BIDIRECTIONAL);
    if (imp->attach)
        dma_buf_detach(imp->dmabuf, imp->attach);
    if (imp->dmabuf)
        dma_buf_put(imp->dmabuf);
    memset(imp, 0, sizeof(*imp));
}

static long g2d_ioctl_blt(struct g2d_dev *g, unsigned long arg)
{
    struct g2d_blt_req req;
    struct g2d_import src, dst;
    int ret;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (!req.width || !req.height ||
        req.width > G2D_MAX_DIM || req.height > G2D_MAX_DIM)
        return -EINVAL;
    if (req.src_format != G2D_FMT_NV12 &&
        req.src_format != G2D_FMT_YUV420_UVC_U1V1U0V0)
        return -EINVAL;
    if (req.dst_format != G2D_FMT_XRGB8888)
        return -EINVAL;
    if (req.src_fd < 0 || req.dst_fd < 0)
        return -EINVAL;

    mutex_lock(&g->lock);

    ret = g2d_import(g->dev, req.src_fd, &src);
    if (ret)
        goto out_unlock;

    ret = g2d_import(g->dev, req.dst_fd, &dst);
    if (ret)
        goto out_src;

    dev_dbg(g->dev, "g2d: blt %ux%u src %pad+%zu dst %pad+%zu\n",
            req.width, req.height, &src.base, src.size, &dst.base, dst.size);

    g2d_release_import(&dst);
    ret = 0;

out_src:
    g2d_release_import(&src);
out_unlock:
    mutex_unlock(&g->lock);
    return ret;
}

static long g2d_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct miscdevice *misc = filp->private_data;
    struct g2d_dev *g = container_of(misc, struct g2d_dev, misc);

    switch (cmd) {
    case G2D_IOC_PING:
        return g2d_rd(g, G2D_SCLK_GATE) & 0xffff;
    case G2D_IOC_BLT:
        return g2d_ioctl_blt(g, arg);
    default:
        return -ENOTTY;
    }
}

static const struct file_operations g2d_fops = {
    .owner = THIS_MODULE, .open = g2d_open, .release = g2d_release,
    .unlocked_ioctl = g2d_ioctl,
};

static int g2d_probe(struct platform_device *pdev)
{
    struct g2d_dev *g;
    int ret;

    g = devm_kzalloc(&pdev->dev, sizeof(*g), GFP_KERNEL);
    if (!g) return -ENOMEM;
    g->dev = &pdev->dev;

    mutex_init(&g->lock);
    init_completion(&g->done);

    g->base = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(g->base)) return PTR_ERR(g->base);

    /*
     * Map the CCU page (second reg) so we can deassert the G2D reset. The
     * mainline CCU driver already owns that memory region, so the reserved
     * named-resource helper can fail with -EBUSY; if so fall back to an
     * unreserved ioremap of the same resource (and say so, never silently).
     */
    g->ccu = devm_platform_ioremap_resource_byname(pdev, "ccu");
    if (IS_ERR(g->ccu)) {
        struct resource *res;
        long map_err = PTR_ERR(g->ccu);

        g->ccu = NULL;
        res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ccu");
        if (!res) {
            dev_warn(&pdev->dev,
                     "g2d: no 'ccu' reg resource in DT; G2D reset stays asserted\n");
        } else {
            g->ccu = devm_ioremap(&pdev->dev, res->start, resource_size(res));
            if (!g->ccu)
                dev_warn(&pdev->dev, "g2d: ccu ioremap failed (%ld)\n", map_err);
            else
                dev_warn(&pdev->dev,
                         "g2d: ccu region shared with clock driver (%pe); used unreserved ioremap\n",
                         ERR_PTR(map_err));
        }
    }

    g->clk = devm_clk_get(&pdev->dev, "g2d");
    if (IS_ERR(g->clk))
        return dev_err_probe(&pdev->dev, PTR_ERR(g->clk), "failed to get g2d clock\n");
    g->clk_bus = devm_clk_get(&pdev->dev, "bus");
    if (IS_ERR(g->clk_bus))
        return dev_err_probe(&pdev->dev, PTR_ERR(g->clk_bus), "failed to get bus clock\n");

    /* sunxi convention: enable the bus clock before the module clock. */
    ret = clk_prepare_enable(g->clk_bus);
    if (ret)
        return dev_err_probe(&pdev->dev, ret, "failed to enable bus clock\n");
    ret = clk_prepare_enable(g->clk);
    if (ret) {
        clk_disable_unprepare(g->clk_bus);
        return dev_err_probe(&pdev->dev, ret, "failed to enable g2d clock\n");
    }

    g->irq = platform_get_irq(pdev, 0);
    if (g->irq < 0) {
        ret = g->irq;
        goto err_clk;
    }
    ret = devm_request_irq(&pdev->dev, g->irq, g2d_irq, 0, "g2d", g);
    if (ret)
        goto err_clk;

    /* Publish the instance before the chrdev can be opened. */
    platform_set_drvdata(pdev, g);
    g->misc.minor = MISC_DYNAMIC_MINOR;
    g->misc.name = "g2d";
    g->misc.fops = &g2d_fops;
    g->misc.parent = g->dev;

    /*
     * Mainline ccu-sun50i-h616.c has no G2D reset binding, so G2D_RST stays
     * asserted from boot and every G2D register access is dead. Deassert it
     * here, out-of-tree. Prototype only: the proper fix is a reset-controller
     * binding for the G2D bus in mainline.
     */
    if (g->ccu) {
        u32 v = readl(g->ccu + CCU_G2D_BGR_REG);

        writel(v | BIT(16), g->ccu + CCU_G2D_BGR_REG);
        dev_info(&pdev->dev, "g2d: CCU G2D_RST %s (0x%08x -> 0x%08x)\n",
                 v & BIT(16) ? "already deasserted" : "deasserted",
                 v, readl(g->ccu + CCU_G2D_BGR_REG));
    } else {
        dev_warn(&pdev->dev, "g2d: no CCU mapping; cannot deassert G2D reset\n");
    }

    g2d_hw_init(g);

    ret = misc_register(&g->misc);
    if (ret) {
        platform_set_drvdata(pdev, NULL);
        goto err_clk;
    }

    dev_info(&pdev->dev, "g2d ready\n");
    return 0;

err_clk:
    clk_disable_unprepare(g->clk);
    clk_disable_unprepare(g->clk_bus);
    return ret;
}

static void g2d_remove(struct platform_device *pdev)
{
    struct g2d_dev *g = platform_get_drvdata(pdev);

    if (!g)
        return;

    /* Unbind is suppressed via suppress_bind_attrs and rmmod is blocked by
     * .owner, so an open fd here would be a driver bug, not a race. */
    WARN_ON(g->open_count);

    misc_deregister(&g->misc);
    g2d_wr(g, G2D_SCLK_GATE, 0x0);
    g2d_wr(g, G2D_HCLK_GATE, 0x0);
    g2d_wr(g, G2D_AHB_RESET, 0x0);
    clk_disable_unprepare(g->clk);
    clk_disable_unprepare(g->clk_bus);
    platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id g2d_of[] = { { .compatible = "allwinner,sunxi-g2d" }, {} };
MODULE_DEVICE_TABLE(of, g2d_of);
static struct platform_driver g2d_driver = {
    .probe = g2d_probe, .remove = g2d_remove,
    .driver = {
        .name = "sunxi-g2d-h616",
        .of_match_table = g2d_of,
        .suppress_bind_attrs = true,
    },
};
module_platform_driver(g2d_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("scy");
MODULE_DESCRIPTION("Allwinner H616 G2D (prototype)");
