/*
 * Copyright (C) 2010 Codership Oy <info@codership.com>
 *
 * $Id$
 */

/*!
 * @file GCS Send Monitor. To ensure fair (FIFO) access to gcs_core_send()
 */

#ifndef _gcs_sm_h_
#define _gcs_sm_h_

#include <galerautils.h>
#include <errno.h>

#ifdef GCS_SM_CONCURRENCY
#define GCS_SM_CC sm->cc
#else
#define GCS_SM_CC 1
#endif /* GCS_SM_CONCURRENCY */

typedef struct gcs_sm_user
{
    gu_cond_t* cond;
    bool       wait;
}
gcs_sm_user_t;

typedef struct gcs_sm
{
    gu_mutex_t    lock;
    unsigned long wait_q_len;
    unsigned long wait_q_mask;
    unsigned long wait_q_head;
    unsigned long wait_q_tail;
    long          users;
    long          entered;
    long          ret;
#ifdef GCS_SM_CONCURRENCY
    long          cc;
#endif /* GCS_SM_CONCURRENCY */
    bool          pause;
    gcs_sm_user_t wait_q[];
}
gcs_sm_t;

/*!
 * Creates send monitor
 *
 * @param len size of the monitor, should be a power of 2
 * @param n   concurrency parameter (how many users can enter at the same time)
 */
extern gcs_sm_t*
gcs_sm_create (long len, long n);

/*!
 * Closes monitor for entering and makes all users to exit with error.
 * (entered users are not affected). Blocks until everybody exits
 */
extern long
gcs_sm_close (gcs_sm_t* sm);

/*!
 * Deallocates resources associated with the monitor
 */
extern void
gcs_sm_destroy (gcs_sm_t* sm);

#define GCS_SM_INCREMENT(cursor) (cursor = ((cursor + 1) & sm->wait_q_mask))

static inline void
_gcs_sm_wake_up_next (gcs_sm_t* sm)
{
    long woken = sm->entered;

    assert (woken >= 0);
    assert (woken <= GCS_SM_CC);

    while (woken < GCS_SM_CC && sm->users > 0) {
        if (gu_likely(sm->wait_q[sm->wait_q_head].wait)) {
            assert (NULL != sm->wait_q[sm->wait_q_head].cond);
            // gu_debug ("Waking up: %lu", sm->wait_q_head);
            gu_cond_signal (sm->wait_q[sm->wait_q_head].cond);
            woken++;
        }
        else { /* skip interrupted */
            assert (NULL == sm->wait_q[sm->wait_q_head].cond);
            gu_debug ("Skipping interrupted: %lu", sm->wait_q_head);
            sm->users--;
            GCS_SM_INCREMENT(sm->wait_q_head);
        }
    }

    assert (woken <= GCS_SM_CC);
    assert (sm->users >= 0);
}

static inline void
_gcs_sm_leave_common (gcs_sm_t* sm)
{
    assert (sm->entered < GCS_SM_CC);

    assert (sm->users > 0);
    sm->users--;
    GCS_SM_INCREMENT(sm->wait_q_head);

    if (!sm->pause) {
        _gcs_sm_wake_up_next(sm);
    }
    else {
        /* gcs_sm_continue() will do the rest */
    }
}

static inline bool
_gcs_sm_enqueue_common (gcs_sm_t* sm, gu_cond_t* cond)
{
    unsigned long tail = sm->wait_q_tail;

    sm->wait_q[tail].cond = cond;
    sm->wait_q[tail].wait = true;
    gu_cond_wait (cond, &sm->lock);
    assert(tail == sm->wait_q_head || false == sm->wait_q[tail].wait);
    assert(sm->wait_q[tail].cond == cond || false == sm->wait_q[tail].wait);
    sm->wait_q[tail].cond = NULL;
    register bool ret = sm->wait_q[tail].wait;
    sm->wait_q[tail].wait = false;
    return ret;
}

#define GCS_SM_HAS_TO_WAIT (sm->entered >= GCS_SM_CC || sm->pause)
/*!
 * Synchronize with entry order to the monitor. Must be always followed by
 * gcs_sm_enter(sm, cond, true)
 *
 * @retval -EAGAIN - out of space
 * @retval -EBADFD - monitor closed
 * @retval >= 0 queue handle
 */
static inline long
gcs_sm_schedule (gcs_sm_t* sm)
{
    if (gu_unlikely(gu_mutex_lock (&sm->lock))) abort();

    long ret = sm->ret;

    if (gu_likely((sm->users < (long)sm->wait_q_len) && (0 == ret))) {

        sm->users++;
        GCS_SM_INCREMENT(sm->wait_q_tail); /* even if we don't queue, cursor
                                            * needs to be advanced */

        if (GCS_SM_HAS_TO_WAIT) {
            ret = sm->wait_q_tail + 1; // waiter handle
        }

        return ret; // success
    }
    else if (0 == ret) {
        assert (sm->users == (long)sm->wait_q_len);
        ret = -EAGAIN;
    }

    assert(ret < 0);

    gu_mutex_unlock (&sm->lock);

    return ret;
}

/*!
 * Enter send monitor critical section
 *
 * @param sm   send monitor object
 * @param cond condition to signal to wake up thread in case of wait
 *
 * @retval -EAGAIN - out of space
 * @retval -EBADFD - monitor closed
 * @retval -EINTR  - was interrupted by another thread
 * @retval 0 - successfully entered
 */
static inline long
gcs_sm_enter (gcs_sm_t* sm, gu_cond_t* cond, bool scheduled)
{
    long ret = 0; /* if scheduled and no queue */

    if (gu_likely (scheduled || (ret = gcs_sm_schedule(sm)) >= 0)) {

        if (GCS_SM_HAS_TO_WAIT) {
            if (gu_likely(_gcs_sm_enqueue_common (sm, cond))) {
                ret = sm->ret;
            }
            else {
                ret = -EINTR;
            }
        }

        assert (ret <= 0);

        if (gu_likely(0 == ret)) {
            assert(sm->users   > 0);
            assert(sm->entered < GCS_SM_CC);
            sm->entered++;
        }
        else {
            if (gu_likely(-EINTR == ret)) {
                /* was interrupted, will be handled by someone else */
            }
            else {
                /* monitor is closed, wake up others */
                assert(sm->users > 0);
                _gcs_sm_leave_common(sm);
            }
        }

        gu_mutex_unlock (&sm->lock);
    }

    return ret;
}

static inline void
gcs_sm_leave (gcs_sm_t* sm)
{
    if (gu_unlikely(gu_mutex_lock (&sm->lock))) abort();

    sm->entered--;
    assert(sm->entered >= 0);

    _gcs_sm_leave_common(sm);

    gu_mutex_unlock (&sm->lock);
}

static inline void
gcs_sm_pause (gcs_sm_t* sm)
{
    if (gu_unlikely(gu_mutex_lock (&sm->lock))) abort();

    sm->pause = (sm->ret == 0); /* don't pause closed monitor */

    gu_mutex_unlock (&sm->lock);
}

static inline void
_gcs_sm_continue_common (gcs_sm_t* sm)
{
    sm->pause = false;

    _gcs_sm_wake_up_next(sm); /* wake up next waiter if any */
}

static inline void
gcs_sm_continue (gcs_sm_t* sm)
{
    if (gu_unlikely(gu_mutex_lock (&sm->lock))) abort();

    if (gu_likely(sm->pause)) {
        _gcs_sm_continue_common (sm);
    }
    else {
        gu_debug ("Trying to continue unpaused monitor");
        assert(0);
    }

    gu_mutex_unlock (&sm->lock);
}

/*!
 * Interrupts waiter identified by handle (returned by gcs_sm_schedule())
 *
 * @retval 0 - success
 * @retval -ESRCH - waiter is not in the queue. For practical purposes
 *                  it is impossible to discern already interrupted waiter and
 *                  the waiter that has entered the monitor
 */
static inline long
gcs_sm_interrupt (gcs_sm_t* sm, long handle)
{
    assert (handle > 0);
    long ret;

    if (gu_unlikely(gu_mutex_lock (&sm->lock))) abort();

    handle--;

    if (gu_likely(sm->wait_q[handle].wait)) {
        assert (sm->wait_q[handle].cond != NULL);
        sm->wait_q[handle].wait = false;
        gu_cond_signal (sm->wait_q[handle].cond);
        sm->wait_q[handle].cond = NULL;
        ret = 0;
        if (!sm->pause && (long)sm->wait_q_head == handle) {
            /* gcs_sm_interrupt() was called right after the waiter was
             * signaled by gcs_sm_continue() or gcs_sm_leave() but before
             * the waiter has woken up. Wake up the next waiter */
            _gcs_sm_wake_up_next(sm);
        }
    }
    else {
        ret = -ESRCH;
    }

    gu_mutex_unlock (&sm->lock);

    return ret;
}

#endif /* _gcs_sm_h_ */