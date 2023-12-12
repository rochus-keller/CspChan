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
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

typedef struct fibonacci_arg {
    CspChan_t* f;
    int x;
} fibonacci_arg;

static int threadcount = 0;
static pthread_mutex_t mtx;
static void inc()
{
    pthread_mutex_lock(&mtx);
    threadcount++;
    pthread_mutex_unlock(&mtx);
}
static void dec()
{
    pthread_mutex_lock(&mtx);
    threadcount--;
    pthread_mutex_unlock(&mtx);
}

static const char* err = "";

/* as in Birch Hansen, Per (1987): Joyce - A Programming Language for Distributed Systems */
static void* fibonacci(void* arg)
{
    fibonacci_arg* fa = (fibonacci_arg*)arg;
    inc();
    /* printf("fibonacci %d started: tc=%d\n", fa->x, threadcount); */
    if( fa->x <= 1 )
        CspChan_send(fa->f, &fa->x);
    else
    {
        CspChan_t* g = CspChan_create(1,sizeof(int));
        fibonacci_arg* arg1 = (fibonacci_arg*)malloc(sizeof(fibonacci_arg));
        arg1->f = g;
        arg1->x = fa->x - 1;
        int y,z;
        int res = 0;
        CspChan_ThreadId t1;
        if( (t1 = CspChan_fork(fibonacci,arg1)) == 0 )
        {
            /* fprintf(stderr,"fibonacci %d cancelled: tc=%d\n", fa->x, threadcount); */
            CspChan_dispose(g);
            CspChan_send(fa->f,&res);
            free(arg1);
            free(arg);
            dec();
            err = "error";
            return 0;
        }

        CspChan_t* h = CspChan_create(1,sizeof(int));
        fibonacci_arg* arg2 = (fibonacci_arg*)malloc(sizeof(fibonacci_arg));
        arg2->f = h;
        arg2->x = fa->x - 2;
        CspChan_ThreadId t2;
        if( (t2 = CspChan_fork(fibonacci,arg2)) == 0 )
        {
            /* fprintf(stderr,"fibonacci %d cancelled: tc=%d\n", fa->x, threadcount); */
            CspChan_receive(g,&y);
            /* CspChan_join(t1); */
            CspChan_dispose(g);
            CspChan_dispose(h);
            CspChan_send(fa->f,&res);
            free(arg2);
            free(arg);
            dec();
            err = "error";
            return 0;
        }

        CspChan_receive(g,&y);
        /* if we don't join it's much faster, but it segfaults if the thread after rendevous still accesses g. */
        /* CspChan_join(t1); */
        CspChan_dispose(g);
        CspChan_receive(h,&z);
        /* CspChan_join(t2); */
        CspChan_dispose(h);
        res = y + z;
        /* CspChan_sleep(3000); */
        CspChan_send(fa->f,&res);
    }
    /* printf("fibonacci %d ended\n", fa->x); */
    /* CspChan_sleep(1000); */
    free(arg);
    dec();
    return 0;
}

static void testFibonacci()
{
    CspChan_t* f = CspChan_create(1,sizeof(int));
    fibonacci_arg* arg = (fibonacci_arg*)malloc(sizeof(fibonacci_arg));
    const int in = 10; /* 11 works, 12 creates too many threads (more than 262, but significantly differs each run) */
    arg->f = f;
    arg->x = in;
    CspChan_ThreadId t = CspChan_fork(fibonacci,arg);

    int out;
    CspChan_receive(f, &out);

    /* CspChan_join(t); */

    CspChan_dispose(f);

    /* I: 0, 1, 2, 3, 4, 5, 6,  7,  8,  9, 10, 11,  12,  13,  14 */
    /* O: 0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377 */
    printf("fibonacci: input %d, output %d, tc=%d %s\n",in,out,threadcount,err);
    /* NOTE that threadcount only goes to 0 if we join all, but that makes it slower; if not joined
       I only observed segfaults (1 of 1000 runs) startig from in=12 */
}

typedef struct sieve_arg {
    CspChan_t* in;
    CspChan_t* eofIn;
    CspChan_t* out;
    CspChan_t* eofOut;
} sieve_arg;

/* another example from Birch Hansen's paper */
static void* sieve(void* arg)
{
    sieve_arg* sa = (sieve_arg*)arg;

    int x, y;
    unsigned char eof = 0, more = 0;
    CspChan_t* succ = CspChan_create(1,4);
    CspChan_t* eofSucc = CspChan_create(1,1);

    CspChan_ThreadId t = 0;

    CspChan_t* receivers[2] = { sa->in, sa->eofIn };
    void* rData[2] = { &x, &eof };
    switch( CspChan_select(receivers,rData,2, 0, 0, 0) )
    {
    case 0:
        {
            sieve_arg* sa2 = (sieve_arg*)malloc(sizeof(sieve_arg));
            sa2->in = succ;
            sa2->eofIn = eofSucc;
            sa2->out = sa->out;
            sa2->eofOut = sa->eofOut;
            t = CspChan_fork(sieve,sa2);
            more = 1;
        }
        break;
    case 1:
        CspChan_send(sa->eofOut,&eof);
        more = 0;
        break;
    }

    while( more )
    {
        rData[0] = &y;
        switch( CspChan_select(receivers,rData,2, 0, 0, 0) )
        {
        case 0:
            if( y % x != 0 )
                CspChan_send(succ,&y);
            break;
        case 1:
            CspChan_send(sa->out,&x);
            eof = 1;
            CspChan_send(eofSucc,&eof);
            more = 0;
            break;
        }
    }

    if( t )
        CspChan_join(t);

    CspChan_dispose(eofSucc);
    CspChan_dispose(succ);
    free(sa);
    return 0;
}

typedef struct generate_arg {
    CspChan_t* out;
    CspChan_t* outEof;
    int a,b,n;
} generate_arg;

static void* generate(void* arg)
{
    generate_arg* ga = (generate_arg*)arg;
    int i = 0;
    while( i < ga->n )
    {
        int tmp = ga->a + i * ga->b;
        CspChan_send(ga->out,&tmp);
        i++;
    }
    unsigned char eos = 1;
    CspChan_send(ga->outEof,&eos);
    free(arg);
    return 0;
}

typedef struct print_arg {
    CspChan_t* out;
    CspChan_t* outEof;
} print_arg;

static void* print(void* arg)
{
    print_arg* pa = (print_arg*)arg;

    int run = 1;
    while( run )
    {
        int x;
        unsigned char eof;
        CspChan_t* receivers[2] = { pa->out, pa->outEof };
        void* rData[2] = { &x, &eof };
        switch( CspChan_select(receivers,rData,2, 0, 0, 0) )
        {
        case 0:
            printf("%d\n", x);
            fflush(stdout);
            break;
        case 1:
            run = 0;
            break;
        }
    }

    free(pa);
    return 0;
}

static void testSieve()
{
    CspChan_t* a = CspChan_create(1,4);
    CspChan_t* aa = CspChan_create(1,1);
    CspChan_t* b = CspChan_create(1,4);
    CspChan_t* bb = CspChan_create(1,1);

    generate_arg* ga = (generate_arg*)malloc(sizeof(generate_arg));
    ga->out = a;
    ga->outEof = aa;
    ga->a = 3;
    ga->b = 2;
    ga->n = 99; /* works up to 599, but not with original 4999 */
    CspChan_ThreadId t1 = CspChan_fork(generate,ga);

    sieve_arg* sa = (sieve_arg*)malloc(sizeof(sieve_arg));
    sa->in = a;
    sa->eofIn = aa;
    sa->out = b;
    sa->eofOut = bb;
    CspChan_ThreadId t2 = CspChan_fork(sieve,sa);

    print_arg* pa = (print_arg*)malloc(sizeof(print_arg));
    pa->out = b;
    pa->outEof = bb;
    CspChan_ThreadId t3 = CspChan_fork(print,pa);

    CspChan_join(t1);
    CspChan_join(t2);
    CspChan_join(t3);

    CspChan_dispose(aa);
    CspChan_dispose(a);
    CspChan_dispose(bb);
    CspChan_dispose(b);
}

static void* senderA(void* arg)
{
    CspChan_t* out = (CspChan_t*)arg;
    int i = 0;
    while(!CspChan_closed(out))
    {
        CspChan_send(out,&i);
        i++;
        CspChan_sleep(1000);
    }
    return 0;
}

static void* senderB(void* arg)
{
    CspChan_t* out = (CspChan_t*)arg;
    int i = -1;
    while(!CspChan_closed(out))
    {
        CspChan_send(out,&i);
        i--;
        CspChan_sleep(2400);
    }
    return 0;
}

typedef struct receiverAB_arg {
    CspChan_t* a;
    CspChan_t* b;
} receiverAB_arg;

static void* receiverAB(void* arg)
{
    receiverAB_arg* ra = (receiverAB_arg*)arg;
    while( !CspChan_closed(ra->a) && !CspChan_closed(ra->b) )
    {
        int a,b;
        CspChan_t* receivers[2] = { ra->a, ra->b };
        void* rData[2] = { &a, &b };
        switch( CspChan_select(receivers,rData,2, 0, 0, 0) ) /* also works with CspChan_nb_select */
        {
        case 0:
            printf("a: %d\n",a);
            fflush(stdout);
            break;
        case 1:
            printf("b: %d\n",b);
            fflush(stdout);
            break;
        }
        /* CspChan_receive(ra->a,&tmp); */
    }
    free(arg);
    return 0;
}

static void testSelect()
{
    CspChan_t* a = CspChan_create(0,4);
    CspChan_t* b = CspChan_create(0,4);
    CspChan_ThreadId t1 = CspChan_fork(senderA,a);
    CspChan_ThreadId t3 = CspChan_fork(senderB,b);
    receiverAB_arg* arg = (receiverAB_arg*)malloc(sizeof(receiverAB_arg));
    arg->a = a;
    arg->b = b;
    CspChan_ThreadId t2 = CspChan_fork(receiverAB,arg);

    CspChan_sleep(9000);
    CspChan_close(a);
    CspChan_close(b);

    CspChan_join(t1);
    CspChan_join(t3);
    CspChan_join(t2);

    CspChan_dispose(a);
    CspChan_dispose(b);
}

static void* tx(void* arg)
{
    CspChan_t* out = (CspChan_t*)arg;
    CspChan_sleep(3000);
    int i = 12345;
    CspChan_send(out,&i);
    return 0;
}

static void* rx(void* arg)
{
    CspChan_t* in = (CspChan_t*)arg;
    CspChan_sleep(2000);
    int i;
    CspChan_receive(in,&i);
    printf("rx: %d\n", i);
    return 0;
}

int main(int argc, char *argv[])
{
    pthread_mutex_init(&mtx,0);
    srand(time(NULL));

#if 1
    /*testFibonacci();
    testSieve();*/
    testSelect();
#else
    CspChan_t* a = CspChan_create(0,4);
    CspChan_ThreadId t1 = CspChan_fork(tx,a);
    CspChan_ThreadId t2 = CspChan_fork(rx,a);

    CspChan_join(t1);
    CspChan_join(t2);
    CspChan_dispose(a);
#endif

    pthread_mutex_destroy(&mtx);
    return 0;
}
