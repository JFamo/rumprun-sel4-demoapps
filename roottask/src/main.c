/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */
#include <autoconf.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/serial.h>
#include <sel4platsupport/arch/io.h>
#include <sel4debug/register_dump.h>
#include <cpio/cpio.h>
#include <sel4/bootinfo.h>
#include <sel4utils/stack.h>
#include <sel4utils/util.h>
#include <vka/object.h>
#include <platsupport/io.h>
#include <vka/object_capops.h>

#include <vspace/vspace.h>
#include "common.h"
#include <rumprun/init_data.h>

#define RUMP_UNTYPED_MEMORY (BIT(25))
/* Number of untypeds to try and use to allocate the driver memory. */
#define RUMP_NUM_UNTYPEDS 16


/* ammount of dev_ram memory to give to Rump kernel */
#define RUMP_DEV_RAM_MEMORY MiB_TO_BYTES(CONFIG_RUMPRUN_MEMORY_MiB)
/* Number of untypeds to try and use to allocate the driver memory.
 * if we cannot get 32mb with 16 untypeds then something is probably wrong */
#define RUMP_NUM_DEV_RAM_UNTYPEDS 20

/* dimensions of virtual memory for the allocator to use */
#define ALLOCATOR_VIRTUAL_POOL_SIZE (BIT(seL4_PageBits) * 1000)

/* static memory for the allocator to bootstrap with */
#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 20)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

/* static memory for virtual memory bootstrapping */
static sel4utils_alloc_data_t data;


/* environment encapsulating allocation interfaces etc */
struct env env;

extern vspace_t *muslc_this_vspace;
extern reservation_t muslc_brk_reservation;
extern void *muslc_brk_reservation_start;
extern char _cpio_archive[];

/* initialise our runtime environment */
static void
init_env(env_t env)
{
    allocman_t *allocman;
    reservation_t virtual_reservation;
    int error;

    /* create an allocator */
    allocman = bootstrap_use_current_simple(&env->simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    ZF_LOGF_IF(allocman == NULL, "Failed to create allocman");

    /* create a vka (interface for interacting with the underlying allocator) */
    allocman_make_vka(&env->vka, allocman);

    /* create a vspace (virtual memory management interface). We pass
     * boot info not because it will use capabilities from it, but so
     * it knows the address and will add it as a reserved region */
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&env->vspace,
                                                           &data, simple_get_pd(&env->simple),
                                                           &env->vka, platsupport_get_bootinfo());
    ZF_LOGF_IF(error, "Failed to bootstrap vspace");

    sel4utils_res_t muslc_brk_reservation_memory;
    error = sel4utils_reserve_range_no_alloc(&env->vspace, &muslc_brk_reservation_memory, 1048576, seL4_AllRights, 1, &muslc_brk_reservation_start);
    ZF_LOGF_IF(error, "Failed to reserve_range");

    muslc_this_vspace = &env->vspace;
    muslc_brk_reservation.res = &muslc_brk_reservation_memory;

    /* fill the allocator with virtual memory */
    void *vaddr;
    virtual_reservation = vspace_reserve_range(&env->vspace,
                                               ALLOCATOR_VIRTUAL_POOL_SIZE, seL4_AllRights, 1, &vaddr);
    ZF_LOGF_IF(virtual_reservation.res == 0, "Failed to provide virtual memory for allocator");

    bootstrap_configure_virtual_pool(allocman, vaddr,
                                     ALLOCATOR_VIRTUAL_POOL_SIZE, simple_get_pd(&env->simple));
}

/* Allocate untypeds till either a certain number of bytes is allocated
 * or a certain number of untyped objects */
static unsigned int
allocate_untypeds(vka_object_t *untypeds, size_t bytes, unsigned int max_untypeds, bool can_use_dev)
{
    unsigned int num_untypeds = 0;
    size_t allocated = 0;

    /* try to allocate as many of each possible untyped size as possible */
    for (uint8_t size_bits = seL4_MaxUntypedBits; size_bits > PAGE_BITS_4K; size_bits--) {
        /* keep allocating until we run out, or if allocating would
         * cause us to allocate too much memory*/
        while (num_untypeds < max_untypeds &&
                allocated + BIT(size_bits) <= bytes &&
                vka_alloc_object_at_maybe_dev(&env.vka, seL4_UntypedObject, size_bits, VKA_NO_PADDR,
                                              can_use_dev, &untypeds[num_untypeds]) == 0) {
            allocated += BIT(size_bits);
            num_untypeds++;
        }
    }
    return num_untypeds;
}

static seL4_CPtr
move_init_cap_to_process(sel4utils_process_t *process, seL4_CPtr cap)
{
    seL4_CPtr copied_cap;
    cspacepath_t path;

    vka_cspace_make_path(&env.vka, cap, &path);

    copied_cap = sel4utils_move_cap_to_process(process, path, NULL);
    ZF_LOGF_IF(copied_cap == 0, "Failed to move cap to process");

    return copied_cap;
}
/* copy untyped caps into a processes cspace, return the cap range they can be found in */
static int
copy_untypeds_to_process(sel4utils_process_t *process, rump_process_data_t *proc_data)
{
    seL4_SlotRegion range = {0};
    int total_caps = proc_data->num_untypeds_devram + proc_data->num_untypeds + proc_data->num_untypeds_dev;
    for (int i = 0; i < total_caps; i++) {
        seL4_CPtr slot = sel4utils_copy_cap_to_process(process, &env.vka, proc_data->untypeds[i].cptr);
        /* ALLOCMAN_UT_KERNEL, ALLOCMAN_UT_DEV, ALLOCMAN_UT_DEV_MEM */
        uint8_t untyped_is_device;
        if (i < proc_data->num_untypeds_devram) {
            untyped_is_device = ALLOCMAN_UT_DEV_MEM;
        } else if (i < proc_data->num_untypeds_devram + proc_data->num_untypeds) {
            untyped_is_device = ALLOCMAN_UT_KERNEL;
        } else {
            untyped_is_device = ALLOCMAN_UT_DEV;
        }
        proc_data->init->untyped_list[i].size_bits = proc_data->untypeds[i].size_bits;
        proc_data->init->untyped_list[i].is_device = untyped_is_device;
        proc_data->init->untyped_list[i].paddr = vka_utspace_paddr(&env.vka, proc_data->untypeds[i].ut, seL4_UntypedObject, proc_data->untypeds[i].size_bits);
        /* set up the cap range */
        if (i == 0) {
            range.start = slot;
        }
        range.end = slot;
    }
    range.end++;
    ZF_LOGF_IF((range.end - range.start) != total_caps, "Invalid number of caps");
    proc_data->init->untypeds = range;
    return 0;
}


/* map the init data into the process, and send the address via ipc */
static void *
send_init_data(env_t env, seL4_CPtr endpoint, sel4utils_process_t *process)
{
    /* map the cap into remote vspace */
    void *remote_vaddr = vspace_map_pages(&process->vspace, env->init_frame_cap_copy, NULL, seL4_AllRights, 2, PAGE_BITS_4K, 1);
    assert(remote_vaddr != 0);

    /* now send a message telling the process what address the data is at */
    seL4_MessageInfo_t info = seL4_MessageInfo_new(seL4_Fault_NullFault, 0, 0, 1);
    seL4_SetMR(0, (seL4_Word) remote_vaddr);
    seL4_Send(endpoint, info);

    return remote_vaddr;
}

int alloc_untypeds(rump_process_data_t *proc_data)
{

    // Allocate DEV_MEM memory.
    bool can_be_dev = true;
    proc_data->num_untypeds_devram = allocate_untypeds(proc_data->untypeds, RUMP_DEV_RAM_MEMORY, RUMP_NUM_DEV_RAM_UNTYPEDS, can_be_dev);
    int current_index = proc_data->num_untypeds_devram;
    can_be_dev = false;
    proc_data->num_untypeds = allocate_untypeds(proc_data->untypeds + current_index, RUMP_UNTYPED_MEMORY, RUMP_NUM_UNTYPEDS, can_be_dev);
    current_index += proc_data->num_untypeds;
    return 0;
}

int alloc_devices(rump_process_data_t *proc_data) {
    proc_data->num_untypeds_dev = 0;
    int num_untypeds = 0;
    int current_index = proc_data->num_untypeds_devram + proc_data->num_untypeds;
    for (int i = 0; i < num_devices; i++) {
        if (strcmp(CONFIG_RUMPRUN_NETWORK_IFNAME, devices[i].name) == 0) {
            for (int j = 0; j < devices[i].num_mmios; j++) {
                int error = vka_alloc_object_at_maybe_dev(&env.vka, seL4_UntypedObject, devices[i].mmios[j].size_bits, devices[i].mmios[j].paddr,
                                                        true, proc_data->untypeds + current_index + num_untypeds);
                ZF_LOGF_IF(error, "Could not allocate untyped");
                num_untypeds++;
            }
            proc_data->init->interrupt_list[0].bus = devices[i].pci.bus;
            proc_data->init->interrupt_list[0].dev = devices[i].pci.dev;
            proc_data->init->interrupt_list[0].function = devices[i].pci.function;
            ps_irq_t irq;
            irq.type = PS_IOAPIC;
            int irq_num = devices[i].irq_num;
            irq.ioapic.vector = irq_num;
            irq.ioapic.ioapic = 0;
            if (irq_num >= 16) {
                irq.ioapic.level = 1;
                irq.ioapic.polarity = 1;
            } else {
                irq.ioapic.level = 0;
                irq.ioapic.polarity = 0;
            }
            proc_data->init->interrupt_list[0].irq = irq;
        }
    }
    proc_data->num_untypeds_dev = num_untypeds;
    return 0;
}


/* Boot Rumprun process. */
int
run_rr(void)
{
    /* elf region data */
    int num_elf_regions;
    sel4utils_elf_region_t elf_regions[MAX_REGIONS];

    /* Unpack elf file from cpio */
    struct cpio_info info2;
    cpio_info(_cpio_archive, &info2);
    const char* bin_name;
    for (int i = 0; i < info2.file_count; i++) {
        unsigned long size;
        cpio_get_entry(_cpio_archive, i, &bin_name, &size);
        ZF_LOGV("name %d: %s\n", i, bin_name);
    }

    /* parse elf region data about the image to pass to the app */
    num_elf_regions = sel4utils_elf_num_regions(bin_name);
    ZF_LOGF_IF(num_elf_regions >= MAX_REGIONS, "Invalid num elf regions");

    sel4utils_elf_reserve(NULL, bin_name, elf_regions);
    /* copy the region list for the process to clone itself */
    memcpy(env.rump_process.init->elf_regions, elf_regions, sizeof(sel4utils_elf_region_t) * num_elf_regions);
    env.rump_process.init->num_elf_regions = num_elf_regions;

    /* setup init priority.  Reduce by 2 so that we can have higher priority serial thread
        for benchmarking */
    env.rump_process.init->priority = seL4_MaxPrio - 2;
    env.rump_process.init->rumprun_memory_size = RUMP_DEV_RAM_MEMORY;

    UNUSED int error;
    sel4utils_process_t process;
    char* a = RUMPCONFIG;
    printf("%zd %s\n", sizeof(RUMPCONFIG), a);
    /* Test intro banner. */
    printf("  starting app\n");

    sel4utils_process_config_t config = process_config_default_simple(&env.simple, bin_name, env.rump_process.init->priority);
    config = process_config_mcp(config, env.rump_process.init->priority);
    /* Set up rumprun process */
    error = sel4utils_configure_process_custom(&process, &env.vka, &env.vspace, config);
    ZF_LOGF_IF(error, "Failed to configure process");

    /* set up init_data process info */
    env.rump_process.init->stack_pages = CONFIG_SEL4UTILS_STACK_SIZE / PAGE_SIZE_4K;
    env.rump_process.init->stack = process.thread.stack_top - CONFIG_SEL4UTILS_STACK_SIZE;

    env.rump_process.init->domain = sel4utils_copy_cap_to_process(&process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapDomain));
    env.rump_process.init->asid_ctrl = sel4utils_copy_cap_to_process(&process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapASIDControl));

#ifdef CONFIG_IOMMU
    env.rump_process.init->io_space = sel4utils_copy_cap_to_process(&process, &env.vka, simple_get_init_cap(&env.simple, seL4_CapIOSpace));
#endif /* CONFIG_IOMMU */
#ifdef CONFIG_ARM_SMMU
    env.rump_process.init->io_space_caps = arch_copy_iospace_caps_to_process(&process, &env);
#endif
    env.rump_process.init->irq_control = move_init_cap_to_process(&process, simple_get_init_cap(&env.simple, seL4_CapIRQControl ));
    env.rump_process.init->sched_control = sel4utils_copy_cap_to_process(&process, &env.vka, simple_get_sched_ctrl(&env.simple, 0));
    /* setup data about untypeds */

    alloc_untypeds(&env.rump_process);
    alloc_devices(&env.rump_process);
    error = sel4utils_copy_timer_caps_to_process(&env.rump_process.init->to, &env.timer_objects, &env.vka, &process);
    ZF_LOGF_IF(error, "sel4utils_copy_timer_caps_to_process failed");

    arch_copy_IOPort_cap(env.rump_process.init, &env, &process);
    copy_untypeds_to_process(&process, &env.rump_process);

    env.rump_process.init->tsc_freq = simple_get_arch_info(&env.simple);
    /* copy the fault endpoint - we wait on the endpoint for a message
     * or a fault to see when the process finishes */
    seL4_CPtr endpoint = sel4utils_copy_cap_to_process(&process, &env.vka, process.fault_endpoint.cptr);
    /* WARNING: DO NOT COPY MORE CAPS TO THE PROCESS BEYOND THIS POINT,
     * AS THE SLOTS WILL BE CONSIDERED FREE AND OVERRIDDEN BY THE PROCESS. */
    /* set up free slot range */
    env.rump_process.init->cspace_size_bits = CONFIG_SEL4UTILS_CSPACE_SIZE_BITS;
    env.rump_process.init->free_slots.start = endpoint + 1;
    env.rump_process.init->free_slots.end = BIT(CONFIG_SEL4UTILS_CSPACE_SIZE_BITS);
    assert(env.rump_process.init->free_slots.start < env.rump_process.init->free_slots.end);
    strncpy(env.rump_process.init->cmdline, RUMPCONFIG, RUMP_CONFIG_MAX);
    NAME_THREAD(process.thread.tcb.cptr, bin_name);

    /* set up args for the process */
    char endpoint_string[WORD_STRING_SIZE];
    char *argv[] = {(char *)bin_name, endpoint_string};
    snprintf(endpoint_string, WORD_STRING_SIZE, "%lu", (unsigned long)endpoint);
    /* spawn the process */
    error = sel4utils_spawn_process_v(&process, &env.vka, &env.vspace,
                                      ARRAY_SIZE(argv), argv, 1);
    assert(error == 0);
    printf("process spawned\n");
    /* send env.init_data to the new process */
    send_init_data(&env, process.fault_endpoint.cptr, &process);
    vka_object_t reply_slot;
    vka_alloc_reply(&env.vka, &reply_slot);
    /* wait on it to finish or fault, report result */
    seL4_MessageInfo_t info = api_recv(process.fault_endpoint.cptr, NULL, reply_slot.cptr);

    int result = seL4_GetMR(0);
    if (seL4_MessageInfo_get_label(info) != seL4_Fault_NullFault) {
        sel4utils_print_fault_message(info, "rumprun");
        sel4debug_dump_registers(process.thread.tcb.cptr);
        result = -1;
    }
    return result;
}


static int
create_thread_handler(sel4utils_thread_entry_fn handler, int priority, int UNUSED timeslice)
{
    sel4utils_thread_config_t thread_config = thread_config_default(&env.simple,
        seL4_CapInitThreadCNode, seL4_NilData, seL4_CapNull, priority);
    if(config_set(CONFIG_KERNEL_RT)) {
        thread_config.sched_params = sched_params_periodic(thread_config.sched_params, &env.simple,
                        0, CONFIG_BOOT_THREAD_TIME_SLICE * US_IN_MS,
                        timeslice * CONFIG_BOOT_THREAD_TIME_SLICE * (US_IN_MS /100),
                        0, 0);
    }
    sel4utils_thread_t new_thread;
    int error = sel4utils_configure_thread_config(&env.vka, &env.vspace, &env.vspace, thread_config, &new_thread);
    if (error) {
        return error;
    }
    error = sel4utils_start_thread(&new_thread, handler, NULL, NULL,
                                   1);
    return error;
}

#ifdef CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER
void *log_buffer;
#endif

void *main_continued(void *arg UNUSED)
{
    /* Print welcome banner. */
    printf("\n");
    printf("Rump on seL4\n");
    printf("============\n");
    printf("\n");

#ifdef CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER
    /* Create 1MB page to use for benchmarking and give to kernel */
    log_buffer = vspace_new_pages(&env.vspace, seL4_AllRights, 1, seL4_LargePageBits);
    ZF_LOGF_IF(log_buffer == NULL, "Could not map 1MB page");
    seL4_CPtr buffer_cap = vspace_get_cap(&env.vspace, log_buffer);
    ZF_LOGF_IF(buffer_cap == NULL, "Could not get cap");
    int res_buf = seL4_BenchmarkSetLogBuffer(buffer_cap);
    ZF_LOGF_IFERR(res_buf, "Could not set log buffer");
#endif //CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER

    /* create a frame that will act as the init data, we can then map
     * into target processes */
    env.rump_process.init = (init_data_t *) vspace_new_pages(&env.vspace, seL4_AllRights, INIT_DATA_NUM_FRAMES, PAGE_BITS_4K);
    ZF_LOGF_IF(env.rump_process.init == NULL, "Could not create init_data frame");

    for (int i = 0; i < INIT_DATA_NUM_FRAMES; i++) {
        cspacepath_t src, dest;
        vka_cspace_make_path(&env.vka, vspace_get_cap(&env.vspace, (((char *)env.rump_process.init) + i * PAGE_SIZE_4K)), &src);

        int error = vka_cspace_alloc(&env.vka, &env.init_frame_cap_copy[i]);
        ZF_LOGF_IF(error, "Failed to alloc cap");
        vka_cspace_make_path(&env.vka, env.init_frame_cap_copy[i], &dest);
        error = vka_cnode_copy(&dest, &src, seL4_AllRights);
        ZF_LOGF_IF(error, "Failed to copy cap");
    }


    /* get the caps we need to set up a timer and serial interrupts */
    sel4platsupport_init_default_timer_caps(&env.vka, &env.vspace, &env.simple, &env.timer_objects);
    sel4platsupport_init_default_serial_caps(&env.vka, &env.vspace, &env.simple, &env.serial_objects);


    int err = seL4_TCB_SetPriority(simple_init_cap(&env.simple, seL4_CapInitThreadTCB), seL4_MaxPrio - 1);
    ZF_LOGF_IFERR(err, "seL4_TCB_SetPriority thread failed");
    /* Create serial thread */
    err = create_thread_handler(serial_interrupt, seL4_MaxPrio, 100);
    ZF_LOGF_IF(err, "Could not create serial thread");
    /* Create idle thread */
    err = create_thread_handler(count_idle, 0, 100);
    ZF_LOGF_IF(err, "Could not create idle thread");

#ifdef CONFIG_USE_HOG_THREAD
#ifndef CONFIG_HOG_BANDWIDTH
#define CONFIG_HOG_BANDWIDTH 100
#endif

    /* Create hog thread */
    err = create_thread_handler(hog_thread, seL4_MaxPrio - 1, CONFIG_HOG_BANDWIDTH);
    if (err) {
        ZF_LOGF("Could not create hog thread thread");
    }
#endif /* CONFIG_USE_HOG_THREAD */

    /* now set up and run rumprun */
    run_rr();

    return NULL;
}

/* entry point of root task */
int main(void)
{
    seL4_BootInfo *info;
    info = platsupport_get_bootinfo();

    NAME_THREAD(seL4_CapInitThreadTCB, "roottask");

    /* Check rump kernel config string length */
    compile_time_assert(rump_config_is_too_long, sizeof(RUMPCONFIG) < RUMP_CONFIG_MAX);
    /* We provide two pages to transfer init data */
    compile_time_assert(init_data_fits, sizeof(init_data_t) < (PAGE_SIZE_4K * INIT_DATA_NUM_FRAMES));
    /* initialise libsel4simple, which abstracts away which kernel version
     * we are running on */
    simple_default_init_bootinfo(&env.simple, info);

    /* initialise the environment - allocator, cspace manager, vspace manager, timer */
    init_env(&env);

    /* enable serial driver */
    platsupport_serial_setup_simple(NULL, &env.simple, &env.vka);

    /* switch to a bigger, safer stack with a guard page
     * before starting Rumprun, resume on main_continued() */
    ZF_LOGI("Switching to a safer, bigger stack... ");
    fflush(stdout);
    void *res;
    sel4utils_run_on_stack(&env.vspace, main_continued, NULL, &res);

    return 0;

}
