//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Backend/BM168x/BM1684X.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Support/Dnnl/Pool.h"
#include "tpu_mlir/Support/Module.h"

#include "tpu_mlir/Support/MathUtils.h"



using namespace tpu_mlir::backend;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pooling_common_spec {
  int32_t kh;
  int32_t kw;
  int32_t pad_h_t;
  int32_t pad_h_b;
  int32_t pad_w_l;
  int32_t pad_w_r;
  int32_t stride_h;
  int32_t stride_w;
  int32_t dh;
  int32_t dw;
  int32_t is_global_pooling;
  int32_t is_avg_pooling;
  int32_t avg_pooling_mode;
  /* for float */
  int32_t if_relu;
  float relu_limit;
  /* for fix8b */
  int32_t ceil_mode;
  int32_t round_mode;
  int32_t avg_pooling_quant_mode;
  int32_t max_pooling_with_mask; // 1: with mask 0: no mask
  int32_t multiplier;
  int32_t rshiftbits;
  /* asymmetric quantize */
  int32_t merge_requant;
  float rq_scale;
  float rq_offset;
} pooling_common_spec_t;

typedef struct {
  int32_t buffer_addr;
  pooling_common_spec_t common;
} pooling_local_spec_t;

#ifdef __cplusplus
}
#endif

static void SpecAssign(const pool_attr_t &attr, pooling_common_spec_t &spec) {
  spec.kh = attr.kh;
  spec.kw = attr.kw;
  spec.pad_h_t = attr.pad_h;
  spec.pad_h_b = attr.pad_h_after;
  spec.pad_w_l = attr.pad_w;
  spec.pad_w_r = attr.pad_w_after;
  spec.stride_h = attr.sh;
  spec.stride_w = attr.sw;
  spec.dh = 1;
  spec.dw = 1;
  spec.is_global_pooling = attr.is_global;
  spec.avg_pooling_mode = attr.count_include_pad ? 0 : 1;
  spec.if_relu = attr.do_relu;
  spec.relu_limit = attr.relu_limit;
  spec.ceil_mode = 0;
  spec.round_mode = ROUNDING_HALF_AWAY_FROM_ZERO;
  /// TODO: may be need support pad value for pooling2D and pooling 3D
  /// spec.pad_value = attr.pad_value;
}

static bool has_pad(const pool_attr_t &attr) {
  if (attr.pad_h != 0 || attr.pad_w != 0 || attr.pad_d != 0)
    return true;
  if ((attr.ih - attr.kh) % attr.sh != 0 ||
      (attr.iw - attr.kw) % attr.sw != 0 || (attr.id - attr.kd) % attr.sd != 0)
    return true;
  if ((attr.ih - attr.kh) / attr.sh + 1 != attr.oh ||
      (attr.iw - attr.kw) / attr.sw + 1 != attr.ow ||
      (attr.id - attr.kd) / attr.sd + 1 != attr.od)
    return true;
  return false;
}

// =========================================
// GlobalGenInterface
// =========================================

void tpu::Pool2DOp::codegen_global_bm1684x() {
  auto op = getOperation();
  auto input_spec = BM168x::get_input_spec(op);
  auto output_spec = BM168x::get_output_spec(op);
  auto attr = parseParam();
  pooling_common_spec_t spec = {0};
  SpecAssign(attr, spec);
  if (pool_mode() == tpu::PoolMode::Avg) {
    spec.is_avg_pooling = true;
    if (module::isUniformQuantized(input())) {
      bool with_pad = has_pad(attr) && attr.count_include_pad == 0;
      spec.avg_pooling_quant_mode =
          module::isAsymmetric() ? (with_pad ? 1 : 2) : 0;

      if (spec.avg_pooling_quant_mode == 0) {
        spec.multiplier = multiplier().has_value() ? multiplier().value() : 1;
        spec.rshiftbits = rshift().has_value() ? rshift().value() : 0;
      } else if (spec.avg_pooling_quant_mode == 2) {
        spec.merge_requant = true;
        spec.rq_scale =
            scale().has_value() ? (scale().value().convertToDouble()) : 1.;
        spec.rq_offset =
            offset().has_value() ? (offset().value().convertToDouble()) : 0.;
      }
    }
  }
  BM168x::call_global_func("backend_api_pooling_global", &spec, sizeof(spec),
                           input_spec->data(), output_spec->data());
}

// =========================================
// LocalGenInterface
// =========================================

int64_t tpu::Pool2DOp::getBufferSize_bm1684x(
    int64_t in_lmem_bytes, int64_t out_lmem_bytes, int64_t in_nslice,
    int64_t in_hslice, int64_t out_nslice, int64_t out_hslice) {
  switch (pool_mode()) {
  case tpu::PoolMode::Max:
    return 0;
  case tpu::PoolMode::Avg:
    int64_t size = 0;
    if (module::isAsymmetric()) {
      auto &p = getPool2DParam(*this);
      auto kernel = module::getI64Array(kernel_shape());
      int64_t dtype_bytes = p.kh * p.kw > 256 ? sizeof(int) : sizeof(short);
      int64_t eu_num = BM168x::eu_num(dtype_bytes);
      int64_t npu_num = BM168x::NPU_NUM;
      size = align_up(out_hslice * p.iw, eu_num) * ceiling_func(p.c, npu_num) *
             dtype_bytes;
    }
    return size;
  }
  llvm_unreachable("unimplemented Pooling.");
}

void tpu::Pool2DOp::assign_sec_info(int64_t n_step, int64_t h_step,
                                    void *sec_info_) {
  local_sec_info_t *sec_info = (local_sec_info_t *)sec_info_;
  memset(sec_info, 0, sizeof(local_sec_info_t));

  auto attr = parseParam();
  auto gi = getGroupInfo(n_step, h_step);
  auto in_gi = LocalGenInterface::getGroupInfo(input(), n_step, h_step);
  int64_t pad_h_b =
      (in_gi.h_idx + in_gi.h_slice == attr.ih ? attr.pad_h_after : 0);
  sec_info->n_slice = in_gi.n_slice;
  sec_info->h_slice = in_gi.h_slice;
  sec_info->h_idx = in_gi.h_idx;
  sec_info->is_h_split = !(in_gi.h_idx == 0 && in_gi.h_slice == attr.ih);
  // to be compatible with nntoolchain
  if (sec_info->is_h_split) {
    sec_info->h_idx = h_step == 0 ? -attr.pad_h : in_gi.h_idx;
    sec_info->h_slice = sec_info->h_idx < 0
                            ? sec_info->h_slice - sec_info->h_idx
                            : sec_info->h_slice;
    sec_info->h_slice = sec_info->h_slice + pad_h_b;
  }
  sec_info->w_slice = attr.iw;
  sec_info->out_n_slice = gi.n_slice;
  sec_info->out_h_idx = gi.h_idx;
  sec_info->out_h_slice = gi.h_slice;
  sec_info->out_w_slice = attr.ow;
}

void tpu::Pool2DOp::codegen_local_bm1684x(int64_t n_step, int64_t h_step,
                                          void *sec_info_) {
  auto op = getOperation();
  auto input_spec = BM168x::get_input_spec(op);
  auto output_spec = BM168x::get_output_spec(op);
  auto gi = getGroupInfo(n_step, h_step);
  auto in_gi = LocalGenInterface::getGroupInfo(input(), n_step, h_step);

  auto attr = parseParam();
  pooling_local_spec_t spec = {0};
  auto &common = spec.common;
  SpecAssign(attr, common);
  spec.buffer_addr = gi.buffer_addr;
  common.pad_h_t = (in_gi.h_idx == 0 ? attr.pad_h : 0);
  common.pad_h_b =
      (in_gi.h_idx + in_gi.h_slice == attr.ih ? attr.pad_h_after : 0);

  if (pool_mode() == tpu::PoolMode::Avg) {
    bool with_pad = has_pad(attr) && attr.count_include_pad == 0;
    common.is_avg_pooling = true;
    common.avg_pooling_quant_mode =
        module::isAsymmetric() ? (with_pad ? 1 : 2) : 0;

    if (common.avg_pooling_quant_mode == 0) {
      common.multiplier = multiplier().has_value() ? multiplier().value() : -1;
      common.rshiftbits = rshift().has_value() ? rshift().value() : -1;
    } else if (common.avg_pooling_quant_mode == 2) {
      common.merge_requant = true;
      common.rq_scale =
          scale().has_value() ? (scale().value().convertToDouble()) : -1.;
      common.rq_offset =
          offset().has_value() ? (offset().value().convertToDouble()) : -1.;
    }
  }

  BM168x::call_local_func("backend_api_pooling_local", &spec, sizeof(spec),
                          sec_info_, input_spec->data(), output_spec->data());
}
