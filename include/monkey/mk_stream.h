/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2015 Monkey Software LLC <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef MK_STREAM_H
#define MK_STREAM_H

#include <monkey/mk_iov.h>
#include <monkey/mk_list.h>

/*
 * Stream types: each stream can have a different
 * source of information and for hence it handler
 * may need to be different for each cases.
 */
#define MK_STREAM_RAW     0  /* raw data from buffer */
#define MK_STREAM_IOV     1  /* mk_iov struct        */
#define MK_STREAM_PTR     2  /* mk_ptr               */
#define MK_STREAM_FILE    3  /* opened file          */
#define MK_STREAM_SOCKET  4  /* socket, scared..     */


/* Channel return values for write event */
#define MK_CHANNEL_ERROR  -1  /* exception when flusing data  */
#define MK_CHANNEL_DONE    0  /* channel consumed all streams */
#define MK_CHANNEL_FLUSH   1  /* channel flushed some data    */
#define MK_CHANNEL_EMPTY   2  /* no streams available         */
#define MK_CHANNEL_UNKNOWN 4  /* unhandled                    */

/* Channel status */
#define MK_CHANNEL_DISABLED 0 /* channel is sleeping */
#define MK_CHANNEL_ENABLED  1 /* channel enabled, have some data */

/*
 * Channel types: by default the only channel supported
 * is a direct write to the network layer.
 */
#define MK_CHANNEL_SOCKET 0

/*
 * A channel represents an end-point of a stream, for short
 * where the stream data consumed is send to.
 */
struct mk_channel {
    int type;
    int fd;
    int status;
    struct mk_list streams;
};

/*
 * A stream represents an Input of data that can be consumed
 * from a specific resource given it's type.
 */
struct mk_stream {
    int type;              /* stream type                      */
    int fd;                /* file descriptor                  */
    int preserve;          /* preserve stream? (do not unlink) */
    int encoding;          /* some output encoding ?           */

    /* bytes info */
    size_t bytes_total;
    off_t  bytes_offset;

    /* the outgoing channel, we do this for all streams */
    struct mk_channel *channel;

    /*
     * Based on the stream type, 'data' could reference a RAW buffer
     * or a mk_iov struct.
     */
    void *buffer;

    /* Some data the user may want to reference with the stream (optional) */
    void *data;

    /* callbacks */
    void (*cb_finished) (struct mk_stream *);
    void (*cb_ok) (struct mk_stream *);
    void (*cb_bytes_consumed) (struct mk_stream *, long);
    void (*cb_exception) (struct mk_stream *, int);

    /* Link to the Channel parent */
    struct mk_list _head;
};

/* exported functions */
static inline void mk_channel_append_stream(struct mk_channel *channel,
                                            struct mk_stream *stream)
{
    mk_list_add(&stream->_head, &channel->streams);
}

static inline void mk_stream_set(struct mk_stream *stream, int type,
                                 struct mk_channel *channel,
                                 void *buffer,
                                 size_t size,
                                 void *data,
                                 void (*cb_finished) (struct mk_stream *),
                                 void (*cb_bytes_consumed) (struct mk_stream *, long),
                                 void (*cb_exception) (struct mk_stream *, int))
{
    mk_ptr_t *ptr;
    struct mk_iov *iov;

    stream->type         = type;
    stream->channel      = channel;
    stream->bytes_offset = 0;
    stream->buffer       = buffer;
    stream->data         = data;
    stream->preserve     = MK_FALSE;

    if (type == MK_STREAM_IOV) {
        iov = buffer;
        stream->bytes_total = iov->total_len;
    }
    else if (type == MK_STREAM_PTR) {
        ptr = buffer;
        stream->bytes_total = ptr->len;
    }
    else {
        stream->bytes_total = size;
    }

    /* callbacks */
    stream->cb_finished       = cb_finished;
    stream->cb_bytes_consumed = cb_bytes_consumed;
    stream->cb_exception      = cb_exception;

    mk_list_add(&stream->_head, &channel->streams);
}

static inline void mk_stream_unlink(struct mk_stream *stream)
{
    mk_list_del(&stream->_head);
}

/* Mark a specific number of bytes served (just on successfull flush) */
static inline void mk_stream_bytes_consumed(struct mk_stream *stream, long bytes)
{
#ifdef TRACE
    char *fmt = NULL;

    if (stream->type == MK_STREAM_RAW) {
        fmt = "[STREAM_RAW %p] bytes consumed %lu/%lu";
    }
    else if (stream->type == MK_STREAM_IOV) {
        fmt = "[STREAM_IOV %p] bytes consumed %lu/%lu";
    }
    else if (stream->type == MK_STREAM_FILE) {
        fmt = "[STREAM_FILE %p] bytes consumed %lu/%lu";
    }
    else if (stream->type == MK_STREAM_SOCKET) {
        fmt = "[STREAM_SOCK %p] bytes consumed %lu/%lu";
    }
    else {
        fmt = "[STREAM_UNKW %p] bytes consumed %lu/%lu";
    }

    MK_TRACE(fmt, stream, stream->bytes_total, bytes);
#endif

    stream->bytes_total -= bytes;
}

static inline void mk_channel_debug(struct mk_channel *channel)
{
    int i = 0;
    struct mk_list *head;
    struct mk_stream *stream;

    printf("\n*** Channel ***\n");
    mk_list_foreach(head, &channel->streams) {
        stream = mk_list_entry(head, struct mk_stream, _head);
        switch (stream->type) {
        case MK_STREAM_RAW:
            printf("%i) [%p] STREAM RAW   : ", i, stream);
            break;
        case MK_STREAM_IOV:
            printf("%i) [%p] STREAM IOV   : ", i, stream);
            break;
        case MK_STREAM_PTR:
            printf("%i) [%p] STREAM PTR   : ", i, stream);
            break;
        case MK_STREAM_FILE:
            printf("%i) [%p] STREAM FILE  : ", i, stream);
            break;
        case MK_STREAM_SOCKET:
            printf("%i) [%p] STREAM SOCKET: ", i, stream);
            break;
        }
#if defined(__APPLE__)
        printf("bytes=%lld/%lu\n", stream->bytes_offset, stream->bytes_total);
#else
        printf("bytes=%ld/%lu\n", stream->bytes_offset, stream->bytes_total);
#endif
        i++;
    }
}

struct mk_stream *mk_stream_new(int type, struct mk_channel *channel,
                           void *buffer, size_t size, void *data,
                           void (*cb_finished) (struct mk_stream *),
                           void (*cb_bytes_consumed) (struct mk_stream *, long),
                           void (*cb_exception) (struct mk_stream *, int));
struct mk_channel *mk_channel_new(int type, int fd);
int mk_channel_write(struct mk_channel *channel);

#endif
