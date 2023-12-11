This C library implements channels, as introduced by Hoare (Communicating Sequential Processes, 1985) and popularized by the Go programming language.

Unbuffered as well as buffered channels are supported, and also the select statement in its blocking and non-blocking variants, as in the Go programming language. 

The library currently works with Ptreads; support for Win32 threads is work in progress.

### How to use

Just include the CspChan.h and CspChan.c files in your project, or build a shared library with the CspChan.c file. 

More information can be found in the source code.

### Example

```c
#include <stdlib.h>
#include <stdio.h>
#include "CspChan.h"

static void* senderA(void* arg) {
    CspChan_t* out = (CspChan_t*)arg;
    int i = 0;
    while(!CspChan_closed(out)) {
        CspChan_send(out,&i);
        i++;
        CspChan_sleep(1000);
    }
    return 0;
}

static void* senderB(void* arg) {
    CspChan_t* out = (CspChan_t*)arg;
    int i = -1;
    while(!CspChan_closed(out)) {
        CspChan_send(out,&i);
        i--;
        CspChan_sleep(3000);
    }
    return 0;
}

typedef struct receiverAB_arg {
    CspChan_t* a;
    CspChan_t* b;
} receiverAB_arg;

static void* receiverAB(void* arg) {
    receiverAB_arg* ra = (receiverAB_arg*)arg;
    while( !CspChan_closed(ra->a) && !CspChan_closed(ra->b) ) {
        int a,b;
        CspChan_t* receivers[2] = { ra->a, ra->b };
        void* rData[2] = { &a, &b };
        switch( CspChan_select(receivers,rData,2, 0, 0, 0) ) {
        case 0:
            printf("a: %d\n",a);
            fflush(stdout);
            break;
        case 1:
            printf("b: %d\n",b);
            fflush(stdout);
            break;
        }
    }
    free(arg);
    return 0;
}

int main()
{
    CspChan_t* a = CspChan_create(1,4);
    CspChan_t* b = CspChan_create(1,4);
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
```

In addition, test.c includes some of the examples from Birch Hansen, Per (1987): Joyce - A Programming Language for Distributed Systems. 


### Planned or work-in-progress features

- [x] Unix version with blocking and non-blocking select
- [ ] Windows version
- [ ] Implement a thread-pool to re-use threads instead of starting a new one with each call to CspChan_fork to improve performance

### Related work

There are a couple of C++ implementations of CSP channels, e.g. https://www.cs.kent.ac.uk/projects/ofa/c++csp/ or https://github.com/atollk/copper, but the intention of the present library is a C89 implementation of Go channels.

For C there are also some libraries, partially with similar goals as the present one.

Pipe (https://github.com/cgaebel/pipe) is a C99 implementation of a thread-safe FIFO. The library is very well documented, but also more complex than the present one, and with more dynamic allocations (CspChan uses a simple fixed size ring buffer instead, like Go). The Pipe library has no select implementation, and adding one seems pretty complicated and requires changes to the library.

The Chan library (https://github.com/tylertreat/chan) is a C implementation of Go channels and shares the same goals as the present library. There is even an implementation of Go select. The implementation is rather complex (assumingly too complex, with separate implementations for buffered and unbuffered channels) and also with more dynamic allocations than strictly necessary; apparently only non-blocking selects are supported (i.e. only Go selects with default); adding blocking selects would require significant changes to the library.


## Support

If you need support or would like to post issues or feature requests please use the Github issue list at https://github.com/rochus-keller/CspChan/issues or send an email to the author.

