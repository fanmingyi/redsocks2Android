/* redsocks2 - transparent TCP-to-proxy redirector
 * Copyright (C) 2013-2017 Zhuofei Wang <semigodking@gmail.com>
 *
 * This code is based on redsocks project developed by Leonid Evdokimov.
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_struct.h>
#include "utils.h"
#include "log.h"
#include "redsocks.h"
#include "encrypt.h"
#include "shadowsocks.h"

typedef enum ss_state_t {
    ss_new,
    ss_connected,
    ss_MAX,
} ss_state;

typedef struct ss_client_t {
    struct enc_ctx e_ctx;
    struct enc_ctx d_ctx;
    short e_ctx_init;
    short d_ctx_init;
} ss_client;

typedef struct ss_instance_t {
    int method; 
    enc_info info;
} ss_instance;

void redsocks_event_error(struct bufferevent *buffev, short what, void *_arg);

int ss_is_valid_cred(const char *method, const char *password)
{
    if (!method || !password)
        return 0;
    if (strlen(method) > 255) {
        log_error(LOG_WARNING, "Shadowsocks encryption method can't be more than 255 chars.");
        return 0;
    }
    if (strlen(password) > 255) {
        log_error(LOG_WARNING, "Shadowsocks encryption password can't be more than 255 chars.");
        return 0;
    }
    return 1;
}

static void ss_client_init(redsocks_client *client)
{
    client->state = ss_new;
}

static void ss_client_fini(redsocks_client *client)
{
    ss_client *sclient = (void*)(client + 1);

    if (sclient->e_ctx_init) {
        enc_ctx_free(&sclient->e_ctx);
        sclient->e_ctx_init = 0;
    }
    if (sclient->d_ctx_init) {
        enc_ctx_free(&sclient->d_ctx);
        sclient->d_ctx_init = 0;
    }
}

static void encrypt_mem(redsocks_client * client,
                      char * data, size_t len,
                      struct evbuffer * buf_out, int decrypt)
{
    ss_client *sclient = (void*)(client + 1);
    struct evbuffer_iovec vec;
    size_t required;
    int rc;

    if (!len || !data)
        return;

    if (decrypt)
        required = ss_calc_buffer_size(&sclient->d_ctx, len);
    else
        required = ss_calc_buffer_size(&sclient->e_ctx, len);
    if (required && evbuffer_reserve_space(buf_out, required, &vec, 1) == 1)
    {
        if (decrypt)
            rc = ss_decrypt(&sclient->d_ctx, data, len, vec.iov_base, &vec.iov_len);
        else
            rc = ss_encrypt(&sclient->e_ctx, data, len, vec.iov_base, &vec.iov_len);
        if (!rc)
            vec.iov_len = 0;
        evbuffer_commit_space(buf_out, &vec, 1);
    }
}


static void encrypt_buffer(redsocks_client *client,
                           struct bufferevent * from,
                           struct bufferevent * to)
{
    // To reduce memory copy, just encrypt one block a time
    struct evbuffer * buf_in = bufferevent_get_input(from);
    size_t input_size = evbuffer_get_contiguous_space(buf_in);
    char * input;

    if (!input_size)
        return;

    input = (char *)evbuffer_pullup(buf_in, input_size);    
    encrypt_mem(client, input, input_size, bufferevent_get_output(to), 0);
    evbuffer_drain(buf_in, input_size);
}

static void decrypt_buffer(redsocks_client * client,
                           struct bufferevent * from,
                           struct bufferevent * to)
{
    // To reduce memory copy, just decrypt one block a time
    struct evbuffer * buf_in = bufferevent_get_input(from);
    size_t input_size = evbuffer_get_contiguous_space(buf_in);
    char * input;

    if (!input_size)
        return;

    input = (char *)evbuffer_pullup(buf_in, input_size);
    encrypt_mem(client, input, input_size, bufferevent_get_output(to), 1);
    evbuffer_drain(buf_in, input_size);
}


static void ss_client_writecb(struct bufferevent *buffev, void *_arg)
{
    redsocks_client *client = _arg;
    struct bufferevent * from = client->relay;
    struct bufferevent * to   = buffev;
    size_t input_size = evbuffer_get_contiguous_space(bufferevent_get_input(from));
    size_t output_size = evbuffer_get_length(bufferevent_get_output(to));

    assert(buffev == client->client);
    redsocks_touch_client(client);

    if (process_shutdown_on_write_(client, from, to))
        return;

    if (client->state == ss_connected) 
    {
        /* encrypt and forward data received from client side */
        if (output_size < get_write_hwm(to))
        {
            if (input_size)
                decrypt_buffer(client, from, to);
            if (!(client->relay_evshut & EV_READ) && bufferevent_enable(from, EV_READ) == -1)
                redsocks_log_errno(client, LOG_ERR, "bufferevent_enable");
        }
    }
    else
    {
        redsocks_drop_client(client);
    }
}

static void ss_client_readcb(struct bufferevent *buffev, void *_arg)
{
    redsocks_client *client = _arg;
    struct bufferevent * from = buffev;
    struct bufferevent * to   = client->relay;
    size_t output_size = evbuffer_get_length(bufferevent_get_output(to));

    assert(buffev == client->client);
    redsocks_touch_client(client);

    if (client->state == ss_connected)
    {
        /* encrypt and forward data to the other side  */
        if (output_size < get_write_hwm(to))
        {
            encrypt_buffer(client, from, to);
            if (bufferevent_enable(from, EV_READ) == -1)
                redsocks_log_errno(client, LOG_ERR, "bufferevent_enable");
        }
        else
        {
            if (bufferevent_disable(from, EV_READ) == -1)
                redsocks_log_errno(client, LOG_ERR, "bufferevent_disable");
        }
    }
    else
    {
        redsocks_drop_client(client);
    }
}


static void ss_relay_writecb(struct bufferevent *buffev, void *_arg)
{
    redsocks_client *client = _arg;
    struct bufferevent * from = client->client;
    struct bufferevent * to   = buffev;
    size_t input_size = evbuffer_get_contiguous_space(bufferevent_get_input(from));
    size_t output_size = evbuffer_get_length(bufferevent_get_output(to));

    assert(buffev == client->relay);
    redsocks_touch_client(client);

    if (process_shutdown_on_write_(client, from, to))
        return;

    if (client->state == ss_connected) 
    {
        /* encrypt and forward data received from client side */
        if (output_size < get_write_hwm(to))
        {
            if (input_size)
                encrypt_buffer(client, from, to);
            if (!(client->client_evshut & EV_READ) && bufferevent_enable(from, EV_READ) == -1)
                redsocks_log_errno(client, LOG_ERR, "bufferevent_enable");
        }
    }
    else
    {
        redsocks_drop_client(client);
    }
}

static void ss_relay_readcb(struct bufferevent *buffev, void *_arg)
{
    redsocks_client *client = _arg;
    struct bufferevent * from = buffev;
    struct bufferevent * to   = client->client;
    size_t input_size = evbuffer_get_contiguous_space(bufferevent_get_input(from));
    size_t output_size = evbuffer_get_length(bufferevent_get_output(to));

    assert(buffev == client->relay);
    redsocks_touch_client(client);

    if (client->state == ss_connected)
    {
        /* decrypt and forward data to client side */
        if (output_size < get_write_hwm(to))
        {
            if (input_size)
                decrypt_buffer(client, from, to);
            if (bufferevent_enable(from, EV_READ) == -1)
                redsocks_log_errno(client, LOG_ERR, "bufferevent_enable");
        }
        else
        {
            if (bufferevent_disable(from, EV_READ) == -1)
                redsocks_log_errno(client, LOG_ERR, "bufferevent_disable");
        }
    }
    else
    {
        redsocks_drop_client(client);
    }
}

static void ss_relay_connected(struct bufferevent *buffev, void *_arg)
{
    redsocks_client *client = _arg;

    assert(buffev == client->relay);
    assert(client->state == ss_new);
    redsocks_touch_client(client);

    if (!red_is_socket_connected_ok(buffev)) {
        redsocks_log_error(client, LOG_DEBUG, "failed to connect to destination");
        redsocks_drop_client(client);
        return;
    }

    client->relay_connected = 1;
    client->state = ss_connected; 

    /* We do not need to detect timeouts any more.
    The two peers will handle it. */
    bufferevent_set_timeouts(client->relay, NULL, NULL);

    if (redsocks_start_relay(client))
        // redsocks_start_relay() drops client on failure
        return;
    /* overwrite theread callback to my function */
    bufferevent_setcb(client->client, ss_client_readcb,
                                     ss_client_writecb,
                                     redsocks_event_error,
                                     client);
    bufferevent_setcb(client->relay, ss_relay_readcb,
                                     ss_relay_writecb,
                                     redsocks_event_error,
                                     client);
    // Write any data received from client side to relay.
    if (evbuffer_get_length(bufferevent_get_input(client->client)))
        ss_relay_writecb(client->relay, client);
    return;

}


static int ss_connect_relay(redsocks_client *client)
{
    char * interface = client->instance->config.interface;
    ss_client *sclient = (void*)(client + 1);
    ss_instance * ss = (ss_instance *)(client->instance+1);
    ss_header_ipv4 header;
    struct timeval tv;
    size_t len = 0;
    char buff[64+sizeof(header)];

    if (enc_ctx_init(&ss->info, &sclient->e_ctx, 1)) {
        log_error(LOG_ERR, "Shadowsocks failed to initialize encryption context.");
        redsocks_drop_client(client);
        return -1;
    }
    sclient->e_ctx_init = 1;  
    if (enc_ctx_init(&ss->info, &sclient->d_ctx, 0)) {
        log_error(LOG_ERR, "Shadowsocks failed to initialize decryption context.");
        redsocks_drop_client(client);
        return -1;
    }
    sclient->d_ctx_init = 1;

    /* build and send header */
    // TODO: Better implementation and IPv6 Support
    header.addr_type = ss_addrtype_ipv4;
    header.addr = client->destaddr.sin_addr.s_addr;
    header.port = client->destaddr.sin_port;
    len += sizeof(header);
    size_t sz = sizeof(buff);
    if (!ss_encrypt(&sclient->e_ctx, (char *)&header, len, &buff[0], &sz)) {
        log_error(LOG_ERR, "Encryption error.");
        redsocks_drop_client(client);
        return -1;
    }
    len = sz;

    tv.tv_sec = client->instance->config.timeout;
    tv.tv_usec = 0;
    client->relay = red_connect_relay_tfo(interface, &client->instance->config.relayaddr,
                    NULL, ss_relay_connected, redsocks_event_error, client, 
                    &tv, &buff[0], &sz);

    if (!client->relay) {
        redsocks_drop_client(client);
        return -1;
    }
    else if (sz && sz != len) {
        log_error(LOG_ERR, "Unexpected length of data sent.");
        redsocks_drop_client(client);
        return -1;
    }
    return 0;
}

static int ss_instance_init(struct redsocks_instance_t *instance)
{
    ss_instance * ss = (ss_instance *)(instance+1);
    const redsocks_config *config = &instance->config;
    char buf1[RED_INET_ADDRSTRLEN];

    int valid_cred =  ss_is_valid_cred(config->login, config->password);
    if (!valid_cred 
    || (ss->method = enc_init(&ss->info, config->password, config->login), ss->method == -1))
    {
        log_error(LOG_ERR, "Invalided encrytion method or password.");
        return -1;
    }
    else
    {
        log_error(LOG_INFO, "%s @ %s: encryption method: %s",
            instance->relay_ss->name,
            red_inet_ntop(&instance->config.bindaddr, buf1, sizeof(buf1)),
            config->login);
    }
    return 0;
}

static void ss_instance_fini(struct redsocks_instance_t *instance)
{
}

relay_subsys shadowsocks_subsys =
{
    .name                 = "shadowsocks",
    .payload_len          = sizeof(ss_client),
    .instance_payload_len = sizeof(ss_instance),
    .init                 = ss_client_init,
    .fini                 = ss_client_fini,
    .connect_relay        = ss_connect_relay,
    .instance_init        = ss_instance_init,
    .instance_fini        = ss_instance_fini,
};


/* vim:set tabstop=4 softtabstop=4 shiftwidth=4: */
/* vim:set foldmethod=marker foldlevel=32 foldmarker={,}: */
