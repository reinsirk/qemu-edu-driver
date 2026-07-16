#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "edu.h"

uint8_t g_interface_nonce[32];

static const char *const device_path = "/dev/edu";

static void print_usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s ident\n", argv0);
    fprintf(stderr, "       %s liveness <number>\n", argv0);
    fprintf(stderr, "       %s factorial <number>\n", argv0);
    fprintf(stderr, "       %s wait\n", argv0);
    fprintf(stderr, "       %s raise <number>\n", argv0);
    fprintf(stderr, "       %s dma-write\n", argv0);
    fprintf(stderr, "       %s dma-read <size>\n", argv0);
}

static bool parse_int_arg(const char *s, unsigned int *val)
{
    const bool is_hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    return sscanf(s, is_hex ? "%x" : "%u", val) == 1;
}

// SBIの戻り値を格納する構造体
struct sbiret
{
    long error; // ret[0] に対応
    long value; // ret[1] に対応
};

// SBI呼び出し用のインラインアセンブラ関数
static inline struct sbiret sbi_ecall(int ext, int fid,
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
    register long a6 asm("a6") = (long)fid; // Function ID (今回は未使用ですが仕様上a6に入れます)
    register long a7 asm("a7") = (long)ext; // Extension ID (0x08000000)

    asm volatile(
        "ecall"
        : "+r"(a0), "=r"(a1)
        : "r"(a2), "r"(a3), "r"(a4), "r"(a6), "r"(a7)
        : "memory");

    ret.error = a0;
    ret.value = a1;
    return ret;
}

int main(int argc, char *argv[])
{
    int fd;
    int ret;
    unsigned int val = 0;
    void *dma_mem;
    ssize_t num_read;
    ssize_t num_written;

    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    fd = open(device_path, O_RDWR);
    if (fd < 0)
    {
        perror("open");
        return -fd;
    }
    if (strcmp(argv[1], "ident") == 0)
    {
        ret = ioctl(fd, EDU_IOCTL_IDENT, &val);
        if (ret)
            goto ioctl_fail;
        // Format is 0xRRrr00ed
        // (RR = major version, rr = minor version)
        if ((val & 0xff) != 0xed)
        {
            fprintf(stderr, "read value has unexpected format: 0x%08x\n", val);
            return EXIT_FAILURE;
        }
        printf("Major version = %u\n", (val & 0xff000000) >> 24);
        printf("Minor version = %u\n", (val & 0x00ff0000) >> 16);
    }
    else if (strcmp(argv[1], "liveness") == 0)
    {
        if (argc != 3 || !parse_int_arg(argv[2], &val))
        {
            goto bad_usage;
        }
        ret = ioctl(fd, EDU_IOCTL_LIVENESS, &val);
        if (ret)
            goto ioctl_fail;
        printf("Inverted value: 0x%08x\n", val);
    }
    else if (strcmp(argv[1], "factorial") == 0)
    {
        if (argc != 3 || !parse_int_arg(argv[2], &val))
        {
            goto bad_usage;
        }
        ret = ioctl(fd, EDU_IOCTL_FACTORIAL, &val);
        if (ret)
            goto ioctl_fail;
        printf("Computed value: %u\n", val);
    }
    else if (strcmp(argv[1], "wait") == 0)
    {
        ret = ioctl(fd, EDU_IOCTL_WAIT_IRQ, &val);
        if (ret)
            goto ioctl_fail;
        printf("IRQ value: %u\n", val);
    }
    else if (strcmp(argv[1], "raise") == 0)
    {
        if (argc != 3 || !parse_int_arg(argv[2], &val))
        {
            goto bad_usage;
        }
        ret = ioctl(fd, EDU_IOCTL_RAISE_IRQ, (unsigned int)val);
        if (ret)
            goto ioctl_fail;
    }
    else if (strcmp(argv[1], "dma-write") == 0)
    {
        dma_mem = mmap(NULL, EDU_DMA_BUF_SIZE, PROT_WRITE, MAP_SHARED, fd, 0);
        if (dma_mem == MAP_FAILED)
            goto mmap_fail;
        num_read = read(STDIN_FILENO, dma_mem, EDU_DMA_BUF_SIZE);
        if (num_read < 0)
            goto read_fail;
        ret = ioctl(fd, EDU_IOCTL_DMA_TO_DEVICE, (unsigned int)num_read);
        if (ret)
            goto ioctl_fail;
    }
    else if (strcmp(argv[1], "dma-read") == 0)
    {
        if (argc != 3 || !parse_int_arg(argv[2], &val))
        {
            goto bad_usage;
        }
        dma_mem = mmap(NULL, EDU_DMA_BUF_SIZE, PROT_READ, MAP_SHARED, fd, 0);
        if (dma_mem == MAP_FAILED)
            goto mmap_fail;
        ret = ioctl(fd, EDU_IOCTL_DMA_FROM_DEVICE, val);
        if (ret)
            goto ioctl_fail;
        num_written = write(STDOUT_FILENO, dma_mem, val);
        if (num_written < 0)
            goto write_fail;
        else if (num_written < val)
            fprintf(stderr, "WARN: partial write\n");
    }
    else if (strcmp(argv[1], "spdm-test") == 0)
    {
        struct edu_spdm_data spdm_args = {0, 0, 0, 0, 0, 0};

        // SPDMメッセージの例: GET_VERSION リクエスト (SPDM 1.0)
        uint8_t spdm_request[] = {0x10, 0x84, 0x00, 0x00};
        uint8_t spdm_response[1024] = {0}; // 受信用の十分なバッファ

        // 2. 構造体にポインタとサイズをセット
        spdm_args.request_ptr = (uint64_t)(uintptr_t)spdm_request;
        spdm_args.request_size = sizeof(spdm_request);

        spdm_args.response_ptr = (uint64_t)(uintptr_t)spdm_response;
        spdm_args.response_size = sizeof(spdm_response); // 受信バッファの最大サイズを教える

        // 3. IOCTLを呼び出す
        printf("Sending SPDM Request...\n");
        if (ioctl(fd, EDU_IOCTL_SPDM_EXCHANGE, &spdm_args) < 0)
        {
            perror("IOCTL_SPDM_EXCHANGE failed");
            close(fd);
            return -1;
        }

        // 4. 結果の確認
        // ioctl成功後、spdm_args.response_size には実際に受信したバイト数が上書きされています
        printf("Received SPDM Response (Size: %u bytes):\n", spdm_args.response_size);
        for (uint32_t i = 0; i < spdm_args.response_size; i++)
        {
            printf("%02X ", spdm_response[i]);
        }
        printf("\n");
    }
    else if (strcmp(argv[1], "challenge") == 0)
    {
        free_mmio_data d;
        d = (free_mmio_data){0x0, 2025};
        ioctl(fd, EDU_IOCTL_MMIO_FREE_WRITE, &d);
        d = (free_mmio_data){0x1000, 521};
        ioctl(fd, EDU_IOCTL_MMIO_FREE_WRITE, &d);
        d = (free_mmio_data){0x0, 2026};
        ioctl(fd, EDU_IOCTL_MMIO_FREE_WRITE, &d);
        d = (free_mmio_data){0x2000, 528};
        ioctl(fd, EDU_IOCTL_MMIO_FREE_WRITE, &d);
        struct edu_spdm_data spdm_args = {0, 0, 0, 0, 0, 0};
        Challenge_resp_req *spdm_request;
        Challenge_resp_req r;
        spdm_request = &r;
        make_Challenge_resp_req(spdm_request);

        uint8_t spdm_response[1024] = {0}; // 受信用の十分なバッファ

        spdm_args.request_ptr = (uint64_t)(uintptr_t)spdm_request;
        // spdm_args.request_size = sizeof(spdm_request);
        spdm_args.request_size = sizeof(Challenge_resp_req);

        spdm_args.response_ptr = (uint64_t)(uintptr_t)spdm_response;
        spdm_args.response_size = sizeof(spdm_response); // 受信バッファの最大サイズを教える

        printf("Sending SPDM Request...\n");
        if (ioctl(fd, EDU_IOCTL_SPDM_EXCHANGE, &spdm_args) < 0)
        {
            perror("IOCTL_SPDM_EXCHANGE failed");
            close(fd);
            return -1;
        }

        Challenge_resp_rsp *ret = spdm_response;
        if (ret->Request_Code == 0x7f)
        {
            perror("TDISP Internal Error!\n");
            close(fd);
            return -1;
        }
        printf("Retrun Response\n");
        for (int i = 0; i < ret->resp_size; i++)
        {
            printf("BAR num: %d, offset: %lu, nonce: %lu\n", ret->resp[i].BAR_num, ret->resp[i].offset, ret->resp[i].nonce);
        }
    }
    else if (strcmp(argv[1], "tdisp_version") == 0)
    {
        struct edu_spdm_data spdm_args = {0, 0, 0, 0, 0, 0};
        TDISP_get_version_req *spdm_request;
        TDISP_get_version_req *r;
        spdm_request = &r;
        make_TDISP_get_version(spdm_request);

        uint8_t spdm_response[1024] = {0}; // 受信用の十分なバッファ

        spdm_args.request_ptr = (uint64_t)(uintptr_t)spdm_request;
        // spdm_args.request_size = sizeof(spdm_request);
        spdm_args.request_size = sizeof(TDISP_get_version_req);

        spdm_args.response_ptr = (uint64_t)(uintptr_t)spdm_response;
        spdm_args.response_size = sizeof(spdm_response); // 受信バッファの最大サイズを教える

        // printf("Sending SPDM Request...\n");
        if (ioctl(fd, EDU_IOCTL_SPDM_EXCHANGE, &spdm_args) < 0)
        {
            perror("IOCTL_SPDM_EXCHANGE failed");
            close(fd);
            return -1;
        }
        TDISP_get_version_rsp *ret;
        ret = spdm_response;
        if (ret->Request_Code == 0x7f)
        {
            perror("TDISP Internal Error!\n");
            close(fd);
            return -1;
        }
    }
    else if (strcmp(argv[1], "lock") == 0)
    {
        struct edu_spdm_data spdm_args = {0, 0, 0, 0, 0, 0};
        TDISP_lock_interface_request *spdm_request;
        TDISP_lock_interface_request r;
        spdm_request = &r;
        make_TDISP_lock_interface_request(spdm_request);

        uint8_t spdm_response[1024] = {0}; // 受信用の十分なバッファ

        spdm_args.request_ptr = (uint64_t)(uintptr_t)spdm_request;
        // spdm_args.request_size = sizeof(spdm_request);
        spdm_args.request_size = sizeof(TDISP_lock_interface_request);

        spdm_args.response_ptr = (uint64_t)(uintptr_t)spdm_response;
        spdm_args.response_size = sizeof(spdm_response); // 受信バッファの最大サイズを教える

        // printf("Sending SPDM Request...\n");
        if (ioctl(fd, EDU_IOCTL_SPDM_EXCHANGE, &spdm_args) < 0)
        {
            perror("IOCTL_SPDM_EXCHANGE failed");
            close(fd);
            return -1;
        }
        TDISP_lock_interface_request_rsp *ret;
        ret = spdm_response;
        if (ret->Request_Code == 0x7f)
        {
            perror("TDISP Internal Error!\n");
            close(fd);
            return -1;
        }
        FILE *fp = fopen("nonce.bin", "wb");
        if (!fp)
        {
            perror("fopen");
            return 1;
        }
        for (int i = 0; i < 32; i++)
        {
            // g_interface_nonce[i] = ret->interface_nonce[i];
            fwrite((ret->interface_nonce), sizeof(uint8_t), 32, fp);
        }
        fclose(fp);
    }
    else if (strcmp(argv[1], "start") == 0)
    {
        struct edu_spdm_data spdm_args = {0, 0, 0, 0, 0, 0};
        TDISP_start_interface_request *spdm_request;
        TDISP_start_interface_request r;
        spdm_request = &r;
        FILE *fp = fopen("nonce.bin", "rb");
        if (!fp)
        {
            perror("fopen");
            return 1;
        }
        for (int i = 0; i < 32; i++)
        {
            // g_interface_nonce[i] = ret->interface_nonce[i];
            fread(g_interface_nonce, sizeof(uint8_t), 32, fp);
        }
        fclose(fp);
        make_TDISP_start_interface_request(spdm_request, g_interface_nonce);

        uint8_t spdm_response[1024] = {0}; // 受信用の十分なバッファ

        spdm_args.request_ptr = (uint64_t)(uintptr_t)spdm_request;
        // spdm_args.request_size = sizeof(spdm_request);
        spdm_args.request_size = sizeof(TDISP_start_interface_request);

        spdm_args.response_ptr = (uint64_t)(uintptr_t)spdm_response;
        spdm_args.response_size = sizeof(spdm_response); // 受信バッファの最大サイズを教える

        // printf("Sending SPDM Request...\n");
        if (ioctl(fd, EDU_IOCTL_SPDM_EXCHANGE, &spdm_args) < 0)
        {
            perror("IOCTL_SPDM_EXCHANGE failed");
            close(fd);
            return -1;
        }
        TDISP_start_interface_request_rsp *ret;
        ret = spdm_response;
        if (ret->Request_Code == 0x7f)
        {
            perror("TDISP Internal Error!\n");
            close(fd);
            return -1;
        }
    }
    else if (strcmp(argv[1], "stop") == 0)
    {
        struct edu_spdm_data spdm_args = {0, 0, 0, 0, 0, 0};
        TDISP_stop_interface_request *spdm_request;
        TDISP_stop_interface_request r;
        spdm_request = &r;
        make_TDISP_stop_interface_request(spdm_request);

        uint8_t spdm_response[1024] = {0}; // 受信用の十分なバッファ

        spdm_args.request_ptr = (uint64_t)(uintptr_t)spdm_request;
        // spdm_args.request_size = sizeof(spdm_request);
        spdm_args.request_size = sizeof(TDISP_stop_interface_request);

        spdm_args.response_ptr = (uint64_t)(uintptr_t)spdm_response;
        spdm_args.response_size = sizeof(spdm_response); // 受信バッファの最大サイズを教える

        // printf("Sending SPDM Request...\n");
        if (ioctl(fd, EDU_IOCTL_SPDM_EXCHANGE, &spdm_args) < 0)
        {
            perror("IOCTL_SPDM_EXCHANGE failed");
            close(fd);
            return -1;
        }
        TDISP_stop_interface_request_rsp *ret;
        ret = spdm_response;
        if (ret->Request_Code == 0x7f)
        {
            perror("TDISP Internal Error!\n");
            close(fd);
            return -1;
        }
    }
    else if (strcmp(argv[1], "spdm-test-sbi") == 0)
    {
        // SPDMメッセージの例: GET_VERSION リクエスト (SPDM 1.0)
        uint8_t spdm_request[] = {0x10, 0x84, 0x00, 0x00};
        uint8_t spdm_response[1024] = {0}; // 受信用の十分なバッファ

        // 引数の準備
        uint32_t guest_rid = 0; // ★環境に合わせて適切なRequester IDを設定してください
        unsigned long req_addr = (uint64_t)(uintptr_t)spdm_request;
        unsigned long req_size = sizeof(spdm_request);
        unsigned long rsp_addr = (uint64_t)(uintptr_t)spdm_response;
        unsigned long rsp_max = sizeof(spdm_response);

        struct edu_spdm_data spdm_args = {req_addr, req_size, 0, rsp_addr, rsp_max, 0};

        printf("Sending SPDM Request via SBI...\n");

        // ioctlの代わりにSBIコールを発行
        // a0 = guest_rid
        // a1 = req_gpa
        // a2 = req_size
        // a3 = rsp_gpa
        // a4 = rsp_max
        printf("Sending SPDM Request...\n");
        if (ioctl(fd, EDU_IOCTL_SBI_SPDM_EXCHANGE, &spdm_args) < 0)
        {
            perror("IOCTL_SPDM_EXCHANGE failed");
            close(fd);
            return -1;
        }
        // QEMU側で SBI_SUCCESS (0) または SBI_ERR_FAILED 等が返される
        // if (ret.error != 0)
        // {
        //     printf("SBI call failed with error: %ld\n", ret.error);
        //     return -1;
        // }

        // QEMU側の実装で run->riscv_sbi.ret[1] = data.response_size; としているため、
        // 実際の受信サイズは ret.value に格納されています。
        uint32_t actual_response_size = (uint32_t)spdm_args.response_size;

        printf("Received SPDM Response (Size: %u bytes):\n", actual_response_size);
        for (uint32_t i = 0; i < actual_response_size; i++)
        {
            printf("%02X ", spdm_response[i]);
        }
        printf("\n");

        return 0;
    }
    else
    {
        goto bad_usage;
    }
    return EXIT_SUCCESS;
bad_usage:
    print_usage(argv[0]);
    return EXIT_FAILURE;
ioctl_fail:
    perror("ioctl");
    return EXIT_FAILURE;
mmap_fail:
    perror("mmap");
    return EXIT_FAILURE;
read_fail:
    perror("read");
    return EXIT_FAILURE;
write_fail:
    perror("write");
    return EXIT_FAILURE;
}
