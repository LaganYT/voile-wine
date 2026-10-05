#if 0
#pragma makedep unix
#endif
/*
 * Copyright 2026 Obelisk
 * SPDX-License-Identifier: MIT
 *
 * No virtio-gpu under LKD on macOS: every virtgpu entry fails, so the Neptune client takes its vtest transport.
 */
#include <stdbool.h>
#include <stddef.h>
#include "npt_unix_internal.h"
#include "npt_unixlib.h"

NTSTATUS npt_do_virtgpu_open(void *args) { ((struct npt_unix_virtgpu_open_params *)args)->result_fd = -1; return STATUS_SUCCESS; }
NTSTATUS npt_do_virtgpu_getparam(void *args) { ((struct npt_unix_virtgpu_getparam_params *)args)->result = -1; return STATUS_SUCCESS; }
NTSTATUS npt_do_virtgpu_get_caps(void *args) { ((struct npt_unix_virtgpu_get_caps_params *)args)->result = -1; return STATUS_SUCCESS; }
NTSTATUS npt_do_virtgpu_context_init(void *args) { ((struct npt_unix_virtgpu_context_init_params *)args)->result = -1; return STATUS_SUCCESS; }
NTSTATUS npt_do_virtgpu_create_blob(void *args) { ((struct npt_unix_virtgpu_create_blob_params *)args)->result = -1; return STATUS_SUCCESS; }
NTSTATUS npt_do_virtgpu_map(void *args) { ((struct npt_unix_virtgpu_map_params *)args)->result_ptr = NULL; return STATUS_SUCCESS; }
NTSTATUS npt_do_virtgpu_execbuffer(void *args)
{
   struct npt_unix_virtgpu_execbuffer_params *p = args;
   p->result = -1;
   p->fence_fd = -1;
   return STATUS_SUCCESS;
}
NTSTATUS npt_do_virtgpu_gem_close(void *args) { (void)args; return STATUS_SUCCESS; }
NTSTATUS npt_do_virtgpu_prime_handle_to_fd(void *args) { ((struct npt_unix_virtgpu_prime_handle_to_fd_params *)args)->result_fd = -1; return STATUS_SUCCESS; }
