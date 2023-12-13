/*
* Copyright 2023 Rochus Keller <mailto:me@rochus-keller.ch>
*
* This file may be used under the terms of the GNU Lesser
* General Public License version 2.1 or version 3 as published by the Free
* Software Foundation and appearing in the file LICENSE.LGPLv21 and
* LICENSE.LGPLv3 included in the packaging of this file. Please review the
* following information to ensure the GNU Lesser General Public License
* requirements will be met: https://www.gnu.org/licenses/lgpl.html and
* http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
*
* Alternatively this file may be used under the terms of the Mozilla
* Public License. If a copy of the MPL was not distributed with this
* file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "CspChan.h"
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <pthread.h>
#include <error.h>
#include <unistd.h>
#include <assert.h>

/* TODO: Win32 implementation */

enum { SignalsCount = 3 };

typedef struct Signals
{
    pthread_cond_t* sig[SignalsCount];
    struct Signals* next;
} Signals;

typedef struct CspChan_t
{
    pthread_mutex_t srMtx, observerMtx;
    pthread_cond_t condA; /* received | waiting for second thread */
    pthread_cond_t condB; /* sent | waiting for channel free */
    unsigned short msgLen;
    unsigned short closed : 1;
    unsigned short unbuffered : 1;
    unsigned short barrierPhase : 2;
    unsigned short expectingSender : 1;
    union
    {
        struct { unsigned short queueLen, msgCount, rIdx, wIdx; };
        void* dataPtr;
    };

    Signals observer;
    unsigned char data[]; /* assumes flexible array members C89 extension, or a C99 compiler, or try with data[0] */
} CspChan_t;

#define CSP_CHECK(call) if( (call)!= 0 ) fprintf(stderr,"error calling " #call " in " __FILE__ " line %d\n", __LINE__);
#define CSP_WARN_CLOSED(c) if( (c)->closed ) fprintf(stderr,"warning: using closed channel in " __FILE__ " line %d\n", __LINE__);

CspChan_t* CspChan_create(unsigned short queueLen, unsigned short msgLen)
{
    if( msgLen == 0 )
        msgLen = 1;
    /* queueLen == 0 is an unbuffered channel */
    CspChan_t* c = (CspChan_t*)malloc(sizeof(CspChan_t) + queueLen*msgLen);
    c->msgLen = msgLen;
    c->closed = 0;
    if( queueLen == 0 )
    {
        c->unbuffered = 1;
        c->dataPtr = 0;
        c->barrierPhase = 0;
    }else
    {
        c->unbuffered = 0;
        c->queueLen = queueLen;
        c->msgCount = 0;
        c->rIdx = 0;
        c->wIdx = 0;
    }
    memset(&c->observer,0,sizeof(Signals));
    CSP_CHECK(pthread_mutex_init(&c->srMtx,0));
    CSP_CHECK(pthread_mutex_init(&c->observerMtx,0));
    CSP_CHECK(pthread_cond_init(&c->condA,0));
    CSP_CHECK(pthread_cond_init(&c->condB,0));
    return c;
}

static void signal_all(CspChan_t* c)
{
    CSP_CHECK(pthread_mutex_lock(&c->observerMtx));
    Signals* s = &c->observer;
    while( s )
    {
        int i;
        for( i = 0; i < SignalsCount; i++ )
        {
            if( s->sig[i] )
                CSP_CHECK(pthread_cond_signal(s->sig[i]));
        }
        s = s->next;
    }
    CSP_CHECK(pthread_mutex_unlock(&c->observerMtx));
}

void CspChan_dispose(CspChan_t* c)
{
    CspChan_close(c);

    CSP_CHECK(pthread_cond_destroy(&c->condB));
    CSP_CHECK(pthread_cond_destroy(&c->condA));
    CSP_CHECK(pthread_mutex_destroy(&c->observerMtx));
    CSP_CHECK(pthread_mutex_destroy(&c->srMtx));

    Signals* s = c->observer.next;
    while(s)
    {
        Signals* ss = s;
        s = s->next;
        free(ss);
    }
    free(c);
}

static int is_full(CspChan_t* c)
{
    return c->msgCount == c->queueLen;
}

static int is_empty(CspChan_t* c)
{
    return c->msgCount == 0;
}

static void add_observer(CspChan_t* c, pthread_cond_t* sig)
{
    CSP_CHECK(pthread_mutex_lock(&c->observerMtx));
    Signals* s = &c->observer;
    while( s )
    {
        int i;
        for( i = 0; i < SignalsCount; i++ )
        {
            if( s->sig[i] == 0 )
            {
                s->sig[i] = sig;
                CSP_CHECK(pthread_mutex_unlock(&c->observerMtx));
                return;
            }
        }
        s = s->next;
    }
    s = (Signals*)malloc(sizeof(Signals));
    s->sig[0] = sig;
    s->next = c->observer.next;
    c->observer.next = s;

    CSP_CHECK(pthread_mutex_unlock(&c->observerMtx));
}

static void remove_observer(CspChan_t* c, pthread_cond_t* sig)
{
    CSP_CHECK(pthread_mutex_lock(&c->observerMtx));
    Signals* s = &c->observer;
    while( s )
    {
        int i;
        for( i = 0; i < SignalsCount; i++ )
        {
            if( s->sig[i] == sig )
            {
                s->sig[i] = 0;
                CSP_CHECK(pthread_mutex_unlock(&c->observerMtx));
                return;
            }
        }
        s = s->next;
    }
    CSP_CHECK(pthread_mutex_unlock(&c->observerMtx));
}

static void send(CspChan_t* c, void* data)
{
    if( c->closed )
        return;
    memcpy(c->data + c->wIdx * c->msgLen, data, c->msgLen);
    c->wIdx = (c->wIdx + 1) % c->queueLen;
    c->msgCount++;
}

static void receive(CspChan_t* c, void* data)
{
    if( c->closed )
        return;
    memcpy(data, c->data + c->rIdx * c->msgLen, c->msgLen);
    c->rIdx = (c->rIdx + 1) % c->queueLen;
    c->msgCount--;
}

static void synctwo(CspChan_t* c, void* dataPtr, int thisIsSender)
{
start:
    /* we come here with srMtx already locked */
    if( c->closed )
    {
        CSP_CHECK(pthread_mutex_unlock(&c->srMtx));
        return;
    }
    switch( c->barrierPhase )
    {
    case 0: /* I'm the first */
        c->barrierPhase = 1;
        c->expectingSender = !thisIsSender;
        c->dataPtr = dataPtr;
        signal_all(c);
        while( !c->closed && c->barrierPhase != 2 )
            CSP_CHECK(pthread_cond_wait(&c->condA,&c->srMtx));
        c->barrierPhase = 0;
        CSP_CHECK(pthread_mutex_unlock(&c->srMtx));
        /* here a third thread can interfere */
        CSP_CHECK(pthread_cond_signal(&c->condB));
        break;
    case 1: /* I'm the second */
        if( c->expectingSender != thisIsSender )
        {
            /* the caller is not the expected one, wait for another and send this one to sleep */
            CSP_CHECK(pthread_cond_wait(&c->condB,&c->srMtx));
            goto start;
        }
        if( thisIsSender )
            memcpy(c->dataPtr,dataPtr,c->msgLen);
        else
            memcpy(dataPtr,c->dataPtr,c->msgLen);
        c->barrierPhase = 2;
        CSP_CHECK(pthread_mutex_unlock(&c->srMtx));
        /* here a third thread can interfere */
        CSP_CHECK(pthread_cond_signal(&c->condA));
        break;
    case 2: /* channel occupied, wait */
        CSP_CHECK(pthread_cond_wait(&c->condB,&c->srMtx));
        goto start;
        break;
    }
}

void CspChan_send(CspChan_t* c, void* dataPtr)
{
    CSP_CHECK(pthread_mutex_lock(&c->srMtx));

    CSP_WARN_CLOSED(c); /* TODO: Golang panics in this case */

    if( c->unbuffered )
    {
        synctwo(c,dataPtr,1);
    }else
    {
        while( !c->closed && is_full(c) )
            CSP_CHECK(pthread_cond_wait(&c->condA,&c->srMtx));

        send(c,dataPtr);

        CSP_CHECK(pthread_mutex_unlock(&c->srMtx));

        /* NOTE: if the receiver disposes of the channel before the following is complete, a segfault might occur;
           the order is relevant; c->sent must be signalled last, because observers usually don't dispose the channel */
        signal_all(c);
        CSP_CHECK(pthread_cond_signal(&c->condB));
    }
}

void CspChan_receive(CspChan_t* c, void* dataPtr)
{
    CSP_CHECK(pthread_mutex_lock(&c->srMtx));

    if( c->closed )
    {
        memset(dataPtr,0,c->msgLen);
        return;
    }

    if( c->unbuffered )
    {
        synctwo(c,dataPtr,0);
    }else
    {
        while( !c->closed && is_empty(c) )
            CSP_CHECK(pthread_cond_wait(&c->condB,&c->srMtx));

        receive(c,dataPtr);

        CSP_CHECK(pthread_mutex_unlock(&c->srMtx));

        signal_all(c);
        CSP_CHECK(pthread_cond_signal(&c->condA));
    }
}

static int anyready(CspChan_t** receiver, unsigned int rCount,
                     CspChan_t** sender, unsigned int sCount, CspChan_t** ready)
{
    int i = 0, n = 0, closed = 0;
    while( i < (rCount+sCount) )
    {
        CspChan_t* c = 0;
        if( i < rCount )
            c = receiver[i];
        else
        {
            c = sender[i-rCount];
            CSP_WARN_CLOSED(c);
        }
        if( c->closed )
        {
            ready[i] = 0;
            closed++;
        }else if( pthread_mutex_trylock(&c->srMtx) == 0 )
        {
            int ok = 0;
            if( c->unbuffered )
            {
                ok = c->barrierPhase == 1 &&
                        ((c->expectingSender && i >= rCount) || (!c->expectingSender && i < rCount) );
            }else
            {
                if( i < rCount )
                    ok = !is_empty(c);
                else
                    ok = !is_full(c);
            }

            if( ok )
            {
                ready[i] = c;
                n++;
            }else
            {
                ready[i] = 0;
                CSP_CHECK(pthread_mutex_unlock(&c->srMtx));
            }
        }else
            ready[i] = 0;
        i++;
    }
    if( n == 0 && closed )
        n = -1;
    return n;
}

static int doselect(int n, void** rData, unsigned int rCount, void** sData, unsigned int sCount, CspChan_t** ready)
{
    if( n <= 0 )
        return -1;

    int candidate = rand() % n;
    CspChan_t* c = 0;
    int i;
    for( i = 0; i < (rCount+sCount); i++ )
    {
        if( ready[i] )
        {
            if( candidate == 0 )
            {
                c = ready[i];
                n = i;
            }else
                CSP_CHECK(pthread_mutex_unlock(&ready[i]->srMtx));
            candidate--;
        }
    }
    if( c->unbuffered )
    {
        if( n >= rCount )
            memcpy(c->dataPtr,sData[n-rCount],c->msgLen);
        else
            memcpy(rData[n],c->dataPtr,c->msgLen);
        c->barrierPhase = 2;
        CSP_CHECK(pthread_mutex_unlock(&c->srMtx));
        signal_all(c);
        CSP_CHECK(pthread_cond_signal(&c->condA));
    }else
    {
        if( n < rCount )
        {
            receive(c,rData[n]);
            CSP_CHECK(pthread_mutex_unlock(&c->srMtx));
            signal_all(c);
            CSP_CHECK(pthread_cond_signal(&c->condA));
        }else
        {
            send(c,sData[n-rCount]);
            CSP_CHECK(pthread_mutex_unlock(&c->srMtx));
            signal_all(c);
            CSP_CHECK(pthread_cond_signal(&c->condB));
        }
    }
    return n;
}

int CspChan_select(CspChan_t** receiver, void** rData, unsigned int rCount,
                         CspChan_t** sender, void** sData, unsigned int sCount)
{
    CspChan_t** ready = (CspChan_t**)malloc(sizeof(CspChan_t*)*(rCount+sCount));

    pthread_mutex_t mtx;
    pthread_cond_t sig;

    CSP_CHECK(pthread_mutex_init(&mtx,0));
    CSP_CHECK(pthread_cond_init(&sig,0));

    CSP_CHECK(pthread_mutex_lock(&mtx));

    int i;
    for( i = 0; i < (rCount+sCount); i++ )
    {
        if( i < rCount )
            add_observer(receiver[i], &sig);
        else
            add_observer(sender[i-rCount], &sig);
    }

    int n;
    while( (n = anyready(receiver, rCount, sender, sCount, ready)) == 0  )
        CSP_CHECK(pthread_cond_wait(&sig,&mtx));

    n = doselect(n, rData, rCount, sData, sCount, ready );

    for( i = 0; i < (rCount+sCount); i++ )
    {
        if( i < rCount )
            remove_observer(receiver[i], &sig);
        else
            remove_observer(sender[i-rCount], &sig);
    }

    CSP_CHECK(pthread_mutex_unlock(&mtx));
    CSP_CHECK(pthread_cond_destroy(&sig));
    CSP_CHECK(pthread_mutex_destroy(&mtx));

    free(ready);

    return n;
}

int CspChan_nb_select(CspChan_t** receiver, void** rData, unsigned int rCount,
                      CspChan_t** sender, void** sData, unsigned int sCount)
{
    CspChan_t** ready = (CspChan_t**)malloc(sizeof(CspChan_t*)*(rCount+sCount));

    int n;

    n = anyready(receiver, rCount, sender, sCount, ready);
    n = doselect(n, rData, rCount, sData, sCount, ready );

    free(ready);

    return n;
}

int CspChan_fork(void* (*agent)(void*), void* arg)
{
    pthread_t t = 0;
    pthread_attr_t attr;
    CSP_CHECK(pthread_attr_init( &attr ));
    CSP_CHECK(pthread_attr_setdetachstate ( &attr , PTHREAD_CREATE_DETACHED ));
    const int res = pthread_create(&t,&attr,agent,arg);
    pthread_attr_destroy ( &attr );
    if( res != 0 )
    {
        fprintf(stderr,"error creating pthread: %d %s\n", res, strerror(res));
        fflush(stderr);
        return 0;
    }else
        return 1;
}

void CspChan_sleep(unsigned int milliseconds)
{
    usleep(milliseconds*1000);
}

void CspChan_close(CspChan_t* c)
{
    pthread_mutex_lock(&c->srMtx);
    c->closed = 1;
    pthread_mutex_unlock(&c->srMtx);
    signal_all(c);
    pthread_cond_broadcast(&c->condB);
    pthread_cond_broadcast(&c->condA);
}

int CspChan_closed(CspChan_t* c)
{
    if( c )
        return c->closed;
    else
        return 1;
}
