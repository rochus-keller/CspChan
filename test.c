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

typedef struct fibonacci_arg {
    CspChan_t* f;
    int x;
} fibonacci_arg;

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
        if( CspChan_fork(fibonacci,arg1) == 0 )
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
        if( CspChan_fork(fibonacci,arg2) == 0 )
        {
            /* fprintf(stderr,"fibonacci %d cancelled: tc=%d\n", fa->x, threadcount); */
            CspChan_receive(g,&y);
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
        CspChan_dispose(g);
        CspChan_receive(h,&z);
        CspChan_dispose(h);
        res = y + z;
        CspChan_send(fa->f,&res);
    }
    /* printf("fibonacci %d ended\n", fa->x); */
    free(arg);
    dec();
    return 0;
}

static void testFibonacci()
{
    CspChan_t* f = CspChan_create(1,sizeof(int));
    fibonacci_arg* arg = (fibonacci_arg*)malloc(sizeof(fibonacci_arg));
    const int in = 11; /* 11 works, 12 creates too many threads (more than 262, but significantly differs each run) */
    arg->f = f;
    arg->x = in;
    CspChan_fork(fibonacci,arg);

    int out;
    CspChan_receive(f, &out);

    CspChan_dispose(f);

    /* I: 0, 1, 2, 3, 4, 5, 6,  7,  8,  9, 10, 11,  12,  13,  14 */
    /* O: 0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377 */
    printf("fibonacci: input %d, output %d, tc=%d %s\n",in,out,threadcount,err);
    /* NOTE that threadcount only goes to 0 if we join all, but that makes it slower; if not joined
       I only observed segfaults (1 of 1000 runs) startig from in=12 */
}

typedef struct sieve_arg {
    CspChan_t* in;
    CspChan_t* inEos;
    CspChan_t* out;
    CspChan_t* outEos;
    CspChan_t* eos;
} sieve_arg;

/* another example from Birch Hansen's paper */
static void* sieve(void* arg)
{
    inc();
    sieve_arg* sa = (sieve_arg*)arg;

    int x, y;
    unsigned char eof = 0, more = 0;
    CspChan_t* succ = 0;
    CspChan_t* succEos = 0;
    CspChan_t* outEos = 0;

    CspChan_t* receivers1[2] = { sa->in, sa->inEos };
    void* rData1[2] = { &x, &eof };
    int forked = 0;
    switch( CspChan_select(receivers1, rData1, 2, 0, 0, 0) )
    {
    case 0:
        {
            sieve_arg* sa2 = (sieve_arg*)malloc(sizeof(sieve_arg));
            succ = CspChan_create(3,4);
            succEos = CspChan_create(1,1);
            outEos = CspChan_create(0,1);
            sa2->in = succ;
            sa2->inEos = succEos;
            sa2->out = sa->out;
            sa2->outEos = sa->outEos;
            sa2->eos = outEos;
            CspChan_fork(sieve,sa2);
            more = 1;
            forked = 1;
        }
        break;
    case 1:
        CspChan_send(sa->outEos,&eof);
        more = 0;
        break;
    }

    while( more )
    {
        CspChan_t* receivers2[2] = { sa->in, sa->inEos };
        void* rData2[2] = { &y, &eof };
        switch( CspChan_select(receivers2, rData2, 2, 0, 0, 0) )
        {
        case 0:
            if( y % x != 0 )
                CspChan_send(succ,&y);
            break;
        case 1:
            CspChan_send(sa->out,&x);
            eof = 1;
            CspChan_send(succEos,&eof);
            more = 0;
            break;
        }
    }

    if( forked )
    {
        CspChan_receive(outEos,&eof);
        CspChan_dispose(outEos);
        CspChan_dispose(succEos);
        CspChan_dispose(succ);
    }
    if( sa->eos )
        CspChan_send(sa->eos,&eof);
    free(sa);
    dec();
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
    CspChan_t* in;
    CspChan_t* inEof;
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
        CspChan_t* receivers[2] = { pa->in, pa->inEof };
        void* rData[2] = { &x, &eof };
        switch( CspChan_select(receivers,rData,2, 0, 0, 0) )
        {
        case 0:
            printf("prime: %d\n", x);
            fflush(stdout);
            break;
        case 1:
            run = 0;
            break;
        }
    }

    unsigned char eof = 1;
    CspChan_send(pa->outEof,&eof);
    free(pa);
    return 0;
}

static void testSieve()
{
    printf("start sieve\n"); fflush(stdout);
    CspChan_t* a = CspChan_create(3,4);
    CspChan_t* aa = CspChan_create(1,1);
    CspChan_t* b = CspChan_create(3,4);
    CspChan_t* bb = CspChan_create(3,1);
    CspChan_t* end = CspChan_create(0,1);

    generate_arg* ga = (generate_arg*)malloc(sizeof(generate_arg));
    ga->out = a;
    ga->outEof = aa;
    ga->a = 3;
    ga->b = 2;
    ga->n = 99; /* works up to 599, but not with original 4999 */
    CspChan_fork(generate,ga);

    sieve_arg* sa = (sieve_arg*)malloc(sizeof(sieve_arg));
    sa->in = a;
    sa->inEos = aa;
    sa->out = b;
    sa->outEos = bb;
    sa->eos = end;
    CspChan_fork(sieve,sa);

    print_arg* pa = (print_arg*)malloc(sizeof(print_arg));
    pa->in = b;
    pa->inEof = bb;
    pa->outEof = end;
    CspChan_fork(print,pa);

    unsigned char eof;
    CspChan_receive(end,&eof);
    CspChan_receive(end,&eof);

    CspChan_dispose(aa);
    CspChan_dispose(a);
    CspChan_dispose(bb);
    CspChan_dispose(b);
    CspChan_dispose(end);
    printf("end sieve\n"); fflush(stdout);
}

typedef struct sieve2_arg {
    CspChan_t* in;
    CspChan_t* out;
    CspChan_t* eos;
} sieve2_arg;

/* another example from Birch Hansen's paper */
static void* sieve2(void* arg)
{
    inc();
    sieve2_arg* sa = (sieve2_arg*)arg;

    int x, y;
    int eof = -1, more = 0;
    CspChan_t* succ = 0;
    CspChan_t* outEos = 0;

    int forked = 0;
    CspChan_receive(sa->in,&x);
    if( x < 0 )
    {
        CspChan_send(sa->out,&x);
        more = 0;
    }else
    {
        sieve2_arg* sa2 = (sieve2_arg*)malloc(sizeof(sieve2_arg));
        succ = CspChan_create(0,4);
        outEos = CspChan_create(0,4);
        sa2->in = succ;
        sa2->out = sa->out;
        sa2->eos = outEos;
        CspChan_fork(sieve2,sa2);
        more = 1;
        forked = 1;
    }

    while( more )
    {
        CspChan_receive(sa->in,&y);
        if( y < 0 )
        {
            CspChan_send(sa->out,&x);
            CspChan_send(succ,&eof);
            more = 0;
        }else
        {
            if( y % x != 0 )
                CspChan_send(succ,&y);
        }
    }

    if( forked )
    {
        CspChan_receive(outEos,&eof);
        CspChan_dispose(outEos);
        CspChan_dispose(succ);
    }
    if( sa->eos )
        CspChan_send(sa->eos,&eof);
    free(sa);
    dec();
    return 0;
}

typedef struct generate2_arg {
    CspChan_t* out;
    int a,b,n;
} generate2_arg;

static void* generate2(void* arg)
{
    generate2_arg* ga = (generate2_arg*)arg;
    int i = 0;
    while( i < ga->n )
    {
        int tmp = ga->a + i * ga->b;
        CspChan_send(ga->out,&tmp);
        i++;
    }
    int eos = -1;
    CspChan_send(ga->out, &eos);
    free(arg);
    return 0;
}

typedef struct print2_arg {
    CspChan_t* in;
    CspChan_t* outEof;
} print2_arg;

static void* print2(void* arg)
{
    print2_arg* pa = (print2_arg*)arg;

    int run = 1;
    while( run )
    {
        int x;
        CspChan_receive(pa->in,&x);
        if( x < 0 )
            run = 0;
        else
        {
            printf("prime: %d\n", x);
            fflush(stdout);
        }
    }

    int eof = -1;
    CspChan_send(pa->outEof,&eof);
    free(pa);
    return 0;
}

static void testSieve2()
{
    printf("start sieve\n"); fflush(stdout);
    CspChan_t* a = CspChan_create(0,4);
    CspChan_t* b = CspChan_create(0,4);
    CspChan_t* end = CspChan_create(0,4);

    generate2_arg* ga = (generate2_arg*)malloc(sizeof(generate2_arg));
    ga->out = a;
    ga->a = 3;
    ga->b = 2;
    ga->n = 99; /* works up to 999, but not with original 4999 */
    CspChan_fork(generate2,ga);

    sieve2_arg* sa = (sieve2_arg*)malloc(sizeof(sieve2_arg));
    sa->in = a;
    sa->out = b;
    sa->eos = end;
    CspChan_fork(sieve2,sa);

    print2_arg* pa = (print2_arg*)malloc(sizeof(print2_arg));
    pa->in = b;
    pa->outEof = end;
    CspChan_fork(print2,pa);

    int eof;
    CspChan_receive(pa->outEof,&eof);
    CspChan_receive(pa->outEof,&eof);

    CspChan_dispose(a);
    CspChan_dispose(b);
    CspChan_dispose(end);
    printf("end sieve\n"); fflush(stdout);
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
        CspChan_sleep(1000);
        CspChan_send(out,&i);
        CspChan_sleep(1000);
        i--;
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
            break;
        case 1:
            printf("b: %d\n",b);
            break;
        }
        fflush(stdout);
    }
    free(arg);
    return 0;
}

static void testSelect()
{
    CspChan_t* a = CspChan_create(0,4);
    CspChan_t* b = CspChan_create(0,4);
    CspChan_fork(senderA,a);
    CspChan_fork(senderB,b);
    receiverAB_arg* arg = (receiverAB_arg*)malloc(sizeof(receiverAB_arg));
    arg->a = a;
    arg->b = b;
    CspChan_fork(receiverAB,arg);

    CspChan_sleep(9000);
    CspChan_close(a);
    CspChan_close(b);

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
#if 1
    printf("tc=%d\n", threadcount);fflush(stdout);
    testFibonacci();
#endif
#if 1
    /* testSieve sometimes deadlocks, probably because there are two channels for data and eof;
     * in the Joyce original there is only one channel with two message types. */
    printf("tc=%d\n", threadcount);fflush(stdout);
    testSieve2();
    printf("tc=%d\n", threadcount);fflush(stdout);
#endif
#if 1
    testSelect();
#endif
#else
    CspChan_t* a = CspChan_create(0,4);
    CspChan_fork(tx,a);
    CspChan_fork(rx,a);

    CspChan_dispose(a);
#endif

    pthread_mutex_destroy(&mtx);
    return 0;
}
