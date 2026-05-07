#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#include <asm/types.h>

#define EDU_IOCTL_BASE 'e'

#define EDU_IOCTL_IDENT           _IOR(EDU_IOCTL_BASE, 1, __u32)
#define EDU_IOCTL_LIVENESS        _IOWR(EDU_IOCTL_BASE, 2, __u32)
#define EDU_IOCTL_FACTORIAL       _IOWR(EDU_IOCTL_BASE, 3, __u32)
#define EDU_IOCTL_WAIT_IRQ        _IOR(EDU_IOCTL_BASE, 4, __u32)
#define EDU_IOCTL_RAISE_IRQ       _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 5, 0)
#define EDU_IOCTL_DMA_TO_DEVICE   _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 6, 0)
#define EDU_IOCTL_DMA_FROM_DEVICE _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 7, 0)
#define EDU_IOCTL_SPDM_EXCHANGE _IOWR(EDU_IOCTL_BASE, 8, struct edu_spdm_data)

#define EDU_DMA_BUF_SIZE 4096

// ユーザー空間からSPDMリクエストを受け渡しするための構造体
struct edu_spdm_data {
    __u64 request_ptr;   // アプリが32bitでも64bitでも必ず8バイト
    __u32 request_size;  // 必ず4バイト
    __u32 padding1;      // アライメントの隙間を埋めるためのパディング
    __u64 response_ptr;
    __u32 response_size;
    __u32 padding2;
};

#ifndef PCI_DOE_FEATURE_CMA
#define PCI_DOE_FEATURE_DISCOVERY 0
#define PCI_DOE_FEATURE_CMA       1
#define PCI_DOE_FEATURE_SSESSION  2
#endif