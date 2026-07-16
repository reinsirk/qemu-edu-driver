#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/cdev.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/pci-doe.h>
#include <linux/io.h>
#include <asm/sbi.h> // Linuxカーネル内のSBI関連ヘッダ
#include "edu.h"

// See https://github.com/qemu/qemu/blob/stable-7.2/docs/specs/edu.txt
#define PCI_VENDOR_ID_QEMU 0x1234
#define PCI_DEVICE_ID_QEMU_EDU 0x11e8

// The number of bits be changed in QEMU via '-device edu,dma_mask=<mask>'
#define EDU_DMA_BITS 64
#define EDU_DMA_BUF_DEVICE_OFFSET 0x40000
#define EDU_DMA_CMD_START_XFER 1
#define EDU_DMA_CMD_RAM_TO_DEVICE 0
#define EDU_DMA_CMD_DEVICE_TO_RAM 2
#define EDU_DMA_CMD_RAISE_IRQ 4
#define EDU_STATUS_COMPUTING 0x01
#define EDU_STATUS_RAISE_IRQ 0x80

#define EDU_ADDR_IDENT 0x0
#define EDU_ADDR_LIVENESS 0x04
#define EDU_ADDR_FACTORIAL 0x08
#define EDU_ADDR_STATUS 0x20
#define EDU_ADDR_IRQ_STATUS 0x24
#define EDU_ADDR_IRQ_RAISE 0x60
#define EDU_ADDR_IRQ_ACK 0x64
#define EDU_ADDR_DMA_SRC 0x80
#define EDU_ADDR_DMA_DST 0x88
#define EDU_ADDR_DMA_XFER 0x90
#define EDU_ADDR_DMA_CMD 0x98

static const struct pci_device_id edu_pci_tbl[] = {
    {PCI_DEVICE(PCI_VENDOR_ID_QEMU, PCI_DEVICE_ID_QEMU_EDU)},
    {}};

// e.g. insmod edu.ko debug=1
// e.g. echo 1 > /sys/module/edu/parameters/debug
static bool param_debug;
module_param_named(debug, param_debug, bool, S_IRUGO | S_IWUSR);
#define edu_log(...) \
    if (param_debug) \
    pr_info(__VA_ARGS__)

// Load with msi=1 to use MSI instead of INTx
static bool param_msi;
module_param_named(msi, param_msi, bool, S_IRUGO);

struct edu_device
{
    bool registered_irq_handler;
    bool added_cdev;
    struct cdev cdev;
    char __iomem *iomem;
    unsigned int irq;
    u32 irq_value;
    wait_queue_head_t irq_wait_queue;
    dma_addr_t dma_bus_addr;
    void *dma_virt_addr;

    struct pci_doe_mb *doe_mb; // 追加: DOEメールボックスへのポインタ
};

static dev_t devno;
static const int minor = 0;
static struct edu_device *edu_dev;

// SBI呼び出し用のインラインアセンブラ関数（カーネル内で実行）
static inline struct sbiret custom_sbi_ecall(int ext, int fid, 
                                             unsigned long arg0, unsigned long arg1, 
                                             unsigned long arg2, unsigned long arg3, 
                                             unsigned long arg4) 
{
    struct sbiret ret;
    register long a0 asm("a0") = (long)arg0;
    register long a1 asm("a1") = (long)arg1;
    register long a2 asm("a2") = (long)arg2;
    register long a3 asm("a3") = (long)arg3;
    register long a4 asm("a4") = (long)arg4;
    register long a6 asm("a6") = (long)fid;
    register long a7 asm("a7") = (long)ext;

    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2), "r"(a3), "r"(a4), "r"(a6), "r"(a7)
        : "memory"
    );

    ret.error = a0;
    ret.value = a1;
    return ret;
}

static int edu_open(struct inode *inode, struct file *filp)
{
    struct edu_device *dev;

    nonseekable_open(inode, filp);
    dev = container_of(inode->i_cdev, struct edu_device, cdev);
    filp->private_data = dev;
    return 0;
}

static int edu_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static int edu_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct edu_device *dev = filp->private_data;
    unsigned long len = vma->vm_end - vma->vm_start;

    if (len > EDU_DMA_BUF_SIZE)
    {
        return -EINVAL;
    }
    // Only allow offset=0
    if (vma->vm_pgoff)
    {
        return -EINVAL;
    }
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    // VM_IO | VM_DONTEXPAND | VM_DONTDUMP are set by remap_pfn_range()
    return vm_iomap_memory(vma, __pa(dev->dma_virt_addr), len);
}

static int ioctl_ident(struct edu_device *dev, u32 __user *arg)
{
    u32 val = readl(dev->iomem + EDU_ADDR_IDENT);
    return put_user(val, arg);
}

static int ioctl_liveness(struct edu_device *dev, u32 __user *arg)
{
    u32 val;
    if (get_user(val, arg))
    {
        return -EFAULT;
    }
    writel(val, dev->iomem + EDU_ADDR_LIVENESS);
    val = readl(dev->iomem + EDU_ADDR_LIVENESS);
    return put_user(val, arg);
}

static bool is_computing_factorial(struct edu_device *dev)
{
    return readl(dev->iomem + EDU_ADDR_STATUS) & EDU_STATUS_COMPUTING;
}

static int ioctl_factorial(struct edu_device *dev, u32 __user *arg)
{
    u32 val;
    if (get_user(val, arg))
    {
        return -EFAULT;
    }
    // raise interrupt after finishing factorial computation
    writel(EDU_STATUS_RAISE_IRQ, dev->iomem + EDU_ADDR_STATUS);
    edu_log("Writing %u to register\n", val);
    writel(val, dev->iomem + EDU_ADDR_FACTORIAL);
    if (wait_event_interruptible(dev->irq_wait_queue, !is_computing_factorial(dev)))
    {
        return -ERESTARTSYS;
    }
    // read result
    val = readl(dev->iomem + EDU_ADDR_FACTORIAL);
    edu_log("Got factorial result: %u\n", val);
    return put_user(val, arg);
}

static int ioctl_wait_irq(struct edu_device *dev, u32 __user *arg)
{
    DEFINE_WAIT(wait);
    prepare_to_wait(&dev->irq_wait_queue, &wait, TASK_INTERRUPTIBLE);
    schedule();
    finish_wait(&dev->irq_wait_queue, &wait);
    if (signal_pending(current))
    {
        return -ERESTARTSYS;
    }
    return put_user(READ_ONCE(dev->irq_value), arg);
}

static int ioctl_raise_irq(struct edu_device *dev, u32 arg)
{
    writel(arg, dev->iomem + EDU_ADDR_IRQ_RAISE);
    return 0;
}

static bool is_doing_dma(struct edu_device *dev)
{
    return readl(dev->iomem + EDU_ADDR_DMA_CMD) & EDU_DMA_CMD_START_XFER;
}

static int do_dma(struct edu_device *dev, u32 len, bool to_device)
{
    u64 src, dst, cmd;
    if (len == 0 || len > EDU_DMA_BUF_SIZE)
    {
        return -EINVAL;
    }
    if (to_device)
    {
        src = (u64)dev->dma_bus_addr;
        dst = EDU_DMA_BUF_DEVICE_OFFSET;
        cmd = EDU_DMA_CMD_START_XFER | EDU_DMA_CMD_RAM_TO_DEVICE | EDU_DMA_CMD_RAISE_IRQ;
    }
    else
    {
        src = EDU_DMA_BUF_DEVICE_OFFSET;
        dst = (u64)dev->dma_bus_addr;
        cmd = EDU_DMA_CMD_START_XFER | EDU_DMA_CMD_DEVICE_TO_RAM | EDU_DMA_CMD_RAISE_IRQ;
    }
    edu_log("src=0x%016lx dst=0x%016lx len=%u\n", src, dst, len);
    writeq(src, dev->iomem + EDU_ADDR_DMA_SRC);
    writeq(dst, dev->iomem + EDU_ADDR_DMA_DST);
    writel(len, dev->iomem + EDU_ADDR_DMA_XFER);
    writel(cmd, dev->iomem + EDU_ADDR_DMA_CMD);
    if (wait_event_interruptible(dev->irq_wait_queue, !is_doing_dma(dev)))
    {
        return -ERESTARTSYS;
    }
    return 0;
}

static int ioctl_dma_to_device(struct edu_device *dev, u32 arg)
{
    return do_dma(dev, arg, true);
}

static int ioctl_dma_from_device(struct edu_device *dev, u32 arg)
{
    return do_dma(dev, arg, false);
}

static int ioctl_spdm_exchange(struct edu_device *dev, struct edu_spdm_data __user *arg)
{
    struct edu_spdm_data data;
    void *req_buf = NULL;
    void *resp_buf = NULL;
    void __user *user_req_ptr;
    void __user *user_resp_ptr;
    int ret;

    // メールボックスが未発見の場合はエラー
    if (!dev->doe_mb)
        return -ENODEV;

    // 1. ユーザー空間から構造体をコピー
    if (copy_from_user(&data, arg, sizeof(data)))
        return -EFAULT;

    // 2. 64bit型で受け取ったポインタをカーネル空間用にキャスト
    user_req_ptr = (void __user *)(uintptr_t)data.request_ptr;
    user_resp_ptr = (void __user *)(uintptr_t)data.response_ptr;

    // 3. リクエスト用バッファの確保とコピー
    req_buf = memdup_user(user_req_ptr, data.request_size);
    if (IS_ERR(req_buf))
        return PTR_ERR(req_buf);

    // 4. レスポンス用バッファの確保
    resp_buf = kzalloc(data.response_size, GFP_KERNEL);
    if (!resp_buf)
    {
        kfree(req_buf);
        return -ENOMEM;
    }

    // 5. 【修正箇所】 新しいカーネルの同期型APIを呼び出し
    // pci_doe()は内部でステートマシンやタイムアウト処理を行い、結果（受信バイト数）を返します
    ret = pci_doe(dev->doe_mb,
                  PCI_VENDOR_ID_PCI_SIG,
                  PCI_DOE_FEATURE_CMA,
                  req_buf, data.request_size,
                  resp_buf, data.response_size);

    // エラー発生時はそのまま終了処理へ
    if (ret < 0)
    {
        goto out;
    }

    // pci_doe()が成功した場合、retには実際にデバイスから受信したバイト数が入っている
    // 6. ユーザー空間へレスポンスをコピー
    if (copy_to_user(user_resp_ptr, resp_buf, ret))
    {
        ret = -EFAULT;
        goto out;
    }

    // 7. 実際のレスポンスサイズを更新して構造体を書き戻す
    data.response_size = ret;
    if (copy_to_user(arg, &data, sizeof(data)))
    {
        ret = -EFAULT;
        goto out;
    }

    ret = 0; // 正常終了

out:
    kfree(req_buf);
    kfree(resp_buf);
    return ret;
}

static int ioctl_mmio_free_write(struct edu_device *dev, free_mmio_data *arg)
{
    free_mmio_data data;
    if (copy_from_user(&data, arg, sizeof(data)))
        return -EFAULT;
    writel(data.val, dev->iomem + data.offset);
    return 0;
}

static long ioctl_spdm_sbi(struct edu_device *dev, struct edu_spdm_data *arg)
{
    struct edu_spdm_data spdm_args;
    void *req_buf, *rsp_buf;
    unsigned long req_gpa, rsp_gpa;
    struct sbiret sbi_ret;

    {
        if (copy_from_user(&spdm_args, arg, sizeof(spdm_args))) {
            pr_err("edu_spdm: Failed to copy spdm_args from user\n");
            return -EFAULT;
        }

        pr_info("edu_spdm: ioctl_spdm_sbi called. req_size=%u, rsp_max=%u\n", 
                spdm_args.request_size, spdm_args.response_size);

        // 1. カーネル空間に連続した物理メモリを確保 (kmalloc)
        req_buf = kmalloc(spdm_args.request_size, GFP_KERNEL);
        rsp_buf = kmalloc(spdm_args.response_size, GFP_KERNEL);
        if (!req_buf || !rsp_buf) {
            pr_err("edu_spdm: kmalloc failed (req_buf=%p, rsp_buf=%p)\n", req_buf, rsp_buf);
            kfree(req_buf); kfree(rsp_buf);
            return -ENOMEM;
        }

        // 2. ユーザー空間のデータをカーネルバッファにコピー
        if (copy_from_user(req_buf, (void __user *)spdm_args.request_ptr, spdm_args.request_size)) {
            pr_err("edu_spdm: Failed to copy request payload from user\n");
            kfree(req_buf); kfree(rsp_buf);
            return -EFAULT;
        }

        for (uint32_t i = 0; i < spdm_args.request_size; i++)
        {
            pr_info("%02X ", ((uint8_t*)req_buf)[i]);
        }
        pr_info("\n");
        // 3. 仮想アドレス(VA)をゲスト物理アドレス(GPA)に変換
        req_gpa = virt_to_phys(req_buf);
        rsp_gpa = virt_to_phys(rsp_buf);
        
        pr_info("edu_spdm: Translated addresses. req_gpa=0x%lx, rsp_gpa=0x%lx\n", 
                req_gpa, rsp_gpa);
        pr_info("edu_spdm: Invoking custom SBI call (ext=0x08000000)...\n");

        // 4. 特権モード(S-mode)からSBIコールを発行
        sbi_ret = custom_sbi_ecall(0x08000000, 0, 
                                   8 /* guest_rid */, req_gpa, spdm_args.request_size, 
                                   rsp_gpa, spdm_args.response_size);

        pr_info("edu_spdm: SBI call returned. error=%ld, value=%ld\n", 
                sbi_ret.error, sbi_ret.value);

        if (sbi_ret.error == 0) {
            // 5. 成功したら、レスポンスをユーザー空間にコピーして戻す
            spdm_args.response_size = sbi_ret.value; // 実際の受信サイズ
            
            if (copy_to_user((void __user *)spdm_args.response_ptr, rsp_buf, spdm_args.response_size)) {
                pr_err("edu_spdm: Failed to copy response payload to user\n");
                kfree(req_buf); kfree(rsp_buf);
                return -EFAULT;
            }
            
            if (copy_to_user((void __user *)arg, &spdm_args, sizeof(spdm_args))) {
                pr_err("edu_spdm: Failed to copy spdm_args back to user\n");
                kfree(req_buf); kfree(rsp_buf);
                return -EFAULT;
            }
            pr_info("edu_spdm: Successfully copied %u bytes back to user\n", spdm_args.response_size);
        } else {
            pr_err("edu_spdm: SBI call failed with error code %ld\n", sbi_ret.error);
        }

        kfree(req_buf);
        kfree(rsp_buf);
        
        return sbi_ret.error; // SBIの戻り値をユーザー空間に返す
    }
    return -ENOTTY;
}
static long edu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct edu_device *dev = filp->private_data;
    switch (cmd)
    {
    case EDU_IOCTL_IDENT:
        return ioctl_ident(dev, (u32 __user *)arg);
    case EDU_IOCTL_LIVENESS:
        return ioctl_liveness(dev, (u32 __user *)arg);
    case EDU_IOCTL_FACTORIAL:
        return ioctl_factorial(dev, (u32 __user *)arg);
    case EDU_IOCTL_WAIT_IRQ:
        return ioctl_wait_irq(dev, (u32 __user *)arg);
    case EDU_IOCTL_RAISE_IRQ:
        return ioctl_raise_irq(dev, (u32)arg);
    case EDU_IOCTL_DMA_TO_DEVICE:
        return ioctl_dma_to_device(dev, (u32)arg);
    case EDU_IOCTL_DMA_FROM_DEVICE:
        return ioctl_dma_from_device(dev, (u32)arg);
    case EDU_IOCTL_SPDM_EXCHANGE:
        return ioctl_spdm_exchange(dev, (struct edu_spdm_data __user *)arg);
    case EDU_IOCTL_MMIO_FREE_WRITE:
        return ioctl_mmio_free_write(dev, (free_mmio_data __user *)arg);
    case EDU_IOCTL_SBI_SPDM_EXCHANGE:
        return ioctl_spdm_sbi(dev, (struct edu_spdm_data __user *)arg);
    default:
        return -ENOTTY;
    }
}

struct file_operations edu_fops = {
    .owner = THIS_MODULE,
    .open = edu_open,
    .release = edu_release,
    .unlocked_ioctl = edu_ioctl,
    .mmap = edu_mmap,
};

static void edu_dev_init(struct edu_device *dev)
{
    cdev_init(&dev->cdev, &edu_fops);
    dev->cdev.owner = THIS_MODULE;
    init_waitqueue_head(&dev->irq_wait_queue);
}

static irqreturn_t edu_irq_handler(int irq, void *dev_id)
{
    struct edu_device *dev = dev_id;
    u32 irq_value;

    // Read the value which raised the interrupt
    irq_value = readl(dev->iomem + EDU_ADDR_IRQ_STATUS);
    edu_log("irq_value = %u\n", irq_value);
    // Clear the interrupt
    writel(irq_value, dev->iomem + EDU_ADDR_IRQ_ACK);
    // Wake up any tasks waiting on the queue
    WRITE_ONCE(dev->irq_value, irq_value);
    wake_up_interruptible(&dev->irq_wait_queue);
    return IRQ_HANDLED;
}

static void edu_pci_cleanup(struct pci_dev *pdev)
{
    if (!edu_dev)
    {
        return;
    }
    if (edu_dev->added_cdev)
    {
        cdev_del(&edu_dev->cdev);
        edu_dev->added_cdev = false;
    }
    if (edu_dev->registered_irq_handler)
    {
        free_irq(edu_dev->irq, edu_dev);
        edu_dev->registered_irq_handler = false;
    }
    if (pci_dev_msi_enabled(pdev))
    {
        pci_free_irq_vectors(pdev);
    }
}

static int edu_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    int err;
    int nvec;

    // enable PCI device
    err = pcim_enable_device(pdev);
    if (err)
    {
        goto fail;
    }

    // enable DMA (note that this is necessary for MSI)
    pci_set_master(pdev);
    err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(EDU_DMA_BITS));
    if (err)
    {
        goto fail;
    }

    // set up DMA mapping
    edu_dev->dma_virt_addr = dmam_alloc_coherent(
        &pdev->dev, EDU_DMA_BUF_SIZE, &edu_dev->dma_bus_addr, GFP_KERNEL);
    if (!edu_dev->dma_virt_addr)
    {
        err = -ENOMEM;
        goto fail;
    }
    edu_log("DMA bus addr = 0x%08lx\n", (unsigned long)edu_dev->dma_bus_addr);
    edu_log("DMA virt addr = %p\n", edu_dev->dma_virt_addr);

    // request and iomap PCI BAR
    // There is one memory region, 1 MB in size
    edu_log("resource 0: start=0x%08llx end=0x%08llx\n", pci_resource_start(pdev, 0), pci_resource_end(pdev, 0));
    err = pcim_iomap_regions(pdev, BIT(0), KBUILD_MODNAME);
    if (err)
    {
        goto fail;
    }
    edu_dev->iomem = pcim_iomap_table(pdev)[0];

    // --- 追加: SPDM用DOEメールボックスの探索 ---
    // PCI_VENDOR_ID_PCI_SIG (0x0001) と Data Object Type SPDM (0x01) を指定
    edu_dev->doe_mb = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG, PCI_DOE_FEATURE_CMA);
    if (!edu_dev->doe_mb)
    {
        edu_log("Warning: SPDM DOE mailbox not found on this device\n");
        // ※エラーにはせず、他のeduの機能は使えるようにしておく等の設計が考えられます
    }
    else
    {
        edu_log("Found SPDM DOE mailbox!\n");
    }
    // ---------------------------------------------

    if (param_msi)
    {
        // Fall back to INTx if MSI isn't available
        nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
        if (nvec < 0)
        {
            err = nvec;
            goto fail;
        }
        edu_dev->irq = pci_irq_vector(pdev, 0);
    }
    else
    {
        edu_dev->irq = pdev->irq;
    }
    edu_log("irq = %u\n", edu_dev->irq);
    // need IRQF_SHARED because all (legacy) PCI IRQ lines can be shared
    err = request_irq(edu_dev->irq, edu_irq_handler, IRQF_SHARED, KBUILD_MODNAME, edu_dev);
    if (err)
    {
        goto fail;
    }
    edu_dev->registered_irq_handler = true;

    // add char device
    err = cdev_add(&edu_dev->cdev, devno, 1);
    if (err)
    {
        goto fail;
    }
    edu_dev->added_cdev = true;

    // if (pdev->is_physfn&&!pci_num_vf(pdev))
    // {
    //     err = pci_enable_sriov(pdev, 2); // VF 数
    //     if (err)
    //     {
    //         printk(KERN_ERR "Failed to enable SR-IOV on edu PF %d\n",err);
    //         // pci_disable_device(pdev);
    //         // return err;
    //     }
    //     else
    //     {
    //         printk(KERN_INFO "SR-IOV enabled, VFs available\n");
    //     }
    // }

    return 0;
fail:
    edu_pci_cleanup(pdev);
    return err;
}

static void edu_pci_remove(struct pci_dev *pdev)
{
    pr_info("removing\n");
    pci_disable_sriov(pdev);
    edu_pci_cleanup(pdev);
}

static int dev_sriov_configure(struct pci_dev *dev, int numvfs)
{
    if (numvfs > 0)
    {
        int err = pci_enable_sriov(dev, numvfs);
        if (err)
        {
            printk(KERN_ERR "Failed to enable SR-IOV on edu PF %d\n", err);
            // pci_disable_device(pdev);
            // return err;
        }
        else
        {
            printk(KERN_INFO "SR-IOV enabled, VFs available\n");
        }
        return numvfs;
    }
    if (numvfs == 0)
    {
        pci_disable_sriov(dev);
        return 0;
    }
    return -1;
}

static struct pci_driver edu_pci_driver = {
    .name = KBUILD_MODNAME,
    .id_table = edu_pci_tbl,
    .probe = edu_pci_probe,
    .remove = edu_pci_remove,
    .sriov_configure = dev_sriov_configure,
};

static void edu_driver_cleanup(void)
{
    if (edu_dev)
    {
        kfree(edu_dev);
        edu_dev = NULL;
    }
    if (devno)
    {
        unregister_chrdev_region(devno, 1);
        devno = 0;
    }
}

// Uncomment __init if you don't need to debug this function in gdb
static int /* __init */ edu_init(void)
{
    int err;

    // allocate device number
    err = alloc_chrdev_region(&devno, minor, 1, KBUILD_MODNAME);
    if (err)
    {
        return err;
    }
    // You can also get the major number by checking /proc/devices
    pr_alert("device number is %d:%d\n", MAJOR(devno), minor);
    // Now from userspace you can run e.g.
    // mknod /dev/edu c 250 0

    // initialize driver state
    edu_dev = kzalloc(sizeof(*edu_dev), GFP_KERNEL);
    if (!edu_dev)
    {
        err = -ENOMEM;
        goto fail;
    }
    edu_dev_init(edu_dev);

    err = pci_register_driver(&edu_pci_driver);
    if (err)
    {
        goto fail;
    }
    return 0;

fail:
    edu_driver_cleanup();
    return err;
}

// Uncomment __exit if you don't need to debug this function in gdb
static void /* __exit */ edu_exit(void)
{
    pci_unregister_driver(&edu_pci_driver);
    edu_driver_cleanup();
}

module_init(edu_init);
module_exit(edu_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QEMU EDU device driver");
