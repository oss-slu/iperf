/*
 * iperf, Copyright (c) 2014-2024, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * If you have questions about your rights to use or distribute this
 * software, please contact Berkeley Lab's Technology Transfer
 * Department at TTD@lbl.gov.
 *
 * NOTICE.  This software is owned by the U.S. Department of Energy.
 * As such, the U.S. Government has been granted for itself and others
 * acting on its behalf a paid-up, nonexclusive, irrevocable,
 * worldwide license in the Software to reproduce, prepare derivative
 * works, and perform publicly and display publicly.  Beginning five
 * (5) years after the date permission to assert copyright is obtained
 * from the U.S. Department of Energy, and subject to any subsequent
 * five (5) year renewals, the U.S. Government is granted for itself
 * and others acting on its behalf a paid-up, nonexclusive,
 * irrevocable, worldwide license in the Software to reproduce,
 * prepare derivative works, distribute copies to the public, perform
 * publicly and display publicly, and to permit others to do so.
 *
 * This code is distributed under a BSD style license, see the LICENSE
 * file for complete information.
 */
#include "iperf_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_quic.h"
#include "iperf_util.h"
#include "net.h"

#ifdef HAVE_MSQUIC
#include "msquic.h"

/*
 * Buffered receive chunk -- incoming data from MsQuic callbacks gets
 * copied here so the iperf recv path can consume it synchronously.
 */
struct quic_recv_chunk {
    uint8_t			*data;
    size_t			 len;
    size_t			 off;
    struct quic_recv_chunk	*next;
};

/*
 * Per-stream state that bridges a MsQuic HQUIC stream handle to the
 * iperf_stream it belongs to.  Stored in sp->data.
 */
struct iperf_quic_stream_ctx {
    HQUIC			 stream;
    pthread_mutex_t		 lock;
    pthread_cond_t		 cond;
    struct quic_recv_chunk	*head;
    struct quic_recv_chunk	*tail;
    size_t			 queued;
    int				 pending_sends;
    int				 fin;
    struct iperf_test		*test;
};

/*
 * Map an integer stream-id (returned from accept/connect) to the
 * per-stream context so attach_stream() can look it up.
 */
struct quic_stream_map {
    int				 id;
    struct iperf_quic_stream_ctx *sc;
    struct quic_stream_map	*next;
};

/*
 * Simple FIFO node for streams that have arrived on the server but
 * haven't been attached to an iperf_stream yet.
 */
struct quic_accept_node {
    struct iperf_quic_stream_ctx *sc;
    struct quic_accept_node	*next;
};

/*
 * Heap-allocated wrapper for StreamSend.  MsQuic requires both the
 * QUIC_BUFFER array *and* the data it points to to stay valid until
 * SEND_COMPLETE fires, so we bundle them together in one allocation.
 */
struct quic_send_buf {
    QUIC_BUFFER			 qb;
    uint8_t			 data[];
};

/*
 * Top-level per-test QUIC state.  Allocated once per iperf_test when
 * the user selects --quic, freed on cleanup.
 */
struct iperf_quic_context {
    const QUIC_API_TABLE	*api;
    HQUIC			 reg;
    HQUIC			 cfg;
    HQUIC			 listener;
    HQUIC			 conn;
    pthread_mutex_t		 lock;
    pthread_cond_t		 cond;
    int				 ready;
    int				 server;
    int				 done;
    int				 nfd[2];	/* pipe for select() wakeup */
    int				 next_id;
    struct quic_stream_map	*map;
    struct quic_accept_node	*ahead;
    struct quic_accept_node	*atail;
};

static const QUIC_BUFFER alpn = {
    sizeof("iperf") - 1, (uint8_t *) "iperf"
};
static const QUIC_REGISTRATION_CONFIG reg_cfg = {
    "iperf3", QUIC_EXECUTION_PROFILE_LOW_LATENCY
};
static const QUIC_API_TABLE *MsQuic;

static void
set_nonblock(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
	(void) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
notify_pipe(struct iperf_quic_context *ctx)
{
    uint8_t one = 1;

    if (ctx->nfd[1] >= 0)
	(void) write(ctx->nfd[1], &one, 1);
}


/* quic_sc_new
 *
 * allocate per-stream context and initialize its mutex/condvar
 */
static struct iperf_quic_stream_ctx *
quic_sc_new(HQUIC stream, struct iperf_test *test)
{
    struct iperf_quic_stream_ctx *sc;

    sc = calloc(1, sizeof(*sc));
    if (!sc)
	return NULL;
    sc->stream = stream;
    sc->test = test;
    pthread_mutex_init(&sc->lock, NULL);
    pthread_cond_init(&sc->cond, NULL);
    return sc;
}


/* quic_sc_free
 *
 * drain any queued receive chunks and free the context
 */
static void
quic_sc_free(struct iperf_quic_stream_ctx *sc)
{
    struct quic_recv_chunk *c, *tmp;

    c = sc->head;
    while (c) {
	tmp = c->next;
	free(c->data);
	free(c);
	c = tmp;
    }
    pthread_mutex_destroy(&sc->lock);
    pthread_cond_destroy(&sc->cond);
    free(sc);
}


static void
map_add(struct iperf_quic_context *ctx, int id,
    struct iperf_quic_stream_ctx *sc)
{
    struct quic_stream_map *n;

    n = calloc(1, sizeof(*n));
    if (!n)
	return;
    n->id = id;
    n->sc = sc;
    n->next = ctx->map;
    ctx->map = n;
}


/* map_take
 *
 * remove a stream from the id->context map and return it
 */
static struct iperf_quic_stream_ctx *
map_take(struct iperf_quic_context *ctx, int id)
{
    struct quic_stream_map *prev, *cur;
    struct iperf_quic_stream_ctx *sc;

    prev = NULL;
    cur = ctx->map;
    while (cur) {
	if (cur->id == id) {
	    if (prev)
		prev->next = cur->next;
	    else
		ctx->map = cur->next;
	    sc = cur->sc;
	    free(cur);
	    return sc;
	}
	prev = cur;
	cur = cur->next;
    }
    return NULL;
}


static void
accept_enq(struct iperf_quic_context *ctx,
    struct iperf_quic_stream_ctx *sc)
{
    struct quic_accept_node *n;

    n = calloc(1, sizeof(*n));
    if (!n)
	return;
    n->sc = sc;
    if (!ctx->atail) {
	ctx->ahead = ctx->atail = n;
    } else {
	ctx->atail->next = n;
	ctx->atail = n;
    }
    notify_pipe(ctx);
}


static struct iperf_quic_stream_ctx *
accept_deq(struct iperf_quic_context *ctx)
{
    struct quic_accept_node *n;
    struct iperf_quic_stream_ctx *sc;

    n = ctx->ahead;
    if (!n)
	return NULL;
    ctx->ahead = n->next;
    if (!ctx->ahead)
	ctx->atail = NULL;
    sc = n->sc;
    free(n);
    return sc;
}


static QUIC_STATUS QUIC_API
stream_cb(HQUIC Stream, void *Context, QUIC_STREAM_EVENT *Event)
{
    struct iperf_quic_stream_ctx *sc;
    const QUIC_BUFFER *b;
    struct quic_recv_chunk *chunk;
    uint32_t i;

    sc = (struct iperf_quic_stream_ctx *) Context;
    if (!sc)
	return QUIC_STATUS_SUCCESS;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
	if (Event->SEND_COMPLETE.ClientContext)
	    free(Event->SEND_COMPLETE.ClientContext);
	pthread_mutex_lock(&sc->lock);
	if (sc->pending_sends > 0)
	    sc->pending_sends--;
	pthread_cond_signal(&sc->cond);
	pthread_mutex_unlock(&sc->lock);
	break;

    case QUIC_STREAM_EVENT_RECEIVE:
	/*
	 * Copy incoming data into our recv queue.  Returning SUCCESS
	 * tells MsQuic the buffers are consumed; do NOT also call
	 * StreamReceiveComplete (that is only for the PENDING path).
	 */
	for (i = 0; i < Event->RECEIVE.BufferCount; i++) {
	    b = &Event->RECEIVE.Buffers[i];
	    chunk = calloc(1, sizeof(*chunk));
	    if (!chunk)
		break;
	    chunk->data = malloc(b->Length);
	    if (!chunk->data) {
		free(chunk);
		break;
	    }
	    memcpy(chunk->data, b->Buffer, b->Length);
	    chunk->len = b->Length;

	    pthread_mutex_lock(&sc->lock);
	    if (!sc->tail)
		sc->head = sc->tail = chunk;
	    else {
		sc->tail->next = chunk;
		sc->tail = chunk;
	    }
	    sc->queued += chunk->len;
	    pthread_cond_signal(&sc->cond);
	    pthread_mutex_unlock(&sc->lock);
	}
	break;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
	pthread_mutex_lock(&sc->lock);
	sc->fin = 1;
	pthread_cond_broadcast(&sc->cond);
	pthread_mutex_unlock(&sc->lock);
	break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
	pthread_mutex_lock(&sc->lock);
	sc->fin = 1;
	pthread_cond_broadcast(&sc->cond);
	pthread_mutex_unlock(&sc->lock);
	if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress)
	    MsQuic->StreamClose(Stream);
	break;

    default:
	break;
    }
    return QUIC_STATUS_SUCCESS;
}


static QUIC_STATUS QUIC_API
conn_cb(HQUIC Connection, void *Context, QUIC_CONNECTION_EVENT *Event)
{
    struct iperf_test *test;
    struct iperf_quic_context *ctx;
    HQUIC stream;
    struct iperf_quic_stream_ctx *sc;

    test = (struct iperf_test *) Context;
    ctx = test->quic_ctx;
    (void) Connection;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
	pthread_mutex_lock(&ctx->lock);
	ctx->ready = 1;
	pthread_cond_broadcast(&ctx->cond);
	pthread_mutex_unlock(&ctx->lock);
	break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
	stream = Event->PEER_STREAM_STARTED.Stream;
	sc = quic_sc_new(stream, test);
	if (!sc)
	    break;
	ctx->api->SetCallbackHandler(stream, (void *) stream_cb, sc);
	pthread_mutex_lock(&ctx->lock);
	accept_enq(ctx, sc);
	pthread_mutex_unlock(&ctx->lock);
	break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
	pthread_mutex_lock(&ctx->lock);
	ctx->ready = 0;
	ctx->done = 1;
	pthread_cond_broadcast(&ctx->cond);
	pthread_mutex_unlock(&ctx->lock);
	break;

    default:
	break;
    }
    return QUIC_STATUS_SUCCESS;
}


static QUIC_STATUS QUIC_API
listener_cb(HQUIC Listener, void *Context, QUIC_LISTENER_EVENT *Event)
{
    struct iperf_test *test;
    struct iperf_quic_context *ctx;

    test = (struct iperf_test *) Context;
    ctx = test->quic_ctx;
    (void) Listener;

    if (Event->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
	ctx->conn = Event->NEW_CONNECTION.Connection;
	ctx->api->SetCallbackHandler(ctx->conn,
	    (void *) conn_cb, test);
	ctx->api->ConnectionSetConfiguration(ctx->conn, ctx->cfg);
	{
	    QUIC_STREAM_SCHEDULING_SCHEME rr =
		QUIC_STREAM_SCHEDULING_SCHEME_ROUND_ROBIN;
	    ctx->api->SetParam(ctx->conn,
		QUIC_PARAM_CONN_STREAM_SCHEDULING_SCHEME,
		sizeof(rr), &rr);
	}
    }
    return QUIC_STATUS_SUCCESS;
}


static int
open_api(struct iperf_quic_context *ctx)
{
    QUIC_STATUS s;

    s = MsQuicOpen2(&ctx->api);
    if (QUIC_FAILED(s))
	return -1;
    MsQuic = ctx->api;

    s = ctx->api->RegistrationOpen(&reg_cfg, &ctx->reg);
    if (QUIC_FAILED(s))
	return -1;
    return 0;
}


static int
open_config(struct iperf_test *test, struct iperf_quic_context *ctx)
{
    QUIC_SETTINGS	 settings;
    QUIC_STATUS		 s;
    QUIC_CREDENTIAL_CONFIG cred;
    uint32_t		 bufsz;

    memset(&settings, 0, sizeof(settings));

    if (test->settings->idle_timeout > 0) {
	settings.IdleTimeoutMs = (uint64_t) test->settings->idle_timeout * 1000;
	settings.IsSet.IdleTimeoutMs = 1;
    }

    bufsz = test->quic_buf_size ? test->quic_buf_size : (32 * 1024 * 1024);
    settings.StreamRecvWindowDefault = bufsz;
    settings.StreamRecvWindowBidiLocalDefault = bufsz;
    settings.StreamRecvWindowBidiRemoteDefault = bufsz;
    settings.StreamRecvWindowUnidiDefault = bufsz;
    settings.StreamRecvBufferDefault = bufsz;
    settings.ConnFlowControlWindow = (uint64_t) bufsz * 4;
    settings.SendBufferingEnabled = 1;
    settings.IsSet.StreamRecvWindowDefault = 1;
    settings.IsSet.StreamRecvWindowBidiLocalDefault = 1;
    settings.IsSet.StreamRecvWindowBidiRemoteDefault = 1;
    settings.IsSet.StreamRecvWindowUnidiDefault = 1;
    settings.IsSet.StreamRecvBufferDefault = 1;
    settings.IsSet.ConnFlowControlWindow = 1;
    settings.IsSet.SendBufferingEnabled = 1;

    if (test->num_streams > 0) {
	settings.PeerBidiStreamCount =
	    (uint16_t)(test->num_streams * (test->bidirectional ? 2 : 1));
	if (settings.PeerBidiStreamCount == 0)
	    settings.PeerBidiStreamCount = 1;
	settings.IsSet.PeerBidiStreamCount = 1;
    }

    s = ctx->api->ConfigurationOpen(ctx->reg, &alpn, 1,
	&settings, sizeof(settings), NULL, &ctx->cfg);
    if (QUIC_FAILED(s)) {
	iperf_err(test, "MsQuic ConfigurationOpen failed, 0x%x", s);
	return -1;
    }

    memset(&cred, 0, sizeof(cred));
    if (ctx->server) {
	/*
	 * Server needs a TLS certificate.  We support PKCS#12 bundles
	 * (--quic-p12) and PEM cert+key (--quic-cert/--quic-key).
	 */
	if (test->quic_p12_file) {
	    FILE	*fp;
	    long	 sz;
	    uint8_t	*blob;
	    QUIC_CERTIFICATE_PKCS12 p12;

	    fp = fopen(test->quic_p12_file, "rb");
	    if (!fp) {
		i_errno = IEQUICCERT;
		return -1;
	    }
	    if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		i_errno = IEQUICCERT;
		return -1;
	    }
	    sz = ftell(fp);
	    if (sz <= 0) {
		fclose(fp);
		i_errno = IEQUICCERT;
		return -1;
	    }
	    if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		i_errno = IEQUICCERT;
		return -1;
	    }
	    blob = malloc((size_t) sz);
	    if (!blob) {
		fclose(fp);
		i_errno = IEQUICCERT;
		return -1;
	    }
	    if (fread(blob, 1, (size_t) sz, fp) != (size_t) sz) {
		fclose(fp);
		free(blob);
		i_errno = IEQUICCERT;
		return -1;
	    }
	    fclose(fp);

	    p12.Asn1Blob = blob;
	    p12.Asn1BlobLength = (uint32_t) sz;
	    p12.PrivateKeyPassword = test->quic_p12_password;

	    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_PKCS12;
	    cred.CertificatePkcs12 = &p12;
	    cred.Flags = QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;

	    s = ctx->api->ConfigurationLoadCredential(ctx->cfg, &cred);
	    free(blob);
	    if (QUIC_FAILED(s)) {
		iperf_err(test, "MsQuic ConfigurationLoadCredential failed, 0x%x", s);
		return -1;
	    }
	} else {
	    if (!test->quic_cert_file || !test->quic_key_file) {
		i_errno = IEQUICCERT;
		return -1;
	    }
	    cred.Flags = QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
	    if (test->quic_key_password) {
		QUIC_CERTIFICATE_FILE_PROTECTED prot = {
		    test->quic_cert_file,
		    test->quic_key_file,
		    test->quic_key_password
		};
		cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE_PROTECTED;
		cred.CertificateFileProtected = &prot;
	    } else {
		QUIC_CERTIFICATE_FILE cf = {
		    test->quic_cert_file,
		    test->quic_key_file
		};
		cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
		cred.CertificateFile = &cf;
	    }
	    s = ctx->api->ConfigurationLoadCredential(ctx->cfg, &cred);
	    if (QUIC_FAILED(s)) {
		iperf_err(test, "MsQuic ConfigurationLoadCredential failed, 0x%x", s);
		return -1;
	    }
	}
    } else {
	/* Client side: skip certificate validation for test purposes */
	cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
	cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
	    QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
	s = ctx->api->ConfigurationLoadCredential(ctx->cfg, &cred);
	if (QUIC_FAILED(s)) {
	    iperf_err(test, "MsQuic ConfigurationLoadCredential failed, 0x%x", s);
	    return -1;
	}
    }

    return 0;
}


static int
quic_ctx_init(struct iperf_test *test, int is_server)
{
    struct iperf_quic_context *ctx;

    if (test->quic_ctx)
	return 0;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
	return -1;
    ctx->server = is_server;
    ctx->nfd[0] = ctx->nfd[1] = -1;
    ctx->done = 0;
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond, NULL);

    if (open_api(ctx) < 0)
	goto fail;
    if (open_config(test, ctx) < 0)
	goto fail;
    if (pipe(ctx->nfd) != 0)
	goto fail;
    set_nonblock(ctx->nfd[0]);
    set_nonblock(ctx->nfd[1]);

    test->quic_ctx = ctx;
    return 0;

fail:
    test->quic_ctx = ctx;
    iperf_quic_free_test(test);
    return -1;
}


static void
close_stream(struct iperf_quic_context *ctx,
    struct iperf_quic_stream_ctx *sc)
{
    if (!sc)
	return;
    if (sc->stream) {
	ctx->api->StreamShutdown(sc->stream,
	    QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
	ctx->api->StreamClose(sc->stream);
	sc->stream = NULL;
    }
}


/* iperf_quic_init
 *
 * protocol-specific initialisation; nothing needed for QUIC
 */
int
iperf_quic_init(struct iperf_test *test)
{
    (void) test;
    return 0;
}


/* iperf_quic_listen
 *
 * set up the MsQuic listener on the QUIC data port
 */
int
iperf_quic_listen(struct iperf_test *test)
{
    struct iperf_quic_context *ctx;
    QUIC_STATUS	 status;
    QUIC_ADDR	 addr;
    uint16_t	 port;
    int		 af;

    if (quic_ctx_init(test, 1) < 0) {
	if (i_errno == 0)
	    i_errno = IELISTEN;
	return -1;
    }
    ctx = test->quic_ctx;

    status = ctx->api->ListenerOpen(ctx->reg,
	listener_cb, test, &ctx->listener);
    if (QUIC_FAILED(status)) {
	iperf_err(test, "MsQuic ListenerOpen failed, 0x%x", status);
	i_errno = IELISTEN;
	return -1;
    }

    memset(&addr, 0, sizeof(addr));
    af = 0;
    if (test->bind_address) {
	struct sockaddr_in *v4 = (struct sockaddr_in *) &addr;
	if (inet_pton(AF_INET, test->bind_address, &v4->sin_addr) == 1) {
	    af = AF_INET;
	    v4->sin_family = AF_INET;
	} else {
	    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *) &addr;
	    if (inet_pton(AF_INET6, test->bind_address, &v6->sin6_addr) == 1) {
		af = AF_INET6;
		v6->sin6_family = AF_INET6;
	    }
	}
    }
    if (af == 0) {
	if (test->settings->domain == AF_INET)
	    af = AF_INET;
	else if (test->settings->domain == AF_INET6)
	    af = AF_INET6;
	else
	    af = AF_INET;
    }
    QuicAddrSetFamily(&addr,
	af == AF_INET ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6);

    port = (test->quic_port > 0)
	? (uint16_t) test->quic_port
	: (uint16_t) test->server_port;
    QuicAddrSetPort(&addr, port);

    status = ctx->api->ListenerStart(ctx->listener, &alpn, 1, &addr);
    if (QUIC_FAILED(status)) {
	iperf_err(test, "MsQuic ListenerStart failed, 0x%x", status);
	i_errno = IELISTEN;
	return -1;
    }
    if (test->debug)
	iperf_printf(test, "QUIC: listener started on UDP port %u\n",
	    (unsigned) port);

    if (ctx->nfd[0] >= 0)
	test->prot_listener = ctx->nfd[0];
    return test->prot_listener;
}


/* iperf_quic_accept
 *
 * accept a new QUIC stream on the server side.
 * returns stream id (>0) on success, -1 if no streams are queued.
 */
int
iperf_quic_accept(struct iperf_test *test)
{
    struct iperf_quic_context *ctx;
    struct iperf_quic_stream_ctx *sc;
    int id;

    ctx = test->quic_ctx;
    if (!ctx) {
	i_errno = IEACCEPT;
	return -1;
    }

    /* drain one notification byte from the pipe */
    if (ctx->nfd[0] >= 0) {
	uint8_t junk;
	(void) read(ctx->nfd[0], &junk, 1);
    }

    pthread_mutex_lock(&ctx->lock);
    sc = accept_deq(ctx);
    if (!sc) {
	pthread_mutex_unlock(&ctx->lock);
	return -1;
    }
    id = ++ctx->next_id;
    map_add(ctx, id, sc);
    pthread_mutex_unlock(&ctx->lock);

    if (test->debug)
	iperf_printf(test, "QUIC: accepted stream id %d (handle %p)\n",
	    id, (void *) sc->stream);
    return id;
}


/* iperf_quic_connect
 *
 * client side: open a QUIC connection and create one stream
 */
int
iperf_quic_connect(struct iperf_test *test)
{
    struct iperf_quic_context *ctx;
    QUIC_STATUS status;
    QUIC_ADDRESS_FAMILY fam;
    uint16_t port;
    HQUIC stream;
    struct iperf_quic_stream_ctx *sc;
    int id;

    if (quic_ctx_init(test, 0) < 0) {
	if (i_errno == 0)
	    i_errno = IECONNECT;
	return -1;
    }
    ctx = test->quic_ctx;

    if (!ctx->conn) {
	status = ctx->api->ConnectionOpen(ctx->reg,
	    conn_cb, test, &ctx->conn);
	if (QUIC_FAILED(status)) {
	    i_errno = IECONNECT;
	    return -1;
	}

	fam = QUIC_ADDRESS_FAMILY_UNSPEC;
	if (test->settings->domain == AF_INET)
	    fam = QUIC_ADDRESS_FAMILY_INET;
	else if (test->settings->domain == AF_INET6)
	    fam = QUIC_ADDRESS_FAMILY_INET6;

	port = (test->quic_port > 0)
	    ? (uint16_t) test->quic_port
	    : (uint16_t) test->server_port;
	status = ctx->api->ConnectionStart(ctx->conn, ctx->cfg,
	    fam, test->server_hostname, port);
	if (QUIC_FAILED(status)) {
	    i_errno = IECONNECT;
	    return -1;
	}
	{
	    QUIC_STREAM_SCHEDULING_SCHEME rr =
		QUIC_STREAM_SCHEDULING_SCHEME_ROUND_ROBIN;
	    ctx->api->SetParam(ctx->conn,
		QUIC_PARAM_CONN_STREAM_SCHEDULING_SCHEME,
		sizeof(rr), &rr);
	}
	if (test->debug)
	    iperf_printf(test, "QUIC: connecting to %s:%u\n",
		test->server_hostname, (unsigned) port);
    }

    /* wait for handshake */
    pthread_mutex_lock(&ctx->lock);
    if (!ctx->ready) {
	if (test->settings->connect_timeout > 0) {
	    struct timespec ts;
	    clock_gettime(CLOCK_REALTIME, &ts);
	    ts.tv_sec += test->settings->connect_timeout / 1000;
	    ts.tv_nsec += (test->settings->connect_timeout % 1000) * 1000000;
	    if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	    }
	    while (!ctx->ready) {
		if (pthread_cond_timedwait(&ctx->cond,
		    &ctx->lock, &ts) == ETIMEDOUT)
		    break;
	    }
	} else {
	    while (!ctx->ready)
		pthread_cond_wait(&ctx->cond, &ctx->lock);
	}
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!ctx->ready) {
	i_errno = IECONNECT;
	return -1;
    }

    /* open one bidirectional stream */
    stream = NULL;
    status = ctx->api->StreamOpen(ctx->conn,
	QUIC_STREAM_OPEN_FLAG_NONE, stream_cb, NULL, &stream);
    if (QUIC_FAILED(status)) {
	i_errno = IECONNECT;
	return -1;
    }
    sc = quic_sc_new(stream, test);
    if (!sc) {
	ctx->api->StreamClose(stream);
	i_errno = IECONNECT;
	return -1;
    }
    ctx->api->SetCallbackHandler(stream, (void *) stream_cb, sc);

    status = ctx->api->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE);
    if (QUIC_FAILED(status)) {
	close_stream(ctx, sc);
	quic_sc_free(sc);
	i_errno = IECONNECT;
	return -1;
    }
    if (test->debug)
	iperf_printf(test, "QUIC: client stream started\n");

    pthread_mutex_lock(&ctx->lock);
    id = ++ctx->next_id;
    map_add(ctx, id, sc);
    pthread_mutex_unlock(&ctx->lock);
    return id;
}


/* iperf_quic_attach_stream
 *
 * look up the per-stream context by id and attach it to sp->data
 */
int
iperf_quic_attach_stream(struct iperf_test *test, struct iperf_stream *sp,
    int stream_id)
{
    struct iperf_quic_context *ctx;
    struct iperf_quic_stream_ctx *sc;

    ctx = test->quic_ctx;
    if (!ctx)
	return -1;
    pthread_mutex_lock(&ctx->lock);
    sc = map_take(ctx, stream_id);
    pthread_mutex_unlock(&ctx->lock);
    if (!sc)
	return -1;
    sp->data = sc;
    return 0;
}


/* iperf_quic_send
 *
 * send one block of data on a QUIC stream
 */
#define QUIC_MAX_PENDING_SENDS	32

int
iperf_quic_send(struct iperf_stream *sp)
{
    struct iperf_quic_stream_ctx *sc;
    struct iperf_quic_context *ctx;
    struct quic_send_buf *sb;
    size_t		 len;
    QUIC_STATUS		 status;
    int			 old_cancel;

    sc = (struct iperf_quic_stream_ctx *) sp->data;
    ctx = sp->test->quic_ctx;
    if (!sc || !ctx)
	return NET_HARDERROR;

    /*
     * Disable thread cancellation while we hold sc->lock.
     * pthread_cancel would leave the mutex locked, deadlocking
     * MsQuic's cleanup callbacks.  We rely on sp->done /
     * test->done + timedwait for thread exit instead.
     */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);

    /*
     * Throttle: don't queue more than QUIC_MAX_PENDING_SENDS in MsQuic
     * at a time.  Without this, one stream can monopolise the connection-
     * level flow-control window and starve the other parallel streams.
     */
    pthread_mutex_lock(&sc->lock);
    while (sc->pending_sends >= QUIC_MAX_PENDING_SENDS &&
	   !sc->fin && !sp->done && !sp->test->done) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += 100000000;	/* 100 ms */
	if (deadline.tv_nsec >= 1000000000) {
	    deadline.tv_sec++;
	    deadline.tv_nsec -= 1000000000;
	}
	pthread_cond_timedwait(&sc->cond, &sc->lock, &deadline);
    }
    if (sc->fin || sp->done || sp->test->done) {
	pthread_mutex_unlock(&sc->lock);
	pthread_setcancelstate(old_cancel, NULL);
	return 0;
    }
    sc->pending_sends++;
    pthread_mutex_unlock(&sc->lock);

    if (!sp->pending_size)
	sp->pending_size = sp->settings->blksize;
    len = (size_t) sp->pending_size;

    sb = malloc(sizeof(*sb) + len);
    if (!sb)
	return NET_HARDERROR;
    memcpy(sb->data, sp->buffer, len);
    sb->qb.Length = (uint32_t) len;
    sb->qb.Buffer = sb->data;

    status = ctx->api->StreamSend(sc->stream, &sb->qb, 1,
	QUIC_SEND_FLAG_NONE, sb);
    if (QUIC_FAILED(status)) {
	if (sp->test->debug)
	    iperf_printf(sp->test, "QUIC: StreamSend failed, 0x%x\n", status);
	free(sb);
	return NET_HARDERROR;
    }

    sp->pending_size = 0;
    sp->result->bytes_sent += len;
    sp->result->bytes_sent_this_interval += len;
    pthread_setcancelstate(old_cancel, NULL);
    return (int) len;
}


/* iperf_quic_recv
 *
 * receive data from a QUIC stream, blocking until at least some
 * data is available (or the peer shuts down)
 */
int
iperf_quic_recv(struct iperf_stream *sp)
{
    struct iperf_quic_stream_ctx *sc;
    struct quic_recv_chunk *c;
    size_t want, got, avail, take;
    int old_cancel;

    sc = (struct iperf_quic_stream_ctx *) sp->data;
    if (!sc) {
	if (sp->test->debug)
	    iperf_printf(sp->test,
		"QUIC recv: stream %d has no context attached\n", sp->id);
	return NET_HARDERROR;
    }

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
    pthread_mutex_lock(&sc->lock);
    while (sc->queued == 0 && !sc->fin && !sp->done && !sp->test->done) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += 100000000;	/* 100 ms */
	if (deadline.tv_nsec >= 1000000000) {
	    deadline.tv_sec++;
	    deadline.tv_nsec -= 1000000000;
	}
	pthread_cond_timedwait(&sc->cond, &sc->lock, &deadline);
    }
    if (sc->queued == 0) {
	pthread_mutex_unlock(&sc->lock);
	pthread_setcancelstate(old_cancel, NULL);
	return 0;
    }

    want = sp->settings->blksize;
    if (want > sc->queued)
	want = sc->queued;
    got = 0;
    while (got < want && sc->head) {
	c = sc->head;
	avail = c->len - c->off;
	take = want - got;
	if (take > avail)
	    take = avail;
	memcpy(sp->buffer + got, c->data + c->off, take);
	c->off += take;
	got += take;
	sc->queued -= take;
	if (c->off == c->len) {
	    sc->head = c->next;
	    if (!sc->head)
		sc->tail = NULL;
	    free(c->data);
	    free(c);
	}
    }
    pthread_mutex_unlock(&sc->lock);

    pthread_setcancelstate(old_cancel, NULL);

    if (sp->test->state == TEST_RUNNING) {
	sp->result->bytes_received += got;
	sp->result->bytes_received_this_interval += got;
    }
    return (int) got;
}


/* iperf_quic_stream_cleanup
 *
 * shut down a single QUIC stream and free its context
 */
void
iperf_quic_stream_cleanup(struct iperf_stream *sp)
{
    struct iperf_quic_stream_ctx *sc;
    struct iperf_quic_context *ctx;

    sc = (struct iperf_quic_stream_ctx *) sp->data;
    ctx = sp->test->quic_ctx;
    if (!sc)
	return;
    if (ctx)
	close_stream(ctx, sc);
    quic_sc_free(sc);
    sp->data = NULL;
}


/* iperf_quic_close_listener
 *
 * tear down the QUIC listener and its notification pipe
 */
void
iperf_quic_close_listener(struct iperf_test *test)
{
    struct iperf_quic_context *ctx;

    ctx = test->quic_ctx;
    if (!ctx)
	return;
    if (ctx->listener) {
	ctx->api->ListenerStop(ctx->listener);
	ctx->api->ListenerClose(ctx->listener);
	ctx->listener = NULL;
    }
    if (test->prot_listener > -1) {
	FD_CLR(test->prot_listener, &test->read_set);
	FD_CLR(test->prot_listener, &test->write_set);
    }
    if (ctx->nfd[0] >= 0) {
	close(ctx->nfd[0]);
	ctx->nfd[0] = -1;
    }
    if (ctx->nfd[1] >= 0) {
	close(ctx->nfd[1]);
	ctx->nfd[1] = -1;
    }
    test->prot_listener = -1;
}


void
iperf_quic_reset_test(struct iperf_test *test)
{
    iperf_quic_free_test(test);
}


/* iperf_quic_free_test
 *
 * release all QUIC resources for a test
 */
void
iperf_quic_free_test(struct iperf_test *test)
{
    struct iperf_quic_context *ctx;
    struct quic_stream_map *m, *mnext;
    struct quic_accept_node *a, *anext;

    ctx = test->quic_ctx;
    if (!ctx)
	return;

    if (ctx->listener) {
	ctx->api->ListenerStop(ctx->listener);
	ctx->api->ListenerClose(ctx->listener);
	ctx->listener = NULL;
    }

    /*
     * Abort and close all streams before the connection so
     * that ConnectionClose doesn't block on pending sends.
     */
    m = ctx->map;
    while (m) {
	mnext = m->next;
	if (m->sc && m->sc->stream) {
	    ctx->api->StreamShutdown(m->sc->stream,
		QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
		QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0);
	    ctx->api->StreamClose(m->sc->stream);
	    m->sc->stream = NULL;
	}
	quic_sc_free(m->sc);
	free(m);
	m = mnext;
    }
    ctx->map = NULL;

    a = ctx->ahead;
    while (a) {
	anext = a->next;
	if (a->sc && a->sc->stream) {
	    ctx->api->StreamShutdown(a->sc->stream,
		QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
		QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0);
	    ctx->api->StreamClose(a->sc->stream);
	    a->sc->stream = NULL;
	}
	quic_sc_free(a->sc);
	free(a);
	a = anext;
    }
    ctx->ahead = ctx->atail = NULL;

    if (ctx->conn) {
	ctx->api->ConnectionShutdown(ctx->conn,
	    QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
	ctx->api->ConnectionClose(ctx->conn);
	ctx->conn = NULL;
    }

    if (ctx->nfd[0] >= 0)
	close(ctx->nfd[0]);
    if (ctx->nfd[1] >= 0)
	close(ctx->nfd[1]);

    if (ctx->cfg) {
	ctx->api->ConfigurationClose(ctx->cfg);
	ctx->cfg = NULL;
    }
    if (ctx->reg) {
	ctx->api->RegistrationClose(ctx->reg);
	ctx->reg = NULL;
    }
    if (ctx->api)
	MsQuicClose(ctx->api);

    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cond);
    free(ctx);
    test->quic_ctx = NULL;
    test->prot_listener = -1;
}


int
iperf_quic_shutdown_complete(struct iperf_test *test)
{
    if (!test || !test->quic_ctx)
	return 0;
    return test->quic_ctx->done != 0;
}

#else /* !HAVE_MSQUIC */

int
iperf_quic_init(struct iperf_test *test)
{
    (void) test;
    i_errno = IEUNIMP;
    return -1;
}

int
iperf_quic_listen(struct iperf_test *test)
{
    (void) test;
    i_errno = IEUNIMP;
    return -1;
}

int
iperf_quic_accept(struct iperf_test *test)
{
    (void) test;
    i_errno = IEUNIMP;
    return -1;
}

int
iperf_quic_connect(struct iperf_test *test)
{
    (void) test;
    i_errno = IEUNIMP;
    return -1;
}

int
iperf_quic_send(struct iperf_stream *sp)
{
    (void) sp;
    i_errno = IEUNIMP;
    return -1;
}

int
iperf_quic_recv(struct iperf_stream *sp)
{
    (void) sp;
    i_errno = IEUNIMP;
    return -1;
}

int
iperf_quic_attach_stream(struct iperf_test *test, struct iperf_stream *sp,
    int stream_id)
{
    (void) test;
    (void) sp;
    (void) stream_id;
    i_errno = IEUNIMP;
    return -1;
}

void iperf_quic_stream_cleanup(struct iperf_stream *sp) { (void) sp; }
void iperf_quic_close_listener(struct iperf_test *test) { (void) test; }
void iperf_quic_reset_test(struct iperf_test *test) { (void) test; }
void iperf_quic_free_test(struct iperf_test *test) { (void) test; }
int iperf_quic_shutdown_complete(struct iperf_test *test)
{
    (void) test;
    return 0;
}

#endif /* HAVE_MSQUIC */
