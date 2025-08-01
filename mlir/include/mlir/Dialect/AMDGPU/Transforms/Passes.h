//===-- Passes.h - AMDGPU transformation pass declarations --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the transformation passes for the TOSA Dialect in MLIR.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_DIALECT_AMDGPU_TRANSFORMS_PASSES_H_
#define MLIR_DIALECT_AMDGPU_TRANSFORMS_PASSES_H_

#include "mlir/Dialect/AMDGPU/Utils/Chipset.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"

namespace mlir {
class ConversionTarget;
namespace amdgpu {

#define GEN_PASS_DECL_AMDGPUEMULATEATOMICSPASS
#define GEN_PASS_DECL_AMDGPUFOLDMEMREFOPSPASS
#define GEN_PASS_DECL_AMDGPUMASKEDLOADTOLOADPASS
#define GEN_PASS_DECL_AMDGPURESOLVESTRIDEDMETADATAPASS
#define GEN_PASS_REGISTRATION
#include "mlir/Dialect/AMDGPU/Transforms/Passes.h.inc"

void populateAmdgpuEmulateAtomicsPatterns(ConversionTarget &target,
                                          RewritePatternSet &patterns,
                                          Chipset chipset,
                                          PatternBenefit benefit = 1);

void populateAmdgpuResolveStridedMetadataPatterns(RewritePatternSet &patterns,
                                                  PatternBenefit benefit = 1);

void populateAmdgpuMaskedloadToLoadPatterns(RewritePatternSet &patterns,
                                            PatternBenefit benefit = 1);

void populateAmdgpuFoldMemRefOpsPatterns(RewritePatternSet &patterns,
                                         PatternBenefit benefit = 1);

} // namespace amdgpu
} // namespace mlir

#endif // MLIR_DIALECT_AMDGPU_TRANSFORMS_PASSES_H_
