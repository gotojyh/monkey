/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2014 Monkey Software LLC <eduardo@monkey.io>
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

/* mk_request.c */
#include "mk_header.h"
#include "mk_file.h"
#include "mk_memory.h"
#include "mk_scheduler.h"
#include "mk_limits.h"

#ifndef MK_REQUEST_H
#define MK_REQUEST_H

/* Request buffer chunks = 4KB */
#define MK_REQUEST_CHUNK (int) 4096
#define MK_REQUEST_DEFAULT_PAGE  "<HTML><HEAD><STYLE type=\"text/css\"> body {font-size: 12px;} </STYLE></HEAD><BODY><H1>%s</H1>%s<BR><HR><ADDRESS>Powered by %s</ADDRESS></BODY></HTML>"

#define MK_CRLF "\r\n"
#define MK_ENDBLOCK "\r\n\r\n"

extern const mk_ptr_t mk_crlf;
extern const mk_ptr_t mk_endblock;

/* Headers */
#define RH_ACCEPT "Accept:"
#define RH_ACCEPT_CHARSET "Accept-Charset:"
#define RH_ACCEPT_ENCODING "Accept-Encoding:"
#define RH_ACCEPT_LANGUAGE "Accept-Language:"
#define RH_CONNECTION "Connection:"
#define RH_COOKIE "Cookie:"
#define RH_CONTENT_LENGTH "Content-Length:"
#define RH_CONTENT_RANGE "Content-Range:"
#define RH_CONTENT_TYPE	"Content-Type:"
#define RH_IF_MODIFIED_SINCE "If-Modified-Since:"
#define RH_HOST	"Host:"
#define RH_LAST_MODIFIED "Last-Modified:"
#define RH_LAST_MODIFIED_SINCE "Last-Modified-Since:"
#define RH_REFERER "Referer:"
#define RH_RANGE "Range:"
#define RH_USER_AGENT "User-Agent:"

extern const mk_ptr_t mk_rh_accept;
extern const mk_ptr_t mk_rh_accept_charset;
extern const mk_ptr_t mk_rh_accept_encoding;
extern const mk_ptr_t mk_rh_accept_language;
extern const mk_ptr_t mk_rh_connection;
extern const mk_ptr_t mk_rh_cookie;
extern const mk_ptr_t mk_rh_content_length;
extern const mk_ptr_t mk_rh_content_range;
extern const mk_ptr_t mk_rh_content_type;
extern const mk_ptr_t mk_rh_if_modified_since;
extern const mk_ptr_t mk_rh_host;
extern const mk_ptr_t mk_rh_last_modified;
extern const mk_ptr_t mk_rh_last_modified_since;
extern const mk_ptr_t mk_rh_referer;
extern const mk_ptr_t mk_rh_range;
extern const mk_ptr_t mk_rh_user_agent;

/* String limits */
#define MAX_REQUEST_METHOD 10
#define MAX_REQUEST_URI 1025
#define MAX_REQUEST_PROTOCOL 10
#define MAX_SCRIPTALIAS 3

#define MK_REQUEST_STATUS_INCOMPLETE -1
#define MK_REQUEST_STATUS_COMPLETED 0

#define EXIT_NORMAL 0
#define EXIT_ERROR -1
#define EXIT_ABORT -2
#define EXIT_PCONNECTION 24

#define MK_HEADERS_TOC_LEN 32

struct response_headers
{
    int status;

    /* Connection flag, if equal -1, the connection header is ommited */
    int connection;

    /*
     * If some plugins wants to set a customized HTTP status, here
     * is the 'how and where'
     */
    mk_ptr_t custom_status;

    /* Length of the content to send */
    long content_length;

    /* Private value, real length of the file requested */
    long real_length;

    int cgi;
    int pconnections_left;
    int breakline;

    int transfer_encoding;

    int ranges[2];

    time_t last_modified;
    mk_ptr_t allow_methods;
    mk_ptr_t content_type;
    mk_ptr_t content_encoding;
    char *location;

    /*
     * This field allow plugins to add their own response
     * headers
     */
    struct mk_iov *_extra_rows;

    /* Flag to track if the response headers were sent */
    int sent;

};

struct header_toc_row
{
    char *init;
    char *end;
    int status;                 /* 0: not found, 1: found = skip! */
};

struct headers_toc
{
    struct header_toc_row rows[MK_HEADERS_TOC_LEN];
    int length;
};

struct session_request
{
    int status;
    int protocol;
    /* is keep-alive request ? */
    int keep_alive;

    /* is it serving a user's home directory ? */
    int user_home;

    /*-Connection-*/
    long port;
    /*------------*/

    /*
     * Static file file descriptor: the following twp fields represents an
     * opened file in the file system and a flag saying which mechanism
     * was used to open it.
     *
     *  - fd_file  : common file descriptor
     *  - fd_is_fdt: set to MK_TRUE if fd_file was opened using Vhost FDT, or
     *               MK_FALSE for the opposite case.
     */
    int fd_file;
    int fd_is_fdt;


    int headers_len;

    /*----First header of client request--*/
    int method;
    mk_ptr_t method_p;
    mk_ptr_t uri;                  /* original request */
    mk_ptr_t uri_processed;        /* processed request (decoded) */

    mk_ptr_t protocol_p;

    mk_ptr_t body;





    /* If request specify Connection: close, Monkey will
     * close the connection after send the response, by
     * default this var is set to VAR_OFF;
     */
    int close_now;

    /*---Request headers--*/
    int content_length;

    mk_ptr_t content_type;
    mk_ptr_t connection;

    mk_ptr_t host;
    mk_ptr_t host_port;
    mk_ptr_t if_modified_since;
    mk_ptr_t last_modified_since;
    mk_ptr_t range;

    /*---------------------*/

    /* POST/PUT data */
    mk_ptr_t data;
    /*-----------------*/

    /*-Internal-*/
    mk_ptr_t real_path;        /* Absolute real path */

    /*
     * If a full URL length is less than MAX_PATH_BASE (defined in limits.h),
     * it will be stored here and real_path will point this buffer
     */
    char real_path_static[MK_PATH_BASE];

    /* Query string: ?.... */
    mk_ptr_t query_string;


    /* STAGE_30 block flag: in mk_http_init() when the file is not found, it
     * triggers the plugin STAGE_30 to look for a plugin handler. In some
     * cases the plugin would overwrite the real path of the requested file
     * and make Monkey handle the new path for the static file. At this point
     * we need to block STAGE_30 calls from mk_http_init().
     *
     * For short.. if a plugin overwrites the real_path, let Monkey handle that
     * and do not trigger more STAGE_30's.
     */
    int stage30_blocked;

    /* Static file information */
    long loop;
    long bytes_to_send;
    off_t bytes_offset;
    struct file_info file_info;

    /* Vhost */
    int vhost_fdt_id;
    unsigned int vhost_fdt_hash;

    struct host       *host_conf;     /* root vhost config */
    struct host_alias *host_alias;    /* specific vhost matched */

    /* Response headers */
    struct response_headers headers;

    struct mk_list _head;
    /* HTTP Headers Table of Content */
    struct headers_toc headers_toc;

};

struct client_session
{
    int pipelined;              /* Pipelined request */
    int socket;
    int counter_connections;    /* Count persistent connections */
    int status;                 /* Request status */

    unsigned int body_size;
    unsigned int body_length;

    int body_pos_end;
    int first_method;

    /* red-black tree head */
    struct rb_node _rb_head;
    struct mk_list request_list;
    struct mk_list request_incomplete;

    time_t init_time;

    /* request body buffer */
    char *body;

    /* Initial fixed size buffer for small requests */
    char body_fixed[MK_REQUEST_CHUNK];
    struct session_request sr_fixed;
};

extern pthread_key_t request_list;

/* Request plugin Handler, each request can be handled by
 * several plugins, we handle list in a simple list */
struct handler
{
    struct plugin *p;
    struct handler *next;
};

void mk_request_free(struct session_request *sr);
int mk_request_header_toc_parse(struct headers_toc *toc, const char *data, int len);
mk_ptr_t mk_request_index(char *pathfile, char *file_aux, const unsigned int flen);
mk_ptr_t mk_request_header_get(struct headers_toc *toc,
                                 const char *key_name, int key_len);

int mk_request_error(int http_status, struct client_session *cs,
                     struct session_request *sr);

void mk_request_free_list(struct client_session *cs);

struct client_session *mk_session_create(int socket, struct sched_list_node *sched);
struct client_session *mk_session_get(int socket);
void mk_session_remove(int socket);

void mk_request_init_error_msgs(void);

int mk_handler_read(int socket, struct client_session *cs);
int mk_handler_write(int socket, struct client_session *cs);

void mk_request_ka_next(struct client_session *cs);
#endif