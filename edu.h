#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#include <asm/types.h>

#define EDU_IOCTL_BASE 'e'

#define EDU_IOCTL_IDENT _IOR(EDU_IOCTL_BASE, 1, __u32)
#define EDU_IOCTL_LIVENESS _IOWR(EDU_IOCTL_BASE, 2, __u32)
#define EDU_IOCTL_FACTORIAL _IOWR(EDU_IOCTL_BASE, 3, __u32)
#define EDU_IOCTL_WAIT_IRQ _IOR(EDU_IOCTL_BASE, 4, __u32)
#define EDU_IOCTL_RAISE_IRQ _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 5, 0)
#define EDU_IOCTL_DMA_TO_DEVICE _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 6, 0)
#define EDU_IOCTL_DMA_FROM_DEVICE _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 7, 0)
#define EDU_IOCTL_SPDM_EXCHANGE _IOWR(EDU_IOCTL_BASE, 8, struct edu_spdm_data)
#define EDU_IOCTL_MMIO_FREE_WRITE _IOWR(EDU_IOCTL_BASE, 9, free_mmio_data)
#define EDU_IOCTL_SBI_SPDM_EXCHANGE _IOWR(EDU_IOCTL_BASE, 10, struct edu_spdm_data)
#define EDU_DMA_BUF_SIZE 4096

// ユーザー空間からSPDMリクエストを受け渡しするための構造体
struct edu_spdm_data
{
    __u64 request_ptr;  // アプリが32bitでも64bitでも必ず8バイト
    __u32 request_size; // 必ず4バイト
    __u32 padding1;     // アライメントの隙間を埋めるためのパディング
    __u64 response_ptr;
    __u32 response_size;
    __u32 padding2;
};

#ifndef PCI_DOE_FEATURE_CMA
#define PCI_DOE_FEATURE_DISCOVERY 0
#define PCI_DOE_FEATURE_CMA 1
#define PCI_DOE_FEATURE_SSESSION 2
#endif

/* TDISP response code */

#define PCI_TDISP_VERSION 0x01
#define PCI_TDISP_CAPABILITIES 0x02
#define PCI_TDISP_LOCK_INTERFACE_RSP 0x03
#define PCI_TDISP_DEVICE_INTERFACE_REPORT 0x04
#define PCI_TDISP_DEVICE_INTERFACE_STATE 0x05
#define PCI_TDISP_START_INTERFACE_RSP 0x06
#define PCI_TDISP_STOP_INTERFACE_RSP 0x07
#define PCI_TDISP_BIND_P2P_STREAM_RSP 0x08
#define PCI_TDISP_UNBIND_P2P_STREAM_RSP 0x09
#define PCI_TDISP_SET_MMIO_ATTRIBUTE_RSP 0x0A
#define PCI_TDISP_VDM_RSP 0x0B
#define PCI_TDISP_ERROR 0x7F

/* TDISP request code */

#define PCI_TDISP_GET_VERSION 0x81
#define PCI_TDISP_GET_CAPABILITIES 0x82
#define PCI_TDISP_LOCK_INTERFACE_REQ 0x83
#define PCI_TDISP_GET_DEVICE_INTERFACE_REPORT 0x84
#define PCI_TDISP_GET_DEVICE_INTERFACE_STATE 0x85
#define PCI_TDISP_START_INTERFACE_REQ 0x86
#define PCI_TDISP_STOP_INTERFACE_REQ 0x87
#define PCI_TDISP_BIND_P2P_STREAM_REQ 0x88
#define PCI_TDISP_UNBIND_P2P_STREAM_REQ 0x89
#define PCI_TDISP_SET_MMIO_ATTRIBUTE_REQ 0x8A
#define PCI_TDISP_VDM_REQ 0x8B

#define TDISP_VERSION 0x10

/* TriniTEE request code */
#define PCI_TDISP_TRINITEE_CHALLENGE_RESP_REQ 0xC1

/* TriniTEE Response code*/
#define PCI_TDISP_TRINITEE_CHALLENGE_RESP_RSP 0x51

// 11 byte
#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    uint8_t SPDM_Version;   // 0x14
    uint8_t Request_Code;   // 0xFE
    uint8_t Param1;         // 0x0
    uint8_t Param2;         // 0x0
    uint16_t Standard_ID;   /* PCI-SIG Standard ID = 3 */
    uint8_t Vendor_ID_Len;  // 0x2
    uint16_t Vendor_ID;     /* SPDM_VENDOR_ID_PCISIG = 0x0001 */
    uint16_t PayloadLength; //?
} SPDM_Header;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

static void make_SPDM_Header(SPDM_Header *s, uint32_t len)
{
    s->SPDM_Version = 0x14;
    s->Request_Code = 0xFE;
    s->Param1 = 0x0;
    s->Param2 = 0x0;
    s->Standard_ID = 0x3;
    s->Vendor_ID_Len = 0x2;
    s->Vendor_ID = 0x01;
    s->PayloadLength = len;
}

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
} Challenge_resp_req;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

static inline void make_Challenge_resp_req(Challenge_resp_req *r)
{
    make_SPDM_Header(&(r->sh), 17);
    r->Protocol_ID = 0x1;
    r->TDISP_Version = TDISP_VERSION;
    r->Request_Code = PCI_TDISP_TRINITEE_CHALLENGE_RESP_REQ;
    r->reserved = 0;
    r->RID = 0;
    r->Interface_ID_Reserved = 0;
}

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    uint8_t reserve[3];
    // TO DO: Make it possible to return multiple sets {BAR_num, offset} with a single query.
    uint8_t BAR_num;
    uint64_t offset;
    uint64_t nonce;
} challenge_resp_internal;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0x81
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
    uint32_t resp_size;
    challenge_resp_internal resp[8];
} Challenge_resp_rsp;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
} TDISP_get_version_req;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

static inline void make_TDISP_get_version(TDISP_get_version_req *r)
{
    make_SPDM_Header(&(r->sh), 17);
    r->Protocol_ID = 0x1;
    r->TDISP_Version = TDISP_VERSION;
    r->Request_Code = PCI_TDISP_GET_VERSION;
    r->reserved = 0;
    r->RID = 0;
    r->Interface_ID_Reserved = 0;
}

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
    uint8_t version_num_count;
    uint8_t version_num_entry[8];
} TDISP_get_version_rsp;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 37byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
    uint16_t flags;
    uint8_t default_stream_id;
    uint8_t reserved2;
    uint64_t mmio_offset;
    uint64_t p2p_address_mask;
} TDISP_lock_interface_request;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

static inline void make_TDISP_lock_interface_request(TDISP_lock_interface_request *r)
{
    make_SPDM_Header(&(r->sh), 37);
    r->Protocol_ID = 0x1;
    r->TDISP_Version = TDISP_VERSION;
    r->Request_Code = PCI_TDISP_LOCK_INTERFACE_REQ;
    r->reserved = 0;
    r->RID = 0;
    r->Interface_ID_Reserved = 0;
    r->flags = 0;
    r->default_stream_id = 0;
    r->reserved2 = 0;
    r->mmio_offset = 0;
    r->p2p_address_mask = 0;
}

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
    uint8_t interface_nonce[32];
} TDISP_lock_interface_request_rsp;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 49byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
    uint8_t interface_nonce[32];
} TDISP_start_interface_request;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

static inline void make_TDISP_start_interface_request(TDISP_start_interface_request *r, uint8_t *interface_nonce)
{
    make_SPDM_Header(&(r->sh), 49);
    r->Protocol_ID = 0x1;
    r->TDISP_Version = TDISP_VERSION;
    r->Request_Code = PCI_TDISP_START_INTERFACE_REQ;
    r->reserved = 0;
    r->RID = 0;
    r->Interface_ID_Reserved = 0;
    for (int i = 0; i < 32; i++)
    {
        r->interface_nonce[i] = interface_nonce[i];
    }
}

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
} TDISP_start_interface_request_rsp;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
} TDISP_stop_interface_request;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

static inline void make_TDISP_stop_interface_request(TDISP_stop_interface_request *r)
{
    make_SPDM_Header(&(r->sh), 17);
    r->Protocol_ID = 0x1;
    r->TDISP_Version = TDISP_VERSION;
    r->Request_Code = PCI_TDISP_STOP_INTERFACE_REQ;
    r->reserved = 0;
    r->RID = 0;
    r->Interface_ID_Reserved = 0;
}

#if defined(_MSC_VER)
#pragma pack(push, 1)
typedef struct
#elif defined(__GNUC__)
typedef struct __attribute__((packed))
#endif
{
    SPDM_Header sh;
    // 17byte
    uint8_t Protocol_ID;   // 0x1
    uint8_t TDISP_Version; // 0x10
    uint8_t Request_Code;  // 0xC1
    uint16_t reserved;     // 0x0
    uint32_t RID;
    uint64_t Interface_ID_Reserved;
} TDISP_stop_interface_request_rsp;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

typedef struct
{
    uint64_t offset;
    uint64_t val;
} free_mmio_data;