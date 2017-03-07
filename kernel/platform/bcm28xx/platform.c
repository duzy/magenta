// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#include <reg.h>
#include <err.h>
#include <debug.h>
#include <trace.h>

#include <dev/uart.h>
#include <arch.h>
#include <lk/init.h>
#include <kernel/vm.h>
#include <kernel/spinlock.h>
#include <dev/display.h>
#include <dev/hw_rng.h>

#include <platform.h>
#include <arch/arm64/platform.h>
#include <dev/interrupt/bcm28xx-intc.h>
#include <platform/bcm28xx.h>
#include <platform/videocore.h>

#include <target.h>

#include <arch/arm64.h>
#include <arch/arm64/mmu.h>

#include <magenta/bootdata.h>
#include <mdi/mdi.h>
#include <mdi/mdi-defs.h>
#include <pdev/pdev.h>

static void* ramdisk_base;
static size_t ramdisk_size;

static mdi_node_ref_t  cpu_map = {0};

/* initial memory mappings. parsed by start.S */
struct mmu_initial_mapping mmu_initial_mappings[] = {
 /* 1GB of sdram space */
 {
     .phys = SDRAM_BASE,
     .virt = KERNEL_BASE,
     .size = MEMORY_APERTURE_SIZE,
     .flags = 0,
     .name = "memory"
 },

 /* peripherals */
 {
     .phys = BCM_PERIPH_BASE_PHYS,
     .virt = BCM_PERIPH_BASE_VIRT,
     .size = BCM_PERIPH_SIZE,
     .flags = MMU_INITIAL_MAPPING_FLAG_DEVICE,
     .name = "bcm peripherals"
 },

 /* null entry to terminate the list */
 {}
};

#define DEBUG_UART 1

extern void arm_reset(void);

static pmm_arena_info_t arena = {
    .name = "sdram",
    .base = SDRAM_BASE,
    .size = MEMSIZE,
    .flags = PMM_ARENA_FLAG_KMAP,
};

void platform_init_mmu_mappings(void)
{
}

void* platform_get_ramdisk(size_t *size) {
    if (ramdisk_base) {
        *size = ramdisk_size;
        return ramdisk_base;
    } else {
        *size = 0;
        return NULL;
    }
}

static void platform_mdi_init(void) {
    mdi_node_ref_t  root;

    // Look for MDI data in ramdisk bootdata
    size_t offset = 0;
    while (offset < ramdisk_size) {
        bootdata_t* header = (ramdisk_base + offset);

        if (header->magic != BOOTDATA_MAGIC) {
            panic("bad magic in bootdata header\n");
        }
        if (header->type == BOOTDATA_TYPE_MDI) {
            break;
        }
        offset += BOOTDATA_ALIGN(sizeof(*header) + header->insize);
    }
    if (offset >= ramdisk_size) {
        panic("No MDI found in ramdisk\n");
    }

    if (mdi_init(ramdisk_base + offset, ramdisk_size - offset, &root) != NO_ERROR) {
        panic("mdi_init failed\n");
    }

    // search top level nodes for CPU info and kernel drivers
    mdi_node_ref_t  child;
    mdi_each_child(&root, &child) {
        mdi_id_t id = mdi_id(&child);

        if (id == MDI_CPU_MAP) {
            cpu_map = child;
        } else if (id == MDI_KERNEL_DRIVERS) {
            pdev_init(&child);
        }
    }
}

void platform_early_init(void)
{
    uart_init_early();

    read_device_tree(&ramdisk_base, &ramdisk_size, NULL);

    platform_mdi_init();

    intc_init();

    /* add the main memory arena */
    pmm_add_arena(&arena);

    /* reserve the first 64k of ram, which should be holding the fdt */
    struct list_node list = LIST_INITIAL_VALUE(list);
    pmm_alloc_range(MEMBASE, 0x80000 / PAGE_SIZE, &list);

    platform_preserve_ramdisk();
}

void platform_init(void)
{
    uart_init();
#if WITH_SMP
    /* TODO - number of cpus (and topology) should be parsed from device index or command line */

#if BCM2837

    uintptr_t sec_entry = (uintptr_t)(&arm_reset - KERNEL_ASPACE_BASE);
    unsigned long long *spin_table = (void *)(KERNEL_ASPACE_BASE + 0xd8);

    // 1 cluster with SMP_MAX_CPUS cpus
    uint cluster_cpus[] = { SMP_MAX_CPUS };
    arch_init_cpu_map(countof(cluster_cpus), cluster_cpus);

    for (uint i = 1; i < SMP_MAX_CPUS; i++) {

        arm64_set_secondary_sp(0, i, pmm_alloc_kpages(ARCH_DEFAULT_STACK_SIZE / PAGE_SIZE , NULL, NULL));

        spin_table[i] = sec_entry;
        __asm__ __volatile__ ("" : : : "memory");
        arch_clean_cache_range(0xffff000000000000,256);     // clean out all the VC bootstrap area
        __asm__ __volatile__("sev");                        //  where the entry vectors live.
    }
#else
    /* start the other cpus */
    uintptr_t sec_entry = (uintptr_t)&arm_reset;
    sec_entry -= (KERNEL_BASE - MEMBASE);
    for (uint i = 1; i <= SMP_MAX_CPUS; i++) {
        *REG32(ARM_LOCAL_BASE + 0x8c + 0x10 * i) = sec_entry;
    }
#endif
#endif
}

void target_init(void)
{

}

void platform_dputs(const char* str, size_t len)
{
    while (len-- > 0) {
        char c = *str++;
        if (c == '\n') {
            uart_putc(DEBUG_UART, '\r');
        }
        uart_putc(DEBUG_UART, c);
    }
}

int platform_dgetc(char *c, bool wait)
{
    int ret = uart_getc(DEBUG_UART, wait);
    if (ret == -1)
        return -1;
    *c = ret;
    return 0;
}

/* Default implementation of panic time getc/putc.
 * Just calls through to the underlying dputc/dgetc implementation
 * unless the platform overrides it.
 */
__WEAK void platform_pputc(char c)
{
    return platform_dputc(c);
}

__WEAK int platform_pgetc(char *c, bool wait)
{
    return platform_dgetc(c, wait);
}

/* stub out the hardware rng entropy generator, which doesn't exist on this platform */
size_t hw_rng_get_entropy(void* buf, size_t len, bool block) {
    return 0;
}

/* no built in framebuffer */
status_t display_get_info(struct display_info *info) {
    return ERR_NOT_FOUND;
}
