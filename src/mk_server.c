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

#include <monkey/monkey.h>
#include <monkey/mk_config.h>
#include <monkey/mk_scheduler.h>
#include <monkey/mk_plugin.h>
#include <monkey/mk_utils.h>
#include <monkey/mk_macros.h>
#include <monkey/mk_server.h>
#include <monkey/mk_event.h>
#include <monkey/mk_connection.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/resource.h>

/* Return the number of clients that can be attended  */
unsigned int mk_server_capacity()
{
    int ret;
    int cur;
    struct rlimit lim;

    /* Limit by system */
    getrlimit(RLIMIT_NOFILE, &lim);
    cur = lim.rlim_cur;

    if (mk_config->fd_limit > cur) {
        lim.rlim_cur = mk_config->fd_limit;
        lim.rlim_max = mk_config->fd_limit;

        ret = setrlimit(RLIMIT_NOFILE, &lim);
        if (ret == -1) {
            mk_warn("Could not increase FDLimit to %i.", mk_config->fd_limit);
        }
        else {
            cur = mk_config->fd_limit;
        }
    }
    else if (mk_config->fd_limit > 0) {
        cur = mk_config->fd_limit;
    }

    return cur;
}

int mk_server_listen_check(struct mk_server_listen *listen, int server_fd)
{
    struct mk_server_listen_entry *listen_entry;
    unsigned int i;

    if (listen == NULL)
        goto error;

    for (i = 0; i < listen->count; i++) {
        listen_entry = &listen->listen_list[i];
        if (listen_entry->server_fd == server_fd)
            return MK_TRUE;
    }
error:
    return MK_FALSE;
}

int mk_server_listen_handler(struct sched_list_node *sched,
                             struct mk_server_listen *listen,
                             int server_fd)
{
    struct mk_server_listen_entry *listen_entry;
    struct sched_list_node *local_sched;
    unsigned int i;
    int client_fd = -1;
    int result;

    if (listen == NULL)
        goto error;

    local_sched = mk_sched_get_thread_conf();

    for (i = 0; i < listen->count; i++) {
        listen_entry = &listen->listen_list[i];
        if (listen_entry->server_fd != server_fd)
            continue;

        if (mk_sched_check_capacity(sched) == -1)
            goto error;

        client_fd = mk_socket_accept(server_fd);
        if (mk_unlikely(client_fd == -1)) {
            MK_TRACE("[server] Accept connection failed: %s", strerror(errno));
            goto error;
        }

        result = mk_event_add(sched->loop, client_fd, MK_EVENT_READ, NULL);
        if (mk_unlikely(result != 0)) {
            mk_err("[server] Error registering file descriptor: %s",
                   strerror(errno));
            goto error;
        }

        if (sched == local_sched) {
            result = mk_sched_register_client(client_fd, sched);
            if (mk_unlikely(result != 0)) {
                mk_err("[server] Failed to register client.");
                goto error;
            }
        }

        sched->accepted_connections++;
        MK_TRACE("[server] New connection arrived: FD %i", client_fd);
        return client_fd;
    }
error:
    if (client_fd != -1)
        mk_socket_close(client_fd);
    return -1;
}

void mk_server_listen_free(struct mk_server_listen *server_listen)
{
    mk_mem_free(server_listen->listen_list);
    server_listen->listen_list = NULL;
    server_listen->count = 0;
}

int mk_server_listen_init(struct mk_server_config *config,
                          struct mk_server_listen *server_listen)
{
    int i = 0;
    unsigned int count = 0;
    int server_fd;
    int reuse_port;
    struct mk_list *head;
    struct mk_config_listener *listen;
    struct mk_server_listen_entry *listen_list = NULL;

    if (config == NULL)
        goto error;
    if (server_listen == NULL)
        goto error;

    reuse_port = config->scheduler_mode == MK_SCHEDULER_REUSEPORT;

    mk_list_foreach(head, &mk_config->listeners) {
        count++;
    }

    listen_list = calloc(count, sizeof(*listen_list));
    if (listen_list == NULL) {
        mk_err("[server] Calloc failed: %s", strerror(errno));
        goto error;
    }

    mk_list_foreach(head, &config->listeners) {
        listen = mk_list_entry(head, struct mk_config_listener, _head);

        server_fd = mk_socket_server(listen->port,
                listen->address,
                reuse_port);
        if (server_fd >= 0) {
            if (mk_socket_set_tcp_defer_accept(server_fd) != 0) {
#if defined (__linux__)
                mk_warn("[server] Could not set TCP_DEFER_ACCEPT");
#endif
            }
            listen_list[i].listen = listen;
            listen_list[i].server_fd = server_fd;
        }
        else {
            listen_list[i].server_fd = -1;
            mk_warn("[server] Failed to bind server socket to %s:%s.",
                    listen->address,
                    listen->port);
        }
        i += 1;
    }

    server_listen->count = count;
    server_listen->listen_list = listen_list;
    return 0;

error:
    if (listen_list != NULL) free(listen_list);
    return -1;
}

/* Here we launch the worker threads to attend clients */
void mk_server_launch_workers()
{
    int i;
    pthread_t skip;

    /* Launch workers */
    for (i = 0; i < mk_config->workers; i++) {
        mk_sched_launch_thread(mk_config->server_capacity, &skip);
    }

    /* Wait until all workers report as ready */
    while (1) {
        int ready = 0;

        pthread_mutex_lock(&mutex_worker_init);
        for (i = 0; i < mk_config->workers; i++) {
            if (sched_list[i].initialized)
                ready++;
        }
        pthread_mutex_unlock(&mutex_worker_init);

        if (ready == mk_config->workers) break;
        usleep(10000);
    }
}


void mk_server_loop_balancer()
{
    int i;
    int fd;
    int mask;
    struct sched_list_node *sched;
    struct mk_server_listen listen;
    mk_event_loop_t *evl;

    /* Init the listeners */
    if (mk_server_listen_init(mk_config, &listen)) {
        mk_err("Failed to initialize listen sockets.");
        return;
    }
    /* Create an event loop context */
    evl = mk_event_loop_create(MK_EVENT_QUEUE_SIZE);
    if (!evl) {
        mk_err("Could not initialize event loop");
        exit(EXIT_FAILURE);
    }

    /* Register the listeners */
    for (i = 0; (unsigned) i < listen.count; i++) {
        mk_event_add(evl, listen.listen_list[i].server_fd, MK_EVENT_READ, NULL);
    }

    while (1) {
        mk_event_wait(evl);
        mk_event_foreach(evl, fd, mask) {
            if (mask & MK_EVENT_READ) {
                /*
                 * Accept connection: determinate which thread may work on this
                 * new connection.
                 */
                sched = mk_sched_next_target();
                if (sched != NULL) {
                    mk_server_listen_handler(sched, &listen, fd);
#ifdef TRACE
                    struct sched_list_node *node;
                    node = sched_list;
                    for (i = 0; i < mk_config->workers; i++) {
                        MK_TRACE("Worker Status");
                        MK_TRACE(" WID %i / conx = %llu",
                                 node[i].idx,
                                 node[i].accepted_connections -
                                 node[i].closed_connections);
                    }
#endif
                }
                else {
                    mk_warn("[server] Over capacity.");
                }
            }
            else if (mask & MK_EVENT_CLOSE) {
                mk_err("[server] Error on socket %d: %s",
                       fd, strerror(errno));
            }
        }
    }
}

/*
 * This function is called when the scheduler is running in the REUSEPORT
 * mode. That means that each worker is listening on shared TCP ports.
 *
 * When using shared TCP ports the Kernel decides to which worker the
 * connection will be assigned.
 */
void mk_server_worker_loop(struct mk_server_listen *listen)
{
    int i;
    int fd;
    int ret = -1;
    int mask;
    int timeout_fd;
    uint64_t val;
    mk_event_loop_t *evl;
    struct sched_list_node *sched;
    struct mk_server_listen_entry *listen_entry;

    /* Get thread conf */
    sched = mk_sched_get_thread_conf();
    evl = sched->loop;

    /*
     * The worker will NOT process any connection until the master
     * process through mk_server_loop() send us the green light
     * signal MK_SERVER_SIGNAL_START.
     */
    mk_event_wait(evl);
    mk_event_foreach(evl, fd, mask) {
        if (mask & MK_EVENT_READ) {
            if (fd == sched->signal_channel_r) {
                ret = read(fd, &val, sizeof(val));
                if (ret < 0) {
                    mk_libc_error("read");
                    continue;
                }

                if (val == MK_SERVER_SIGNAL_START) {
                    break;
                }
            }
        }
    }

    /* Register listeners */
    for (i = 0; i < (int) listen->count; i++) {
        listen_entry = &listen->listen_list[i];
        if (listen_entry->server_fd < 0)
            continue;

        mk_event_add(sched->loop, listen_entry->server_fd, MK_EVENT_READ, NULL);
    }

    /* create a new timeout file descriptor */
    timeout_fd = mk_event_timeout_create(evl, mk_config->timeout);

    while (1) {
        mk_event_wait(evl);
        mk_event_foreach(evl, fd, mask) {
            if (mask & MK_EVENT_READ) {
                /* Check if we have a worker signal */
                if (mk_unlikely(fd == sched->signal_channel_r)) {
                    ret = read(fd, &val, sizeof(val));
                    if (ret < 0) {
                        mk_libc_error("read");
                        continue;
                    }

                    if (val == MK_SCHEDULER_SIGNAL_DEADBEEF) {
                        mk_sched_sync_counters();
                        continue;
                    }
                    else if (val == MK_SCHEDULER_SIGNAL_FREE_ALL) {
                        mk_event_loop_destroy(evl);
                        mk_sched_worker_free();
                        return;
                    }
                }
                else if (mk_unlikely(fd == timeout_fd)) {
                    ret = read(fd, &val, sizeof(val));
                    if (ret < 0) {
                        mk_libc_error("read");
                    }
                    else {
                        mk_sched_check_timeouts(sched);
                    }
                    continue;
                }
                else if (listen && mk_server_listen_check(listen, fd)) {
                    /*
                     * A new connection have been accepted..or failed, despite
                     * the result, we let the loop continue processing the other
                     * events triggered.
                     */
                    mk_server_listen_handler(sched, listen, fd);
                    continue;
                }
                else {
                    ret = mk_conn_read(fd);
                }
            }
            else if (mask & MK_EVENT_WRITE) {
                MK_TRACE("[FD %i] EPoll Event WRITE", fd);
                ret = mk_conn_write(fd);
            }
            else if (mask & MK_EVENT_CLOSE) {
                ret = -1;
            }

            if (ret < 0) {
                MK_TRACE("[FD %i] Epoll Event FORCE CLOSE | ret = %i", fd, ret);
                mk_conn_close(fd, MK_EP_SOCKET_CLOSED);
            }
        }
    }
}

void mk_server_loop(void)
{
    int n;
    int i;
    uint64_t val;

    /* Rename worker */
    mk_utils_worker_rename("monkey: server");

    mk_info("HTTP Server started");

    /* Wake up workers */
    val = MK_SERVER_SIGNAL_START;
    for (i = 0; i < mk_config->workers; i++) {
        n = write(sched_list[i].signal_channel_w, &val, sizeof(val));
        if (n < 0) {
            perror("write");
        }
    }

    /*
     * When using REUSEPORT mode on the Scheduler, we need to signal
     * them so they can start processing connections.
     */
    if (mk_config->scheduler_mode == MK_SCHEDULER_REUSEPORT) {
        /* Hang here */
        while (1) sleep(60);
        return;
    }
    else {
        mk_server_loop_balancer();
    }
}
