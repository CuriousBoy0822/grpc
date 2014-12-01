/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/grpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test/core/util/test_config.h"
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/string.h>
#include <grpc/support/time.h>
#include "test/core/util/port.h"

static grpc_completion_queue *cq;
static grpc_server *server;

static const grpc_status status_ok = {GRPC_STATUS_OK, NULL};

typedef struct {
  gpr_refcount pending_ops;
  gpr_intmax bytes_read;
} call_state;

static void request_call() {
  call_state *tag = gpr_malloc(sizeof(*tag));
  gpr_ref_init(&tag->pending_ops, 2);
  tag->bytes_read = 0;
  grpc_server_request_call(server, tag);
}

static void assert_read_ok(call_state *s, grpc_byte_buffer *b) {
  grpc_byte_buffer_reader *bb_reader = NULL;
  gpr_slice read_slice;
  int i;

  bb_reader = grpc_byte_buffer_reader_create(b);
  while (grpc_byte_buffer_reader_next(bb_reader, &read_slice)) {
    for (i = 0; i < GPR_SLICE_LENGTH(read_slice); i++) {
      GPR_ASSERT(GPR_SLICE_START_PTR(read_slice)[i] == s->bytes_read % 256);
      s->bytes_read++;
    }
    gpr_slice_unref(read_slice);
  }
  grpc_byte_buffer_reader_destroy(bb_reader);
}

int main(int argc, char **argv) {
  grpc_event *ev;
  char *addr;
  call_state *s;

  grpc_test_init(argc, argv);

  grpc_init();
  srand(clock());

  if (argc == 2) {
    addr = gpr_strdup(argv[1]);
  } else {
    gpr_join_host_port(&addr, "::", grpc_pick_unused_port_or_die());
  }
  gpr_log(GPR_INFO, "creating server on: %s", addr);

  cq = grpc_completion_queue_create();
  server = grpc_server_create(cq, NULL);
  GPR_ASSERT(grpc_server_add_http2_port(server, addr));
  gpr_free(addr);
  grpc_server_start(server);

  request_call();

  for (;;) {
    ev = grpc_completion_queue_next(cq, gpr_inf_future);
    GPR_ASSERT(ev);
    s = ev->tag;
    switch (ev->type) {
      case GRPC_SERVER_RPC_NEW:
        /* initial ops are already started in request_call */
        grpc_call_accept(ev->call, cq, s, GRPC_WRITE_BUFFER_HINT);
        GPR_ASSERT(grpc_call_start_read(ev->call, s) == GRPC_CALL_OK);
        request_call();
        break;
      case GRPC_WRITE_ACCEPTED:
        GPR_ASSERT(ev->data.write_accepted == GRPC_OP_OK);
        GPR_ASSERT(grpc_call_start_read(ev->call, s) == GRPC_CALL_OK);
        break;
      case GRPC_READ:
        if (ev->data.read) {
          assert_read_ok(ev->tag, ev->data.read);
          GPR_ASSERT(grpc_call_start_write(ev->call, ev->data.read, s,
                                           GRPC_WRITE_BUFFER_HINT) ==
                     GRPC_CALL_OK);
        } else {
          GPR_ASSERT(grpc_call_start_write_status(ev->call, status_ok, s) ==
                     GRPC_CALL_OK);
        }
        break;
      case GRPC_FINISH_ACCEPTED:
      case GRPC_FINISHED:
        if (gpr_unref(&s->pending_ops)) {
          grpc_call_destroy(ev->call);
          gpr_free(s);
        }
        break;
      default:
        abort();
    }
    grpc_event_finish(ev);
  }

  grpc_shutdown();

  return 0;
}