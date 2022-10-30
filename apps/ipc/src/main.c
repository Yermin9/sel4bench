/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
/* This is very much a work in progress IPC benchmarking set. Goal is
   to eventually use this to replace the rest of the random benchmarking
   happening in this app with just what we need */

#include <autoconf.h>
#include <sel4benchipc/gen_config.h>
#include <allocman/vka.h>
#include <allocman/bootstrap.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <sel4/sel4.h>
#include <sel4bench/sel4bench.h>
#include <sel4utils/process.h>
#include <string.h>
#include <utils/util.h>
#include <vka/vka.h>
#include <sel4runtime.h>
#include <muslcsys/vsyscall.h>
#include <utils/attribute.h>

#include <benchmark.h>
#include <ipc.h>

/* arch/ipc.h requires these defines */
#define NOPS ""

#include <arch/ipc.h>

#define NUM_ARGS 3
/* Ensure that enough warmups are performed to prevent the FPU from
 * being restored. */
#ifdef CONFIG_FPU_MAX_RESTORES_SINCE_SWITCH
#define WARMUPS (RUNS + CONFIG_FPU_MAX_RESTORES_SINCE_SWITCH)
#else
#define WARMUPS RUNS
#endif
#define OVERHEAD_RETRIES 4

#ifndef CONFIG_CYCLE_COUNT

#define GENERIC_COUNTER_MASK (BIT(0))
#undef READ_COUNTER_BEFORE
#undef READ_COUNTER_AFTER
#define READ_COUNTER_BEFORE(x) sel4bench_get_counters(GENERIC_COUNTER_MASK, &x);
#define READ_COUNTER_AFTER(x) sel4bench_get_counters(GENERIC_COUNTER_MASK, &x)
#endif

typedef struct helper_thread {
    sel4utils_process_t process;
    seL4_CPtr ep;
    seL4_CPtr result_ep;
    char *argv[NUM_ARGS];
    char argv_strings[NUM_ARGS][WORD_STRING_SIZE];
} helper_thread_t;

void abort(void)
{
    benchmark_finished(EXIT_FAILURE);
}

static void timing_init(void)
{
    sel4bench_init();
#ifdef CONFIG_GENERIC_COUNTER
    event_id_t event = GENERIC_EVENTS[CONFIG_GENERIC_COUNTER_ID];
    sel4bench_set_count_event(0, event);
    sel4bench_reset_counters();
    sel4bench_start_counters(GENERIC_COUNTER_MASK);
#endif
#ifdef CONFIG_PLATFORM_COUNTER
    sel4bench_set_count_event(0, CONFIG_PLATFORM_COUNTER_CONSTANT);
    sel4bench_reset_counters();
    sel4bench_start_counters(GENERIC_COUNTER_MASK);
#endif
}

void timing_destroy(void)
{
#ifdef CONFIG_GENERIC_COUNTER
    sel4bench_stop_counters(GENERIC_COUNTER_MASK);
    sel4bench_destroy();
#endif
}

static inline void dummy_seL4_Send(seL4_CPtr ep, seL4_MessageInfo_t tag)
{
    (void)ep;
    (void)tag;
}

static inline void dummy_seL4_Call(seL4_CPtr ep, seL4_MessageInfo_t tag)
{
    (void)ep;
    (void)tag;
}

static inline void dummy_seL4_Reply(UNUSED seL4_CPtr reply, seL4_MessageInfo_t tag)
{
    (void)tag;
}

static inline void dummy_cache_func(void) {}

#ifdef CONFIG_CLEAN_L1_ICACHE
#define CACHE_FUNC() do {                           \
    seL4_BenchmarkFlushL1Caches(seL4_ARM_CacheI);   \
} while (0)

#elif CONFIG_CLEAN_L1_DCACHE
#define CACHE_FUNC() do {                           \
    seL4_BenchmarkFlushL1Caches(seL4_ARM_CacheD);   \
} while (0)

#elif CONFIG_CLEAN_L1_CACHE
#define CACHE_FUNC() do {                           \
    seL4_BenchmarkFlushL1Caches(seL4_ARM_CacheID);  \
} while (0)

#elif CONFIG_DIRTY_L1_DCACHE
#define L1_CACHE_LINE_SIZE BIT(CONFIG_L1_CACHE_LINE_SIZE_BITS)
#define POLLUTE_ARRARY_SIZE CONFIG_L1_DCACHE_SIZE/L1_CACHE_LINE_SIZE/sizeof(int)
#define POLLUTE_RUNS 5
#define CACHE_FUNC() do {                                       \
    ALIGN(L1_CACHE_LINE_SIZE) volatile                          \
    int pollute_array[POLLUTE_ARRARY_SIZE][L1_CACHE_LINE_SIZE]; \
    for (int i = 0; i < POLLUTE_RUNS; i++) {                    \
        for (int j = 0; j < L1_CACHE_LINE_SIZE; j++) {          \
            for (int k = 0; k < POLLUTE_ARRARY_SIZE; k++) {     \
                pollute_array[k][j]++;                          \
            }                                                   \
        }                                                       \
    }                                                           \
} while (0)

#else
#define CACHE_FUNC dummy_cache_func
#endif

seL4_Word ipc_call_func(int argc, char *argv[]);
seL4_Word ipc_call_func2(int argc, char *argv[]);
seL4_Word ipc_call_10_func(int argc, char *argv[]);
seL4_Word ipc_call_10_func2(int argc, char *argv[]);
seL4_Word ipc_replyrecv_func2(int argc, char *argv[]);
seL4_Word ipc_replyrecv_func(int argc, char *argv[]);
seL4_Word ipc_replyrecv_10_func2(int argc, char *argv[]);
seL4_Word ipc_replyrecv_10_func(int argc, char *argv[]);
seL4_Word ipc_send_func(int argc, char *argv[]);
seL4_Word ipc_recv_func(int argc, char *argv[]);

static helper_func_t bench_funcs[] = {
    ipc_call_func,
    ipc_call_func2,
    ipc_call_10_func,
    ipc_call_10_func2,
    ipc_replyrecv_func2,
    ipc_replyrecv_func,
    ipc_replyrecv_10_func2,
    ipc_replyrecv_10_func,
    ipc_send_func,
    ipc_recv_func
};

#define IPC_CALL_FUNC(name, bench_func, send_func, call_func, send_start_end, length, cache_func) \
    seL4_Word name(int argc, char *argv[]) { \
    uint32_t i; \
    ccnt_t start UNUSED, end UNUSED; \
    seL4_CPtr ep = atoi(argv[0]);\
    seL4_CPtr result_ep = atoi(argv[1]);\
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, length); \
    call_func(ep, tag); \
    COMPILER_MEMORY_FENCE(); \
    for (i = 0; i < WARMUPS; i++) { \
        cache_func(); \
        READ_COUNTER_BEFORE(start); \
        bench_func(ep, tag); \
        READ_COUNTER_AFTER(end); \
    } \
    COMPILER_MEMORY_FENCE(); \
    send_result(result_ep, send_start_end); \
    send_func(ep, tag); \
    api_wait(ep, NULL);/* block so we don't run off the stack */ \
    return 0; \
}

IPC_CALL_FUNC(ipc_call_func, DO_REAL_CALL, seL4_Send, dummy_seL4_Call, end, 0, dummy_cache_func)
IPC_CALL_FUNC(ipc_call_func2, DO_REAL_CALL, dummy_seL4_Send, seL4_Call, start, 0, CACHE_FUNC)
IPC_CALL_FUNC(ipc_call_10_func, DO_REAL_CALL_10, seL4_Send, dummy_seL4_Call, end, 10, dummy_cache_func)
IPC_CALL_FUNC(ipc_call_10_func2, DO_REAL_CALL_10, dummy_seL4_Send, seL4_Call, start, 10, CACHE_FUNC)

#define IPC_REPLY_RECV_FUNC(name, bench_func, reply_func, recv_func, send_start_end, length, cache_func) \
seL4_Word name(int argc, char *argv[]) { \
    uint32_t i; \
    ccnt_t start UNUSED, end UNUSED; \
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, length); \
    seL4_CPtr ep = atoi(argv[0]);\
    seL4_CPtr result_ep = atoi(argv[1]);\
    seL4_CPtr reply = atoi(argv[2]);\
    if (config_set(CONFIG_KERNEL_MCS)) {\
        api_nbsend_recv(ep, tag, ep, NULL, reply);\
    } else {\
        recv_func(ep, NULL, reply); \
    }\
    COMPILER_MEMORY_FENCE(); \
    for (i = 0; i < WARMUPS; i++) { \
        cache_func(); \
        READ_COUNTER_BEFORE(start); \
        bench_func(ep, tag, reply); \
        READ_COUNTER_AFTER(end); \
    } \
    COMPILER_MEMORY_FENCE(); \
    reply_func(reply, tag); \
    send_result(result_ep, send_start_end); \
    api_wait(ep, NULL); /* block so we don't run off the stack */ \
    return 0; \
}

IPC_REPLY_RECV_FUNC(ipc_replyrecv_func2, DO_REAL_REPLY_RECV, api_reply, api_recv, end, 0, dummy_cache_func)
IPC_REPLY_RECV_FUNC(ipc_replyrecv_func, DO_REAL_REPLY_RECV, dummy_seL4_Reply, api_recv, start, 0, CACHE_FUNC)
IPC_REPLY_RECV_FUNC(ipc_replyrecv_10_func2, DO_REAL_REPLY_RECV_10, api_reply, api_recv, end, 10, dummy_cache_func)
IPC_REPLY_RECV_FUNC(ipc_replyrecv_10_func, DO_REAL_REPLY_RECV_10, dummy_seL4_Reply, api_recv, start, 10, CACHE_FUNC)

seL4_Word
ipc_recv_func(int argc, char *argv[])
{
    uint32_t i;
    ccnt_t start UNUSED, end UNUSED;
    seL4_CPtr ep = atoi(argv[0]);
    seL4_CPtr result_ep = atoi(argv[1]);
    UNUSED seL4_CPtr reply = atoi(argv[2]);

    COMPILER_MEMORY_FENCE();
    for (i = 0; i < WARMUPS; i++) {
        READ_COUNTER_BEFORE(start);
        DO_REAL_RECV(ep, reply);
        READ_COUNTER_AFTER(end);
    }
    COMPILER_MEMORY_FENCE();
    DO_REAL_RECV(ep, reply);
    send_result(result_ep, end);
    return 0;
}

seL4_Word ipc_send_func(int argc, char *argv[])
{
    uint32_t i;
    ccnt_t start UNUSED, end UNUSED;
    seL4_CPtr ep = atoi(argv[0]);
    seL4_CPtr result_ep = atoi(argv[1]);
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);
    COMPILER_MEMORY_FENCE();
    for (i = 0; i < WARMUPS; i++) {
        READ_COUNTER_BEFORE(start);
        DO_REAL_SEND(ep, tag);
        READ_COUNTER_AFTER(end);
    }
    COMPILER_MEMORY_FENCE();
    send_result(result_ep, start);
    DO_REAL_SEND(ep, tag);
    return 0;
}

#define MEASURE_OVERHEAD(op, dest, decls) do { \
    uint32_t i; \
    timing_init(); \
    for (i = 0; i < OVERHEAD_RETRIES; i++) { \
        uint32_t j; \
        for (j = 0; j < RUNS; j++) { \
            uint32_t k; \
            decls; \
            ccnt_t start, end; \
            COMPILER_MEMORY_FENCE(); \
            for (k = 0; k < WARMUPS; k++) { \
                READ_COUNTER_BEFORE(start); \
                op; \
                READ_COUNTER_AFTER(end); \
            } \
            COMPILER_MEMORY_FENCE(); \
            dest[j] = end - start; \
        } \
        if (results_stable(dest, RUNS)) break; \
    } \
    timing_destroy(); \
} while(0)

static void measure_overhead(ipc_results_t *results)
{
    MEASURE_OVERHEAD(DO_NOP_CALL(0, tag),
                     results->overhead_benchmarks[CALL_OVERHEAD],
                     seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0));
    MEASURE_OVERHEAD(DO_NOP_REPLY_RECV(0, tag, 0),
                     results->overhead_benchmarks[REPLY_RECV_OVERHEAD],
                     seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0));
    MEASURE_OVERHEAD(DO_NOP_SEND(0, tag),
                     results->overhead_benchmarks[SEND_OVERHEAD],
                     seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0));
    MEASURE_OVERHEAD(DO_NOP_RECV(0, 0),
                     results->overhead_benchmarks[RECV_OVERHEAD],
                     {});
    MEASURE_OVERHEAD(DO_NOP_CALL_10(0, tag10),
                     results->overhead_benchmarks[CALL_10_OVERHEAD],
                     seL4_MessageInfo_t tag10 = seL4_MessageInfo_new(0, 0, 0, 10));
    MEASURE_OVERHEAD(DO_NOP_REPLY_RECV_10(0, tag10, 0),
                     results->overhead_benchmarks[REPLY_RECV_10_OVERHEAD],
                     seL4_MessageInfo_t tag10 = seL4_MessageInfo_new(0, 0, 0, 10));
}

void run_bench(env_t *env, cspacepath_t result_ep_path, cspacepath_t ep_path,
               const benchmark_params_t *params,
               ccnt_t *ret1, ccnt_t *ret2,
               helper_thread_t *client, helper_thread_t *server,
               uint64_t threshold)
{

    timing_init();

    /* start processes */
    int error = benchmark_spawn_process(&server->process, &env->slab_vka, &env->vspace, NUM_ARGS,
                                        server->argv, 1);
    ZF_LOGF_IF(error, "Failed to spawn server\n");

    if (config_set(CONFIG_KERNEL_MCS) && params->server_fn != IPC_RECV_FUNC) {
        /* wait for server to tell us its initialised */
        seL4_Wait(ep_path.capPtr, NULL);

        if (params->passive) {
            /* convert server to passive */
            error = api_sc_unbind_object(server->process.thread.sched_context.cptr,
                                         server->process.thread.tcb.cptr);
            ZF_LOGF_IF(error, "Failed to convert server to passive");
        }
    }
    #if defined(CONFIG_KERNEL_IPCTHRESHOLDS) && defined(CONFIG_KERNEL_MCS)
    /* Set the threshold */
    if (config_set(CONFIG_KERNEL_IPCTHRESHOLDS)) {
        error = seL4_CNode_Endpoint_SetThreshold(ep_path.root, ep_path.capPtr, ep_path.capDepth, threshold);
        ZF_LOGF_IF(error, "Failed to set endpoint threshold\n");
    }
    #endif

    api_sched_ctrl_configure(simple_get_sched_ctrl(&env->simple, 0), client->process.thread.sched_context.cptr,
                            800 * US_IN_S, 800 * US_IN_S,
                                5, 0);
 
    if (threshold==0 || !config_set(CONFIG_KERNEL_IPCTHRESHOLDS)) {
        error = benchmark_spawn_process(&client->process, &env->slab_vka, &env->vspace, NUM_ARGS, client->argv, 1);
        ZF_LOGF_IF(error, "Failed to spawn client\n");
    } else {
        error = benchmark_spawn_process(&client->process, &env->slab_vka, &env->vspace, NUM_ARGS, client->argv, 0);
        /* Alter the SC parameters */


                                        
        seL4_TCB_Resume(client->process.thread.tcb.cptr);
    }

    /* get results */
    *ret1 = get_result(result_ep_path.capPtr);

    if (config_set(CONFIG_KERNEL_MCS) && params->server_fn != IPC_RECV_FUNC && params->passive) {
        /* convert server to active so it can send us the result */
        error = api_sc_bind(server->process.thread.sched_context.cptr,
                            server->process.thread.tcb.cptr);
        ZF_LOGF_IF(error, "Failed to convert server to active");
    }

    *ret2 = get_result(result_ep_path.capPtr);

    /* clean up - clean server first in case it is sharing the client's cspace and vspace */
    seL4_TCB_Suspend(client->process.thread.tcb.cptr);
    seL4_TCB_Suspend(server->process.thread.tcb.cptr);

    timing_destroy();
}

static env_t *env;

void CONSTRUCTOR(MUSLCSYS_WITH_VSYSCALL_PRIORITY) init_env(void)
{
    static size_t object_freq[seL4_ObjectTypeCount] = {
        [seL4_TCBObject] = 4,
        [seL4_EndpointObject] = 2,
#ifdef CONFIG_KERNEL_MCS
        [seL4_SchedContextObject] = 4,
        [seL4_ReplyObject] = 4
#endif
    };

    env = benchmark_get_env(
              sel4runtime_argc(),
              sel4runtime_argv(),
              sizeof(ipc_results_t),
              object_freq
          );
}

#if defined(CONFIG_KERNEL_IPCTHRESHOLDS) && defined(CONFIG_KERNEL_MCS)
seL4_Word threshold_defer_call_fp(int argc, char *argv[]) {
    uint32_t i;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);

    seL4_CPtr ep = atoi(argv[0]);
    seL4_CPtr result_ep = atoi(argv[1]);
    seL4_CPtr high_low_ep = atoi(argv[2]);

    ccnt_t start=0;


    /* Wakeup low_prio initially */
    // seL4_Debug_PutChar('C');
    seL4_Send(high_low_ep, tag);

    for (i = 0; i < 3; i++) {

        

        /* Burn some budget */
        COMPILER_MEMORY_FENCE();
        int i =10;

        volatile long int j=0;
        while (j<35000000) {
            j=j+1;
        }
        COMPILER_MEMORY_FENCE();
        
        /* Wakeup low_prio */
        seL4_Send(high_low_ep, tag);
        
         /* CALL */
        // DO_REAL_CALL(ep, tag);
        READ_COUNTER_BEFORE(start);
        seL4_Call(ep,tag);
        COMPILER_MEMORY_FENCE();
    }
    

    /* Send 'start' back*/
    send_result(result_ep, start);

    while (1) {}
}

seL4_Word threshold_defer_call_sp(int argc, char *argv[]) {
    uint32_t i;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 10);

    seL4_CPtr ep = atoi(argv[0]);
    seL4_CPtr result_ep = atoi(argv[1]);
    seL4_CPtr high_low_ep = atoi(argv[2]);

    ccnt_t start=0;


    /* Wakeup low_prio initially */
    // seL4_Debug_PutChar('C');
    seL4_Send(high_low_ep, tag);

    for (i = 0; i < 3; i++) {

        

        /* Burn some budget */
        COMPILER_MEMORY_FENCE();
        int i =10;

        volatile long int j=0;
        while (j<35000000) {
            j=j+1;
        }
        COMPILER_MEMORY_FENCE();
        
        /* Wakeup low_prio */
        seL4_Send(high_low_ep, tag);
        
         /* CALL */
        // DO_REAL_CALL(ep, tag);
        READ_COUNTER_BEFORE(start);
        seL4_Call(ep,tag);
        COMPILER_MEMORY_FENCE();
    }
    

    /* Send 'start' back*/
    send_result(result_ep, start);

    while (1) {}
}


seL4_Word threshold_defer_recv(int argc, char *argv[]) {
    uint32_t i;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);

    seL4_CPtr ep = atoi(argv[0]);
    seL4_CPtr result_ep = atoi(argv[1]);
    seL4_CPtr reply = atoi(argv[2]);

    /* Notify the initialiser that we are ready, then wait */
    seL4_NBSendRecv(result_ep, tag, ep, NULL, reply);

    for (i = 0; i < WARMUPS; i++) {
        /* ReplyRecv */
        seL4_ReplyRecv(ep, tag, NULL, reply);
    }
    
    while (1) {}
}

seL4_Word threshold_defer_low_prio(int argc, char *argv[]) {
    uint32_t i;
    seL4_CPtr result_ep = atoi(argv[0]);
    seL4_CPtr high_low_ep = atoi(argv[1]);
    seL4_CPtr reply = atoi(argv[2]);

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);

    ccnt_t end=0;
    
    /* Notify the initialiser that we are ready, then wait */
    seL4_NBSendRecv(result_ep, tag, high_low_ep, NULL, reply);

    for (i = 0; i < 3; i++) {
        /* Wait for client to signal us */
        seL4_Wait(high_low_ep, NULL);
        /* We will be runnable, but preempted. */
        /* Once client blocks due to threshold, read the counter */
        COMPILER_MEMORY_FENCE();
        READ_COUNTER_AFTER(end);
        COMPILER_MEMORY_FENCE();
    }


    /* Send 'end' back */
    send_result(result_ep, end);

    while (1) {}
}


#endif

seL4_Word ipc_block_caller_fp(int argc, char *argv[]) {
    seL4_CPtr ep = atoi(argv[0]);
    seL4_CPtr result_ep = atoi(argv[1]);
    seL4_CPtr high_low_ep = atoi(argv[2]);

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);


    ccnt_t start=0;

    seL4_Send(high_low_ep, tag);

    int i=0;
    for (i = 0; i < 2; i++) {
        /* Signal low prio */
        
        READ_COUNTER_BEFORE(start);
        seL4_Call(high_low_ep,tag);
    }

    send_result(result_ep, start);

}

seL4_Word ipc_block_caller_sp(int argc, char *argv[]) {
    seL4_CPtr ep = atoi(argv[0]);
    seL4_CPtr result_ep = atoi(argv[1]);
    seL4_CPtr high_low_ep = atoi(argv[2]);

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 10);


    ccnt_t start=0;

    seL4_Send(high_low_ep, tag);

    int i=0;
    for (i = 0; i < 2; i++) {
        /* Signal low prio */
        
        READ_COUNTER_BEFORE(start);
        seL4_Call(high_low_ep,tag);
    }

    send_result(result_ep, start);

}

seL4_Word ipc_block_low_prio(int argc, char *argv[]) {
    seL4_CPtr result_ep = atoi(argv[0]);
    seL4_CPtr high_low_ep = atoi(argv[1]);
    seL4_CPtr reply = atoi(argv[2]);

    ccnt_t end=0;


    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);
    
    /* Notify the initialiser that we are ready, then wait */
    seL4_NBSendRecv(result_ep, tag, high_low_ep, NULL, reply);


    int i=0;
    for (i = 0; i < 2; i++) {

        READ_COUNTER_AFTER(end);

        // Recv the client
        seL4_Recv(high_low_ep, NULL, reply);
        seL4_Send(reply, tag);
    }

    send_result(result_ep, end);
}



int main(int argc, char **argv)
{
    vka_object_t ep, result_ep;
    cspacepath_t ep_path, result_ep_path;

    ipc_results_t *results = (ipc_results_t *) env->results;

    /* allocate benchmark endpoint - the IPC's that we benchmark
       will be sent over this ep */
    if (vka_alloc_endpoint(&env->slab_vka, &ep) != 0) {
        ZF_LOGF("Failed to allocate endpoint");
    }
    vka_cspace_make_path(&env->slab_vka, ep.cptr, &ep_path);

    /* allocate result ep - the IPC threads will send their timestamps
       to this ep */
    if (vka_alloc_endpoint(&env->slab_vka, &result_ep) != 0) {
        ZF_LOGF("Failed to allocate endpoint");
    }
    vka_cspace_make_path(&env->slab_vka, result_ep.cptr, &result_ep_path);

    /* measure benchmarking overhead */
    measure_overhead(results);

    helper_thread_t client, server_thread, server_process;

    benchmark_shallow_clone_process(env, &client.process, seL4_MinPrio, 0, "client");
    benchmark_shallow_clone_process(env, &server_process.process, seL4_MinPrio, 0, "server process");
    benchmark_configure_thread_in_process(env, &client.process, &server_thread.process, seL4_MinPrio, 0, "server thread");

    client.ep = sel4utils_copy_path_to_process(&client.process, ep_path);
    client.result_ep = sel4utils_copy_path_to_process(&client.process, result_ep_path);

    server_process.ep = sel4utils_copy_path_to_process(&server_process.process, ep_path);
    server_process.result_ep = sel4utils_copy_path_to_process(&server_process.process, result_ep_path);

    server_thread.ep = client.ep;
    server_thread.result_ep = client.result_ep;

    sel4utils_create_word_args(client.argv_strings, client.argv, NUM_ARGS, client.ep, client.result_ep, 0);
    sel4utils_create_word_args(server_process.argv_strings, server_process.argv, NUM_ARGS,
                               server_process.ep, server_process.result_ep, SEL4UTILS_REPLY_SLOT);
    sel4utils_create_word_args(server_thread.argv_strings, server_thread.argv, NUM_ARGS,
                               server_thread.ep, server_thread.result_ep, SEL4UTILS_REPLY_SLOT);

    /* run the benchmark */
    seL4_CPtr auth = simple_get_tcb(&env->simple);
    ccnt_t start, end;
    for (int i = 0; i < RUNS; i++) {
        int j;
        ZF_LOGI("--------------------------------------------------\n");
        ZF_LOGI("Doing iteration %d\n", i);
        ZF_LOGI("--------------------------------------------------\n");
        for (j = 0; j < ARRAY_SIZE(benchmark_params); j++) {
            const struct benchmark_params *params = &benchmark_params[j];
            if (!params->threshold_defer) {
                ZF_LOGI("%s\t: IPC duration (%s), client prio: %3d server prio %3d, %s vspace, %s, length %2d\n",
                        params->name,
                        params->direction == DIR_TO ? "client --> server" : "server --> client",
                        params->client_prio, params->server_prio,
                        params->same_vspace ? "same" : "diff",
                        (config_set(CONFIG_KERNEL_MCS) && params->passive) ? "passive" : "active", params->length);

                /* set up client for benchmark */
                int error = seL4_TCB_SetPriority(client.process.thread.tcb.cptr, auth, params->client_prio);
                ZF_LOGF_IF(error, "Failed to set client prio");
                client.process.entry_point = bench_funcs[params->client_fn];

                if (params->same_vspace) {
                    error = seL4_TCB_SetPriority(server_thread.process.thread.tcb.cptr, auth, params->server_prio);
                    assert(error == seL4_NoError);
                    server_thread.process.entry_point = bench_funcs[params->server_fn];
                } else {
                    error = seL4_TCB_SetPriority(server_process.process.thread.tcb.cptr, auth, params->server_prio);
                    assert(error == seL4_NoError);
                    server_process.process.entry_point = bench_funcs[params->server_fn];
                }

                run_bench(env, result_ep_path, ep_path, params, &end, &start, &client,
                        params->same_vspace ? &server_thread : &server_process, params->threshold);

                if (end > start) {
                    results->benchmarks[j][i] = end - start;
                } else {
                    results->benchmarks[j][i] = start - end;
                }
            }
        }
    }




/* TODO, working here */
// #if defined(CONFIG_KERNEL_IPCTHRESHOLDS) && defined(CONFIG_KERNEL_MCS)
    printf("Starting threshold tests.\n");
    int error;


    /* Create the call endpoint */
    vka_object_t call_ep, return_ep, high_low_ep;
    cspacepath_t call_ep_path, return_ep_path, high_low_ep_path;
    if (vka_alloc_endpoint(&env->slab_vka, &call_ep) != 0) {
        ZF_LOGF("Failed to allocate endpoint");
    }
    vka_cspace_make_path(&env->slab_vka, call_ep.cptr, &call_ep_path);

    /* Create a result endpoint */
    if (vka_alloc_endpoint(&env->slab_vka, &return_ep) != 0) {
        ZF_LOGF("Failed to allocate endpoint");
    }
    vka_cspace_make_path(&env->slab_vka, return_ep.cptr, &return_ep_path);

    /* Create a high_low endpoint */
    if (vka_alloc_endpoint(&env->slab_vka, &high_low_ep) != 0) {
        ZF_LOGF("Failed to allocate endpoint");
    }
    vka_cspace_make_path(&env->slab_vka, high_low_ep.cptr, &high_low_ep_path);



    helper_thread_t client_t, server_thread_t, low_prio_t;
    benchmark_shallow_clone_process(env, &client_t.process, 15, 0, "client");
    benchmark_shallow_clone_process(env, &server_thread_t.process, 20, 0, "server process");
    benchmark_shallow_clone_process(env, &low_prio_t.process, 10, 0, "low_prio process");

#if defined(CONFIG_KERNEL_IPCTHRESHOLDS) && defined(CONFIG_KERNEL_MCS)

    server_thread_t.process.entry_point = threshold_defer_recv;
    low_prio_t.process.entry_point = threshold_defer_low_prio;
    client_t.process.entry_point = threshold_defer_call_fp;

#endif
    cspacepath_t client_t_sc_path;
    /* Make path to client SC */
    vka_cspace_make_path(&env->slab_vka, client_t.process.thread.sched_context.cptr, &client_t_sc_path);
    
    client_t.ep = sel4utils_copy_path_to_process(&client_t.process, call_ep_path);
    client_t.result_ep = sel4utils_copy_path_to_process(&client_t.process, return_ep_path);
    seL4_CPtr client_t_highlow = sel4utils_copy_path_to_process(&client_t.process, high_low_ep_path);
    seL4_CPtr client_t_sc = sel4utils_copy_path_to_process(&client_t.process, client_t_sc_path);


    server_thread_t.ep = sel4utils_copy_path_to_process(&server_thread_t.process, call_ep_path);
    server_thread_t.result_ep = sel4utils_copy_path_to_process(&server_thread_t.process, return_ep_path);

    low_prio_t.result_ep = sel4utils_copy_path_to_process(&low_prio_t.process, return_ep_path);
    seL4_CPtr low_prio_t_highlow = sel4utils_copy_path_to_process(&low_prio_t.process, high_low_ep_path);


    sel4utils_create_word_args(client_t.argv_strings, client_t.argv, 3, 
                            client_t.ep, client_t.result_ep, client_t_highlow);

    sel4utils_create_word_args(server_thread_t.argv_strings, server_thread_t.argv, NUM_ARGS,
                               server_thread_t.ep, server_thread_t.result_ep, SEL4UTILS_REPLY_SLOT);

    sel4utils_create_word_args(low_prio_t.argv_strings, low_prio_t.argv, NUM_ARGS, 
                                low_prio_t.result_ep, low_prio_t_highlow, SEL4UTILS_REPLY_SLOT);
    int j=0;
    for (j = 0; j < ARRAY_SIZE(benchmark_params); j++) {
    const struct benchmark_params *params = &benchmark_params[j];
        if(params->threshold_defer) {

#if defined(CONFIG_KERNEL_IPCTHRESHOLDS) && defined(CONFIG_KERNEL_MCS)

            switch(params->client_fn) {
                case IPC_CALL_FUNC:
                    client_t.process.entry_point = threshold_defer_call_fp;
                    break;
                case IPC_CALL_FUNC2:
                    client_t.process.entry_point = threshold_defer_call_sp;
                    break;
            }
#endif
            if (params->ep_block) {
                for (int i = 0; i < RUNS; i++) {

                    timing_init();

                    switch(params->client_fn) {
                        case IPC_CALL_FUNC:
                            client_t.process.entry_point = ipc_block_caller_fp;
                            break;
                        case IPC_CALL_FUNC2:
                            client_t.process.entry_point = ipc_block_caller_sp;
                            break;
                    }
                    low_prio_t.process.entry_point = ipc_block_low_prio;

                    /* Start low_prio */
                    printf("Starting low prio.\n");
                    int error = benchmark_spawn_process(&(low_prio_t.process), &env->slab_vka, &env->vspace, NUM_ARGS,
                                                low_prio_t.argv, 1);
                    ZF_LOGF_IF(error, "Failed to start low prio");

                    /* Wait for it to   tell us its initialised */
                    printf("Waiting for low prio.\n");
                    seL4_Wait(return_ep_path.capPtr, NULL);


                    /* Start client */
                    printf("Starting client.\n");
                    error = benchmark_spawn_process(&(client_t.process), &env->slab_vka, &env->vspace, 3, client_t.argv, 1);
                    ZF_LOGF_IF(error, "Failed to spawn client\n");

                    ccnt_t ret1 = get_result(return_ep_path.capPtr);
                    printf("Got result1 %lu\n", ret1);



                    ccnt_t ret2 = get_result(return_ep_path.capPtr);
                    printf("Got result2 %lu\n", ret2);

                    if (ret1 > ret2) {
                        results->benchmarks[j][i] = ret1 - ret2;
                    } else {
                        results->benchmarks[j][i] = ret2 - ret1;
                    }

                    seL4_TCB_Suspend(client_t.process.thread.tcb.cptr);
                    seL4_TCB_Suspend(server_thread_t.process.thread.tcb.cptr);
                    seL4_TCB_Suspend(low_prio_t.process.thread.tcb.cptr);

                    timing_destroy();
                }  
            }
            else {
                for (int i = 0; i < RUNS; i++) {

                    timing_init();
#if defined(CONFIG_KERNEL_IPCTHRESHOLDS) && defined(CONFIG_KERNEL_MCS)
                    /* Set EP Threshold on call_endpoint */
                    error = seL4_CNode_Endpoint_SetThreshold(call_ep_path.root, call_ep_path.capPtr, call_ep_path.capDepth, 150*US_IN_MS);
                    ZF_LOGF_IF(error, "Failed to set threshold\n");
#endif

                    /* Start low_prio */
                    printf("Starting low prio.\n");
                    int error = benchmark_spawn_process(&(low_prio_t.process), &env->slab_vka, &env->vspace, NUM_ARGS,
                                                low_prio_t.argv, 1);
                    ZF_LOGF_IF(error, "Failed to start low prio");

                    
                    /* Wait for it to tell us its initialised */
                    printf("Waiting for low prio.\n");
                    seL4_Wait(return_ep_path.capPtr, NULL);


                    /* Start the server */
                    printf("Starting server.\n");
                    error = benchmark_spawn_process(&(server_thread_t.process), &env->slab_vka, &env->vspace, NUM_ARGS,
                                        server_thread_t.argv, 1);
                    ZF_LOGF_IF(error, "Failed to start server");

                    /* wait for server to tell us its initialised */
                    seL4_Wait(return_ep_path.capPtr, NULL);

                    /* convert server to passive */
                    error = api_sc_unbind_object(server_thread_t.process.thread.sched_context.cptr,
                                                    server_thread_t.process.thread.tcb.cptr);
                    ZF_LOGF_IF(error, "Failed to convert server to passive");



                    error = api_sc_unbind_object(client_t.process.thread.sched_context.cptr,
                                                    client_t.process.thread.tcb.cptr);


                    /* Set clients SC properties */
                    api_sched_ctrl_configure(simple_get_sched_ctrl(&env->simple, 0), client_t.process.thread.sched_context.cptr,
                                    250 * US_IN_MS, 350 * US_IN_MS,
                                        0, 0);

                    error = api_sc_bind(client_t.process.thread.sched_context.cptr,
                                                    client_t.process.thread.tcb.cptr);


                    /* Start client */
                    printf("Starting client.\n");
                    error = benchmark_spawn_process(&(client_t.process), &env->slab_vka, &env->vspace, 3, client_t.argv, 1);
                    ZF_LOGF_IF(error, "Failed to spawn client\n");




                    /* get results */

                    ccnt_t ret1 = get_result(return_ep_path.capPtr);
                    printf("Got result1 %lu\n", ret1);



                    ccnt_t ret2 = get_result(return_ep_path.capPtr);
                    printf("Got result2 %lu\n", ret2);

                    if (ret1 > ret2) {
                        results->benchmarks[j][i] = ret1 - ret2;
                    } else {
                        results->benchmarks[j][i] = ret2 - ret1;
                    }


                    // Bind SC to server

                    error = api_sc_bind(server_thread_t.process.thread.sched_context.cptr,
                                    server_thread_t.process.thread.tcb.cptr);


                    /* clean up - clean server first in case it is sharing the client's cspace and vspace */
                    seL4_TCB_Suspend(client_t.process.thread.tcb.cptr);
                    seL4_TCB_Suspend(server_thread_t.process.thread.tcb.cptr);
                    seL4_TCB_Suspend(low_prio_t.process.thread.tcb.cptr);

                    timing_destroy();
                }
            }
        }
    }

// #endif

    /* done -> results are stored in shared memory so we can now return */
    benchmark_finished(EXIT_SUCCESS);
    return 0;
}






 /* Useful functions */

 /* 
//  Set the SC parameters
 api_sched_ctrl_configure();

 Set threshold value
 


 
 
  */

// Copy this for SetThreshold
//  static inline int vka_cnode_cancelBadgedSends(const cspacepath_t *src)
// {
//     return seL4_CNode_CancelBadgedSends(
//                /* _service */      src->root,
//                /* index */         src->capPtr,
//                /* depth */         src->capDepth
//            );
// }

