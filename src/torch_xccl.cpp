/**
 * * Copyright (C) Mellanox Technologies Ltd. 2001-2020.  ALL RIGHTS RESERVED.
 * *
 * * See file LICENSE for terms.
 * */

#include <torch_xccl.hpp>
#include <map>

namespace c10d {

struct xccl_oob_allgather_req_t {
    xccl_ep_range_t     range;
    void                *sbuf;
    void                *rbuf;
    void                *oob_coll_ctx;
    int                 my_rank;
    size_t              msglen;
    int                 iter;
    torch_ucx_request_t *reqs[2];
};

static xccl_status_t oob_allgather_test(void *req)
{
  xccl_oob_allgather_req_t *oob_req = static_cast<xccl_oob_allgather_req_t*>(req);
  int rank, size, sendto, recvfrom, recvdatafrom, senddatafrom;
  torch_ucx_comm_t *oob_ctx = static_cast<torch_ucx_comm_t*>(oob_req->oob_coll_ctx);
  char *tmpsend = NULL, *tmprecv = NULL;
  size_t msglen = oob_req->msglen;
  torch_ucx_status_t st;

  if (oob_req->range.type == XCCL_EP_RANGE_UNDEFINED) {
    size = oob_ctx->size;
    rank = oob_ctx->rank;
  } else {
    size = oob_req->range.ep_num;
    rank = oob_req->my_rank;
  }

  if (oob_req->iter == 0) {
    tmprecv = (char*) oob_req->rbuf + (ptrdiff_t)(rank * msglen);
    memcpy(tmprecv, oob_req->sbuf, msglen);
  }
  sendto   = (rank + 1) % size;
  recvfrom = (rank - 1 + size) % size;
  if (oob_req->range.type != XCCL_EP_RANGE_UNDEFINED) {
    sendto   = xccl_range_to_rank(oob_req->range, sendto);
    recvfrom = xccl_range_to_rank(oob_req->range, recvfrom);
  }
  for (; oob_req->iter < size - 1; oob_req->iter++) {
    if (oob_req->iter > 0) {
      st = torch_ucx_req_test(oob_ctx, oob_req->reqs, 2, NULL, 1, 2);
      if (st == TORCH_UCX_INPROGRESS) {
        return XCCL_INPROGRESS;
      }
    }
    recvdatafrom = (rank - oob_req->iter - 1 + size) % size;
    senddatafrom = (rank - oob_req->iter + size) % size;
    tmprecv = (char*)oob_req->rbuf + (ptrdiff_t)(recvdatafrom * msglen);
    tmpsend = (char*)oob_req->rbuf + (ptrdiff_t)(senddatafrom * msglen);

    torch_ucx_send_nb(oob_ctx, tmpsend, msglen, sendto, 1,
                      &oob_req->reqs[0], TORCH_UCX_OOB_TAG);

    torch_ucx_recv_nb(oob_ctx, tmprecv, msglen, recvfrom, 1,
                      &oob_req->reqs[1], TORCH_UCX_OOB_TAG);
  }

  st = torch_ucx_req_test(oob_ctx, oob_req->reqs, 2, NULL, 1, 2);
  if (st == TORCH_UCX_INPROGRESS) {
    return XCCL_INPROGRESS;
  }

  return XCCL_OK;
}

static xccl_status_t oob_allgather_free(void *req)
{
  xccl_oob_allgather_req_t *request = static_cast<xccl_oob_allgather_req_t*>(req);
  delete request;

  return XCCL_OK;
}

static int oob_allgather(void *sbuf, void *rbuf, size_t msglen,
                         int my_rank, xccl_ep_range_t range,
                         void *oob_coll_ctx, void **req)
{
  xccl_oob_allgather_req_t *oob_req = new(xccl_oob_allgather_req_t);
  oob_req->sbuf         = sbuf;
  oob_req->rbuf         = rbuf;
  oob_req->msglen       = msglen;
  oob_req->range        = range;
  oob_req->oob_coll_ctx = oob_coll_ctx;
  oob_req->my_rank      = my_rank;
  oob_req->iter         = 0;
  *req = oob_req;

  return oob_allgather_test(oob_req);
}

torch_ucc_status_t torch_xccl_comm_init(torch_ucx_comm_t *p2p_comm,
                                        void **comm)
{
    torch_xccl_comm_t *xccl_comm;
    xccl_lib_params_t lib_params;
    xccl_lib_config_t *cfg;
    xccl_status_t     st;
  
    xccl_comm = new torch_xccl_comm_t;
    memset(&lib_params, 0, sizeof(lib_params));
    lib_params.field_mask = XCCL_LIB_PARAM_FIELD_TEAM_USAGE |
                            XCCL_LIB_PARAM_FIELD_COLL_TYPES;
  
    lib_params.team_usage = XCCL_LIB_PARAMS_TEAM_USAGE_SW_COLLECTIVES |
                            XCCL_LIB_PARAMS_TEAM_USAGE_HW_COLLECTIVES;
  
    lib_params.coll_types = XCCL_COLL_CAP_BCAST |
                            XCCL_COLL_CAP_ALLREDUCE |
                            XCCL_COLL_CAP_ALLTOALL |
                            XCCL_COLL_CAP_ALLTOALLV;
  
    cfg = NULL;
    st = xccl_lib_init(&lib_params, cfg, &xccl_comm->xccl_lib);
    if (st != XCCL_OK) {
        fprintf(stderr, "TorchUCC: failed to init XCCL lib\n");
        goto free_comm;
    }
  
    xccl_context_config_t *ctx_config;
    st = xccl_context_config_read(xccl_comm->xccl_lib, "TORCH", NULL, &ctx_config);
    if (st != XCCL_OK) {
        fprintf(stderr, "TorchUCC: failed to read XCCL context config\n");
        goto free_lib;
    }
    
    xccl_context_params_t ctx_params;
  
    ctx_params.field_mask       = XCCL_CONTEXT_PARAM_FIELD_THREAD_MODE |
                                  XCCL_CONTEXT_PARAM_FIELD_OOB |
                                  XCCL_CONTEXT_PARAM_FIELD_TEAM_COMPLETION_TYPE |
                                  XCCL_CONTEXT_PARAM_FIELD_TLS;
  
    ctx_params.thread_mode      = XCCL_THREAD_MODE_MULTIPLE;
  
    ctx_params.completion_type  = XCCL_TEAM_COMPLETION_TYPE_BLOCKING;
  
    ctx_params.tls              = XCCL_TL_UCX;
  
    ctx_params.oob.allgather    = oob_allgather;
    ctx_params.oob.req_test     = oob_allgather_test;
    ctx_params.oob.req_free     = oob_allgather_free;
    ctx_params.oob.coll_context = static_cast<void*>(p2p_comm);
    ctx_params.oob.rank         = p2p_comm->rank;
    ctx_params.oob.size         = p2p_comm->size;
  
    st = xccl_context_create(xccl_comm->xccl_lib, &ctx_params, ctx_config,
                             &xccl_comm->xccl_ctx);
    xccl_context_config_release(ctx_config);
    if (st != XCCL_OK) {
        fprintf(stderr, "TorchUCC: failed to create XCCL context\n");
        goto free_lib;
    }
  
    xccl_team_params_t team_params;
  
    team_params.field_mask           = XCCL_TEAM_PARAM_FIELD_EP_RANGE |
                                       XCCL_TEAM_PARAM_FIELD_OOB;
  
    team_params.range.type           = XCCL_EP_RANGE_STRIDED;
    team_params.range.strided.start  = 0;
    team_params.range.strided.stride = 1;
    team_params.oob.allgather        = oob_allgather;
    team_params.oob.req_test         = oob_allgather_test;
    team_params.oob.req_free         = oob_allgather_free;
    team_params.oob.coll_context     = static_cast<void*>(p2p_comm);
    team_params.oob.rank             = p2p_comm->rank;
    team_params.oob.size             = p2p_comm->size;
  
    st = xccl_team_create_post(xccl_comm->xccl_ctx, &team_params,
                               &xccl_comm->xccl_team);
    if (st != XCCL_OK) {
        fprintf(stderr, "TorchUCC: failed to create XCCL team\n");
        goto free_context;
    }
    while (XCCL_INPROGRESS == xccl_team_create_test(xccl_comm->xccl_team));
    *comm = xccl_comm;

    return TORCH_UCC_OK;
free_context:
    xccl_context_destroy(xccl_comm->xccl_ctx);
free_lib:
    xccl_lib_cleanup(xccl_comm->xccl_lib);
free_comm:
    delete xccl_comm;
    *comm = NULL;
    return TORCH_UCC_ERROR;
}

struct torch_xccl_request_t {
    torch_ucc_coll_request_t super;
    xccl_coll_req_h          request;
    torch_ucc_status_t       status;
};

torch_ucc_status_t torch_xccl_comm_close(void *comm)
{
    torch_xccl_comm_t *xccl_comm = (torch_xccl_comm_t*)comm;

    xccl_team_destroy(xccl_comm->xccl_team);
    xccl_context_destroy(xccl_comm->xccl_ctx);
    xccl_lib_cleanup(xccl_comm->xccl_lib);
    delete xccl_comm;

    return TORCH_UCC_OK;
}

std::map<ReduceOp, xccl_op_t> xccl_op_map = {
    {ReduceOp::MIN,     XCCL_OP_MIN},
    {ReduceOp::MAX,     XCCL_OP_MAX},
    {ReduceOp::SUM,     XCCL_OP_SUM},
    {ReduceOp::PRODUCT, XCCL_OP_PROD},
};

std::map<at::ScalarType, xccl_dt_t> xccl_type_map = {
    {at::kByte,   XCCL_DT_UINT8},
    {at::kChar,   XCCL_DT_INT8},
    {at::kHalf,   XCCL_DT_FLOAT16},
    {at::kDouble, XCCL_DT_FLOAT64},
    {at::kFloat,  XCCL_DT_FLOAT32},
    {at::kInt,    XCCL_DT_INT32},
    {at::kLong,   XCCL_DT_INT64},
};

torch_ucc_status_t torch_xccl_alltoall(void *coll_comm,
                                       void *send_buffer, torch_ucx_memtype_t send_mtype,
                                       void *recv_buffer, torch_ucx_memtype_t recv_mtype,
                                       size_t len, torch_ucc_coll_request_t **request)
{
    torch_xccl_comm_t    *xccl_comm = (torch_xccl_comm_t*)coll_comm;
    xccl_coll_req_h      xccl_req;
    xccl_coll_op_args_t  coll_args;
    torch_xccl_request_t *coll_req;

    coll_req = new torch_xccl_request_t;
    coll_req->status = TORCH_UCC_INPROGRESS;

    coll_args.coll_type              = XCCL_ALLTOALL;
    coll_args.buffer_info.src_buffer = send_buffer;
    coll_args.buffer_info.dst_buffer = recv_buffer;
    coll_args.buffer_info.len        = len;
    coll_args.alg.set_by_user        = 0;

    xccl_collective_init(&coll_args, &xccl_req, xccl_comm->xccl_team);
    xccl_collective_post(xccl_req);

    coll_req->request = xccl_req;
    *request = (torch_ucc_coll_request_t*)coll_req;

    return TORCH_UCC_OK;
}

torch_ucc_status_t torch_xccl_alltoallv(void *coll_comm,
                                        void *send_buffer, torch_ucx_memtype_t send_mtype,
                                        at::ScalarType send_data_type,
                                        uint32_t *send_lengths, uint32_t *send_offsets,
                                        void *recv_buffer, torch_ucx_memtype_t recv_mtype,
                                        at::ScalarType recv_data_type,
                                        uint32_t *recv_lengths, uint32_t *recv_offsets,
                                        torch_ucc_coll_request_t **request)
{
    torch_xccl_comm_t    *xccl_comm = (torch_xccl_comm_t*)coll_comm;
    xccl_coll_req_h      xccl_req;
    xccl_coll_op_args_t  coll_args;
    torch_xccl_request_t *coll_req;

    coll_req = new torch_xccl_request_t;
    coll_req->status = TORCH_UCC_INPROGRESS;

    coll_args.coll_type                     = XCCL_ALLTOALLV;
    coll_args.buffer_info.src_buffer        = send_buffer;
    coll_args.buffer_info.src_displacements = send_offsets;
    coll_args.buffer_info.src_counts        = send_lengths;
    coll_args.buffer_info.src_datatype      = xccl_type_map.at(send_data_type);
    coll_args.buffer_info.dst_buffer        = recv_buffer;
    coll_args.buffer_info.dst_displacements = recv_offsets;
    coll_args.buffer_info.dst_counts        = recv_lengths;
    coll_args.buffer_info.dst_datatype      = xccl_type_map.at(recv_data_type);
    coll_args.alg.set_by_user               = 0;

    xccl_collective_init(&coll_args, &xccl_req, xccl_comm->xccl_team);
    xccl_collective_post(xccl_req);

    coll_req->request = xccl_req;
    *request = (torch_ucc_coll_request_t*)coll_req;

    return TORCH_UCC_OK;
}

torch_ucc_status_t torch_xccl_allreduce(void *coll_comm,
                                        void *send_buffer, torch_ucx_memtype_t send_mtype,
                                        void *recv_buffer, torch_ucx_memtype_t recv_mtype,
                                        int count, int element_size, at::ScalarType data_type,
                                        ReduceOp op, torch_ucc_coll_request_t **request)
{
    torch_xccl_comm_t    *xccl_comm = (torch_xccl_comm_t*)coll_comm;
    xccl_coll_req_h      xccl_req;
    xccl_coll_op_args_t  coll_args;
    torch_xccl_request_t *coll_req;

    coll_req = new torch_xccl_request_t;
    coll_req->status = TORCH_UCC_INPROGRESS;

    coll_args.coll_type              = XCCL_ALLREDUCE;
    coll_args.buffer_info.src_buffer = send_buffer;
    coll_args.buffer_info.dst_buffer = recv_buffer;
    coll_args.buffer_info.len        = count * element_size;
    coll_args.reduce_info.dt         = xccl_type_map.at(data_type);
    coll_args.reduce_info.op         = xccl_op_map.at(op);
    coll_args.reduce_info.count      = count;
    coll_args.alg.set_by_user        = 0;

    xccl_collective_init(&coll_args, &xccl_req, xccl_comm->xccl_team);
    xccl_collective_post(xccl_req);

    coll_req->request = xccl_req;
    *request = (torch_ucc_coll_request_t*)coll_req;

    return TORCH_UCC_OK;
}

torch_ucc_status_t torch_xccl_progress(torch_ucc_coll_request_t *request)
{
    torch_xccl_request_t *req = (torch_xccl_request_t*)request;
    xccl_status_t st;

    if (req->status == TORCH_UCC_INPROGRESS) {
        st = xccl_collective_test(req->request);
        if (st != XCCL_INPROGRESS) {
            req->status = TORCH_UCC_OK;
            xccl_collective_finalize(req->request);
        }
    }

    return TORCH_UCC_OK;
}

torch_ucc_status_t torch_xccl_test(torch_ucc_coll_request_t *request)
{
    torch_xccl_request_t *req = (torch_xccl_request_t*)request;

    return req->status;
}


torch_ucc_status_t torch_xccl_free(torch_ucc_coll_request_t *request)
{
    torch_xccl_request_t *req = (torch_xccl_request_t*)request;

    delete req;
    return TORCH_UCC_OK;
}

torch_ucc_coll_ops_t xccl_coll_ops {
    torch_xccl_comm_init,
    torch_xccl_alltoall,
    torch_xccl_alltoallv,
    torch_xccl_allreduce,
    torch_xccl_progress,
    torch_xccl_test,
    torch_xccl_free,
    torch_xccl_comm_close
};

}
