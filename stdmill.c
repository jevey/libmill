/* Sat Aug 16 08:00:45 +0200 2014:
   This file was generated by mill from gen/stdmill.mc */

/*
    Copyright (c) 2014 Martin Sustrik  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "stdmill.h"

/******************************************************************************/
/* Generic helpers.                                                           */
/******************************************************************************/

#define mill_cont(ptr, type, member) \
    (ptr ? ((type*) (((char*) ptr) - offsetof(type, member))) : NULL)

#define uv_assert(x)\
    do {\
        if (x < 0) {\
            fprintf (stderr, "%s: %s (%s:%d)\n", uv_err_name (x),\
                uv_strerror (x), __FILE__, __LINE__);\
            fflush (stderr);\
            abort ();\
        }\
    } while (0)

/******************************************************************************/
/* Tracing support.                                                           */
/******************************************************************************/

static int mill_trace = 0;

void _mill_trace ()
{
    mill_trace = 1;
}

static mill_printstack (void *cfptr)
{
    struct mill_cfh *cfh;

    cfh = (struct mill_cfh*) cfptr;
    if (cfh->parent) {
        mill_printstack (cfh->parent);
        printf ("/");
    }
    printf ("%s", cfh->type->name);
}

/******************************************************************************/
/* The event loop. */
/******************************************************************************/

static void loop_close_cb (uv_handle_t *handle)
{
    struct mill_loop *self;

    self = mill_cont (handle, struct mill_loop, idle);
    uv_stop (&self->uv_loop);
}

static void loop_cb (uv_idle_t* handle)
{
    struct mill_loop *self;
    struct mill_cfh *src;
    struct mill_cfh *dst;
    struct mill_cfh *it;
    struct mill_cfh *prev;

    self = mill_cont (handle, struct mill_loop, idle);

    while (self->first) {
        src = self->first;

        /*  If top level coroutine exits, we can stop the event loop.
            However, first we have to close the 'idle' object. */
        if (!src->parent) {

            /* Deallocate the coframe. */
            mill_coframe_term (src);
            free (src);

            uv_close ((uv_handle_t*) &self->idle, loop_close_cb);
            return;
        }

        dst = src->parent;

        /* Invoke the handler for the finished coroutine. If the event can't
           be processed immediately.... */
        if (dst->type->handler (dst, src) != 0) {

            /* Remove it from the loop's event queue. */
            self->first = src->nextev;

            /* And put it to the end of parent's pending event queue. */
            src->nextev = NULL;
            if (dst->pfirst)
                dst->plast->nextev = src;
            else
                dst->pfirst = src;
            dst->plast = src;

            continue;
        }

        /* The event was processed successfully. That may have caused some of
           the pending events to become applicable. */
        it = dst->pfirst;
        prev = NULL;
        while (it != NULL) {

            /* Try to process the event. */
            if (dst->type->handler (dst, it) == 0) {

                /* Even was processed successfully. Remove it from the queue. */
                if (prev)
                    prev->nextev = it->nextev;
                else
                    dst->pfirst = it->nextev;
                if (!it->nextev)
                    dst->plast = prev;

                /* The coframe was already terminated by the handler.
                   Now deallocate the memory. */
                free (it);

                /* Start checking the pending event queue from beginning so
                   that older events are processed before newer ones. */
                it = dst->pfirst;
                prev = NULL;
                continue;
            }

            /* If the event isn't applicable, try the next one. */
            prev = it;
            it = it->nextev;
        }

        /* Move to the next event. */
        self->first = src->nextev;

        /* The coframe was already terminated by the handler.
           Now deallocate the memory. */
        free (src);
    }
    self->last = NULL;
}

void mill_loop_init (
    struct mill_loop *self)
{
    int rc;

    rc = uv_loop_init (&self->uv_loop);
    uv_assert (rc);
    rc = uv_idle_init (&self->uv_loop, &self->idle);
    uv_assert (rc);
    rc = uv_idle_start (&self->idle, loop_cb);
    uv_assert (rc);
    self->first = NULL;
    self->last = NULL;
}

void mill_loop_term (
    struct mill_loop *self)
{
    int rc;

    rc = uv_idle_stop (&self->idle);
    uv_assert (rc);
    rc = uv_loop_close (&self->uv_loop);
    uv_assert (rc);
}

void mill_loop_run (
    struct mill_loop *self)
{
    int rc;

    rc = uv_run (&self->uv_loop, UV_RUN_DEFAULT);
    uv_assert (rc);
}

void mill_loop_emit (
    struct mill_loop *self,
    struct mill_cfh *ev)
{
    if (self->first == NULL)
        self->first = ev;
    else
        self->last->nextev = ev;
    ev->nextev = NULL;
    self->last = ev;
}

/******************************************************************************/
/*  Helpers used to implement mill keywords.                                  */
/******************************************************************************/

void mill_coframe_init (
    void *cfptr,
    const struct mill_type *type,
    void *parent,
    struct mill_loop *loop)
{
    struct mill_cfh *cfh;
    struct mill_cfh *pcfh;

    cfh = (struct mill_cfh*) cfptr;

    /*  Initialise the coframe. */
    cfh->type = type;
    cfh->pc = 0;
    cfh->nextev = NULL;
    cfh->pfirst = NULL;
    cfh->plast = NULL;
    cfh->parent = parent;
    cfh->children = NULL;
    cfh->next = NULL;
    cfh->prev = NULL;
    cfh->loop = loop;

    /* Add the coframe to the parent's list of child coroutines. */
    if (parent) {
        pcfh = (struct mill_cfh*) parent;
        cfh->prev = NULL;
        cfh->next = pcfh->children;
        if (pcfh->children)
            pcfh->children->prev = cfh;
        pcfh->children = cfh;
    }

    /* Trace start of the new coroutine. */
    if (mill_trace) {
        printf ("mill ==> go     ");
        mill_printstack (cfh);
        printf ("\n");
    }
}

void mill_coframe_term (
    void *cfptr)
{
    struct mill_cfh *cfh;

    cfh = (struct mill_cfh*) cfptr;

    /* Tracing support. No need to trace deallocation of the top-level coframe
       as the return from the coroutine is already reported and the two are
       basically the same thing. */
    if (mill_trace && cfh->parent) {
        printf ("mill ==> select ");
        mill_printstack (cfh);
        printf ("\n");
    }

    /* Copy the 'out' arguments to their final destinations. */
    cfh->type->output (cfh);

    /* Remove the coframe from the paren't list of child coroutines. */
    if (cfh->parent) {
        if (cfh->parent->children == cfh)
            cfh->parent->children = cfh->next;
        if (cfh->prev)
            cfh->prev->next = cfh->next;
        if (cfh->next)
            cfh->next->prev = cfh->prev;
        cfh->prev = NULL;
        cfh->next = NULL;
    }

    /* This is a heuristic that should cause an error if deallocated
       coframe is used. */
    cfh->type = NULL;
}

void mill_emit (
    void *cfptr)
{
    struct mill_cfh *cfh;

    cfh = (struct mill_cfh*) cfptr;

    /* At this point all the child coroutines should be stopped. */
    assert (!cfh->children);

    /* Add the coroutine to the event queue. */
    mill_loop_emit (cfh->loop, cfh);

    if (mill_trace) {
        printf ("mill ==> return ");
        mill_printstack (cfh);
        printf ("\n");
    }
}

void mill_cancel_children (
    void *cfptr)
{
    struct mill_cfh *cfh;
    struct mill_cfh *child;

    cfh = (struct mill_cfh*) cfptr;

    /* Ask all child coroutines to cancel. */
    for (child = cfh->children; child != NULL; child = child->next) {
        if (mill_trace) {
            printf ("mill ==> cancel ");
            mill_printstack (child);
            printf ("\n");
        }
        child->type->handler (child, NULL);
    }
}

int mill_has_children (void *cfptr)
{
    struct mill_cfh *cfh;

    cfh = (struct mill_cfh*) cfptr;
    return cfh->children ? 1 : 0;
}

/******************************************************************************/
/* msleep                                                                     */
/******************************************************************************/

/* Forward declarations. */
static void msleep_timer_cb (
    uv_timer_t *timer);
static void msleep_close_cb (
    uv_handle_t *handle);

struct mill_cf_msleep {

    /* Generic coframe header. */
    struct mill_cfh mill_cfh;

    /* Coroutine arguments. */
    int rc;
    int milliseconds;

    /* Local variables. */
    uv_timer_t timer;

    /* Destinations for out arguments. */
    int *mill_out_rc;
};

static int mill_handler_msleep (void *cfptr, void *event)
{
    struct mill_cf_msleep *cf;

    cf = (struct mill_cf_msleep*) cfptr;

    /* Continue from the point where the coroutine was interrupted. */
    switch (cf->mill_cfh.pc) {
        case 0: break;
        case 1: goto mill_pc_1;
        case 2: goto mill_pc_2;
        case 3: goto mill_pc_3;
        case 4: goto mill_pc_4;
        default: assert (0);
    }

    /* Start the timer. */
    *(&cf->rc) = uv_timer_init (&cf->mill_cfh.loop->uv_loop, &(cf->timer));
    uv_assert (*(&cf->rc));
    *(&cf->rc) = uv_timer_start (&(cf->timer), msleep_timer_cb, (cf->milliseconds), 0);
    uv_assert (*(&cf->rc));
    
    /* Wait till it finishes or the coroutine is canceled. */
    cf->mill_cfh.pc = 1;
    return 0;
    mill_pc_1:;
    if (event == msleep_timer_cb)
        *(&cf->rc) = 0;
    else if (event == NULL)
        *(&cf->rc) = ECANCELED;
    else
        assert (0);
    
    /* Close the timer. Ignore cancel requests during this phase. */
    uv_close ((uv_handle_t*) &(cf->timer), msleep_close_cb);
    while (1) {
        cf->mill_cfh.pc = 2;
        return 0;
        mill_pc_2:;
        if (event == msleep_close_cb)
           break;
        assert (event == NULL);
    }

    /* The coroutine have exited. Cancel any remaining child coroutines. */
mill_finally:
    mill_cancel_children (cf);
    cf->mill_cfh.pc = 3;
    goto mill_test_children;
mill_pc_3:
    mill_coframe_term (event);

    /* If there are no remaining children notify the parent that
       the coroutine have finished. */
mill_test_children:
    if (mill_has_children (cf))
        return 0;
    mill_emit (cf);
    cf->mill_cfh.pc = 4;
    return 0;

    /* The finished coroutine was canceled before if was selected. */
mill_pc_4:
    assert (!event);

    /* There is no further code in the coroutine.
       Set the program counter to an invalid value. */
    cf->mill_cfh.pc = -1;
    return 0;
}

static void mill_output_msleep (void *cfptr)
{
    struct mill_cf_msleep *cf;

    cf = (struct mill_cf_msleep*) cfptr;    
    if (cf->mill_out_rc)
        *cf->mill_out_rc = cf->rc;
}

const struct mill_type mill_type_msleep = {
    mill_type_tag,
    mill_handler_msleep,
    mill_output_msleep,
    "msleep"
};

void mill_go_msleep (
    const struct mill_type *type,
    struct mill_loop *loop,
    void *parent,
    int *rc,
    int milliseconds)
{
    struct mill_cf_msleep *cf;

    /* Allocate new coframe. */
    cf = malloc (sizeof (struct mill_cf_msleep));
    assert (cf);

    /* Inialise the coframe. */
    mill_coframe_init (cf, type, parent, loop);
    cf->mill_out_rc = rc;
    cf->milliseconds = milliseconds;

    /* Execute the initial part of the coroutine. */
    mill_handler_msleep (&cf->mill_cfh, 0);
}

void msleep (
    int *rc, 
    int milliseconds)
{
    struct mill_loop loop;

    mill_loop_init (&loop);

    /* Execute initial part of the coroutine. */
    mill_go_msleep (&mill_type_msleep, &loop, NULL, rc, milliseconds);

    /* Run the event loop until the coroutine exits. */
    mill_loop_run (&loop);

    mill_loop_term (&loop);
}

static void msleep_timer_cb (
    uv_timer_t *timer)
{
    struct mill_cf_msleep *cf;

    cf = mill_cont (timer, struct mill_cf_msleep, timer);
    mill_handler_msleep (cf, (void*) msleep_timer_cb);
}

static void msleep_close_cb (
    uv_handle_t *handle)
{
    struct mill_cf_msleep *cf;

    cf = mill_cont (handle, struct mill_cf_msleep, timer);
    mill_handler_msleep (cf, (void*) msleep_close_cb);
}

/******************************************************************************/
/* tcpsocket                                                                  */
/******************************************************************************/

/* Forward declarations. */
static void tcpsocket_close_cb (
    uv_handle_t* handle);
static void tcpsocket_listen_cb (
    uv_stream_t *s,
    int status);
static void tcpsocket_connect_cb (
    uv_connect_t* req,
    int status);
static void tcpsocket_send_cb (
    uv_write_t* req,
    int status);
static void tcpsocket_alloc_cb (
    uv_handle_t* handle,
    size_t suggested_size,
    uv_buf_t* buf);
static void tcpsocket_recv_cb (
    uv_stream_t* stream,
    ssize_t nread,
    const uv_buf_t* buf);

int tcpsocket_init (
    struct tcpsocket *self,
    struct mill_loop *loop)
{
    int rc;

    self->loop = &loop->uv_loop;
    self->pc = 0;
    self->recvcfptr = NULL;
    self->sendcfptr = NULL;
    rc = uv_tcp_init (&loop->uv_loop, &self->s);
    if (rc != 0)
        return rc;
}

struct mill_cf_tcpsocket_term {

    /* Generic coframe header. */
    struct mill_cfh mill_cfh;

    /* Coroutine arguments. */
    struct tcpsocket * self;
};

static int mill_handler_tcpsocket_term (void *cfptr, void *event)
{
    struct mill_cf_tcpsocket_term *cf;

    cf = (struct mill_cf_tcpsocket_term*) cfptr;

    /* Continue from the point where the coroutine was interrupted. */
    switch (cf->mill_cfh.pc) {
        case 0: break;
        case 1: goto mill_pc_1;
        case 2: goto mill_pc_2;
        case 3: goto mill_pc_3;
        default: assert (0);
    }

    /* Initiate the termination. */
    (cf->self)->recvcfptr = cf;
    uv_close ((uv_handle_t*) &(cf->self)->s, tcpsocket_close_cb);
    
    /* Wait till socket is closed. In the meantime ignore cancel requests. */
    while (1) {
        cf->mill_cfh.pc = 1;
        return 0;
        mill_pc_1:;
        if (event == tcpsocket_close_cb)
           break;
        assert (event == NULL);
    }

    /* The coroutine have exited. Cancel any remaining child coroutines. */
mill_finally:
    mill_cancel_children (cf);
    cf->mill_cfh.pc = 2;
    goto mill_test_children;
mill_pc_2:
    mill_coframe_term (event);

    /* If there are no remaining children notify the parent that
       the coroutine have finished. */
mill_test_children:
    if (mill_has_children (cf))
        return 0;
    mill_emit (cf);
    cf->mill_cfh.pc = 3;
    return 0;

    /* The finished coroutine was canceled before if was selected. */
mill_pc_3:
    assert (!event);

    /* There is no further code in the coroutine.
       Set the program counter to an invalid value. */
    cf->mill_cfh.pc = -1;
    return 0;
}

static void mill_output_tcpsocket_term (void *cfptr)
{
    struct mill_cf_tcpsocket_term *cf;

    cf = (struct mill_cf_tcpsocket_term*) cfptr;    
}

const struct mill_type mill_type_tcpsocket_term = {
    mill_type_tag,
    mill_handler_tcpsocket_term,
    mill_output_tcpsocket_term,
    "tcpsocket_term"
};

void mill_go_tcpsocket_term (
    const struct mill_type *type,
    struct mill_loop *loop,
    void *parent,
    struct tcpsocket * self)
{
    struct mill_cf_tcpsocket_term *cf;

    /* Allocate new coframe. */
    cf = malloc (sizeof (struct mill_cf_tcpsocket_term));
    assert (cf);

    /* Inialise the coframe. */
    mill_coframe_init (cf, type, parent, loop);
    cf->self = self;

    /* Execute the initial part of the coroutine. */
    mill_handler_tcpsocket_term (&cf->mill_cfh, 0);
}

void tcpsocket_term (
    struct tcpsocket * self)
{
    struct mill_loop loop;

    mill_loop_init (&loop);

    /* Execute initial part of the coroutine. */
    mill_go_tcpsocket_term (&mill_type_tcpsocket_term, &loop, NULL, self);

    /* Run the event loop until the coroutine exits. */
    mill_loop_run (&loop);

    mill_loop_term (&loop);
}

int tcpsocket_bind (
    struct tcpsocket *self,
    struct sockaddr *addr,
    int flags)
{
    return uv_tcp_bind (&self->s, addr, flags);
}

int tcpsocket_listen (
    struct tcpsocket *self,
    int backlog)
{
    int rc;

    rc = uv_listen ((uv_stream_t*) &self->s, backlog, tcpsocket_listen_cb);
    if (rc != 0)
        return rc;
}

struct mill_cf_tcpsocket_connect {

    /* Generic coframe header. */
    struct mill_cfh mill_cfh;

    /* Coroutine arguments. */
    int rc;
    struct tcpsocket * self;
    struct sockaddr * addr;

    /* Local variables. */
    uv_connect_t req;

    /* Destinations for out arguments. */
    int *mill_out_rc;
};

static int mill_handler_tcpsocket_connect (void *cfptr, void *event)
{
    struct mill_cf_tcpsocket_connect *cf;

    cf = (struct mill_cf_tcpsocket_connect*) cfptr;

    /* Continue from the point where the coroutine was interrupted. */
    switch (cf->mill_cfh.pc) {
        case 0: break;
        case 1: goto mill_pc_1;
        case 2: goto mill_pc_2;
        case 3: goto mill_pc_3;
        default: assert (0);
    }

    /* Start connecting. */
    (cf->self)->recvcfptr = cf;
    *(&cf->rc) = uv_tcp_connect (&(cf->req), &(cf->self)->s, (cf->addr), tcpsocket_connect_cb);
    if (*(&cf->rc) != 0)
        goto mill_finally;
    
    /* Wait till connecting finishes. */
    cf->mill_cfh.pc = 1;
    return 0;
    mill_pc_1:;
    
    /* TODO: Canceling connect operation requires closing the entire socket. */
    if (!event) {
        assert (0);
    }
    
    assert (event == tcpsocket_connect_cb);

    /* The coroutine have exited. Cancel any remaining child coroutines. */
mill_finally:
    mill_cancel_children (cf);
    cf->mill_cfh.pc = 2;
    goto mill_test_children;
mill_pc_2:
    mill_coframe_term (event);

    /* If there are no remaining children notify the parent that
       the coroutine have finished. */
mill_test_children:
    if (mill_has_children (cf))
        return 0;
    mill_emit (cf);
    cf->mill_cfh.pc = 3;
    return 0;

    /* The finished coroutine was canceled before if was selected. */
mill_pc_3:
    assert (!event);

    /* There is no further code in the coroutine.
       Set the program counter to an invalid value. */
    cf->mill_cfh.pc = -1;
    return 0;
}

static void mill_output_tcpsocket_connect (void *cfptr)
{
    struct mill_cf_tcpsocket_connect *cf;

    cf = (struct mill_cf_tcpsocket_connect*) cfptr;    
    if (cf->mill_out_rc)
        *cf->mill_out_rc = cf->rc;
}

const struct mill_type mill_type_tcpsocket_connect = {
    mill_type_tag,
    mill_handler_tcpsocket_connect,
    mill_output_tcpsocket_connect,
    "tcpsocket_connect"
};

void mill_go_tcpsocket_connect (
    const struct mill_type *type,
    struct mill_loop *loop,
    void *parent,
    int *rc,
    struct tcpsocket * self,
    struct sockaddr * addr)
{
    struct mill_cf_tcpsocket_connect *cf;

    /* Allocate new coframe. */
    cf = malloc (sizeof (struct mill_cf_tcpsocket_connect));
    assert (cf);

    /* Inialise the coframe. */
    mill_coframe_init (cf, type, parent, loop);
    cf->mill_out_rc = rc;
    cf->self = self;
    cf->addr = addr;

    /* Execute the initial part of the coroutine. */
    mill_handler_tcpsocket_connect (&cf->mill_cfh, 0);
}

void tcpsocket_connect (
    int *rc, 
    struct tcpsocket * self, 
    struct sockaddr * addr)
{
    struct mill_loop loop;

    mill_loop_init (&loop);

    /* Execute initial part of the coroutine. */
    mill_go_tcpsocket_connect (&mill_type_tcpsocket_connect, &loop, NULL, rc, self, addr);

    /* Run the event loop until the coroutine exits. */
    mill_loop_run (&loop);

    mill_loop_term (&loop);
}

struct mill_cf_tcpsocket_accept {

    /* Generic coframe header. */
    struct mill_cfh mill_cfh;

    /* Coroutine arguments. */
    int rc;
    struct tcpsocket * self;
    struct tcpsocket * newsock;

    /* Destinations for out arguments. */
    int *mill_out_rc;
};

static int mill_handler_tcpsocket_accept (void *cfptr, void *event)
{
    struct mill_cf_tcpsocket_accept *cf;

    cf = (struct mill_cf_tcpsocket_accept*) cfptr;

    /* Continue from the point where the coroutine was interrupted. */
    switch (cf->mill_cfh.pc) {
        case 0: break;
        case 1: goto mill_pc_1;
        case 2: goto mill_pc_2;
        case 3: goto mill_pc_3;
        default: assert (0);
    }

    /* Link the lisening socket with the accepting socket. */
    (cf->self)->recvcfptr = cf;
    
    /* Wait for an incoming connection. */
    cf->mill_cfh.pc = 1;
    return 0;
    mill_pc_1:;
    if (!event) {
        *(&cf->rc) = ECANCELED;
        goto mill_finally;
    }
    
    /* There's a new incoming connection. Let's accept it. */
    assert (event == tcpsocket_listen_cb);
    *(&cf->rc) = uv_accept ((uv_stream_t*) &(cf->self)->s, (uv_stream_t*) &(cf->newsock)->s);

    /* The coroutine have exited. Cancel any remaining child coroutines. */
mill_finally:
    mill_cancel_children (cf);
    cf->mill_cfh.pc = 2;
    goto mill_test_children;
mill_pc_2:
    mill_coframe_term (event);

    /* If there are no remaining children notify the parent that
       the coroutine have finished. */
mill_test_children:
    if (mill_has_children (cf))
        return 0;
    mill_emit (cf);
    cf->mill_cfh.pc = 3;
    return 0;

    /* The finished coroutine was canceled before if was selected. */
mill_pc_3:
    assert (!event);

    /* There is no further code in the coroutine.
       Set the program counter to an invalid value. */
    cf->mill_cfh.pc = -1;
    return 0;
}

static void mill_output_tcpsocket_accept (void *cfptr)
{
    struct mill_cf_tcpsocket_accept *cf;

    cf = (struct mill_cf_tcpsocket_accept*) cfptr;    
    if (cf->mill_out_rc)
        *cf->mill_out_rc = cf->rc;
}

const struct mill_type mill_type_tcpsocket_accept = {
    mill_type_tag,
    mill_handler_tcpsocket_accept,
    mill_output_tcpsocket_accept,
    "tcpsocket_accept"
};

void mill_go_tcpsocket_accept (
    const struct mill_type *type,
    struct mill_loop *loop,
    void *parent,
    int *rc,
    struct tcpsocket * self,
    struct tcpsocket * newsock)
{
    struct mill_cf_tcpsocket_accept *cf;

    /* Allocate new coframe. */
    cf = malloc (sizeof (struct mill_cf_tcpsocket_accept));
    assert (cf);

    /* Inialise the coframe. */
    mill_coframe_init (cf, type, parent, loop);
    cf->mill_out_rc = rc;
    cf->self = self;
    cf->newsock = newsock;

    /* Execute the initial part of the coroutine. */
    mill_handler_tcpsocket_accept (&cf->mill_cfh, 0);
}

void tcpsocket_accept (
    int *rc, 
    struct tcpsocket * self, 
    struct tcpsocket * newsock)
{
    struct mill_loop loop;

    mill_loop_init (&loop);

    /* Execute initial part of the coroutine. */
    mill_go_tcpsocket_accept (&mill_type_tcpsocket_accept, &loop, NULL, rc, self, newsock);

    /* Run the event loop until the coroutine exits. */
    mill_loop_run (&loop);

    mill_loop_term (&loop);
}

struct mill_cf_tcpsocket_send {

    /* Generic coframe header. */
    struct mill_cfh mill_cfh;

    /* Coroutine arguments. */
    int rc;
    struct tcpsocket * self;
    const void * buf;
    size_t len;

    /* Local variables. */
    uv_write_t req;
    uv_buf_t buffer;

    /* Destinations for out arguments. */
    int *mill_out_rc;
};

static int mill_handler_tcpsocket_send (void *cfptr, void *event)
{
    struct mill_cf_tcpsocket_send *cf;

    cf = (struct mill_cf_tcpsocket_send*) cfptr;

    /* Continue from the point where the coroutine was interrupted. */
    switch (cf->mill_cfh.pc) {
        case 0: break;
        case 1: goto mill_pc_1;
        case 2: goto mill_pc_2;
        case 3: goto mill_pc_3;
        default: assert (0);
    }

    /* Start the send operation. */
    (cf->buffer).base = (void*) (cf->buf);
    (cf->buffer).len = (cf->len);
    *(&cf->rc) = uv_write (&(cf->req), (uv_stream_t*) &(cf->self)->s, &(cf->buffer), 1,
        tcpsocket_send_cb);
    if (*(&cf->rc) != 0)
        goto mill_finally;
    
    /* Mark the socket as being in process of sending. */
    (cf->self)->sendcfptr = cf;
    
    /* Wait till sending is done. */
    cf->mill_cfh.pc = 1;
    return 0;
    mill_pc_1:;
    
    /* TODO: Cancelling a send operation requires closing the entire socket. */
    if (!event) {
        assert (0);
    }
    
    assert (event == tcpsocket_send_cb);
    (cf->self)->sendcfptr = NULL;
    *(&cf->rc) = 0;

    /* The coroutine have exited. Cancel any remaining child coroutines. */
mill_finally:
    mill_cancel_children (cf);
    cf->mill_cfh.pc = 2;
    goto mill_test_children;
mill_pc_2:
    mill_coframe_term (event);

    /* If there are no remaining children notify the parent that
       the coroutine have finished. */
mill_test_children:
    if (mill_has_children (cf))
        return 0;
    mill_emit (cf);
    cf->mill_cfh.pc = 3;
    return 0;

    /* The finished coroutine was canceled before if was selected. */
mill_pc_3:
    assert (!event);

    /* There is no further code in the coroutine.
       Set the program counter to an invalid value. */
    cf->mill_cfh.pc = -1;
    return 0;
}

static void mill_output_tcpsocket_send (void *cfptr)
{
    struct mill_cf_tcpsocket_send *cf;

    cf = (struct mill_cf_tcpsocket_send*) cfptr;    
    if (cf->mill_out_rc)
        *cf->mill_out_rc = cf->rc;
}

const struct mill_type mill_type_tcpsocket_send = {
    mill_type_tag,
    mill_handler_tcpsocket_send,
    mill_output_tcpsocket_send,
    "tcpsocket_send"
};

void mill_go_tcpsocket_send (
    const struct mill_type *type,
    struct mill_loop *loop,
    void *parent,
    int *rc,
    struct tcpsocket * self,
    const void * buf,
    size_t len)
{
    struct mill_cf_tcpsocket_send *cf;

    /* Allocate new coframe. */
    cf = malloc (sizeof (struct mill_cf_tcpsocket_send));
    assert (cf);

    /* Inialise the coframe. */
    mill_coframe_init (cf, type, parent, loop);
    cf->mill_out_rc = rc;
    cf->self = self;
    cf->buf = buf;
    cf->len = len;

    /* Execute the initial part of the coroutine. */
    mill_handler_tcpsocket_send (&cf->mill_cfh, 0);
}

void tcpsocket_send (
    int *rc, 
    struct tcpsocket * self, 
    const void * buf, 
    size_t len)
{
    struct mill_loop loop;

    mill_loop_init (&loop);

    /* Execute initial part of the coroutine. */
    mill_go_tcpsocket_send (&mill_type_tcpsocket_send, &loop, NULL, rc, self, buf, len);

    /* Run the event loop until the coroutine exits. */
    mill_loop_run (&loop);

    mill_loop_term (&loop);
}

struct mill_cf_tcpsocket_recv {

    /* Generic coframe header. */
    struct mill_cfh mill_cfh;

    /* Coroutine arguments. */
    int rc;
    struct tcpsocket * self;
    void * buf;
    size_t len;

    /* Destinations for out arguments. */
    int *mill_out_rc;
};

static int mill_handler_tcpsocket_recv (void *cfptr, void *event)
{
    struct mill_cf_tcpsocket_recv *cf;

    cf = (struct mill_cf_tcpsocket_recv*) cfptr;

    /* Continue from the point where the coroutine was interrupted. */
    switch (cf->mill_cfh.pc) {
        case 0: break;
        case 1: goto mill_pc_1;
        case 2: goto mill_pc_2;
        case 3: goto mill_pc_3;
        default: assert (0);
    }

    /* Sart the receiving. */
    *(&cf->rc) = uv_read_start ((uv_stream_t*) &(cf->self)->s,
        tcpsocket_alloc_cb, tcpsocket_recv_cb);
    if (*(&cf->rc) != 0)
        goto mill_finally;
    
    /* Mark the socket as being in process of receiving. */
    cf->self->recvcfptr = cf;
    
    while (1) {
    
        /* Wait for next chunk of data. */
        cf->mill_cfh.pc = 1;
        return 0;
        mill_pc_1:;
    
        /* User asks operation to be canceled. */
        if (!event) {
            uv_read_stop ((uv_stream_t*) &(cf->self)->s);
            (cf->self)->recvcfptr = NULL;
            *(&cf->rc) = ECANCELED;
            goto mill_finally;
        }
    
        /* If there are no more data to be read, stop reading. */
        if (!(cf->len)) {
            uv_read_stop ((uv_stream_t*) &(cf->self)->s);
            (cf->self)->recvcfptr = NULL;
            *(&cf->rc) = 0;
            break;
        }
    }

    /* The coroutine have exited. Cancel any remaining child coroutines. */
mill_finally:
    mill_cancel_children (cf);
    cf->mill_cfh.pc = 2;
    goto mill_test_children;
mill_pc_2:
    mill_coframe_term (event);

    /* If there are no remaining children notify the parent that
       the coroutine have finished. */
mill_test_children:
    if (mill_has_children (cf))
        return 0;
    mill_emit (cf);
    cf->mill_cfh.pc = 3;
    return 0;

    /* The finished coroutine was canceled before if was selected. */
mill_pc_3:
    assert (!event);

    /* There is no further code in the coroutine.
       Set the program counter to an invalid value. */
    cf->mill_cfh.pc = -1;
    return 0;
}

static void mill_output_tcpsocket_recv (void *cfptr)
{
    struct mill_cf_tcpsocket_recv *cf;

    cf = (struct mill_cf_tcpsocket_recv*) cfptr;    
    if (cf->mill_out_rc)
        *cf->mill_out_rc = cf->rc;
}

const struct mill_type mill_type_tcpsocket_recv = {
    mill_type_tag,
    mill_handler_tcpsocket_recv,
    mill_output_tcpsocket_recv,
    "tcpsocket_recv"
};

void mill_go_tcpsocket_recv (
    const struct mill_type *type,
    struct mill_loop *loop,
    void *parent,
    int *rc,
    struct tcpsocket * self,
    void * buf,
    size_t len)
{
    struct mill_cf_tcpsocket_recv *cf;

    /* Allocate new coframe. */
    cf = malloc (sizeof (struct mill_cf_tcpsocket_recv));
    assert (cf);

    /* Inialise the coframe. */
    mill_coframe_init (cf, type, parent, loop);
    cf->mill_out_rc = rc;
    cf->self = self;
    cf->buf = buf;
    cf->len = len;

    /* Execute the initial part of the coroutine. */
    mill_handler_tcpsocket_recv (&cf->mill_cfh, 0);
}

void tcpsocket_recv (
    int *rc, 
    struct tcpsocket * self, 
    void * buf, 
    size_t len)
{
    struct mill_loop loop;

    mill_loop_init (&loop);

    /* Execute initial part of the coroutine. */
    mill_go_tcpsocket_recv (&mill_type_tcpsocket_recv, &loop, NULL, rc, self, buf, len);

    /* Run the event loop until the coroutine exits. */
    mill_loop_run (&loop);

    mill_loop_term (&loop);
}

static void tcpsocket_close_cb (
    uv_handle_t* handle)
{
    struct tcpsocket *self;
    struct mill_cf_tcpsocket_term *cf;

    self = mill_cont (handle, struct tcpsocket, s);
    cf = (struct mill_cf_tcpsocket_term*) self->recvcfptr;
    mill_handler_tcpsocket_term (cf, (void*) tcpsocket_close_cb);
}

static void tcpsocket_listen_cb (
    uv_stream_t *s,
    int status)
{
    struct tcpsocket *self;
    struct mill_cf_tcpsocket_accept *cf;

    self = mill_cont (s, struct tcpsocket, s);

    /* If nobody is accepting, close the incoming connection. */
    if (!self->recvcfptr) {
        assert (0);
    }

    /* If somebody is accepting, move the accept coroutine on. */
    cf = (struct mill_cf_tcpsocket_accept*) self->recvcfptr;
    mill_handler_tcpsocket_accept (cf, (void*) tcpsocket_listen_cb);
}

static void tcpsocket_connect_cb (
    uv_connect_t* req,
    int status)
{
    struct mill_cf_tcpsocket_connect *cf;

    cf = mill_cont (req, struct mill_cf_tcpsocket_connect, req);
    mill_handler_tcpsocket_connect (cf, (void*) tcpsocket_connect_cb);
}

static void tcpsocket_send_cb (
    uv_write_t* req,
    int status)
{
    struct mill_cf_tcpsocket_send *cf;

    cf = mill_cont (req, struct mill_cf_tcpsocket_send, req);
    mill_handler_tcpsocket_send (cf, (void*) tcpsocket_send_cb);
}

static void tcpsocket_alloc_cb (
    uv_handle_t* handle,
    size_t suggested_size,
    uv_buf_t* buf)
{
    struct tcpsocket *self;
    struct mill_cf_tcpsocket_recv *cf;

    self = mill_cont (handle, struct tcpsocket, s);
    assert (self->recvcfptr);
    cf = (struct mill_cf_tcpsocket_recv*) self->recvcfptr;

    buf->base = cf->buf;
    buf->len = cf->len;
}

static void tcpsocket_recv_cb (
    uv_stream_t* stream,
    ssize_t nread,
    const uv_buf_t* buf)
{
    struct tcpsocket *self;
    struct mill_cf_tcpsocket_recv *cf;

    self = mill_cont (stream, struct tcpsocket, s);
    assert (self->recvcfptr);
    cf = (struct mill_cf_tcpsocket_recv*) self->recvcfptr;

    /* Adjust the input buffer to not cover the data already received. */
    assert (buf->base == cf->buf);
    assert (nread <= cf->len);
    cf->buf = ((char*) cf->buf) + nread;
    cf->len -= nread;

    mill_handler_tcpsocket_recv (cf, (void*) tcpsocket_recv_cb);
}

