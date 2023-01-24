//===----------------------------------------------------------------------===//
//
// Copyright (c) 2020-2030 by Sophgo floatechnologies Inc. All rights reserved.
//
// Licensed under the Apache License v2.0.
// See http://www.apache.org/licenses/LICENSE-2.0 for license information.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Dialect/Top/IR/TopOps.h"
#include "tpu_mlir/Support/Dnnl/Dnnl.h"
#include "tpu_mlir/Support/Module.h"
#include "tpu_mlir/Support/MathUtils.h"

int64_t top::TopKOp::getFLOPs() { return 0; }

LogicalResult top::TopKOp::init(InferenceParameter &p) { return success(); }
void top::TopKOp::deinit(InferenceParameter &p) {}

LogicalResult top::TopKOp::inference(InferenceParameter &p) {
  auto axis = getAxis();
  auto is_largest = getLargest();
  auto K = getK();
  auto is_sorted = getSorted();
  if (is_sorted == false) {
    llvm_unreachable("Not supported");
  }
  auto input_shape = module::getShape(getInput());
  if (axis != input_shape.size() - 1) {
    llvm_unreachable("Not supported");
  }
  bool has_values = !module::isNone(getValues());
  bool has_indices = !module::isNone(getIndices());
  int axis_dim = input_shape[axis];
  int outer_dim = 1;
  for (int i = 0; i < axis; i++) {
    outer_dim *= input_shape[i];
  }
#pragma omp parallel for schedule(static, omp_schedule(outer_dim))
  for (int i = 0; i < outer_dim; i++) {
    auto *ptr = p.inputs[0] + i * axis_dim;
    std::vector<std::pair<int, float>> result;
    topk_indices(result, ptr, axis_dim, K, is_largest);
    for (int k = 0; k < K; k++) {
      if (has_indices) {
        auto indices_ptr = p.outputs[1] + i * K + k;
        *indices_ptr = (float)result[k].first;
      }
      if (has_values) {
        auto values_ptr = p.outputs[0] + i * K + k;
        *values_ptr = result[k].second;
      }
    }
  }

  return success();
}
