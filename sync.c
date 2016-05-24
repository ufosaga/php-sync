/*
 Direct port of the cross platform 'sync' library:  https://github.com/cubiclesoft/cross-platform-cpp
 This source file is under the MIT, LGPL, or version 3.01 of the PHP license, your choice.
 (C) 2014 CubicleSoft.  All rights reserved.
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "zend_exceptions.h"
#include "ext/standard/info.h"
#include <stdint.h>
#include "php_sync.h"

/* {{{ sync_module_entry
 */
zend_module_entry sync_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
        STANDARD_MODULE_HEADER,
#endif
        "sync",
        NULL,
        PHP_MINIT(sync),
        PHP_MSHUTDOWN(sync),
        NULL,
        NULL,
        PHP_MINFO(sync),
#if ZEND_MODULE_API_NO >= 20010901
        PHP_SYNC_VERSION,
#endif
        STANDARD_MODULE_PROPERTIES };
/* }}} */

#ifdef COMPILE_DL_SYNC
ZEND_GET_MODULE(sync)
#endif

#if ZEND_MODULE_API_NO < 20151012
#define TYPE_STR_LEN     int
#define TYPE_OBJ_CREATE  zend_object_value
#define TYPE_OBJ_FREE    void *
#define fetch_sync_object(t) \
        (sync_##t##_object *) zend_object_store_get_object(getThis() TSRMLS_CC);
#else
#define TYPE_STR_LEN     size_t
#define TYPE_OBJ_CREATE  zend_object *
#define TYPE_OBJ_FREE    zend_object *
#define fetch_sync_object(t) \
        ((sync_##t##_object *) ((char *) (Z_OBJ_P(getThis())) - XtOffsetOf(sync_##t##_object, std)))
#endif

#define fetch_sync_Mutex()         fetch_sync_object(Mutex)
#define fetch_sync_Semaphore()     fetch_sync_object(Semaphore)
#define fetch_sync_Event()         fetch_sync_object(Event)
#define fetch_sync_ReaderWriter()  fetch_sync_object(ReaderWriter)

#ifndef INFINITE
#	define INFINITE   0xFFFFFFFF
#endif

/* Handlers for Sync */
zend_object_handlers Mutex_handlers;
zend_object_handlers Semaphore_handlers;
zend_object_handlers Event_handlers;
zend_object_handlers ReaderWriter_handlers;

/* Define some generic functions used several places. */

/* POSIX pthreads. */
sync_ThreadIDType sync_GetCurrentThreadID()
{
    return pthread_self();
}

uint64_t sync_GetUnixMicrosecondTime()
{
    struct timeval TempTime;

    if (gettimeofday(&TempTime, NULL))
        return 0;

    return (uint64_t) ((uint64_t) TempTime.tv_sec * (uint64_t) 1000000 + (uint64_t) TempTime.tv_usec);
}

/* Simplifies timer-based mutex/semaphore acquisition. */
int sync_WaitForSemaphore(sem_t *SemPtr, uint32_t Wait)
{
    if (SemPtr == SEM_FAILED)
        return 0;

    if (Wait == INFINITE) {
        while (sem_wait(SemPtr) < 0) {
            if (errno != EINTR)
                return 0;
        }
    } else if (Wait == 0) {
        while (sem_trywait(SemPtr) < 0) {
            if (errno != EINTR)
                return 0;
        }
    } else {
        struct timespec TempTime;

        if (clock_gettime(CLOCK_REALTIME, &TempTime) == -1)
            return 0;
        TempTime.tv_sec += Wait / 1000;
        TempTime.tv_nsec += (Wait % 1000) * 1000000;
        TempTime.tv_sec += TempTime.tv_nsec / 1000000000;
        TempTime.tv_nsec = TempTime.tv_nsec % 1000000000;

        while (sem_timedwait(SemPtr, &TempTime) < 0) {
            if (errno != EINTR)
                return 0;
        }
    }

    return 1;
}

/* Mutex */
PHP_SYNC_API zend_class_entry *sync_Mutex_ce;

void sync_Mutex_free_object(TYPE_OBJ_FREE object TSRMLS_DC);

/* {{{ Initialize internal Mutex structure. */
TYPE_OBJ_CREATE sync_Mutex_create_object(zend_class_entry *ce TSRMLS_DC)
{
#if ZEND_MODULE_API_NO < 20151012
    zend_object_value retval;
#endif
    /* Create the object. */
    sync_Mutex_object *obj = (sync_Mutex_object *) ecalloc(1, sizeof(sync_Mutex_object));

    /* Initialize Zend. */
    zend_object_std_init(&obj->std, ce TSRMLS_CC);
    object_properties_init(&obj->std, ce);

    /* Initialize destructor callback. */
#if ZEND_MODULE_API_NO < 20151012
    retval.handle = zend_objects_store_put((void *) obj,
            (zend_objects_store_dtor_t) zend_objects_destroy_object, sync_Mutex_free_object,
            NULL TSRMLS_CC);
    retval.handlers = zend_get_std_object_handlers();
#else
    memcpy(&Mutex_handlers, zend_get_std_object_handlers(), sizeof(Mutex_handlers));
    Mutex_handlers.free_obj = sync_Mutex_free_object;
    obj->std.handlers = &Mutex_handlers;
#endif

    /* Initialize Mutex information. */
    obj->MxSemMutex = SEM_FAILED;
    obj->MxAllocated = 0;
    pthread_mutex_init(&obj->MxPthreadCritSection, NULL);
    obj->MxOwnerID = 0;
    obj->MxCount = 0;

#if ZEND_MODULE_API_NO < 20151012
    return retval;
#else
    return &obj->std;
#endif
}
/* }}} */

/* {{{ Unlocks a mutex. */
int sync_Mutex_unlock_internal(sync_Mutex_object *obj, int all)
{
    if (pthread_mutex_lock(&obj->MxPthreadCritSection) != 0)
        return 0;

    /* Make sure the mutex exists and make sure it is owned by the calling thread. */
    if (obj->MxSemMutex == SEM_FAILED || obj->MxOwnerID != sync_GetCurrentThreadID()) {
        pthread_mutex_unlock(&obj->MxPthreadCritSection);
        return 0;
    }

    if (all)
        obj->MxCount = 1;
    obj->MxCount--;
    if (!obj->MxCount) {
        obj->MxOwnerID = 0;

        /* Release the mutex. */
        sem_post(obj->MxSemMutex);
    }

    pthread_mutex_unlock(&obj->MxPthreadCritSection);
    return 1;
}
/* }}} */

/* {{{ Free internal Mutex structure. */
void sync_Mutex_free_object(TYPE_OBJ_FREE object TSRMLS_DC)
{
    sync_Mutex_object *obj = (sync_Mutex_object *) object;

    sync_Mutex_unlock_internal(obj, 1);

    if (obj->MxSemMutex != SEM_FAILED) {
        if (obj->MxAllocated)
        efree(obj->MxSemMutex);
        else
        sem_close(obj->MxSemMutex);
    }

    pthread_mutex_destroy(&obj->MxPthreadCritSection);
}
/* }}} */

/* {{{ proto void Sync_Mutex::__construct([string $name = null])
 Constructs a named or unnamed mutex object. */
PHP_METHOD(sync_Mutex, __construct)
{
    char *name = NULL;
    char *name2 = NULL;
    TYPE_STR_LEN name_len = 0;

    sync_Mutex_object *obj = fetch_sync_Mutex();
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &name, &name_len) == FAILURE) {
        return;
    }

    if (name != NULL) {
        name2 = emalloc(name_len + 20);

        sprintf(name2, "/Sync_Mutex_%s_0", name);
        obj->MxSemMutex = sem_open(name2, O_CREAT, 0666, 1);

        efree(name2);
    } else {
        obj->MxAllocated = 1;
        obj->MxSemMutex = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemMutex, 0, 1);
    }

    if (obj->MxSemMutex == SEM_FAILED) {
        zend_throw_exception(zend_exception_get_default(TSRMLS_C), "Mutex could not be created",
                0 TSRMLS_CC);
        return;
    }
}
/* }}} */

/* {{{ proto bool Sync_Mutex::lock([int $wait = -1])
 Locks a mutex object. */
PHP_METHOD(sync_Mutex, lock)
{
    long wait = -1;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &wait) == FAILURE)
        return;

    sync_Mutex_object *obj = fetch_sync_Mutex();
    if (pthread_mutex_lock(&obj->MxPthreadCritSection) != 0) {
        zend_throw_exception(zend_exception_get_default(TSRMLS_C),
                "Unable to acquire mutex critical section", 0 TSRMLS_CC);
        RETURN_FALSE;
    }

    /* Check to see if this mutex is already owned by the calling thread. */
    if (obj->MxOwnerID == sync_GetCurrentThreadID()) {
        obj->MxCount++;
        pthread_mutex_unlock(&obj->MxPthreadCritSection);

        RETURN_TRUE;
    }

    pthread_mutex_unlock(&obj->MxPthreadCritSection);

    if (!sync_WaitForSemaphore(obj->MxSemMutex, (uint32_t) (wait > -1 ? wait : INFINITE)))
        RETURN_FALSE;

    pthread_mutex_lock(&obj->MxPthreadCritSection);
    obj->MxOwnerID = sync_GetCurrentThreadID();
    obj->MxCount = 1;
    pthread_mutex_unlock(&obj->MxPthreadCritSection);

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Mutex::unlock([bool $all = false])
 Unlocks a mutex object. */
PHP_METHOD(sync_Mutex, unlock)
{
    long all = 0;
    sync_Mutex_object *obj = fetch_sync_Mutex();
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &all) == FAILURE)
        return;

    if (!sync_Mutex_unlock_internal(obj, all))
        RETURN_FALSE;

    RETURN_TRUE;
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_mutex___construct, 0, 0, 0) ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_mutex_lock, 0, 0, 0) ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_mutex_unlock, 0, 0, 0) ZEND_ARG_INFO(0, all)
ZEND_END_ARG_INFO()

static const zend_function_entry sync_Mutex_methods[] = {
PHP_ME(sync_Mutex, __construct, arginfo_sync_mutex___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
PHP_ME(sync_Mutex, lock, arginfo_sync_mutex_lock, ZEND_ACC_PUBLIC)
PHP_ME(sync_Mutex, unlock, arginfo_sync_mutex_unlock, ZEND_ACC_PUBLIC)
PHP_FE_END };

/* Semaphore */
PHP_SYNC_API zend_class_entry *sync_Semaphore_ce;

void sync_Semaphore_free_object(TYPE_OBJ_FREE object TSRMLS_DC);

/* {{{ Initialize internal Semaphore structure. */
TYPE_OBJ_CREATE sync_Semaphore_create_object(zend_class_entry *ce TSRMLS_DC)
{
#if ZEND_MODULE_API_NO < 20151012
    zend_object_value retval;
#endif

    /* Create the object. */
    sync_Semaphore_object *obj = (sync_Semaphore_object *) ecalloc(1,
            sizeof(sync_Semaphore_object));

    /* Initialize Zend. */
    zend_object_std_init(&obj->std, ce TSRMLS_CC);
    object_properties_init(&obj->std, ce);

    /* Initialize destructor callback. */
#if ZEND_MODULE_API_NO < 20151012
    retval.handle = zend_objects_store_put((void *) obj,
            (zend_objects_store_dtor_t) zend_objects_destroy_object, sync_Semaphore_free_object,
            NULL TSRMLS_CC);
    retval.handlers = zend_get_std_object_handlers();
#else
    memcpy(&Semaphore_handlers, zend_get_std_object_handlers(), sizeof(Semaphore_handlers));
    Semaphore_handlers.free_obj = sync_Semaphore_free_object;
    obj->std.handlers = &Semaphore_handlers;
#endif

    /* Initialize Semaphore information. */
    obj->MxSemSemaphore = SEM_FAILED;
    obj->MxAllocated = 0;
    obj->MxAutoUnlock = 0;
    obj->MxCount = 0;

#if ZEND_MODULE_API_NO < 20151012
    return retval;
#else
    return &obj->std;
#endif
}
/* }}} */

/* {{{ Free internal Semaphore structure. */
void sync_Semaphore_free_object(TYPE_OBJ_FREE object TSRMLS_DC)
{
    sync_Semaphore_object *obj = (sync_Semaphore_object *) object;

    if (obj->MxAutoUnlock) {
        while (obj->MxCount) {
            sem_post(obj->MxSemSemaphore);
            obj->MxCount--;
        }
    }
    if (obj->MxSemSemaphore != SEM_FAILED) {
        if (obj->MxAllocated)
        efree(obj->MxSemSemaphore);
        else
        sem_close(obj->MxSemSemaphore);
    }
}
/* }}} */

/* {{{ proto void Sync_Semaphore::__construct([string $name = null, [int $initialval = 1, [bool $autounlock = true]]])
 Constructs a named or unnamed semaphore object.  Don't set $autounlock to false unless you really know what you are doing. */
PHP_METHOD(sync_Semaphore, __construct)
{
    char *name = NULL;
    char *name2 = NULL;
    TYPE_STR_LEN name_len;
    long initialval = 1;
    long autounlock = 1;
    sync_Semaphore_object *obj = fetch_sync_Semaphore();
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sll", &name, &name_len, &initialval,
            &autounlock) == FAILURE)
        return;

    obj->MxAutoUnlock = (autounlock ? 1 : 0);

    if (name != NULL) {
        name2 = emalloc(name_len + 20);

        sprintf(name2, "/Sync_Semaphore_%s_0", name);
        obj->MxSemSemaphore = sem_open(name2, O_CREAT, 0666, (unsigned int) initialval);

        efree(name2);
    } else {
        obj->MxAllocated = 1;

        obj->MxSemSemaphore = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemSemaphore, 0, (unsigned int) initialval);
    }

    if (obj->MxSemSemaphore == SEM_FAILED) {
        zend_throw_exception(zend_exception_get_default(TSRMLS_C), "Semaphore could not be created",
                0 TSRMLS_CC);
        return;
    }
}
/* }}} */

/* {{{ proto bool Sync_Semaphore::lock([int $wait = -1])
 Locks a semaphore object. */
PHP_METHOD(sync_Semaphore, lock)
{
    long wait = -1;
    sync_Semaphore_object *obj = fetch_sync_Semaphore();
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &wait) == FAILURE)
        return;

    if (!sync_WaitForSemaphore(obj->MxSemSemaphore, (uint32_t) (wait > -1 ? wait : INFINITE)))
        RETURN_FALSE;
    if (obj->MxAutoUnlock)
        obj->MxCount++;

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Semaphore::unlock([int &$prevcount])
 Unlocks a semaphore object. */
PHP_METHOD(sync_Semaphore, unlock)
{
    int count = 0;
    int argc = ZEND_NUM_ARGS();
    sync_Semaphore_object *obj = fetch_sync_Semaphore();

#if ZEND_MODULE_API_NO < 20151012
    zval **zprevcount = NULL;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|Z", &zprevcount) == FAILURE) {
        return;
    }
#else
    zval *zprevcount = NULL;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|z/", &zprevcount) == FAILURE) {
        return;
    }
#endif

    /* Get the current value first. */
    sem_getvalue(obj->MxSemSemaphore, &count);

    /* Release the semaphore. */
    sem_post(obj->MxSemSemaphore);

    if (argc > 0) {
#if ZEND_MODULE_API_NO < 20151012
        zval_dtor(*zprevcount);
        ZVAL_LONG(*zprevcount, count);
#else
        zval_dtor(zprevcount);
        ZVAL_LONG(zprevcount, count);
#endif
    }

    if (obj->MxAutoUnlock)
        obj->MxCount--;

    RETURN_TRUE;
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_semaphore___construct, 0, 0, 0) ZEND_ARG_INFO(0, name)
ZEND_ARG_INFO(0, initialval)
ZEND_ARG_INFO(0, autounlock)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_semaphore_lock, 0, 0, 0) ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_semaphore_unlock, 0, 0, 0) ZEND_ARG_INFO(1, prevcount)
ZEND_END_ARG_INFO()

static const zend_function_entry sync_Semaphore_methods[] =
        {
                        PHP_ME(sync_Semaphore, __construct, arginfo_sync_semaphore___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
                PHP_ME(sync_Semaphore, lock, arginfo_sync_semaphore_lock, ZEND_ACC_PUBLIC)
                PHP_ME(sync_Semaphore, unlock, arginfo_sync_semaphore_unlock, ZEND_ACC_PUBLIC)
                PHP_FE_END };

/* Event */
PHP_SYNC_API zend_class_entry *sync_Event_ce;

void sync_Event_free_object(TYPE_OBJ_FREE object TSRMLS_DC);

/* {{{ Initialize internal Event structure. */
TYPE_OBJ_CREATE sync_Event_create_object(zend_class_entry *ce TSRMLS_DC)
{
#if ZEND_MODULE_API_NO < 20151012
    zend_object_value retval;
#endif

    /* Create the object. */
    sync_Event_object *obj = (sync_Event_object *) ecalloc(1, sizeof(sync_Event_object));

    /* Initialize Zend. */
    zend_object_std_init(&obj->std, ce TSRMLS_CC);
    object_properties_init(&obj->std, ce);

    /* Initialize destructor callback. */
#if ZEND_MODULE_API_NO < 20151012
    retval.handle = zend_objects_store_put((void *) obj,
            (zend_objects_store_dtor_t) zend_objects_destroy_object, sync_Event_free_object,
            NULL TSRMLS_CC);
    retval.handlers = zend_get_std_object_handlers();
#else
    memcpy(&Event_handlers, zend_get_std_object_handlers(), sizeof(Event_handlers));
    Event_handlers.free_obj = sync_Event_free_object;
    obj->std.handlers = &Event_handlers;
#endif

    /* Initialize Event information. */
    obj->MxSemWaitMutex = SEM_FAILED;
    obj->MxSemWaitEvent = SEM_FAILED;
    obj->MxSemWaitCount = SEM_FAILED;
    obj->MxSemWaitStatus = SEM_FAILED;
    obj->MxAllocated = 0;
    obj->MxManual = 0;

#if ZEND_MODULE_API_NO < 20151012
    return retval;
#else
    return &obj->std;
#endif
}
/* }}} */

/* {{{ Free internal Event structure. */
void sync_Event_free_object(TYPE_OBJ_FREE object TSRMLS_DC)
{
    sync_Event_object *obj = (sync_Event_object *) object;

    if (obj->MxAllocated) {
        if (obj->MxSemWaitStatus != SEM_FAILED)
        efree(obj->MxSemWaitStatus);
        if (obj->MxSemWaitCount != SEM_FAILED)
        efree(obj->MxSemWaitCount);
        if (obj->MxSemWaitEvent != SEM_FAILED)
        efree(obj->MxSemWaitEvent);
        if (obj->MxSemWaitMutex != SEM_FAILED)
        efree(obj->MxSemWaitMutex);
    } else {
        if (obj->MxSemWaitStatus != SEM_FAILED)
        sem_close(obj->MxSemWaitStatus);
        if (obj->MxSemWaitCount != SEM_FAILED)
        sem_close(obj->MxSemWaitCount);
        if (obj->MxSemWaitEvent != SEM_FAILED)
        sem_close(obj->MxSemWaitEvent);
        if (obj->MxSemWaitMutex != SEM_FAILED)
        sem_close(obj->MxSemWaitMutex);
    }
}
/* }}} */

/* {{{ proto void Sync_Event::__construct([string $name = null, [bool $manual = false]])
 Constructs a named or unnamed event object. */
PHP_METHOD(sync_Event, __construct)
{
    char *name = NULL;
    char *name2 = NULL;
    long manual = 0;
    TYPE_STR_LEN name_len;
    sync_Event_object *obj = fetch_sync_Event();

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sl", &name, &name_len, &manual)
            == FAILURE)
        return;

    obj->MxManual = (manual ? 1 : 0);

    if (name != NULL) {
        name2 = emalloc(name_len + 20);

        sprintf(name2, "/Sync_Event_%s_0", name);
        obj->MxSemWaitMutex = sem_open(name2, O_CREAT, 0666, 1);
        sprintf(name2, "/Sync_Event_%s_1", name);
        obj->MxSemWaitEvent = sem_open(name2, O_CREAT, 0666, 0);

        if (manual) {
            sprintf(name2, "/Sync_Event_%s_2", name);
            obj->MxSemWaitCount = sem_open(name2, O_CREAT, 0666, 0);

            sprintf(name2, "/Sync_Event_%s_3", name);
            obj->MxSemWaitStatus = sem_open(name2, O_CREAT, 0666, 0);
        }

        efree(name2);
    } else {
        obj->MxAllocated = 1;

        obj->MxSemWaitMutex = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemWaitMutex, 0, 1);

        obj->MxSemWaitEvent = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemWaitEvent, 0, 0);

        if (manual) {
            obj->MxSemWaitCount = (sem_t *) ecalloc(1, sizeof(sem_t));
            sem_init(obj->MxSemWaitCount, 0, 0);

            obj->MxSemWaitStatus = (sem_t *) ecalloc(1, sizeof(sem_t));
            sem_init(obj->MxSemWaitStatus, 0, 0);
        }
    }

    if (obj->MxSemWaitMutex == SEM_FAILED || obj->MxSemWaitEvent == SEM_FAILED
            || (manual && (obj->MxSemWaitCount == SEM_FAILED || obj->MxSemWaitStatus == SEM_FAILED))) {
        zend_throw_exception(zend_exception_get_default(TSRMLS_C),
                "Event object could not be created", 0 TSRMLS_CC);
        return;
    }
}
/* }}} */

/* {{{ proto bool Sync_Event::wait([int $wait = -1])
 Waits for an event object to fire. */
PHP_METHOD(sync_Event, wait)
{
    long wait = -1;
    int Val, Result;
    uint32_t WaitAmt;
    uint64_t StartTime, CurrTime;
    sync_Event_object *obj = fetch_sync_Event();
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &wait) == FAILURE)
        return;

    WaitAmt = (uint32_t) (wait > -1 ? wait : INFINITE);

    /* Get current time in milliseconds. */
    StartTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);

    if (!obj->MxManual)
        CurrTime = StartTime;
    else {
        /* Lock the mutex. */
        if (!sync_WaitForSemaphore(obj->MxSemWaitMutex, WaitAmt))
            RETURN_FALSE
        ;

        /* Get the status.  If it is 1, then the event has been fired. */
        sem_getvalue(obj->MxSemWaitStatus, &Val);
        if (Val == 1) {
            sem_post(obj->MxSemWaitMutex);
            RETURN_TRUE;
        }

        /* Increment the wait count. */
        CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
        if (WaitAmt < CurrTime - StartTime) {
            sem_post(obj->MxSemWaitMutex);

            RETURN_FALSE;
        }
        sem_post(obj->MxSemWaitCount);

        /* Release the mutex. */
        sem_post(obj->MxSemWaitMutex);
    }

    /* Wait for the semaphore. */
    Result = sync_WaitForSemaphore(obj->MxSemWaitEvent, WaitAmt - (CurrTime - StartTime));

    if (obj->MxManual) {
        /* Lock the mutex. */
        sync_WaitForSemaphore(obj->MxSemWaitMutex, INFINITE);

        /* Decrease the wait count. */
        sync_WaitForSemaphore(obj->MxSemWaitCount, INFINITE);

        /* Release the mutex. */
        sem_post(obj->MxSemWaitMutex);
    }

    if (!Result)
        RETURN_FALSE;

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Event::fire()
 Lets a thread through that is waiting.  Lets multiple threads through that are waiting if the event object is 'manual'. */
PHP_METHOD(sync_Event, fire)
{
    int x, Val;

    sync_Event_object *obj = fetch_sync_Event();

    if (obj->MxManual) {
        /* Lock the mutex. */
        if (!sync_WaitForSemaphore(obj->MxSemWaitMutex, INFINITE))
            RETURN_FALSE;

        /* Update the status.  No wait. */
        sem_getvalue(obj->MxSemWaitStatus, &Val);
        if (Val == 0)
            sem_post(obj->MxSemWaitStatus);

        /* Release the mutex. */
        sem_post(obj->MxSemWaitMutex);

        /* Release all waiting threads.  Might do too many sem_post() calls. */
        sem_getvalue(obj->MxSemWaitCount, &Val);
        for (x = 0; x < Val; x++)
            sem_post(obj->MxSemWaitEvent);
    } else {
        /* Release one thread. */
        sem_getvalue(obj->MxSemWaitEvent, &Val);
        if (Val == 0)
            sem_post(obj->MxSemWaitEvent);
    }

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Event::reset()
 Resets the event object state.  Only use when the event object is 'manual'. */
PHP_METHOD(sync_Event, reset)
{
    int Val = 0;
    sync_Event_object *obj = fetch_sync_Event();

    if (!obj->MxManual)
        RETURN_FALSE;

    /* Lock the mutex. */
    if (!sync_WaitForSemaphore(obj->MxSemWaitMutex, INFINITE))
        RETURN_FALSE;

    /* Restrict the semaphore.  Fixes the too many sem_post() calls in Fire(). */
    while (sync_WaitForSemaphore(obj->MxSemWaitEvent, 0)) {
    }

    /* Update the status.  Start waiting. */
    sem_getvalue(obj->MxSemWaitStatus, &Val);
    if (Val == 1)
        sync_WaitForSemaphore(obj->MxSemWaitStatus, INFINITE);

    /* Release the mutex. */
    sem_post(obj->MxSemWaitMutex);

    RETURN_TRUE;
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event___construct, 0, 0, 0) ZEND_ARG_INFO(0, name)
ZEND_ARG_INFO(0, manual)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event_wait, 0, 0, 0) ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event_fire, 0, 0, 0) ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event_reset, 0, 0, 0) ZEND_END_ARG_INFO()

static const zend_function_entry sync_Event_methods[] = {
PHP_ME(sync_Event, __construct, arginfo_sync_event___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
PHP_ME(sync_Event, wait, arginfo_sync_event_wait, ZEND_ACC_PUBLIC)
PHP_ME(sync_Event, fire, arginfo_sync_event_fire, ZEND_ACC_PUBLIC)
PHP_ME(sync_Event, reset, arginfo_sync_event_reset, ZEND_ACC_PUBLIC)
PHP_FE_END };

/* Reader-Writer */
PHP_SYNC_API zend_class_entry *sync_ReaderWriter_ce;

void sync_ReaderWriter_free_object(TYPE_OBJ_FREE object TSRMLS_DC);

/* {{{ Initialize internal Reader-Writer structure. */
TYPE_OBJ_CREATE sync_ReaderWriter_create_object(zend_class_entry *ce TSRMLS_DC)
{
#if ZEND_MODULE_API_NO < 20151012
    zend_object_value retval;
#endif

    /* Create the object. */
    sync_ReaderWriter_object *obj = (sync_ReaderWriter_object *) ecalloc(1,
            sizeof(sync_ReaderWriter_object));

    /* Initialize Zend. */
    zend_object_std_init(&obj->std, ce TSRMLS_CC);
    object_properties_init(&obj->std, ce);

    /* Initialize destructor callback. */
#if ZEND_MODULE_API_NO < 20151012
    retval.handle = zend_objects_store_put((void *) obj,
            (zend_objects_store_dtor_t) zend_objects_destroy_object, sync_ReaderWriter_free_object,
            NULL TSRMLS_CC);
    retval.handlers = zend_get_std_object_handlers();
#else
    memcpy(&ReaderWriter_handlers, zend_get_std_object_handlers(), sizeof(ReaderWriter_handlers));
    ReaderWriter_handlers.free_obj = sync_ReaderWriter_free_object;
    obj->std.handlers = &ReaderWriter_handlers;
#endif

    /* Initialize Reader-Writer information. */
    obj->MxSemRSemMutex = SEM_FAILED;
    obj->MxSemRSemaphore = SEM_FAILED;
    obj->MxSemRWaitEvent = SEM_FAILED;
    obj->MxSemWWaitMutex = SEM_FAILED;
    obj->MxAllocated = 0;

    obj->MxAutoUnlock = 1;
    obj->MxReadLocks = 0;
    obj->MxWriteLock = 0;

#if ZEND_MODULE_API_NO < 20151012
    return retval;
#else
    return &obj->std;
#endif
}
/* }}} */

/* {{{ Unlocks a read lock. */
int sync_ReaderWriter_readunlock_internal(sync_ReaderWriter_object *obj)
{
    int Result, Val;

    if (obj->MxSemRSemMutex == NULL || obj->MxSemRSemaphore == NULL || obj->MxSemRWaitEvent == NULL)
        return 0;

    /* Acquire the semaphore mutex. */
    if (!sync_WaitForSemaphore(obj->MxSemRSemMutex, INFINITE))
        return 0;

    if (obj->MxReadLocks)
        obj->MxReadLocks--;

    /* Release the semaphore. */
    Result = sem_post(obj->MxSemRSemaphore);
    if (Result != 0) {
        sem_post(obj->MxSemRSemMutex);

        return 0;
    }

    /* Update the event state. */
    sem_getvalue(obj->MxSemRSemaphore, &Val);
    if (Val == SEM_VALUE_MAX) {
        if (sem_post(obj->MxSemRWaitEvent) != 0) {
            sem_post(obj->MxSemRSemMutex);

            return 0;
        }
    }

    /* Release the semaphore mutex. */
    sem_post(obj->MxSemRSemMutex);

    return (Result == 0);
}
/* }}} */

/* {{{ Unlocks a write lock. */
int sync_ReaderWriter_writeunlock_internal(sync_ReaderWriter_object *obj)
{
    if (obj->MxSemWWaitMutex == NULL)
        return 0;

    obj->MxWriteLock = 0;

    /* Release the write lock. */
    sem_post(obj->MxSemWWaitMutex);

    return 1;
}
/* }}} */

/* {{{ Free internal Reader-Writer structure. */
void sync_ReaderWriter_free_object(TYPE_OBJ_FREE object TSRMLS_DC)
{
    sync_ReaderWriter_object *obj = (sync_ReaderWriter_object *) object;

    if (obj->MxAutoUnlock) {
        while (obj->MxReadLocks)
        sync_ReaderWriter_readunlock_internal(obj);

        if (obj->MxWriteLock)
        sync_ReaderWriter_writeunlock_internal(obj);
    }

    if (obj->MxAllocated) {
        if (obj->MxSemWWaitMutex != SEM_FAILED)
        efree(obj->MxSemWWaitMutex);
        if (obj->MxSemRWaitEvent != SEM_FAILED)
        efree(obj->MxSemRWaitEvent);
        if (obj->MxSemRSemaphore != SEM_FAILED)
        efree(obj->MxSemRSemaphore);
        if (obj->MxSemRSemMutex != SEM_FAILED)
        efree(obj->MxSemRSemMutex);
    } else {
        if (obj->MxSemWWaitMutex != SEM_FAILED)
        sem_close(obj->MxSemWWaitMutex);
        if (obj->MxSemRWaitEvent != SEM_FAILED)
        sem_close(obj->MxSemRWaitEvent);
        if (obj->MxSemRSemaphore != SEM_FAILED)
        sem_close(obj->MxSemRSemaphore);
        if (obj->MxSemRSemMutex != SEM_FAILED)
        sem_close(obj->MxSemRSemMutex);
    }
}
/* }}} */

/* {{{ proto void Sync_ReaderWriter::__construct([string $name = null, [bool $autounlock = true]])
 Constructs a named or unnamed reader-writer object.  Don't set $autounlock to false unless you really know what you are doing. */
PHP_METHOD(sync_ReaderWriter, __construct)
{
    char *name = NULL;
    char *name2 = NULL;
    TYPE_STR_LEN name_len;
    long autounlock = 1;
    sync_ReaderWriter_object *obj = fetch_sync_ReaderWriter();;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sl", &name, &name_len, &autounlock)
            == FAILURE)
        return;

    obj->MxAutoUnlock = (autounlock ? 1 : 0);

    if (name == NULL)
        name2 = NULL;
    else
        name2 = emalloc(name_len + 20);

    if (name2 != NULL) {
        sprintf(name2, "/Sync_ReadWrite_%s_0", name);
        obj->MxSemRSemMutex = sem_open(name2, O_CREAT, 0666, 1);
        sprintf(name2, "/Sync_ReadWrite_%s_1", name);
        obj->MxSemRSemaphore = sem_open(name2, O_CREAT, 0666, SEM_VALUE_MAX);
        sprintf(name2, "/Sync_ReadWrite_%s_2", name);
        obj->MxSemRWaitEvent = sem_open(name2, O_CREAT, 0666, 1);
        sprintf(name2, "/Sync_ReadWrite_%s_3", name);
        obj->MxSemWWaitMutex = sem_open(name2, O_CREAT, 0666, 1);
    } else {
        obj->MxAllocated = 1;

        obj->MxSemRSemMutex = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemRSemMutex, 0, 1);

        obj->MxSemRSemaphore = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemRSemaphore, 0, SEM_VALUE_MAX);

        obj->MxSemRWaitEvent = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemRWaitEvent, 0, 1);

        obj->MxSemWWaitMutex = (sem_t *) ecalloc(1, sizeof(sem_t));
        sem_init(obj->MxSemWWaitMutex, 0, 1);
    }

    if (name2 != NULL)
        efree(name2);

    if (obj->MxSemRSemMutex == SEM_FAILED || obj->MxSemRSemaphore == SEM_FAILED
            || obj->MxSemRWaitEvent == SEM_FAILED || obj->MxSemWWaitMutex == SEM_FAILED) {
        zend_throw_exception(zend_exception_get_default(TSRMLS_C),
                "Reader-Writer object could not be created", 0 TSRMLS_CC);
        return;
    }
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::readlock([int $wait = -1])
 Read locks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, readlock)
{
    int Val = 0;
    long wait = -1;
    uint32_t WaitAmt;
    uint64_t StartTime, CurrTime;
    sync_ReaderWriter_object *obj = fetch_sync_ReaderWriter();

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &wait) == FAILURE)
        return;

    WaitAmt = (uint32_t) (wait > -1 ? wait : INFINITE);

    /* Get current time in milliseconds. */
    StartTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);

    /* Acquire the write lock mutex.  Guarantees that readers can't starve the writer. */
    if (!sync_WaitForSemaphore(obj->MxSemWWaitMutex, WaitAmt))
        RETURN_FALSE;

    /* Acquire the semaphore mutex. */
    CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
    if (WaitAmt < CurrTime - StartTime) {
        sem_post(obj->MxSemWWaitMutex);

        RETURN_FALSE;
    }
    if (!sync_WaitForSemaphore(obj->MxSemRSemMutex, WaitAmt - (CurrTime - StartTime))) {
        sem_post(obj->MxSemWWaitMutex);

        RETURN_FALSE;
    }

    /* Acquire the semaphore. */
    CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
    if (WaitAmt < CurrTime - StartTime) {
        sem_post(obj->MxSemRSemMutex);
        sem_post(obj->MxSemWWaitMutex);

        RETURN_FALSE;
    }
    if (!sync_WaitForSemaphore(obj->MxSemRSemaphore, WaitAmt - (CurrTime - StartTime))) {
        sem_post(obj->MxSemRSemMutex);
        sem_post(obj->MxSemWWaitMutex);

        RETURN_FALSE;
    }

    /* Update the event state. */
    sem_getvalue(obj->MxSemRSemaphore, &Val);
    if (Val == SEM_VALUE_MAX - 1) {
        if (!sync_WaitForSemaphore(obj->MxSemRWaitEvent, INFINITE)) {
            sem_post(obj->MxSemRSemaphore);
            sem_post(obj->MxSemRSemMutex);
            sem_post(obj->MxSemWWaitMutex);

            RETURN_FALSE;
        }
    }

    obj->MxReadLocks++;

    /* Release the mutexes. */
    sem_post(obj->MxSemRSemMutex);
    sem_post(obj->MxSemWWaitMutex);

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::writelock([int $wait = -1])
 Write locks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, writelock)
{
    long wait = -1;
    uint32_t WaitAmt;
    uint64_t StartTime, CurrTime;
    sync_ReaderWriter_object *obj = fetch_sync_ReaderWriter();

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &wait) == FAILURE)
        return;

    WaitAmt = (uint32_t) (wait > -1 ? wait : INFINITE);

    /* Get current time in milliseconds. */
    StartTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);

    /* Acquire the write lock mutex. */
    if (!sync_WaitForSemaphore(obj->MxSemWWaitMutex, WaitAmt))
        RETURN_FALSE;

    /* Wait for readers to reach zero. */
    CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
    if (!sync_WaitForSemaphore(obj->MxSemRWaitEvent, WaitAmt - (CurrTime - StartTime))) {
        sem_post(obj->MxSemWWaitMutex);

        RETURN_FALSE;
    }

    /* Release the semaphore to avoid a later deadlock. */
    sem_post(obj->MxSemRWaitEvent);

    obj->MxWriteLock = 1;

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::readunlock()
 Read unlocks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, readunlock)
{
    sync_ReaderWriter_object *obj = fetch_sync_ReaderWriter();

    if (!sync_ReaderWriter_readunlock_internal(obj))
        RETURN_FALSE;
    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::writeunlock()
 Write unlocks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, writeunlock)
{
    sync_ReaderWriter_object *obj = fetch_sync_ReaderWriter();

    if (!sync_ReaderWriter_writeunlock_internal(obj))
        RETURN_FALSE;

    RETURN_TRUE;
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter___construct, 0, 0, 0) ZEND_ARG_INFO(0, name)
ZEND_ARG_INFO(0, autounlock)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter_readlock, 0, 0, 0) ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter_writelock, 0, 0, 0) ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter_readunlock, 0, 0, 0) ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter_writeunlock, 0, 0, 0) ZEND_END_ARG_INFO()

static const zend_function_entry sync_ReaderWriter_methods[] =
{
        PHP_ME(sync_ReaderWriter, __construct, arginfo_sync_readerwriter___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
        PHP_ME(sync_ReaderWriter, readlock, arginfo_sync_readerwriter_readlock, ZEND_ACC_PUBLIC)
        PHP_ME(sync_ReaderWriter, writelock, arginfo_sync_readerwriter_writelock, ZEND_ACC_PUBLIC)
        PHP_ME(sync_ReaderWriter, readunlock, arginfo_sync_readerwriter_readunlock, ZEND_ACC_PUBLIC)
        PHP_ME(sync_ReaderWriter, writeunlock, arginfo_sync_readerwriter_writeunlock, ZEND_ACC_PUBLIC)
        PHP_FE_END
};

/* {{{ PHP_MINIT_FUNCTION(sync)
 */
PHP_MINIT_FUNCTION(sync)
{
    zend_class_entry mutex_class_entry;
    zend_class_entry semaphore_class_entry;
    zend_class_entry event_class_entry;
    zend_class_entry readerwriter_class_entry;

    INIT_CLASS_ENTRY(mutex_class_entry, "SyncMutex", sync_Mutex_methods);
    sync_Mutex_ce = zend_register_internal_class(&mutex_class_entry TSRMLS_CC);
    sync_Mutex_ce->create_object = sync_Mutex_create_object;

    INIT_CLASS_ENTRY(semaphore_class_entry, "SyncSemaphore", sync_Semaphore_methods);
    sync_Semaphore_ce = zend_register_internal_class(&semaphore_class_entry TSRMLS_CC);
    sync_Semaphore_ce->create_object = sync_Semaphore_create_object;

    INIT_CLASS_ENTRY(event_class_entry, "SyncEvent", sync_Event_methods);
    sync_Event_ce = zend_register_internal_class(&event_class_entry TSRMLS_CC);
    sync_Event_ce->create_object = sync_Event_create_object;

    INIT_CLASS_ENTRY(readerwriter_class_entry, "SyncReaderWriter", sync_ReaderWriter_methods);
    sync_ReaderWriter_ce = zend_register_internal_class(&readerwriter_class_entry TSRMLS_CC);
    sync_ReaderWriter_ce->create_object = sync_ReaderWriter_create_object;

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(sync)
{
    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(sync)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "sync support", "enabled");
    php_info_print_table_end();
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
