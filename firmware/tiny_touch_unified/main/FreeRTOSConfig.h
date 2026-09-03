#pragma once

#define configUSE_PREEMPTION 1
#define configUSE_TICKLESS_IDLE 0
#define configCPU_CLOCK_HZ 125000000
#define configTICK_RATE_HZ 1000
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 256
#define configMAX_TASK_NAME_LEN 16
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE 8
#define configUSE_QUEUE_SETS 0
#define configUSE_TIME_SLICING 1
#define configUSE_NEWLIB_REENTRANT 0
#define configENABLE_BACKWARD_COMPATIBILITY 0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_MINI_LIST_ITEM 1
#define configSTACK_DEPTH_TYPE uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE size_t

#define configSUPPORT_STATIC_ALLOCATION 0
#define configSUPPORT_DYNAMIC_ALLOCATION 1
// mbedTLS also allocates from this heap; RSA-2048 generation is the peak user.
#define configTOTAL_HEAP_SIZE (128 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP 0

#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0

#define configGENERATE_RUN_TIME_STATS 0
#define configUSE_TRACE_FACILITY 0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configUSE_CO_ROUTINES 0
// The RP2040 port's pico_sync interop uses event groups, which need the timer task.
#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH 8
#define configTIMER_TASK_STACK_DEPTH 256

#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xResumeFromISR 1
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle 0
#define INCLUDE_eTaskGetState 1
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_xTaskAbortDelay 0
#define INCLUDE_xTaskGetHandle 0
#define INCLUDE_xTaskResumeFromISR 1

// RP2040 port: single core, but let pico_sync and pico_time block properly.
#define configNUMBER_OF_CORES 1
#define configTICK_CORE 0
#define configRUN_MULTIPLE_PRIORITIES 1
#define configUSE_CORE_AFFINITY 0
#define configUSE_PASSIVE_IDLE_HOOK 0
#define configSUPPORT_PICO_SYNC_INTEROP 1
#define configSUPPORT_PICO_TIME_INTEROP 1

// Always evaluated: several call sites rely on the asserted expression's side
// effects. A failed assertion parks the core until the watchdog restarts it.
#define configASSERT(x)                    \
  do {                                     \
    if (!(x)) {                            \
      portDISABLE_INTERRUPTS();            \
      for (;;) {}                          \
    }                                      \
  } while (0)
