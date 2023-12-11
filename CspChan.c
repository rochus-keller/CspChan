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
    pthread_cond_t received, sent;
    unsigned short msgLen, queueLen, msgCount, rIdx, wIdx;
    unsigned char closed;
    Signals observer;
    unsigned char data[]; /* assumes flexible array members C89 extension, or a C99 compiler, or try with data[0] */
} CspChan_t;

CspChan_t* CspChan_create(unsigned short queueLen, unsigned short msgLen)
{
    if( queueLen == 0 )
        queueLen = 1;
    if( msgLen == 0 )
        msgLen = 1;
    CspChan_t* c = (CspChan_t*)malloc(sizeof(CspChan_t) + queueLen*msgLen);
    c->queueLen = queueLen;
    c->msgCount = 0;
    c->msgLen = msgLen;
    c->rIdx = 0;
    c->wIdx = 0;
    c->closed = 0;
    memset(&c->observer,0,sizeof(Signals));
    pthread_mutex_init(&c->srMtx,0);
    pthread_mutex_init(&c->observerMtx,0);
    pthread_cond_init(&c->received,0);
    pthread_cond_init(&c->sent,0);
    return c;
}

static void signal_all(CspChan_t* c)
{
    pthread_mutex_lock(&c->observerMtx);
    Signals* s = &c->observer;
    while( s )
    {
        int i;
        for( i = 0; i < SignalsCount; i++ )
        {
            if( s->sig[i] )
                pthread_cond_signal(s->sig[i]);
        }
        s = s->next;
    }
    pthread_mutex_unlock(&c->observerMtx);
}

void CspChan_dispose(CspChan_t* c)
{
    signal_all(c);
    Signals* s = c->observer.next;
    while(s)
    {
        Signals* ss = s;
        s = s->next;
        free(ss);
    }
    pthread_cond_broadcast(&c->sent);
    pthread_cond_broadcast(&c->received);
    pthread_cond_destroy(&c->sent);
    pthread_cond_destroy(&c->received);
    pthread_mutex_destroy(&c->observerMtx);
    pthread_mutex_destroy(&c->srMtx);
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
    pthread_mutex_lock(&c->observerMtx);
    Signals* s = &c->observer;
    while( s )
    {
        int i;
        for( i = 0; i < SignalsCount; i++ )
        {
            if( s->sig[i] == 0 )
            {
                s->sig[i] = sig;
                pthread_mutex_unlock(&c->observerMtx);
                return;
            }
        }
        s = s->next;
    }
    s = (Signals*)malloc(sizeof(Signals));
    s->sig[0] = sig;
    s->next = c->observer.next;
    c->observer.next = s;

    pthread_mutex_unlock(&c->observerMtx);
}

static void remove_observer(CspChan_t* c, pthread_cond_t* sig)
{
    pthread_mutex_lock(&c->observerMtx);
    Signals* s = &c->observer;
    while( s )
    {
        int i;
        for( i = 0; i < SignalsCount; i++ )
        {
            if( s->sig[i] == sig )
            {
                s->sig[i] = 0;
                pthread_mutex_unlock(&c->observerMtx);
                return;
            }
        }
        s = s->next;
    }
    pthread_mutex_unlock(&c->observerMtx);
}

static void send(CspChan_t* c, void* data)
{
    if( c->closed )
        return;
    memcpy(c->data + c->wIdx * c->msgLen, data, c->msgLen);
    c->wIdx = (c->wIdx + 1) % c->queueLen;
    c->msgCount++;
}

void CspChan_send(CspChan_t* c, void* dataPtr)
{
    pthread_mutex_lock(&c->srMtx);

    while( !c->closed && is_full(c) )
    {
        int i = 0;
        pthread_cond_wait(&c->received,&c->srMtx);
        i = 1;
    }

    send(c,dataPtr);

    pthread_mutex_unlock(&c->srMtx);

    /* NOTE: if the receiver disposes of the channel before the following is complete, a segfault might occur;
       the order is relevant; c->sent must be signalled last, because observers usually don't dispose the channel */
    signal_all(c);
    pthread_cond_signal(&c->sent);
}

static void receive(CspChan_t* c, void* data)
{
    if( c->closed )
        return;
    memcpy(data, c->data + c->rIdx * c->msgLen, c->msgLen);
    c->rIdx = (c->rIdx + 1) % c->queueLen;
    c->msgCount--;
}

void CspChan_receive(CspChan_t* c, void* dataPtr)
{
    pthread_mutex_lock(&c->srMtx);

    while( !c->closed && is_empty(c) )
    {
        int i = 0;
        pthread_cond_wait(&c->sent,&c->srMtx);
        i = 1;
    }

    receive(c,dataPtr);

    pthread_mutex_unlock(&c->srMtx);

    signal_all(c);
    pthread_cond_signal(&c->received);
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
            c = sender[i-rCount];
        if( c->closed )
        {
            ready[i] = 0;
            closed++;
        }else if( pthread_mutex_trylock(&c->srMtx) == 0 )
        {
            int ok = 0;
            if( i < rCount )
                ok = !is_empty(c);
            else
                ok = !is_full(c);

            if( ok )
            {
                ready[i] = c;
                n++;
            }else
            {
                ready[i] = 0;
                pthread_mutex_unlock(&c->srMtx);
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
                pthread_mutex_unlock(&ready[i]->srMtx);
            candidate--;
        }
    }
    if( n < rCount )
    {
        receive(c,rData[n]);
        pthread_mutex_unlock(&c->srMtx);
        signal_all(c);
        pthread_cond_signal(&c->received);
    }else
    {
        send(c,sData[n-rCount]);
        pthread_mutex_unlock(&c->srMtx);
        signal_all(c);
        pthread_cond_signal(&c->sent);
    }
    return n;
}

int CspChan_select(CspChan_t** receiver, void** rData, unsigned int rCount,
                         CspChan_t** sender, void** sData, unsigned int sCount)
{
    CspChan_t** ready = (CspChan_t**)malloc(sizeof(CspChan_t*)*(rCount+sCount));

    pthread_mutex_t mtx;
    pthread_cond_t sig;
    pthread_mutex_init(&mtx,0);
    pthread_cond_init(&sig,0);

    pthread_mutex_lock(&mtx);

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
        pthread_cond_wait(&sig,&mtx);

    n = doselect(n, rData, rCount, sData, sCount, ready );

    for( i = 0; i < (rCount+sCount); i++ )
    {
        if( i < rCount )
            remove_observer(receiver[i], &sig);
        else
            remove_observer(sender[i-rCount], &sig);
    }

    pthread_cond_destroy(&sig);
    pthread_mutex_destroy(&mtx);

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

CspChan_ThreadId CspChan_fork(void* (*agent)(void*), void* arg)
{
    pthread_t t = 0;
    const int res = pthread_create(&t,0,agent,arg);
    if( res == 0 )
        return t;
    else
    {
        fprintf(stderr,"error creating pthread: %d %s\n", res, strerror(res));
        fflush(stderr);
        return 0;
    }
}

void CspChan_join(CspChan_ThreadId t)
{
    if( t == 0 )
        return;
    pthread_join(t,0);
}

void CspChan_sleep(unsigned int milliseconds)
{
    usleep(milliseconds*1000);
}

void CspChan_close(CspChan_t* c)
{
    signal_all(c);
    pthread_cond_broadcast(&c->sent);
    pthread_cond_broadcast(&c->received);
    c->closed = 1;
}

int CspChan_closed(CspChan_t* c)
{
    if( c )
        return c->closed;
    else
        return 1;
}