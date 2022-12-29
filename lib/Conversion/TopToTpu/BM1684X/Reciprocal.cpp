//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/TypeRange.h"
#include "mlir/IR/ValueRange.h"
#include "tpu_mlir/Conversion/TopToTpu/LoweringBM1684X.h"
#include "tpu_mlir/Dialect/Top/IR/TopOps.h"
#include "tpu_mlir/Support/Module.h"


namespace tpu_mlir {
namespace bm1684x {

void ReciprocalLowering::LoweringF32(PatternRewriter &rewriter,
                                     top::ReciprocalOp op) const {
  lowering_common_f32<tpu::ReciprocalOp>(rewriter, op);
}
void ReciprocalLowering::LoweringINT4(PatternRewriter &rewriter, top::ReciprocalOp op,
                                   bool asymmetric) const {
  LoweringINT8(rewriter, op, asymmetric);
}
void ReciprocalLowering::LoweringINT8(PatternRewriter &rewriter,
                                      top::ReciprocalOp op,
                                      bool asymmetric) const {

  double const_s = op.const_val().convertToDouble();
  Value table =
      create_lookup_table(op.input(), op.output(), asymmetric,
                          [const_s](double val) { return const_s / val; });
  auto newType = getQuantInt8Type(op.output(), asymmetric);
  rewriter.replaceOpWithNewOp<tpu::LutOp>(op, newType,
                                          ValueRange{op.input(), table});
}

void ReciprocalLowering::LoweringBF16(PatternRewriter &rewriter,
                                      top::ReciprocalOp op) const {
  // lowering_common_bf16<tpu::ReciprocalOp>(rewriter, op);
  LoweringF32(rewriter, op);
}

void ReciprocalLowering::LoweringF16(PatternRewriter &rewriter,
                                     top::ReciprocalOp op) const {
  // lowering_common_f16<tpu::ReciprocalOp>(rewriter, op);
  LoweringF32(rewriter, op);
}

void ReciprocalLowering::LoweringQuantized(PatternRewriter &rewriter,
                                           top::ReciprocalOp op) const {
  llvm_unreachable("Not Implemented");
}

} // namespace bm1684x
} // namespace tpu_mlir
