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
#ifndef	IPERF_QUIC_H
#define	IPERF_QUIC_H

struct iperf_quic_context;

/**
 * iperf_quic_accept -- accepts a new QUIC stream
 * returns stream id on success, -1 on error
 */
int iperf_quic_accept(struct iperf_test *);

/**
 * iperf_quic_recv -- receives data from a QUIC stream
 * returns number of bytes read
 */
int iperf_quic_recv(struct iperf_stream *);

/**
 * iperf_quic_send -- sends data on a QUIC stream
 * returns number of bytes sent
 */
int iperf_quic_send(struct iperf_stream *);

int iperf_quic_listen(struct iperf_test *);
int iperf_quic_connect(struct iperf_test *);
int iperf_quic_init(struct iperf_test *);

int iperf_quic_attach_stream(struct iperf_test *, struct iperf_stream *, int);
void iperf_quic_stream_cleanup(struct iperf_stream *);
void iperf_quic_close_listener(struct iperf_test *);
void iperf_quic_reset_test(struct iperf_test *);
void iperf_quic_free_test(struct iperf_test *);
int iperf_quic_shutdown_complete(struct iperf_test *);

#endif /* IPERF_QUIC_H */
