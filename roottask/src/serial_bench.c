/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(D61_BSD)
 */

#include <rumprun/init_data.h>
#include <platsupport/plat/pit.h>
#include <platsupport/io.h>
#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/plat/timer.h>
#include <sel4platsupport/arch/io.h>
#include <sel4utils/benchmark_track.h>
#include "common.h"
#include <sel4/benchmark_track_types.h>
/* This file is for dumping benchmark results over serial to an external receiver */

void __arch_putchar(char);
char __arch_getchar(void);
/* Print out a summary of what has been tracked */
#ifdef CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER
static inline void
seL4_BenchmarkTrackDumpSummary_pri(benchmark_track_kernel_entry_t *logBuffer, uint32_t logSize)
{
    uint32_t index = 0;
    uint32_t syscall_entries[8];
    uint32_t interrupt_entries = 0;
    uint32_t userlevelfault_entries = 0;
    uint32_t vmfault_entries = 0;
    uint64_t syscall_time[8];
    uint32_t cap_entries[32];
    uint64_t cap_time[32];
    uint64_t interrupt_time = 0;
    uint64_t userlevelfault_time = 0;
    uint64_t vmfault_time = 0;
    uint64_t ksTotalKernelTime =0;
    uint64_t ksOtherKernelTime = 0;

    memset((void *)syscall_entries, 0, 32);
    memset((void *)syscall_time, 0, 64);
    memset((void *)cap_entries, 0, 128);
    memset((void *)cap_time, 0, 256);
    /* Default driver to use for output now is serial.
     * Change this to use other drivers than serial, i.e ethernet
     */
    while (logBuffer[index].start_time != 0 && index < logSize) {
        if (logBuffer[index].entry.path == Entry_Interrupt) {
            ksOtherKernelTime += logBuffer[index].duration;
        } else {
            ksTotalKernelTime += logBuffer[index].duration;
        }
        if (logBuffer[index].entry.path == Entry_Syscall) {
            int syscall_no = logBuffer[index].entry.syscall_no;
            if (syscall_no == 7 ) {
                cap_entries[logBuffer[index].entry.cap_type]++;
                cap_time[logBuffer[index].entry.cap_type]+= logBuffer[index].duration;
                if (logBuffer[index].entry.cap_type == 2) {
                    printf("invoc: %d\n", logBuffer[index].entry.invocation_tag);
                }

            }

            syscall_entries[syscall_no]++;
            syscall_time[syscall_no]+= logBuffer[index].duration;

        } else if (logBuffer[index].entry.path == Entry_Interrupt) {
            interrupt_entries++;
            interrupt_time+= logBuffer[index].duration;

        } else if (logBuffer[index].entry.path == Entry_UserLevelFault) {
            userlevelfault_entries++;
            userlevelfault_time+= logBuffer[index].duration;

        } else if (logBuffer[index].entry.path == Entry_VMFault) {
            vmfault_entries++;
            vmfault_time+= logBuffer[index].duration;

        }
        index++;
    }
    printf("kt: %llx\n ot: %llx\n", ksTotalKernelTime, ksOtherKernelTime);

    for (int i = 0; i < 8; i++) {
        printf("sc: %d i: %d c: %llx\n", i, syscall_entries[i], syscall_time[i]);
    }
    for (int i = 0; i < 32; i++) {
        printf("scc: %d i: %d c: %llx\n", i, cap_entries[i], cap_time[i]);
    }
    printf("int: %d c: %llx\n", interrupt_entries, interrupt_time);
    printf("ulf: %d c: %llx\n", userlevelfault_entries, userlevelfault_time);
    printf("vmf: %d c: %llx\n", vmfault_entries, vmfault_time);
}
#endif // CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER
uint64_t cpucount;
uint64_t cpucount2;

void serial_interrupt(void) {
    vka_object_t serial_notification;
    int error = vka_alloc_notification(&env.vka, &serial_notification);
    if (error != 0) {
        ZF_LOGF("Failed to allocate notification object");
    }


    error = seL4_IRQHandler_SetNotification(env.serial_irq.capPtr, serial_notification.cptr);
    if (error != 0) {
        ZF_LOGF("Failed to do the thing2\n");
    }
    printf("a:%c\n", __arch_getchar());
    char reset_buffer[] = "reset";
    int pos = 0;
    ps_io_port_ops_t ops;
    sel4platsupport_get_io_port_ops(&ops, &env.simple);
    while (true) {
        seL4_Word sender_badge;
        int c;
        seL4_Recv(serial_notification.cptr, &sender_badge);
        seL4_IRQHandler_Ack(env.serial_irq.capPtr);
        while(true) {
            uint32_t hi1, lo1;
            /* get next character */
            c = __arch_getchar();
            if (c == -1) {
                break;
            }

            /* Reset machine if "reset" received over serial */
            if (c == reset_buffer[pos]) {
                pos++;
                if (pos == 5) {
                    ps_io_port_out(&ops, 0x64, 1, 0xFE);
                }
            } else {
                pos = 0;
            }

            /* If a character start benchmarking (reset everything) */
            if (c == 'a') {

#ifdef CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER
                /* Reset log buffer */
                if (log_buffer == NULL) {
                    ZF_LOGF("A user-level buffer has to be set before resetting benchmark.\
                            Use seL4_BenchmarkSetLogBuffer\n");
                }
                int error = seL4_BenchmarkResetLog();
                if (error) {
                    ZF_LOGF("Could not reset log");
                }
#endif /* CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER */
                /* Record tsc count */
                __asm__ __volatile__ ( "rdtsc" : "=a" (lo1), "=d" (hi1));
                cpucount = ((uint64_t) hi1) << 32llu | (uint64_t) lo1;
                ccount = 0;
            }

            /* Stop benchmarking when 'b' character */
            if (c == 'b') {
                /* Record finish time */
                __asm__ __volatile__ ( "rdtsc" : "=a" (lo1), "=d" (hi1));
                cpucount2 = ((uint64_t) hi1) << 32llu | (uint64_t) lo1;

#ifdef CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER
                /* Stop recording kernel entries */
                logIndexFinalized = seL4_BenchmarkFinalizeLog();
#endif /* CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER */
                /* Print total cycles and idle cycles to serial */
                printf("tot: %llx\n idle: %llx\n", cpucount2 - cpucount, ccount);
            }
            if (c == 'c') {
#ifdef CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER
                benchmark_track_kernel_entry_t *ksLog = (benchmark_track_kernel_entry_t *) log_buffer;
                printf("dumping log: %d, %d %d\n", logIndexFinalized, sizeof(benchmark_track_kernel_entry_t), sizeof(kernel_entry_t));
                seL4_BenchmarkTrackDumpSummary_pri(ksLog, logIndexFinalized);//ksLogIndexFinalized);
#endif /* CONFIG_BENCHMARK_USE_KERNEL_LOG_BUFFER */
            }

            /* print char received */
            if (c == '\r') {
                __arch_putchar('\n');
            } else {
                __arch_putchar(c);
            }
        }

    }

}
