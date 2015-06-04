/*

  Copyright (c) 2015 Martin Sustrik

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

#include "cr.h"
#include "debug.h"
#include "libmill.h"
#include "model.h"
#include "utils.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MILL_CT_ASSERT(MILL_CLAUSELEN == sizeof(struct mill_clause));

/* Add new item to the channel buffer. */
static int mill_enqueue(chan ch, void *val) {
    /* If there's a receiver already waiting, let's resume it. */
    if(!mill_list_empty(&ch->receiver.clauses)) {
        struct mill_clause *cl = mill_cont(
            mill_list_begin(&ch->receiver.clauses), struct mill_clause, epitem);
        void *dst = cl->val;
        if(!dst)
            dst = mill_valbuf_alloc(&cl->cr->valbuf, ch->sz);
        memcpy(dst, val, ch->sz);
        mill_resume(cl->cr, cl->idx);
        mill_list_erase(&ch->receiver.clauses, &cl->epitem);
        return 1;
    }
    /* The buffer is full. */
    if(ch->items >= ch->bufsz)
        return 0;
    /* Write the value to the buffer. */
    size_t pos = (ch->first + ch->items) % ch->bufsz;
    memcpy(((char*)(ch + 1)) + (pos * ch->sz) , val, ch->sz);
    ++ch->items;
    return 1;
}

/* Pop one value from the channel buffer. */
static int mill_dequeue(chan ch, void *val) {
    void *dst = val;
    if(!dst)
        dst = mill_valbuf_alloc(
            &mill_cont(mill_list_begin(&ch->receiver.clauses),
            struct mill_clause, epitem)->cr->valbuf, ch->sz);
    /* If there's a sender already waiting, let's resume it. */
    struct mill_clause *cl = mill_cont(
        mill_list_begin(&ch->sender.clauses), struct mill_clause, epitem);
    if(cl) {
        memcpy(dst, cl->val, ch->sz);
        mill_resume(cl->cr, cl->idx);
        mill_list_erase(&ch->sender.clauses, &cl->epitem);
        return 1;
    }
    /* The buffer is empty. */
    if(!ch->items) {
        if(!ch->done)
            return 0;
        /* Receiving from a closed channel yields done-with value. */
        memcpy(dst, ((char*)(ch + 1)) + (ch->bufsz * ch->sz), ch->sz);
        return 1;
    }
    /* Get the value from the buffer. */
    memcpy(dst, ((char*)(ch + 1)) + (ch->first * ch->sz), ch->sz);
    ch->first = (ch->first + 1) % ch->bufsz;
    --ch->items;
    return 1;
}

chan mill_chmake(size_t sz, size_t bufsz, const char *created) {
    /* If there's at least one channel created in the user's code
       we want the debug functions to get into the binary. */
    mill_preserve_debug();
    /* We are allocating 1 additional element after the channel buffer to
       store the done-with value. It can't be stored in the regular buffer
       because that would mean chdone() would block when buffer is full. */
    struct mill_chan *ch = (struct mill_chan*)
        malloc(sizeof(struct mill_chan) + (sz * (bufsz + 1)));
    assert(ch);
    mill_register_chan(&ch->debug, created);
    ch->sz = sz;
    ch->sender.type = MILL_SENDER;
    mill_list_init(&ch->sender.clauses);
    ch->receiver.type = MILL_RECEIVER;
    mill_list_init(&ch->receiver.clauses);
    ch->refcount = 1;
    ch->done = 0;
    ch->bufsz = bufsz;
    ch->items = 0;
    ch->first = 0;
    mill_trace(created, "<%d>=chmake(%d)", (int)ch->debug.id, (int)bufsz);
    return ch;
}

chan mill_chdup(chan ch, const char *current) {
    mill_trace(current, "chdup(<%d>)", (int)ch->debug.id);
    ++ch->refcount;
    return ch;
}

void mill_chs(chan ch, void *val, size_t sz, const char *current) {
    mill_trace(current, "chs(<%d>)", (int)ch->debug.id);
    if(ch->done)
        mill_panic("send to done-with channel");
    if(ch->sz != sz)
        mill_panic("send of a type not matching the channel");
    /* Try to enqueue the value straight away. */
    if(mill_enqueue(ch, val))
        return;
    /* If there's no free space in the buffer we are going to yield
       till a receiver arrives. */
    struct mill_clause cl;
    cl.cr = mill_running;
    mill_running->state = MILL_CHS;
    cl.ep = &ch->sender;
    cl.val = val;
    mill_list_insert(&ch->sender.clauses, &cl.epitem, NULL);
    mill_running->sender.clause = &cl;
    mill_set_current(&mill_running->debug, current);
    mill_suspend();
}

void *mill_chr(chan ch, void *val, size_t sz, const char *current) {
    mill_trace(current, "chr(<%d>)", (int)ch->debug.id);
    if(ch->sz != sz)
        mill_panic("receive of a type not matching the channel");
    /* Try to get a value straight away. */
    if(mill_dequeue(ch, val))
        return val;
    /* If there's no message in the buffer we are going to yield
       till a sender arrives. */
    struct mill_clause cl;
    cl.cr = mill_running;
    mill_running->state = MILL_CHR;
    cl.ep = &ch->receiver;
    cl.val = val;
    mill_list_insert(&ch->receiver.clauses, &cl.epitem, NULL);
    mill_running->receiver.clause = &cl;
    mill_set_current(&mill_running->debug, current);
    mill_suspend();
    return val;
}

void mill_chdone(chan ch, void *val, size_t sz, const char *current) {
    mill_trace(current, "chdone(<%d>)", (int)ch->debug.id);
    if(ch->done)
        mill_panic("chdone on already done-with channel");
    if(ch->sz != sz)
        mill_panic("send of a type not matching the channel");
    /* Panic if there are other senders on the same channel. */
    if(!mill_list_empty(&ch->sender.clauses))
        mill_panic("send to done-with channel");
    /* Put the channel into done-with mode. */
    ch->done = 1;
    /* Store the terminal value into a special position in the channel. */
    memcpy(((char*)(ch + 1)) + (ch->bufsz * ch->sz) , val, ch->sz);
    /* Resume all the receivers currently waiting on the channel. */
    while(!mill_list_empty(&ch->receiver.clauses)) {
        struct mill_clause *cl = mill_cont(
            mill_list_begin(&ch->receiver.clauses), struct mill_clause, epitem);
        void *dst = cl->val;
        if(!dst)
            dst = mill_valbuf_alloc(&cl->cr->valbuf, ch->sz);
        memcpy(dst, val, ch->sz);
        mill_resume(cl->cr, cl->idx);
        mill_list_erase(&ch->receiver.clauses, &cl->epitem);
    }
}

void mill_chclose(chan ch, const char *current) {
    mill_trace(current, "chclose(<%d>)", (int)ch->debug.id);
    assert(ch->refcount >= 1);
    --ch->refcount;
    if(!ch->refcount) {
        mill_list_term(&ch->sender.clauses);
        mill_list_term(&ch->receiver.clauses);
        mill_unregister_chan(&ch->debug);
        free(ch);
    }
}

void mill_choose_init(void) {
    mill_slist_init(&mill_running->chstate.clauses);
    mill_running->chstate.othws = 0;
    mill_running->chstate.available = 0;
}

void mill_choose_in(void *clause, chan ch, size_t sz, int idx) {
    if(ch->sz != sz)
        mill_panic("receive of a type not matching the channel");
    /* Find out whether the clause is immediately available. */
    int available = ch->done || !mill_list_empty(&ch->sender.clauses) ||
        ch->items ? 1 : 0;
    if(available)
        ++mill_running->chstate.available;
    /* If there are available clauses don't bother with non-available ones. */
    if(!available && mill_running->chstate.available)
        return;
    /* Fill in the clause entry. */
    struct mill_clause *cl = (struct mill_clause*) clause;
    cl->cr = mill_running;
    cl->ep = &ch->receiver;
    cl->val = NULL;
    cl->idx = idx;
    cl->available = available;
    mill_slist_push_back(&mill_running->chstate.clauses, &cl->chitem);
    /* Add the clause to the channel's list of waiting clauses. */
    mill_list_insert(&ch->receiver.clauses, &cl->epitem, NULL);
}

void mill_choose_out(void *clause, chan ch, void *val, size_t sz, int idx) {
    if(ch->done)
        mill_panic("send to done-with channel");
    if(ch->sz != sz)
        mill_panic("send of a type not matching the channel");
    /* Find out whether the clause is immediately available. */
    int available = !mill_list_empty(&ch->receiver.clauses) ||
        ch->items < ch->bufsz ? 1 : 0;
    if(available)
        ++mill_running->chstate.available;
    /* If there are available clauses don't bother with non-available ones. */
    if(!available && mill_running->chstate.available)
        return;
    /* Fill in the clause entry. */
    struct mill_clause *cl = (struct mill_clause*) clause;
    cl->cr = mill_running;
    cl->ep = &ch->sender;
    cl->val = val;
    cl->available = available;
    cl->idx = idx;
    mill_slist_push_back(&mill_running->chstate.clauses, &cl->chitem);
    /* Add the clause to the channel's list of waiting clauses. */
    mill_list_insert(&ch->sender.clauses, &cl->epitem, NULL);
}

void mill_choose_otherwise(void) {
    if(mill_running->chstate.othws != 0)
        mill_panic("multiple 'otherwise' clauses in a choose statement");
    mill_running->chstate.othws = 1;
}

int mill_choose_wait(const char *current) {
    mill_trace(current, "choose()");
    struct mill_chstate *chstate = &mill_running->chstate;
    int res = -1;
    struct mill_slist_item *it;
    /* If there are clauses that are immediately available
       randomly choose one of them. */
    if(chstate->available > 0) {
        int chosen = random() % (chstate->available);
        for (it = mill_slist_begin(&chstate->clauses); it != NULL;
              it = mill_slist_next(it)) {
            struct mill_clause *cl = mill_cont(it, struct mill_clause, chitem);
            if(cl->available) {
                if(!chosen) {
                    int ok = cl->ep->type == MILL_SENDER ?
                        mill_enqueue(mill_getchan(cl->ep), cl->val) :
                        mill_dequeue(mill_getchan(cl->ep), NULL);
                    assert(ok);
                    res = cl->idx;
                    break;
                }
                --chosen;
            }
        }
    }
    /* If not so but there's an 'otherwise' clause we can go straight to it. */
    else if(chstate->othws) {
        res = -1;
    }
    /* In all other cases block and wait for an available channel. */
    else {
        mill_running->state = MILL_CHOOSE;
        mill_set_current(&mill_running->debug, current);
        res = mill_suspend();
    }
    /* Clean-up the clause lists in queried channels. */
    for(it = mill_slist_begin(&chstate->clauses); it;
          it = mill_slist_next(it)) {
        struct mill_clause *cl = mill_cont(it, struct mill_clause, chitem);
        mill_list_erase(&cl->ep->clauses, &cl->epitem);
    }
    assert(res >= -1);
    return res;
}

void *mill_choose_val(void) {
    return mill_valbuf_get(&mill_running->valbuf);
}

