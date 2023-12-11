#ifndef CSP_CHANNEL_H
#define CSP_CHANNEL_H

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

/* CspChan:
 * This is a C89 library which implements CSP channels in a similar way as e.g. in the Go programmin language.
 * The library uses pthreads (on Unix) or Win32 threads (on Windows, TBD). Thus the practical number
 * of available threads is much lower than in languages like Go, Joyce or SuperPascal. But the semantics
 * of the channels otherwise corresponds pretty well to the mentioned languages, including the either
 * blocking or non-blocking select. A future version of the library might support thread pools or even
 * something like Go routines (with user-mode scheduling in addition to threads). */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
/* comment the following #define if you statically link the library. */
#define CSPCHANEXP __declspec(dllexport)
#else
#define CSPCHANEXP
#endif

/* Main API */

typedef struct CspChan_t CspChan_t;

/* CspChan_create:
 * Create a channel suited to transport messages of msgLen bytes. The channel blocks on send
 * (if it is full) and receive (if it is empty). As in Go the capacity of the channel can be
 * set using the queueLen parameter. As soon as created a channel can be used for communication
 * until it is closed or disposed. Channels are thread-safe and thus can safely be passed to and
 * used by other threads in parallel. */
CSPCHANEXP CspChan_t* CspChan_create(unsigned short queueLen, unsigned short msgLen );

/* CspChan_close:
 * The call to CspChan_close is optional; it is useful to signal to a thread that it should stop
 * running without an extra channel. It is legal though to directly call CspChan_dispose when
 * the channel is no longer used. This procedure also signals all threads waiting on this channel. */
CSPCHANEXP void CspChan_close(CspChan_t*);

/* CspChan_closed:
 * Returns the closed status of the channel. As soon as CspChan_close was called, this function
 * returns 1, otherwise 0. */
CSPCHANEXP int CspChan_closed(CspChan_t*);

/* CspChan_dispose:
 * Delete a channel which was created by CspChan_create earlier. This procedure also signals all threads
 * waiting on this channel. After the call the channel pointer is invalid. */
CSPCHANEXP void CspChan_dispose(CspChan_t*);

/* CspChan_send:
 * Send a message of msgLen bytes (see CspChan_create) over the channel. If the channel is full (see
 * queueLen of CspChan_create) the calling thread blocks, thus waiting for a rendezvous with a thread
 * calling CspChan_receive on the same channel. If the channel was closed the call immediately returns
 * with no effect. The parameter dataPtr is the address of the variable the data of which are sent. */
CSPCHANEXP void CspChan_send(CspChan_t*, void* dataPtr);

/* CspChan_receive:
 * Receive a message of msgLen bytes (see CspChan_create) from the channel. If the channel is empty (see
 * queueLen of CspChan_create) the calling thread blocks, thus waiting for a rendezvous with a thread
 * calling CspChan_send on the same channel. If the channel was closed the call immediately returns
 * with no effect. The parameter dataPtr is the address of the variable which receives the data. */
CSPCHANEXP void CspChan_receive(CspChan_t*, void* dataPtr);

/* CspChan_select:
 * This function works like the select statement (without default) of the Go programming language.
 * It accepts an array of receiver channels and receiver variable addresses of length rCount and an
 * array of sender channels and sender variable addresses of length sCount. If rCount is 0, receiver
 * and rData can be NULL; if sCount is 0, sender and sData can be NULL.
 * When called, CspChan_select checks which receivers and senders are ready for communication, and
 * randomly (using the C rand() function) selects one and calls CspChan_send/receive on it. If none
 * of the channels is ready for communication, the call blocks until any of the channels is ready, and
 * then calls CspChan_send/receive on it. The function returns the index of the selected channel or -1;
 * the index assumes a combined receiver|sender array, i.e. an index >= rCount applies to the sender array.
 * Closed channels are ignored by this function. */
CSPCHANEXP int CspChan_select(CspChan_t** receiver, void** rData, unsigned int rCount,
                        CspChan_t** sender, void** sData, unsigned int sCount );

/* CspChan_nb_select:
 * This function works like the select statement (with default) of the Go programming language.
 * It accepts an array of receiver channels and receiver variable addresses of length rCount and an
 * array of sender channels and sender variable addresses of length sCount. If rCount is 0, receiver
 * and rData can be NULL; if sCount is 0, sender and sData can be NULL.
 * When called, CspChan_nb_select checks which receivers and senders are ready for communication, and
 * randomly (using the C rand() function) selects one and calls CspChan_send/receive on it. If none
 * of the channels is ready for communication, the call immediately returns with -1.
 * The function otherwise returns the index of the selected channel; the index assumes a combined
 * receiver|sender array, i.e. an index >= rCount applies to the sender array.
 * Closed channels are ignored by this function. */
CSPCHANEXP int CspChan_nb_select(CspChan_t** receiver, void** rData, unsigned int rCount,
                        CspChan_t** sender, void** sData, unsigned int sCount );


/* Helper API for applications which don't want to directly deal with a thread API. */

typedef unsigned long CspChan_ThreadId;

/* CspChan_fork:
 * Create a new thread (or possibly re-use one from the pool) and use it to run the agent function.
 * If the thread could not be started (possibly because no more threads are available) the function
 * returns 0, otherwise it returns a valid ThreadId. */
CSPCHANEXP CspChan_ThreadId CspChan_fork(void* (*agent)(void*), void * arg);

/* CspChan_join:
 * This procedure is optional and not actually necessary for a CSP implementation, because the same effect
 * can be achieved with a channel. It requires the CspChan_ThreadId returned by CspChan_fork which could
 * be ignored otherwise. */
CSPCHANEXP void CspChan_join(CspChan_ThreadId);

/* CspChan_sleep:
 * Suspends the calling thread for the given number of milliseconds. */
CSPCHANEXP void CspChan_sleep(unsigned int milliseconds);

#ifdef __cplusplus
}
#endif

#endif /* CSP_CHANNEL_H */
