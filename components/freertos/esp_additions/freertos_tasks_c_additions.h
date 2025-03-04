/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "esp_assert.h"
#include "freertos/idf_additions.h"
#if CONFIG_FREERTOS_ENABLE_TASK_SNAPSHOT
    #include "freertos/task_snapshot.h"
#endif /* CONFIG_FREERTOS_ENABLE_TASK_SNAPSHOT */
#include "esp_private/freertos_idf_additions_priv.h"

/**
 * This file will be included in `tasks.c` file, thus, it is treated as a source
 * file instead of a header file, and must NOT be included by any (other) file.
 * This file is used to add additional functions to `tasks.c`. See the
 * `esp_additions/include` directory of the headers that expose these `tasks.c`
 * additional API.
 */

#if CONFIG_FREERTOS_USE_KERNEL_10_5_1
    #define pxCurrentTCB    pxCurrentTCBs
#else
#endif

/* ------------------------------------------------- Static Asserts ------------------------------------------------- */

/*
 * Both StaticTask_t and TCB_t structures are provided by FreeRTOS sources.
 * This is just an additional check of the consistency of these structures.
 */
_Static_assert( offsetof( StaticTask_t, pxDummy6 ) == offsetof( TCB_t, pxStack ) );
_Static_assert( offsetof( StaticTask_t, pxDummy8 ) == offsetof( TCB_t, pxEndOfStack ) );
#if CONFIG_FREERTOS_SMP
    _Static_assert( tskNO_AFFINITY == CONFIG_FREERTOS_NO_AFFINITY, "CONFIG_FREERTOS_NO_AFFINITY must be the same as tskNO_AFFINITY" );
#endif /* CONFIG_FREERTOS_SMP */

/* ------------------------------------------------- Kernel Control ------------------------------------------------- */

#if ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) )

/*
 * Wrapper function to take "xKerneLock"
 */
    void prvTakeKernelLock( void )
    {
        /* We call the tasks.c critical section macro to take xKernelLock */
        taskENTER_CRITICAL( &xKernelLock );
    }

#endif /* ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) ) */
/*----------------------------------------------------------*/

#if ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) )

/*
 * Wrapper function to release "xKerneLock"
 */
    void prvReleaseKernelLock( void )
    {
        /* We call the tasks.c critical section macro to release xKernelLock */
        taskEXIT_CRITICAL( &xKernelLock );
    }

#endif /* ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) ) */
/*----------------------------------------------------------*/

#if ( CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) )

/*
 * Workaround for non-thread safe multi-core OS startup (see IDF-4524)
 */
    void prvStartSchedulerOtherCores( void )
    {
        /* This function is always called with interrupts disabled*/
        xSchedulerRunning = pdTRUE;
    }

#endif /* ( CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) ) */
/*----------------------------------------------------------*/

#if ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) )

    BaseType_t xTaskIncrementTickOtherCores( void )
    {
        /* Minor optimization. This function can never switch cores mid
         * execution */
        BaseType_t xCoreID = xPortGetCoreID();
        BaseType_t xSwitchRequired = pdFALSE;

        /* This function should never be called by Core 0. */
        configASSERT( xCoreID != 0 );

        /* Called by the portable layer each time a tick interrupt occurs.
         * Increments the tick then checks to see if the new tick value will
         * cause any tasks to be unblocked. */
        traceTASK_INCREMENT_TICK( xTickCount );

        if( uxSchedulerSuspended[ xCoreID ] == ( UBaseType_t ) 0U )
        {
            /* We need take the kernel lock here as we are about to access
             * kernel data structures. */
            taskENTER_CRITICAL_ISR( &xKernelLock );

            /* A task being unblocked cannot cause an immediate context switch
             * if preemption is turned off. */
            #if ( configUSE_PREEMPTION == 1 )
            {
                /* Check if core 0 calling xTaskIncrementTick() has
                 * unblocked a task that can be run. */
                if( uxTopReadyPriority > pxCurrentTCB[ xCoreID ]->uxPriority )
                {
                    xSwitchRequired = pdTRUE;
                }
                else
                {
                    mtCOVERAGE_TEST_MARKER();
                }
            }
            #endif /* if ( configUSE_PREEMPTION == 1 ) */

            /* Tasks of equal priority to the currently running task will share
             * processing time (time slice) if preemption is on, and the application
             * writer has not explicitly turned time slicing off. */
            #if ( ( configUSE_PREEMPTION == 1 ) && ( configUSE_TIME_SLICING == 1 ) )
            {
                if( listCURRENT_LIST_LENGTH( &( pxReadyTasksLists[ pxCurrentTCB[ xCoreID ]->uxPriority ] ) ) > ( UBaseType_t ) 1 )
                {
                    xSwitchRequired = pdTRUE;
                }
                else
                {
                    mtCOVERAGE_TEST_MARKER();
                }
            }
            #endif /* ( ( configUSE_PREEMPTION == 1 ) && ( configUSE_TIME_SLICING == 1 ) ) */

            /* Release the previously taken kernel lock as we have finished
             * accessing the kernel data structures. */
            taskEXIT_CRITICAL_ISR( &xKernelLock );

            #if ( configUSE_PREEMPTION == 1 )
            {
                if( xYieldPending[ xCoreID ] != pdFALSE )
                {
                    xSwitchRequired = pdTRUE;
                }
                else
                {
                    mtCOVERAGE_TEST_MARKER();
                }
            }
            #endif /* configUSE_PREEMPTION */
        }

        #if ( configUSE_TICK_HOOK == 1 )
        {
            vApplicationTickHook();
        }
        #endif

        return xSwitchRequired;
    }

#endif /* ( !CONFIG_FREERTOS_SMP && ( configNUM_CORES > 1 ) ) */
/*----------------------------------------------------------*/

/* -------------------------------------------------- Task Creation ------------------------------------------------- */

#if ( configSUPPORT_DYNAMIC_ALLOCATION == 1 )

    BaseType_t xTaskCreatePinnedToCore( TaskFunction_t pxTaskCode,
                                        const char * const pcName,
                                        const uint32_t usStackDepth,
                                        void * const pvParameters,
                                        UBaseType_t uxPriority,
                                        TaskHandle_t * const pxCreatedTask,
                                        const BaseType_t xCoreID )
    {
        BaseType_t xReturn;

        #if CONFIG_FREERTOS_SMP
        {
            /* If using Amazon SMP FreeRTOS. This function is just a wrapper around
             * xTaskCreate() or xTaskCreateAffinitySet(). */
            #if ( ( configNUM_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) )
            {
                /* Convert xCoreID into an affinity mask */
                UBaseType_t uxCoreAffinityMask;

                /* Bit shifting << xCoreID is only valid if we have less than
                 * 32 cores. */
                ESP_STATIC_ASSERT( configNUM_CORES < 32 );

                if( xCoreID == tskNO_AFFINITY )
                {
                    uxCoreAffinityMask = tskNO_AFFINITY;
                }
                else
                {
                    uxCoreAffinityMask = ( 1 << xCoreID );
                }

                xReturn = xTaskCreateAffinitySet( pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, uxCoreAffinityMask, pxCreatedTask );
            }
            #else /* ( ( configNUM_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) ) */
            {
                xReturn = xTaskCreate( pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask );
            }
            #endif /* ( ( configNUM_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) ) */
        }
        #else /* CONFIG_FREERTOS_SMP */
        {
            TCB_t * pxNewTCB;

            /* If the stack grows down then allocate the stack then the TCB so the
             * stack does not grow into the TCB.  Likewise if the stack grows up
             * then allocate the TCB then the stack. */
            #if ( portSTACK_GROWTH > 0 )
            {
                /* Allocate space for the TCB.  Where the memory comes from depends on
                 * the implementation of the port malloc function and whether or not static
                 * allocation is being used. */
                pxNewTCB = ( TCB_t * ) pvPortMalloc( sizeof( TCB_t ) );

                if( pxNewTCB != NULL )
                {
                    #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
                    {
                        memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );

                        /* Allocate space for the stack used by the task being created.
                         * The base of the stack memory stored in the TCB so the task can
                         * be deleted later if required. */
                        pxNewTCB->pxStack = ( StackType_t * ) pvPortMallocStack( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e961 MISRA exception as the casts are only redundant for some ports. */
                    }
                    #else /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
                    {
                        /* Allocate space for the stack used by the task being created.
                         * The base of the stack memory stored in the TCB so the task can
                         * be deleted later if required. */
                        pxNewTCB->pxStack = ( StackType_t * ) pvPortMalloc( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e961 MISRA exception as the casts are only redundant for some ports. */
                    }
                    #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */

                    if( pxNewTCB->pxStack == NULL )
                    {
                        /* Could not allocate the stack.  Delete the allocated TCB. */
                        vPortFree( pxNewTCB );
                        pxNewTCB = NULL;
                    }
                }
            }
            #else /* portSTACK_GROWTH */
            {
                StackType_t * pxStack;

                #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
                {
                    /* Allocate space for the stack used by the task being created. */
                    pxStack = pvPortMallocStack( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e9079 All values returned by pvPortMalloc() have at least the alignment required by the MCU's stack and this allocation is the stack. */
                }
                #else /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
                {
                    /* Allocate space for the stack used by the task being created. */
                    pxStack = pvPortMalloc( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e9079 All values returned by pvPortMalloc() have at least the alignment required by the MCU's stack and this allocation is the stack. */
                }
                #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */

                if( pxStack != NULL )
                {
                    /* Allocate space for the TCB. */
                    pxNewTCB = ( TCB_t * ) pvPortMalloc( sizeof( TCB_t ) ); /*lint !e9087 !e9079 All values returned by pvPortMalloc() have at least the alignment required by the MCU's stack, and the first member of TCB_t is always a pointer to the task's stack. */

                    if( pxNewTCB != NULL )
                    {
                        #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
                        {
                            memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );
                        }
                        #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */

                        /* Store the stack location in the TCB. */
                        pxNewTCB->pxStack = pxStack;
                    }
                    else
                    {
                        /* The stack cannot be used as the TCB was not created.  Free
                         * it again. */
                        #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
                        {
                            vPortFreeStack( pxStack );
                        }
                        #else /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
                        {
                            vPortFree( pxStack );
                        }
                        #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
                    }
                }
                else
                {
                    pxNewTCB = NULL;
                }
            }
            #endif /* portSTACK_GROWTH */

            if( pxNewTCB != NULL )
            {
                #if ( tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE != 0 ) /*lint !e9029 !e731 Macro has been consolidated for readability reasons. */
                {
                    /* Tasks can be created statically or dynamically, so note this
                     * task was created dynamically in case it is later deleted. */
                    pxNewTCB->ucStaticallyAllocated = tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB;
                }
                #endif /* tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE */

                prvInitialiseNewTask( pxTaskCode, pcName, ( uint32_t ) usStackDepth, pvParameters, uxPriority, pxCreatedTask, pxNewTCB, NULL, xCoreID );
                prvAddNewTaskToReadyList( pxNewTCB );
                xReturn = pdPASS;
            }
            else
            {
                xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
            }
        }
        #endif /* CONFIG_FREERTOS_SMP */

        return xReturn;
    }

#endif /* ( configSUPPORT_DYNAMIC_ALLOCATION == 1 ) */
/*----------------------------------------------------------*/

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

    TaskHandle_t xTaskCreateStaticPinnedToCore( TaskFunction_t pxTaskCode,
                                                const char * const pcName,
                                                const uint32_t ulStackDepth,
                                                void * const pvParameters,
                                                UBaseType_t uxPriority,
                                                StackType_t * const puxStackBuffer,
                                                StaticTask_t * const pxTaskBuffer,
                                                const BaseType_t xCoreID )
    {
        TaskHandle_t xReturn;

        #if CONFIG_FREERTOS_SMP
        {
            /* If using Amazon SMP FreeRTOS. This function is just a wrapper around
             * xTaskCreateStatic() or xTaskCreateStaticAffinitySet(). */
            #if ( ( configNUM_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) )
            {
                /* Convert xCoreID into an affinity mask */
                UBaseType_t uxCoreAffinityMask;

                if( xCoreID == tskNO_AFFINITY )
                {
                    uxCoreAffinityMask = tskNO_AFFINITY;
                }
                else
                {
                    uxCoreAffinityMask = ( 1 << xCoreID );
                }

                xReturn = xTaskCreateStaticAffinitySet( pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority, puxStackBuffer, pxTaskBuffer, uxCoreAffinityMask );
            }
            #else /* ( ( configNUM_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) ) */
            {
                xReturn = xTaskCreateStatic( pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority, puxStackBuffer, pxTaskBuffer );
            }
            #endif /* ( ( configNUM_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) ) */
        }
        #else /* CONFIG_FREERTOS_SMP */
        {
            TCB_t * pxNewTCB;

            configASSERT( portVALID_STACK_MEM( puxStackBuffer ) );
            configASSERT( portVALID_TCB_MEM( pxTaskBuffer ) );
            configASSERT( ( ( xCoreID >= 0 ) && ( xCoreID < configNUM_CORES ) ) || ( xCoreID == tskNO_AFFINITY ) );

            #if ( configASSERT_DEFINED == 1 )
            {
                /* Sanity check that the size of the structure used to declare a
                 * variable of type StaticTask_t equals the size of the real task
                 * structure. */
                volatile size_t xSize = sizeof( StaticTask_t );
                configASSERT( xSize == sizeof( TCB_t ) );
                ( void ) xSize; /* Prevent lint warning when configASSERT() is not used. */
            }
            #endif /* configASSERT_DEFINED */

            if( ( pxTaskBuffer != NULL ) && ( puxStackBuffer != NULL ) )
            {
                /* The memory used for the task's TCB and stack are passed into this
                 * function - use them. */
                pxNewTCB = ( TCB_t * ) pxTaskBuffer; /*lint !e740 !e9087 Unusual cast is ok as the structures are designed to have the same alignment, and the size is checked by an assert. */

                #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
                {
                    memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );
                }
                #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */

                pxNewTCB->pxStack = ( StackType_t * ) puxStackBuffer;

                #if ( tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE != 0 ) /*lint !e731 !e9029 Macro has been consolidated for readability reasons. */
                {
                    /* Tasks can be created statically or dynamically, so note this
                     * task was created statically in case the task is later deleted. */
                    pxNewTCB->ucStaticallyAllocated = tskSTATICALLY_ALLOCATED_STACK_AND_TCB;
                }
                #endif /* tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE */

                prvInitialiseNewTask( pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority, &xReturn, pxNewTCB, NULL, xCoreID );
                prvAddNewTaskToReadyList( pxNewTCB );
            }
            else
            {
                xReturn = NULL;
            }
        }
        #endif /* CONFIG_FREERTOS_SMP */

        return xReturn;
    }

#endif /* ( configSUPPORT_STATIC_ALLOCATION == 1 ) */
/*----------------------------------------------------------*/

#if ( configUSE_TIMERS == 1 )

/*
 * In ESP-IDF, configUSE_TIMERS is always defined as 1 (i.e., not user configurable).
 * However, tasks.c: vTaskStartScheduler() will always call xTimerCreateTimerTask()
 * if ( configUSE_TIMERS == 1 ), thus causing the linker to link timers.c and
 * wasting some memory (due to the timer task being created)/
 *
 * If we provide a weak version of xTimerCreateTimerTask(), this version will be
 * compiled if the application does not call any other FreeRTOS timer functions.
 * Thus we can save some text/RAM as timers.c will not be linked and the timer
 * task never created.
 */
    BaseType_t __attribute__( ( weak ) ) xTimerCreateTimerTask( void )
    {
        return pdPASS;
    }

#endif /* configUSE_TIMERS */
/*----------------------------------------------------------*/

/* ------------------------------------------------- Task Utilities ------------------------------------------------- */

BaseType_t xTaskGetCoreID( TaskHandle_t xTask )
{
    BaseType_t xReturn;

    #if ( configNUM_CORES > 1 )
    {
        #if CONFIG_FREERTOS_SMP
            UBaseType_t uxCoreAffinityMask;

            /* Get the core affinity mask and covert it to an ID */
            uxCoreAffinityMask = vTaskCoreAffinityGet( xTask );

            /* If the task is not pinned to a particular core, treat it as tskNO_AFFINITY */
            if( uxCoreAffinityMask & ( uxCoreAffinityMask - 1 ) ) /* If more than one bit set */
            {
                xReturn = tskNO_AFFINITY;
            }
            else
            {
                int iIndexPlusOne = __builtin_ffs( uxCoreAffinityMask );
                assert( iIndexPlusOne >= 1 );
                xReturn = iIndexPlusOne - 1;
            }
        #else /* CONFIG_FREERTOS_SMP */
            TCB_t * pxTCB;

            /* Todo: Remove xCoreID for single core builds (IDF-7894) */
            pxTCB = prvGetTCBFromHandle( xTask );

            xReturn = pxTCB->xCoreID;
        #endif /* CONFIG_FREERTOS_SMP */
    }
    #else /* configNUM_CORES > 1 */
    {
        /* Single-core. Just return a core ID of 0 */
        xReturn = 0;
    }
    #endif /* configNUM_CORES > 1 */

    return xReturn;
}
/*----------------------------------------------------------*/

#if ( INCLUDE_xTaskGetIdleTaskHandle == 1 )

    TaskHandle_t xTaskGetIdleTaskHandleForCore( BaseType_t xCoreID )
    {
        #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
        {
            /* If xTaskGetIdleTaskHandle() is called before the scheduler has been
             * started, then xIdleTaskHandle will be NULL. */
            configASSERT( ( xCoreID < configNUMBER_OF_CORES ) && ( xCoreID != tskNO_AFFINITY ) );
            configASSERT( ( xIdleTaskHandle[ xCoreID ] != NULL ) );
            return xIdleTaskHandle[ xCoreID ];
        }
        #else /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
        {
            configASSERT( xCoreID >= 0 && xCoreID < configNUM_CORES );
            configASSERT( ( xIdleTaskHandle[ xCoreID ] != NULL ) );
            return ( TaskHandle_t ) xIdleTaskHandle[ xCoreID ];
        }
        #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
    }

#endif /* INCLUDE_xTaskGetIdleTaskHandle */
/*----------------------------------------------------------*/

#if ( ( INCLUDE_xTaskGetCurrentTaskHandle == 1 ) || ( configUSE_MUTEXES == 1 ) )

    TaskHandle_t xTaskGetCurrentTaskHandleForCore( BaseType_t xCoreID )
    {
        TaskHandle_t xReturn;

        #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
        {
            configASSERT( xCoreID < configNUMBER_OF_CORES );
            configASSERT( xCoreID != tskNO_AFFINITY );

            /* For SMP, we need to take the kernel lock here as we are about to
             * access kernel data structures. For single core, a critical section is
             * not required as this is not called from an interrupt and the current
             * TCB will always be the same for any individual execution thread. */
            taskENTER_CRITICAL_SMP_ONLY( &xKernelLock );
            {
                xReturn = pxCurrentTCBs[ xCoreID ];
            }
            /* Release the previously taken kernel lock. */
            taskEXIT_CRITICAL_SMP_ONLY( &xKernelLock );
        }
        #else /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
        {
            #if CONFIG_FREERTOS_SMP
            {
                xReturn = xTaskGetCurrentTaskHandleCPU( xCoreID );
            }
            #else /* CONFIG_FREERTOS_SMP */
            {
                if( xCoreID < configNUM_CORES )
                {
                    xReturn = pxCurrentTCB[ xCoreID ];
                }
                else
                {
                    xReturn = NULL;
                }
            }
            #endif /* CONFIG_FREERTOS_SMP */
        }
        #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */

        return xReturn;
    }

#endif /* ( ( INCLUDE_xTaskGetCurrentTaskHandle == 1 ) || ( configUSE_MUTEXES == 1 ) ) */
/*----------------------------------------------------------*/

#if ( CONFIG_FREERTOS_USE_KERNEL_10_5_1 && ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) )

    configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimeCounterForCore( BaseType_t xCoreID )
    {
        uint32_t ulRunTimeCounter;

        configASSERT( xCoreID < configNUMBER_OF_CORES );
        configASSERT( xCoreID != tskNO_AFFINITY );

        /* For SMP, we need to take the kernel lock here as we are about to
         * access kernel data structures. */
        taskENTER_CRITICAL_SMP_ONLY( &xKernelLock );
        {
            ulRunTimeCounter = xIdleTaskHandle[ xCoreID ]->ulRunTimeCounter;
        }
        /* Release the previously taken kernel lock. */
        taskEXIT_CRITICAL_SMP_ONLY( &xKernelLock );

        return ulRunTimeCounter;
    }

#endif /* ( CONFIG_FREERTOS_USE_KERNEL_10_5_1 && ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) ) */
/*----------------------------------------------------------*/

#if ( CONFIG_FREERTOS_USE_KERNEL_10_5_1 && ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) )

    configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimePercentForCore( BaseType_t xCoreID )
    {
        configRUN_TIME_COUNTER_TYPE ulTotalTime, ulReturn;

        configASSERT( xCoreID < configNUMBER_OF_CORES );
        configASSERT( xCoreID != tskNO_AFFINITY );

        ulTotalTime = portGET_RUN_TIME_COUNTER_VALUE();

        /* For percentage calculations. */
        ulTotalTime /= ( configRUN_TIME_COUNTER_TYPE ) 100;

        /* Avoid divide by zero errors. */
        if( ulTotalTime > ( configRUN_TIME_COUNTER_TYPE ) 0 )
        {
            /* For SMP, we need to take the kernel lock here as we are about
             * to access kernel data structures. */
            taskENTER_CRITICAL_SMP_ONLY( &xKernelLock );
            {
                ulReturn = xIdleTaskHandle[ xCoreID ]->ulRunTimeCounter / ulTotalTime;
            }
            /* Release the previously taken kernel lock. */
            taskEXIT_CRITICAL_SMP_ONLY( &xKernelLock );
        }
        else
        {
            ulReturn = 0;
        }

        return ulReturn;
    }

#endif /* ( CONFIG_FREERTOS_USE_KERNEL_10_5_1 &&  ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) ) */
/*-----------------------------------------------------------*/

uint8_t * pxTaskGetStackStart( TaskHandle_t xTask )
{
    TCB_t * pxTCB;
    uint8_t * uxReturn;

    pxTCB = prvGetTCBFromHandle( xTask );
    uxReturn = ( uint8_t * ) pxTCB->pxStack;

    return uxReturn;
}
/*----------------------------------------------------------*/

#if ( INCLUDE_vTaskPrioritySet == 1 )

    void prvTaskPriorityRaise( prvTaskSavedPriority_t * pxSavedPriority,
                               UBaseType_t uxNewPriority )
    {
        TCB_t * pxTCB;
        UBaseType_t uxPriorityUsedOnEntry;

        configASSERT( ( uxNewPriority < configMAX_PRIORITIES ) );

        /* Ensure the new priority is valid. */
        if( uxNewPriority >= ( UBaseType_t ) configMAX_PRIORITIES )
        {
            uxNewPriority = ( UBaseType_t ) configMAX_PRIORITIES - ( UBaseType_t ) 1U;
        }

        #if CONFIG_FREERTOS_SMP
            taskENTER_CRITICAL();
        #else
            taskENTER_CRITICAL( &xKernelLock );
        #endif
        {
            pxTCB = prvGetTCBFromHandle( NULL );

            #if ( configUSE_MUTEXES == 1 )
            {
                pxSavedPriority->uxPriority = pxTCB->uxPriority;
                pxSavedPriority->uxBasePriority = pxTCB->uxBasePriority;

                /* If uxNewPriority < uxBasePriority, then there is nothing else to
                 * do, as uxBasePriority is always <= uxPriority. */
                if( uxNewPriority > pxTCB->uxBasePriority )
                {
                    pxTCB->uxBasePriority = uxNewPriority;

                    /* Remember the task's current priority before attempting to
                     * change it. If the task's current priority is changed, it must
                     * be done so before moving the task between task lists) in order
                     * for the taskRESET_READY_PRIORITY() macro to function correctly. */
                    uxPriorityUsedOnEntry = pxTCB->uxPriority;

                    if( uxNewPriority > pxTCB->uxPriority )
                    {
                        pxTCB->uxPriority = uxNewPriority;

                        /* Only reset the event list item value if the value is not
                         * being used for anything else. */
                        if( ( listGET_LIST_ITEM_VALUE( &( pxTCB->xEventListItem ) ) & taskEVENT_LIST_ITEM_VALUE_IN_USE ) == 0UL )
                        {
                            listSET_LIST_ITEM_VALUE( &( pxTCB->xEventListItem ), ( ( TickType_t ) configMAX_PRIORITIES - ( TickType_t ) uxNewPriority ) ); /*lint !e961 MISRA exception as the casts are only redundant for some ports. */
                        }

                        /* If the task is in the blocked or suspended list we need do
                         * nothing more than change its priority variable. However, if
                         * the task is in a ready list it needs to be removed and placed
                         * in the list appropriate to its new priority. */
                        if( listIS_CONTAINED_WITHIN( &( pxReadyTasksLists[ uxPriorityUsedOnEntry ] ), &( pxTCB->xStateListItem ) ) != pdFALSE )
                        {
                            /* The task is currently in its ready list - remove before
                             * adding it to its new ready list.  As we are in a critical
                             * section we can do this even if the scheduler is suspended. */
                            if( uxListRemove( &( pxTCB->xStateListItem ) ) == ( UBaseType_t ) 0 )
                            {
                                /* It is known that the task is in its ready list so
                                 * there is no need to check again and the port level
                                 * reset macro can be called directly. */
                                portRESET_READY_PRIORITY( uxPriorityUsedOnEntry, uxTopReadyPriority );
                            }

                            prvAddTaskToReadyList( pxTCB );
                        }
                    }
                }
            }
            #else /* if ( configUSE_MUTEXES == 1 ) */
            {
                pxSavedPriority->uxPriority = pxTCB->uxPriority;

                if( uxNewPriority > pxTCB->uxPriority )
                {
                    vTaskPrioritySet( NULL, uxNewPriority );
                }
            }
            #endif /* if ( configUSE_MUTEXES == 1 ) */
        }
        #if CONFIG_FREERTOS_SMP
            taskEXIT_CRITICAL();
        #else
            taskEXIT_CRITICAL( &xKernelLock );
        #endif
    }

#endif /* INCLUDE_vTaskPrioritySet == 1 */
/*----------------------------------------------------------*/

#if ( INCLUDE_vTaskPrioritySet == 1 )

    void prvTaskPriorityRestore( prvTaskSavedPriority_t * pxSavedPriority )
    {
        TCB_t * pxTCB;
        UBaseType_t uxNewPriority;
        UBaseType_t uxPriorityUsedOnEntry;
        UBaseType_t uxBasePriorityUsedOnEntry;
        BaseType_t xYieldRequired = pdFALSE;

        #if CONFIG_FREERTOS_SMP
            taskENTER_CRITICAL();
        #else
            taskENTER_CRITICAL( &xKernelLock );
        #endif
        {
            pxTCB = prvGetTCBFromHandle( NULL );

            #if ( configUSE_MUTEXES == 1 )
            {
                /* If the saved uxBasePriority == the task's uxBasePriority, it means
                 * that prvTaskPriorityRaise() never raised the task's uxBasePriority.
                 * In that case, there is nothing else to do. */
                if( pxSavedPriority->uxBasePriority != pxTCB->uxBasePriority )
                {
                    uxBasePriorityUsedOnEntry = pxTCB->uxBasePriority;
                    pxTCB->uxBasePriority = pxSavedPriority->uxBasePriority;

                    /* Remember the task's current priority before attempting to
                     * change it. If the task's current priority is changed, it must
                     * be done so before moving the task between task lists in order
                     * for the taskRESET_READY_PRIORITY() macro to function correctly. */
                    uxPriorityUsedOnEntry = pxTCB->uxPriority;

                    /* Check if the task inherited a priority after prvTaskPriorityRaise().
                     * If this is the case, there is nothing else to do. The priority
                     * will be restored when the task disinherits its priority. */
                    if( pxTCB->uxPriority == uxBasePriorityUsedOnEntry )
                    {
                        if( pxTCB->uxMutexesHeld == 0 )
                        {
                            /* The task may have inherited a priority before prvTaskPriorityRaise()
                             * then disinherited a priority after prvTaskPriorityRaise().
                             * Thus we need set the uxPriority to the saved base priority
                             * so that the task's priority gets restored to the priority
                             * before any inheritance or raising. */
                            pxTCB->uxPriority = pxSavedPriority->uxBasePriority;
                        }
                        else
                        {
                            /* The task may have inherited a priority before prvTaskPriorityRaise()
                             * was called. Thus, we need to restore uxPriority to the
                             * "saved uxPriority" so that the task still retains that
                             * inherited priority. */
                            pxTCB->uxPriority = pxSavedPriority->uxPriority;
                        }

                        uxNewPriority = pxTCB->uxPriority;

                        if( uxNewPriority < uxPriorityUsedOnEntry )
                        {
                            /* Setting the priority of the running task down means
                             * there may now be another task of higher priority that
                             * is ready to execute. */
                            xYieldRequired = pdTRUE;
                        }

                        /* Only reset the event list item value if the value is not
                         * being used for anything else. */
                        if( ( listGET_LIST_ITEM_VALUE( &( pxTCB->xEventListItem ) ) & taskEVENT_LIST_ITEM_VALUE_IN_USE ) == 0UL )
                        {
                            listSET_LIST_ITEM_VALUE( &( pxTCB->xEventListItem ), ( ( TickType_t ) configMAX_PRIORITIES - ( TickType_t ) uxNewPriority ) ); /*lint !e961 MISRA exception as the casts are only redundant for some ports. */
                        }

                        /* If the task is in the blocked or suspended list we need do
                         * nothing more than change its priority variable. However, if
                         * the task is in a ready list it needs to be removed and placed
                         * in the list appropriate to its new priority. */
                        if( listIS_CONTAINED_WITHIN( &( pxReadyTasksLists[ uxPriorityUsedOnEntry ] ), &( pxTCB->xStateListItem ) ) != pdFALSE )
                        {
                            /* The task is currently in its ready list - remove before
                             * adding it to its new ready list.  As we are in a critical
                             * section we can do this even if the scheduler is suspended. */
                            if( uxListRemove( &( pxTCB->xStateListItem ) ) == ( UBaseType_t ) 0 )
                            {
                                /* It is known that the task is in its ready list so
                                 * there is no need to check again and the port level
                                 * reset macro can be called directly. */
                                portRESET_READY_PRIORITY( uxPriorityUsedOnEntry, uxTopReadyPriority );
                            }

                            prvAddTaskToReadyList( pxTCB );
                        }

                        if( xYieldRequired != pdFALSE )
                        {
                            taskYIELD_IF_USING_PREEMPTION();
                        }
                    }
                }
            }
            #else /* if ( configUSE_MUTEXES == 1 ) */
            {
                vTaskPrioritySet( NULL, pxSavedPriority->uxPriority );
            }
            #endif /* if ( configUSE_MUTEXES == 1 ) */
        }
        #if CONFIG_FREERTOS_SMP
            taskEXIT_CRITICAL();
        #else
            taskEXIT_CRITICAL( &xKernelLock );
        #endif
    }

#endif /* ( INCLUDE_vTaskPrioritySet == 1 ) */
/*----------------------------------------------------------*/

/* --------------------------------------------- TLSP Deletion Callbacks -------------------------------------------- */

#if CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS

    void vTaskSetThreadLocalStoragePointerAndDelCallback( TaskHandle_t xTaskToSet,
                                                          BaseType_t xIndex,
                                                          void * pvValue,
                                                          TlsDeleteCallbackFunction_t pvDelCallback )
    {
        /* If TLSP deletion callbacks are enabled, then configNUM_THREAD_LOCAL_STORAGE_POINTERS
         * is doubled in size so that the latter half of the pvThreadLocalStoragePointers
         * stores the deletion callbacks. */
        if( xIndex < ( configNUM_THREAD_LOCAL_STORAGE_POINTERS / 2 ) )
        {
            TCB_t * pxTCB;

            #if ( configNUM_CORES > 1 )
            {
                /* For SMP, we need a critical section as another core could also
                 * update this task's TLSP at the same time. */
                #if CONFIG_FREERTOS_SMP
                {
                    taskENTER_CRITICAL();
                }
                #else /* CONFIG_FREERTOS_SMP */
                {
                    taskENTER_CRITICAL( &xKernelLock );
                }
                #endif /* CONFIG_FREERTOS_SMP */
            }
            #endif /* configNUM_CORES > 1 */

            pxTCB = prvGetTCBFromHandle( xTaskToSet );
            /* Store the TLSP by indexing the first half of the array */
            pxTCB->pvThreadLocalStoragePointers[ xIndex ] = pvValue;

            /* Store the TLSP deletion callback by indexing the second half
             * of the array. */
            pxTCB->pvThreadLocalStoragePointers[ ( xIndex + ( configNUM_THREAD_LOCAL_STORAGE_POINTERS / 2 ) ) ] = ( void * ) pvDelCallback;

            #if ( configNUM_CORES > 1 )
            {
                #if CONFIG_FREERTOS_SMP
                {
                    taskEXIT_CRITICAL();
                }
                #else /* CONFIG_FREERTOS_SMP */
                {
                    taskEXIT_CRITICAL( &xKernelLock );
                }
                #endif /* CONFIG_FREERTOS_SMP */
            }
            #endif /* configNUM_CORES > 1 */
        }
    }

#endif /* CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS */
/*----------------------------------------------------------*/

/* ----------------------------------------------------- Newlib ----------------------------------------------------- */

#if ( configUSE_NEWLIB_REENTRANT == 1 )

/**
 * @brief Get reentrancy structure of the current task
 *
 * - This funciton is required by newlib (when __DYNAMIC_REENT__ is enabled)
 * - It will return a pointer to the current task's reent struct
 * - If FreeRTOS is not running, it will return the global reent struct
 *
 * @return Pointer to a the (current taks's)/(globa) reent struct
 */
    struct _reent * __getreent( void )
    {
        /* No lock needed because if this changes, we won't be running anymore. */
        TCB_t * pxCurTask = xTaskGetCurrentTaskHandle();
        struct _reent * ret;

        if( pxCurTask == NULL )
        {
            /* No task running. Return global struct. */
            ret = _GLOBAL_REENT;
        }
        else
        {
            /* We have a task; return its reentrant struct. */
            #if CONFIG_FREERTOS_USE_KERNEL_10_5_1
            {
                ret = &pxCurTask->xTLSBlock;
            }
            #else /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
            {
                ret = &pxCurTask->xNewLib_reent;
            }
            #endif /* CONFIG_FREERTOS_USE_KERNEL_10_5_1 */
        }

        return ret;
    }

#endif /* configUSE_NEWLIB_REENTRANT == 1 */

/* -------------------------------------------------- Task Snapshot ------------------------------------------------- */

/**
 * @brief List of all task lists in FreeRTOS
 *
 * @note There are currently differing number of task list between SMP FreeRTOS and ESP-IDF FreeRTOS
 */
static List_t * non_ready_task_lists[] = {
    #ifdef CONFIG_FREERTOS_SMP
        &xPendingReadyList,
    #else /* CONFIG_FREERTOS_SMP */
        &xPendingReadyList[ 0 ],
        #ifndef CONFIG_FREERTOS_UNICORE
            &xPendingReadyList[ 1 ],
        #endif /* CONFIG_FREERTOS_UNICORE */
    #endif /* CONFIG_FREERTOS_SMP */
    &xDelayedTaskList1,
    &xDelayedTaskList2,
    #if ( INCLUDE_vTaskDelete == 1 )
        &xTasksWaitingTermination,
    #endif
    #if ( INCLUDE_vTaskSuspend == 1 )
        &xSuspendedTaskList,
    #endif
};
/*----------------------------------------------------------*/

/**
 * @brief Get the next task list to traverse
 *
 * - Given a particular task list, this function returns the next task to traverse.
 * - The task lists are returned in the following precedence
 *      - Ready lists (highest to lowers priority)
 *      - Pending ready list(s)
 *      - Delayed list 1
 *      - Delayed list 2
 *      - Waiting termination list
 *      - Suspended list
 *
 * @param pxCurTaskList Previously traversed task list (or NULL if obtaining the first task list)
 * @return List_t* The next task list to traverse (or NULL of all task lists have been traversed)
 */
static List_t * pxGetNextTaskList( List_t * pxCurTaskList )
{
    List_t * pxNextTaskList = NULL;

    /* No Current List. Start from the highest priority ready task list */
    if( pxCurTaskList == NULL )
    {
        pxNextTaskList = &pxReadyTasksLists[ configMAX_PRIORITIES - 1 ];
    }
    /* Current list is one of the ready task lists. Find the current priority, and return the next lower priority ready task list */
    else if( ( pxCurTaskList >= &pxReadyTasksLists[ 0 ] ) && ( pxCurTaskList <= &pxReadyTasksLists[ configMAX_PRIORITIES - 1 ] ) )
    {
        /* Find the current priority */
        int cur_priority;

        for( cur_priority = configMAX_PRIORITIES - 1; cur_priority >= 0; cur_priority-- )
        {
            if( pxCurTaskList == &pxReadyTasksLists[ cur_priority ] )
            {
                break;
            }
        }

        /* Return the ready task list at (cur_priority - 1), or the pending ready task list */
        if( cur_priority > 0 )
        {
            pxNextTaskList = &pxReadyTasksLists[ cur_priority - 1 ];
        }
        /* We've reached the end of the Ready Task Lists.  We get the next list from the non-ready task lists */
        else if( cur_priority == 0 )
        {
            pxNextTaskList = non_ready_task_lists[ 0 ];
        }
        else
        {
            abort(); /* This should never occur */
        }
    }

    /* Current list is one of the non-ready task lists. Fetch the next non-ready task list */
    if( pxNextTaskList == NULL )
    {
        int cur_list_idx;
        const int num_non_ready_task_lists = ( sizeof( non_ready_task_lists ) / sizeof( List_t * ) );

        /* Note: - 1 so that if the current list is the last on non_ready_task_lists[], the next list will return NULL */
        for( cur_list_idx = 0; cur_list_idx < num_non_ready_task_lists - 1; cur_list_idx++ )
        {
            if( pxCurTaskList == non_ready_task_lists[ cur_list_idx ] )
            {
                pxNextTaskList = non_ready_task_lists[ cur_list_idx + 1 ];
                break;
            }
        }
    }

    return pxNextTaskList;
}
/*----------------------------------------------------------*/

TaskHandle_t pxTaskGetNext( TaskHandle_t pxTask )
{
    TCB_t * pxTCB = ( TCB_t * ) pxTask;

    /* Check current task is valid */
    if( ( pxTCB != NULL ) && !portVALID_TCB_MEM( pxTCB ) )
    {
        return NULL;
    }

    List_t * pxCurTaskList;
    const ListItem_t * pxCurListItem;

    if( pxTCB == NULL )
    {
        /* Starting traversal for the first time */
        pxCurTaskList = pxGetNextTaskList( NULL );
        pxCurListItem = listGET_END_MARKER( pxCurTaskList );
    }
    else
    {
        /* Continuing traversal */
        pxCurTaskList = listLIST_ITEM_CONTAINER( &pxTCB->xStateListItem );
        pxCurListItem = &pxTCB->xStateListItem;
    }

    ListItem_t * pxNextListItem = NULL;

    if( pxCurListItem->pxNext == listGET_END_MARKER( pxCurTaskList ) )
    {
        List_t * pxNextTaskList = pxGetNextTaskList( pxCurTaskList );

        while( pxNextTaskList != NULL )
        {
            if( !listLIST_IS_EMPTY( pxNextTaskList ) )
            {
                /* Get the first item in the next task list */
                pxNextListItem = listGET_HEAD_ENTRY( pxNextTaskList );
                break;
            }

            /* Task list is empty. Get the next task list */
            pxNextTaskList = pxGetNextTaskList( pxNextTaskList );
        }
    }
    else
    {
        /*There are still more items in the current task list. Get the next item */
        pxNextListItem = listGET_NEXT( pxCurListItem );
    }

    TCB_t * pxNextTCB;

    if( pxNextListItem == NULL )
    {
        pxNextTCB = NULL;
    }
    else
    {
        pxNextTCB = ( TCB_t * ) listGET_LIST_ITEM_OWNER( pxNextListItem );
    }

    return pxNextTCB;
}
/*----------------------------------------------------------*/

BaseType_t vTaskGetSnapshot( TaskHandle_t pxTask,
                             TaskSnapshot_t * pxTaskSnapshot )
{
    if( ( portVALID_TCB_MEM( pxTask ) == false ) || ( pxTaskSnapshot == NULL ) )
    {
        return pdFALSE;
    }

    TCB_t * pxTCB = ( TCB_t * ) pxTask;
    pxTaskSnapshot->pxTCB = pxTCB;
    pxTaskSnapshot->pxTopOfStack = ( StackType_t * ) pxTCB->pxTopOfStack;
    pxTaskSnapshot->pxEndOfStack = ( StackType_t * ) pxTCB->pxEndOfStack;
    return pdTRUE;
}
/*----------------------------------------------------------*/

UBaseType_t uxTaskGetSnapshotAll( TaskSnapshot_t * const pxTaskSnapshotArray,
                                  const UBaseType_t uxArrayLength,
                                  UBaseType_t * const pxTCBSize )
{
    UBaseType_t uxArrayNumFilled = 0;

    /*Traverse all of the tasks lists */
    List_t * pxCurTaskList = pxGetNextTaskList( NULL ); /*Get the first task list */

    while( pxCurTaskList != NULL && uxArrayNumFilled < uxArrayLength )
    {
        if( !listLIST_IS_EMPTY( pxCurTaskList ) )
        {
            const ListItem_t * pxCurListItem;
            /*Walk each task on the current task list */
            pxCurListItem = listGET_HEAD_ENTRY( pxCurTaskList );

            while( pxCurListItem != listGET_END_MARKER( pxCurTaskList ) )
            {
                TCB_t * pxTCB = ( TCB_t * ) listGET_LIST_ITEM_OWNER( pxCurListItem );
                vTaskGetSnapshot( ( TaskHandle_t ) pxTCB, &pxTaskSnapshotArray[ uxArrayNumFilled ] );
                uxArrayNumFilled++;

                if( !( uxArrayNumFilled < uxArrayLength ) )
                {
                    break;
                }

                pxCurListItem = listGET_NEXT( pxCurListItem );
            }
        }

        /*Get the next task list */
        pxCurTaskList = pxGetNextTaskList( pxCurTaskList );
    }

    *pxTCBSize = sizeof( TCB_t );
    return uxArrayNumFilled;
}
/*----------------------------------------------------------*/

/* ----------------------------------------------------- OpenOCD ---------------------------------------------------- */

#if CONFIG_FREERTOS_DEBUG_OCDAWARE

/**
 * Debug param indexes. DO NOT change the order. OpenOCD uses the same indexes
 * Entries in FreeRTOS_openocd_params must match the order of these indexes
 */
    enum
    {
        ESP_FREERTOS_DEBUG_TABLE_SIZE = 0,
        ESP_FREERTOS_DEBUG_TABLE_VERSION,
        ESP_FREERTOS_DEBUG_KERNEL_VER_MAJOR,
        ESP_FREERTOS_DEBUG_KERNEL_VER_MINOR,
        ESP_FREERTOS_DEBUG_KERNEL_VER_BUILD,
        ESP_FREERTOS_DEBUG_UX_TOP_USED_PIORITY,
        ESP_FREERTOS_DEBUG_PX_TOP_OF_STACK,
        ESP_FREERTOS_DEBUG_PC_TASK_NAME,
        /* New entries must be inserted here */
        ESP_FREERTOS_DEBUG_TABLE_END,
    };

    const DRAM_ATTR uint8_t FreeRTOS_openocd_params[ ESP_FREERTOS_DEBUG_TABLE_END ] = {
        ESP_FREERTOS_DEBUG_TABLE_END, /* table size */
        1,                            /* table version */
        tskKERNEL_VERSION_MAJOR,
        tskKERNEL_VERSION_MINOR,
        tskKERNEL_VERSION_BUILD,
        configMAX_PRIORITIES - 1,        /* uxTopUsedPriority */
        offsetof( TCB_t, pxTopOfStack ), /* thread_stack_offset; */
        offsetof( TCB_t, pcTaskName ),   /* thread_name_offset; */
    };

#endif /* CONFIG_FREERTOS_DEBUG_OCDAWARE */
/*----------------------------------------------------------*/
