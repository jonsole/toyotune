
#include "hardware/address_mapped.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#include "hardware/spi.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/binary_info.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tlsf/tlsf.h"

#include "rp_pico_alloc.h"

static tlsf_t _mem_heap = NULL;
static pool_t _mem_sram_pool = NULL;
static pool_t _mem_psram_pool = NULL;

static size_t _psram_size = 0;

static bool _bInitalized = false;

#if defined(RP_PICO_ALLOC_WRAP)
static bool _bUseHeapPool = true;
#else
static bool _bUseHeapPool = false;
#endif

static size_t __no_inline_not_in_flash_func(setup_psram)(uint psram_cs_pin)
{
    gpio_set_function(psram_cs_pin, GPIO_FUNC_XIP_CS1);

    size_t psram_size = 0;

    const int max_psram_freq = 133000000;
    const int clock_hz = clock_get_hz(clk_sys);
    int clockDivider = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (clockDivider == 1 && clock_hz > 100000000) {
        clockDivider = 2;
    }
    int rxdelay = clockDivider;
    if (clock_hz / clockDivider > 100000000) {
        rxdelay += 1;
    }

    // - Max select must be <= 8us.  The value is given in multiples of 64 system clocks.
    // - Min deselect must be >= 18ns.  The value is given in system clock cycles - ceil(divisor / 2).
    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int maxSelect = (125 * 1000000) / clock_period_fs;  // 125 = 8000ns / 64
    const int minDeselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (clockDivider + 1) / 2;

    stdio_printf("Max Select: %d, Min Deselect: %d, clock divider: %d\n", maxSelect, minDeselect, clockDivider);

    uint32_t intr_stash = save_and_disable_interrupts();

    // Try and read the PSRAM ID via direct_csr.
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;

    // direct-mode operation
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }

    // Exit out of QMI in case we've inited already
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    // Turn off quad.
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | PSRAM_CMD_QUAD_END;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    // Read the id
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;
    for (size_t i = 0; i < 7; i++)
    {
        if (i == 0)
        {
            qmi_hw->direct_tx = PSRAM_CMD_READ_ID;
        }
        else
        {
            qmi_hw->direct_tx = PSRAM_CMD_NOOP;
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0)
        {
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
        {
        }
        if (i == 5)
        {
            kgd = qmi_hw->direct_rx;
        }
        else if (i == 6)
        {
            eid = qmi_hw->direct_rx;
        }
        else
        {
            (void)qmi_hw->direct_rx;
        }
    }
    
    // Disable direct csr.
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(intr_stash);
    if (kgd != PSRAM_ID)
    {
        printf("Invalid PSRAM ID: %x\n", kgd);
        return psram_size;
    }
    printf("Valid PSRAM ID: %x\n", kgd);
    intr_stash = save_and_disable_interrupts();

    // Enable quad mode.
    qmi_hw->direct_csr = (30 << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    
    // direct-mode operation
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
    {
    }

    // RESETEN, RESET and quad enable
    for (uint8_t i = 0; i < 4; i++)
    {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        if (i == 0)
        {
            qmi_hw->direct_tx = PSRAM_CMD_RSTEN;
        }
        else if (i == 1)
        {
            qmi_hw->direct_tx = PSRAM_CMD_RST;
        }
        else if (i == 2)
        {
            qmi_hw->direct_tx = PSRAM_CMD_QUAD_ENABLE;
        }
        else 
        {
            qmi_hw->direct_tx = PSRAM_CMD_LINEAR_TOGGLE;
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0)
        {
        }
        qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
        for (size_t j = 0; j < 20; j++)
        {
            asm("nop");
        }
        (void)qmi_hw->direct_rx;
    }
    // Disable direct csr.
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    qmi_hw->m[1].timing =
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) | // Break between pages.
        (1 << QMI_M1_TIMING_COOLDOWN_LSB) | (rxdelay << QMI_M1_TIMING_RXDELAY_LSB) |
        (maxSelect << QMI_M1_TIMING_MAX_SELECT_LSB) |  // In units of 64 system clock cycles. PSRAM says 8us max. 8 / 0.00752 /64
                                              // = 16.62
        (minDeselect << QMI_M1_TIMING_MIN_DESELECT_LSB) | // In units of system clock cycles. PSRAM says 50ns.50 / 7.52 = 6.64
        (clockDivider << QMI_M1_TIMING_CLKDIV_LSB);
    
    qmi_hw->m[1].rfmt = (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
                         (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB) |
                         (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
                         (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB) |
                         (QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB) |
                         (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB) |
                         (QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB) |
                         (QMI_M1_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = (PSRAM_CMD_QUAD_READ);
    qmi_hw->m[1].wfmt = (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
                         (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB) |
                         (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
                         (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB) |
                         (QMI_M1_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M1_WFMT_DUMMY_LEN_LSB) |
                         (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB) |
                         (QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB) |
                         (QMI_M1_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = (PSRAM_CMD_QUAD_WRITE);

    psram_size = 1024 * 1024; // 1 MiB
    uint8_t size_id = eid >> 5;
    if (eid == 0x26 || size_id == 2)
    {
        psram_size *= 8;//8
    }
    else if (size_id == 0)
    {
        psram_size *= 1;
    }
    else if (size_id == 1)
    {
        psram_size *= 4;
    }

    // Mark that we can write to PSRAM.
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
    restore_interrupts(intr_stash);
    printf("PSRAM ID: %x %x\n", kgd, eid);
    return psram_size;
}

//-------------------------------------------------------------------------------
// Allocator Routines
//-------------------------------------------------------------------------------

// Location /address where PSRAM starts
#define PSRAM_LOCATION _u(0x11000000)

// Internal function that init's the allocator
static bool rp_pico_alloc_init()
{
   if (_bInitalized)
        return true;

    _mem_heap = NULL;
    _mem_sram_pool = NULL;
    _mem_psram_pool = NULL;

#ifndef RP2350_XIP_CSI_PIN
    printf("PSRAM CS pin not defined - check board file or specify board on build. unable to use PSRAM\n");
    _psram_size = 0;
#else
    // Setup PSRAM if we have it.
    _psram_size = setup_psram(RP2350_XIP_CSI_PIN);
    // qmi_hw->m[1].timing= 0x60460201;
#endif
    printf("PSRAM size: %u\n", _psram_size);
    if (!_bUseHeapPool)
    {
        printf("Got here A!\n");

        if (_psram_size > 0)
        {
            _mem_heap = tlsf_create_with_pool((void *)PSRAM_LOCATION, _psram_size, 64 * 1024 * 1024);
            _mem_psram_pool = tlsf_get_pool(_mem_heap);
        }
    }
    else
    {

        printf("Got here B!\n");

        // First, sram pool. External heap symbols from rpi pico-sdk
        extern uint32_t __heap_start;
        extern uint32_t __heap_end;
        // size
        size_t sram_size = (size_t)(&__heap_end - &__heap_start) * sizeof(uint32_t);
        // printf("point 2 start: %x, end %x, size %X %u\n", &__heap_start, &__heap_end, sram_size, sram_size);

        _mem_heap = tlsf_create_with_pool((void *)&__heap_start, sram_size, 64 * 1024 * 1024);
        _mem_sram_pool = tlsf_get_pool(_mem_heap);

        if (_psram_size > 0)
            _mem_psram_pool = tlsf_add_pool(_mem_heap, (void *)PSRAM_LOCATION, _psram_size);
    }
    _bInitalized = true;
    return true;
}

// Our allocator interface -- same signature as the stdlib malloc/free/realloc/calloc

void *rp_mem_malloc(size_t size)
{
    if (!rp_pico_alloc_init()){
        return NULL;
    }
    return tlsf_malloc(_mem_heap, size);
}

void rp_mem_free(void *ptr)
{
    if (!rp_pico_alloc_init())
        return;
    tlsf_free(_mem_heap, ptr);
}

void *rp_mem_realloc(void *ptr, size_t size)
{
    if (!rp_pico_alloc_init())
        return NULL;
    return tlsf_realloc(_mem_heap, ptr, size);
}

void *rp_mem_calloc(size_t num, size_t size)
{
    if (!rp_pico_alloc_init())
        return NULL;
    void *ptr = tlsf_malloc(_mem_heap, num * size);
    if (ptr)
        memset(ptr, 0, num * size);
    return ptr;
}

static bool max_free_walker(void *ptr, size_t size, int used, void *user)
{
    size_t *max_size = (size_t *)user;
    if (!used && *max_size < size)
    {
        *max_size = size;
    }
    return true;
}
size_t rp_mem_max_free_size(void)
{
    if (!rp_pico_alloc_init())
        return 0;
    size_t max_free = 0;
    tlsf_walk_pool(_mem_sram_pool, max_free_walker, &max_free);
    if (_mem_psram_pool)
        tlsf_walk_pool(_mem_psram_pool, max_free_walker, &max_free);

    return max_free;
}

// Wrappers for the standard malloc/free/realloc/calloc routines - set the wrapper functions
// in the cmake file ...
#if defined(RP_PICO_ALLOC_WRAP)
void *__wrap_malloc(size_t size)
{
    return rp_mem_malloc(size);
}
void __wrap_free(void *ptr)
{
    rp_mem_free(ptr);
}
void *__wrap_realloc(void *ptr, size_t size)
{
    return rp_mem_realloc(ptr, size);
}
void *__wrap_calloc(size_t num, size_t size)
{
    return rp_mem_calloc(num, size);
}
#endif