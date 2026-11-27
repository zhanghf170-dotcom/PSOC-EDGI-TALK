/*
 * RT-Thread retargetable locking for newlib-nano (_RETARGETABLE_LOCKING).
 * Replaces newlib's no-op lock.o stubs so stdio FILE tables, fopen/fclose,
 * env, tz and atexit locks are thread-safe across RT-Thread tasks while
 * remaining safe during early boot and ISR contexts.
 */

#include <rtthread.h>
#include <sys/lock.h>
#include <cmsis_compiler.h>

#if defined(_RETARGETABLE_LOCKING)

#define NEWLIB_LOCK_SECTION __attribute__((section(".cy_socmem_data"), used))
#define NEWLIB_LOCK_FUNC    __attribute__((used))

struct __lock
{
    struct rt_mutex mutex;
    rt_uint8_t initialized;
    rt_uint8_t is_dynamic;
};

NEWLIB_LOCK_SECTION struct __lock __lock___sinit_recursive_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___sfp_recursive_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___atexit_recursive_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___at_quick_exit_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___malloc_recursive_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___env_recursive_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___tz_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___dd_hash_mutex;
NEWLIB_LOCK_SECTION struct __lock __lock___arc4random_mutex;

static rt_bool_t newlib_lock_can_block(void)
{
    return (rt_thread_self() != RT_NULL) &&
           (rt_interrupt_get_nest() == 0U) &&
           (rt_critical_level() == 0U) &&
           (__get_IPSR() == 0U) &&
           (__get_PRIMASK() == 0U);
}

static void newlib_init_static_lock(struct __lock *lk, const char *name)
{
    if (lk != RT_NULL && !lk->initialized)
    {
        if (rt_mutex_init(&lk->mutex, name, RT_IPC_FLAG_PRIO) == RT_EOK)
        {
            lk->initialized = 1U;
            lk->is_dynamic = 0U;
        }
    }
}

static int newlib_retarget_locks_init(void)
{
    newlib_init_static_lock(&__lock___sinit_recursive_mutex, "nl_sini");
    newlib_init_static_lock(&__lock___sfp_recursive_mutex, "nl_sfp");
    newlib_init_static_lock(&__lock___atexit_recursive_mutex, "nl_aex");
    newlib_init_static_lock(&__lock___at_quick_exit_mutex, "nl_aqex");
    newlib_init_static_lock(&__lock___malloc_recursive_mutex, "nl_mal");
    newlib_init_static_lock(&__lock___env_recursive_mutex, "nl_env");
    newlib_init_static_lock(&__lock___tz_mutex, "nl_tz");
    newlib_init_static_lock(&__lock___dd_hash_mutex, "nl_ddh");
    newlib_init_static_lock(&__lock___arc4random_mutex, "nl_rnd");
    return RT_EOK;
}
INIT_BOARD_EXPORT(newlib_retarget_locks_init);

NEWLIB_LOCK_FUNC void __retarget_lock_init(_LOCK_T *lock)
{
    struct __lock *lk;

    if (lock == RT_NULL)
    {
        return;
    }

    *lock = RT_NULL;
    if (!newlib_lock_can_block())
    {
        return;
    }

    lk = (struct __lock *)rt_calloc(1U, sizeof(struct __lock));
    if (lk == RT_NULL)
    {
        return;
    }

    if (rt_mutex_init(&lk->mutex, "nl_dyn", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_free(lk);
        return;
    }

    lk->initialized = 1U;
    lk->is_dynamic = 1U;
    *lock = lk;
}

NEWLIB_LOCK_FUNC void __retarget_lock_init_recursive(_LOCK_T *lock)
{
    __retarget_lock_init(lock);
}

NEWLIB_LOCK_FUNC void __retarget_lock_close(_LOCK_T lock)
{
    if (lock == RT_NULL || !lock->initialized || !lock->is_dynamic)
    {
        return;
    }

    lock->initialized = 0U;
    rt_mutex_detach(&lock->mutex);
    rt_free(lock);
}

NEWLIB_LOCK_FUNC void __retarget_lock_close_recursive(_LOCK_T lock)
{
    __retarget_lock_close(lock);
}

NEWLIB_LOCK_FUNC void __retarget_lock_acquire(_LOCK_T lock)
{
    if (lock == RT_NULL || !lock->initialized || !newlib_lock_can_block())
    {
        return;
    }

    (void)rt_mutex_take(&lock->mutex, RT_WAITING_FOREVER);
}

NEWLIB_LOCK_FUNC void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    __retarget_lock_acquire(lock);
}

NEWLIB_LOCK_FUNC int __retarget_lock_try_acquire(_LOCK_T lock)
{
    if (lock == RT_NULL || !lock->initialized || !newlib_lock_can_block())
    {
        return 1;
    }

    return (rt_mutex_take(&lock->mutex, RT_WAITING_NO) == RT_EOK) ? 1 : 0;
}

NEWLIB_LOCK_FUNC int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
    return __retarget_lock_try_acquire(lock);
}

NEWLIB_LOCK_FUNC void __retarget_lock_release(_LOCK_T lock)
{
    rt_thread_t self;

    if (lock == RT_NULL || !lock->initialized || !newlib_lock_can_block())
    {
        return;
    }

    self = rt_thread_self();
    if (lock->mutex.owner == self)
    {
        (void)rt_mutex_release(&lock->mutex);
    }
}

NEWLIB_LOCK_FUNC void __retarget_lock_release_recursive(_LOCK_T lock)
{
    __retarget_lock_release(lock);
}

#endif /* _RETARGETABLE_LOCKING */
