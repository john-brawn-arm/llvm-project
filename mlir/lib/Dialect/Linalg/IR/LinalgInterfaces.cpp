//===- LinalgInterfaces.cpp - Linalg interfaces implementation ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/Complex/IR/Complex.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineExprVisitor.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/BuiltinTypeInterfaces.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/TypeUtilities.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

using namespace mlir;
using namespace mlir::linalg;

/// Include the definitions of the copy operation interface.
#include "mlir/Dialect/Linalg/IR/LinalgInterfaces.cpp.inc"

//===----------------------------------------------------------------------===//
// Interface utility functions
//===----------------------------------------------------------------------===//

bool linalg::detail::canOpOperandsBeDroppedImpl(
    linalg::LinalgOp linalgOp, ArrayRef<OpOperand *> droppedOperands) {
  SmallVector<AffineMap> indexingMaps;
  for (auto &opOperand : linalgOp->getOpOperands()) {
    if (llvm::is_contained(droppedOperands, &opOperand))
      continue;
    indexingMaps.push_back(linalgOp.getMatchingIndexingMap(&opOperand));
  }
  if (indexingMaps.empty()) {
    // If there are no indexing maps, the operand can only be dropped
    // if the op has no loops.
    return linalgOp.getNumLoops() == 0;
  }
  return inversePermutation(concatAffineMaps(
             indexingMaps, linalgOp.getContext())) != AffineMap();
}

//===----------------------------------------------------------------------===//
// CopyOpInterface implementation
//===----------------------------------------------------------------------===//

bool linalg::isaCopyOpInterface(LinalgOp op) {
  // Check all loops are parallel and linalgOp is single input and output.
  if (!op.isAllParallelLoops() || !op.isSingleInputOutput())
    return false;

  auto mapRange = op.getIndexingMapsArray();
  if (mapRange.size() != 2 || !mapRange.front().isIdentity() ||
      !mapRange.back().isIdentity()) {
    return false;
  }
  // Check yield first block argument.
  Block *body = op.getBlock();
  if (body->getOperations().size() != 1)
    return false;
  auto yieldOp = dyn_cast<linalg::YieldOp>(body->back());
  if (!yieldOp || yieldOp.getNumOperands() != 1)
    return false;
  return yieldOp->getOperand(0) == body->getArgument(0);
}

//===----------------------------------------------------------------------===//
// FillOpInterface implementation
//===----------------------------------------------------------------------===//
/// Detects if a linalg.generic operation represents a fill with an inlined
/// constant. If so, returns the constant value. Otherwise, returns
/// std::nullopt.
static std::optional<Value> isaInlinedFillOp(GenericOp op) {
  if (!op.isAllParallelLoops() || op.getNumDpsInits() != 1 ||
      op.getNumDpsInputs() != 0)
    return std::nullopt;

  // Init should not be referenced.
  if (op.payloadUsesValueFromOperand(op.getDpsInitOperand(0)))
    return std::nullopt;

  Block *body = op.getBody();
  if (body->getOperations().size() != 1)
    return std::nullopt;

  auto yieldOp = dyn_cast<linalg::YieldOp>(body->back());
  if (!yieldOp || yieldOp.getNumOperands() != 1)
    return std::nullopt;

  Value yieldOperand = yieldOp->getOperand(0);
  if (!yieldOperand.getDefiningOp<arith::ConstantOp>() &&
      !yieldOperand.getDefiningOp<complex::ConstantOp>())
    return std::nullopt;

  return yieldOperand;
}

/// Detects if a linalg.generic operation represents an external scalar input.
/// If so, returns the constant value. Otherwise, returns std::nullopt.
static std::optional<Value> isaExternalFillOp(GenericOp op) {
  // Structural.
  if (!op.isAllParallelLoops() || !op.isSingleInputOutput() ||
      !op.isSingleYieldOp())
    return std::nullopt;

  // Input should be referenced and init should not.
  if (!op.payloadUsesValueFromOperand(op.getDpsInputOperand(0)) ||
      op.payloadUsesValueFromOperand(op.getDpsInitOperand(0)))
    return std::nullopt;

  OpOperand *value = op.getDpsInputOperand(0);
  if (!op.isScalar(value))
    return std::nullopt;
  return value->get();
}

std::optional<Value> linalg::isaFillOpInterface(GenericOp op) {
  if (auto fillVal = isaInlinedFillOp(op))
    return fillVal;
  return isaExternalFillOp(op);
}

//===----------------------------------------------------------------------===//
// BroadcastOpInterface implementation
//===----------------------------------------------------------------------===//
std::optional<SmallVector<int64_t>>
linalg::isaBroadcastOpInterface(GenericOp op) {
  // Structural.
  if (!op.isAllParallelLoops() || !op.isSingleInputOutput() ||
      !op.isSingleYieldOp())
    return std::nullopt;

  auto srcTy = op.getDpsInputOperand(0)->get().getType();
  auto dstTy = op.getDpsInitOperand(0)->get().getType();
  if (!isa<MemRefType, RankedTensorType>(srcTy) ||
      !isa<MemRefType, RankedTensorType>(dstTy))
    return std::nullopt;

  // Check output is identity map. Broadcast could additionally be
  // employing permutation of indices and that would be expressible
  // in linalg.generic but is not expressible for named broadcast op.
  auto dstMap = op.getIndexingMapsArray()[1];
  if (!dstMap.isIdentity())
    return std::nullopt;

  SmallVector<int64_t> position;
  auto srcMap = op.getIndexingMapsArray()[0];

  if (srcMap.getResults().size() >= dstMap.getResults().size())
    return std::nullopt;

  // Check input map is monotonically increasing DimIds.
  for (unsigned i = 0; i < srcMap.getNumResults(); ++i) {
    auto expr = llvm::dyn_cast<AffineDimExpr>(srcMap.getResults()[i]);
    if (!expr)
      return std::nullopt;
    int64_t pos = expr.getPosition();
    if (i > 0 && pos <= position[i - 1])
      return std::nullopt;
    position.push_back(expr.getPosition());
  }

  SmallVector<int64_t> broadcastedDims;
  auto numDims = srcMap.getNumDims();
  // This is quadratic but number of items is generally small.
  for (auto dim : llvm::seq<int64_t>(0, numDims)) {
    if (!llvm::is_contained(position, dim))
      broadcastedDims.push_back(dim);
  }
  return broadcastedDims;
}

//===----------------------------------------------------------------------===//
// TransposeOpInterface implementation
//===----------------------------------------------------------------------===//
std::optional<SmallVector<int64_t>>
linalg::isaTransposeOpInterface(GenericOp op) {
  // To specialize as a transpose op, the genericOp must be
  // all parallel loops, single input, single output, and its body
  // should be just a yield op, yielding input as output as is (no compute).
  if (!op.isAllParallelLoops() || !op.isSingleInputOutput() ||
      !op.isSingleYieldOp())
    return std::nullopt;

  auto mapRange = op.getIndexingMapsArray();
  if (mapRange.size() != 2)
    return std::nullopt;

  auto mapOfInput = mapRange.front();
  auto mapOfResult = mapRange.back();

  // linalg.transpose permutes the dimensions of input using this
  // rule: dim(result, i) = dim(input, permutation[i])
  if (!mapOfResult.isIdentity() || !mapOfInput.isPermutation())
    return std::nullopt;

  SmallVector<int64_t> permutation(mapOfInput.getNumDims());
  for (unsigned i = 0; i < mapOfInput.getNumDims(); ++i) {
    auto expr = llvm::cast<AffineDimExpr>(mapOfInput.getResults()[i]);
    permutation[expr.getPosition()] = i;
  }
  return permutation;
}

//===----------------------------------------------------------------------===//
// Elementwise Single Unary/Binary-OpInterface implementation
//===----------------------------------------------------------------------===//
static bool isaElemwiseSingleUnaryOrBinaryOpInterface(linalg::GenericOp op,
                                                      unsigned arity) {
  // Check all loops are parallel.
  if (!op.isAllParallelLoops() || op.getNumLoops() < 1)
    return false;

  // Check there are arity-inputs, 1-output and all are identity-maps.
  if (op.getNumDpsInputs() != arity || op.getNumDpsInits() != 1 ||
      !llvm::all_of(op.getIndexingMapsArray(),
                    [](AffineMap map) { return map.isIdentity(); }))
    return false;

  // Init should not be referenced for elementwise operations.
  if (op.payloadUsesValueFromOperand(op.getDpsInitOperand(0)))
    return false;

  // A linalg.generic could be series of elementwise ops e.g. exp(neg(x)) such
  // as resulting from producer-consumer fusion. Here, we restrict to two ops in
  // the body, where the first is the elementwise single op and the second a
  // yield.
  Block *body = op.getBody();
  if (body->getOperations().size() != 2)
    return false;

  Operation *oper = &body->front();
  if (oper->getNumOperands() != arity || oper->getNumResults() != 1)
    return false;

  auto yieldOp = dyn_cast<linalg::YieldOp>(body->back());
  if (!yieldOp || yieldOp.getNumOperands() != 1 ||
      yieldOp->getOperand(0).getDefiningOp() != oper)
    return false;
  return true;
}

bool linalg::isaElemwiseSingleUnaryOpInterface(linalg::GenericOp op) {
  // All basic elemwise checks.
  if (!isaElemwiseSingleUnaryOrBinaryOpInterface(op, 1))
    return false;

  // Check input is actully used.
  if (!op.payloadUsesValueFromOperand(op.getDpsInputOperand(0)))
    return false;
  return true;
}

bool linalg::isaElemwiseSingleBinaryOpInterface(linalg::GenericOp op) {
  if (!isaElemwiseSingleUnaryOrBinaryOpInterface(op, 2))
    return false;

  // Check both inputs are used (elementwise).
  OpOperand *inputOpOperand0 = op.getDpsInputOperand(0);
  OpOperand *inputOpOperand1 = op.getDpsInputOperand(1);
  if (!op.payloadUsesValueFromOperand(inputOpOperand0) ||
      !op.payloadUsesValueFromOperand(inputOpOperand1))
    return false;
  return true;
}

//===----------------------------------------------------------------------===//
// ContractionOpInterface implementation
//===----------------------------------------------------------------------===//

/// If the value is defined by a chain of unary side effect-free, go up the
/// use-def chain until the first value that isn't defined by such an op.
// TODO: relax to multi-operands with constants, which are technically unary ops
// as needed (e.g. add5).
static Value getSourceSkipUnary(Value value) {
  Operation *op = value.getDefiningOp();
  while (op && op->getNumOperands() == 1) {
    auto iface = dyn_cast<MemoryEffectOpInterface>(op);
    if (!iface || !iface.hasNoEffect())
      break;
    value = op->getOperand(0);
    op = value.getDefiningOp();
  }
  return value;
}

bool mlir::linalg::detail::isContractionBody(
    Block &block, function_ref<bool(Operation *, Operation *)> isaPair,
    llvm::raw_ostream &errs) {
  if (block.empty() || !block.back().mightHaveTrait<OpTrait::IsTerminator>()) {
    errs << "no terminator in the block";
    return false;
  }

  if (block.getNumArguments() != 3) {
    errs << "expected block with 3 arguments";
    return false;
  }

  Operation *terminator = block.getTerminator();
  if (terminator->getNumOperands() != 1) {
    errs << "expected terminator with 1 operand";
    return false;
  }

  Value yielded = getSourceSkipUnary(terminator->getOperand(0));
  Operation *reductionOp = yielded.getDefiningOp();
  if (reductionOp->getNumResults() != 1 || reductionOp->getNumOperands() != 2) {
    errs << "expected reduction op to be binary";
    return false;
  }

  Value reductionLHS = getSourceSkipUnary(reductionOp->getOperand(0));
  Value reductionRHS = getSourceSkipUnary(reductionOp->getOperand(1));

  if (reductionLHS != block.getArgument(2) &&
      reductionRHS != block.getArgument(2)) {
    errs << "expected reduction to take block argument #2 as one of the "
            "operands (modulo unary casts)";
    return false;
  }

  Value contributed = getSourceSkipUnary(
      isa<BlockArgument>(reductionLHS) ? reductionRHS : reductionLHS);
  Operation *elementwiseOp = contributed.getDefiningOp();
  if (!elementwiseOp || elementwiseOp->getNumResults() != 1 ||
      elementwiseOp->getNumOperands() != 2) {
    errs << "expected elementwise op to be binary";
    return false;
  }

  if (!isaPair(elementwiseOp, reductionOp)) {
    errs << "expected reduction/elementwise op kind not satisfied";
    return false;
  }

  Value elementwiseLHS = getSourceSkipUnary(elementwiseOp->getOperand(0));
  Value elementwiseRHS = getSourceSkipUnary(elementwiseOp->getOperand(1));
  if ((elementwiseLHS == block.getArgument(0) &&
       elementwiseRHS == block.getArgument(1)) ||
      (elementwiseLHS == block.getArgument(1) &&
       elementwiseRHS == block.getArgument(0))) {
    return true;
  }

  errs << "expected elementwise op to apply to block arguments (modulo unary "
          "casts)";
  return false;
}

/// Returns true if the two operations are of the kinds specified by a pair of
/// consecutive template arguments.
template <typename AddOpTy, typename MulOpTy, typename... Args>
static bool isPairTemplateImpl(Operation *add, Operation *mul) {
  static_assert(sizeof...(Args) % 2 == 0,
                "expected an even number of template arguments");
  if (isa<AddOpTy>(add) && isa<MulOpTy>(mul))
    return true;

  if constexpr (sizeof...(Args) > 0)
    return isPairTemplateImpl<Args...>(add, mul);
  else
    return false;
}

/// Returns true if the block is a body of a contraction with the kinds of
/// operations given pairwise by template arguments.
template <typename... Args>
static bool isContractionBody(Block &block) {
  return linalg::detail::isContractionBody(block, &isPairTemplateImpl<Args...>);
}

/// Given an `indexingMap` and its corresponding `iterators`, returns
/// the positions of the iterators of type `iter` that are indexed by
/// the `indexingMap` as a permutation. This is useful to infer various
/// subcomputations on a `LinalgOp`. This is performed by looking up
/// each result in the `indexingMap` and determining whether:
///   - It is a single AffineDimExpr.
///   - It is the only result involving this AffineDimExpr.
static llvm::SmallDenseSet<int64_t>
findPermutationsIndexingOperand(AffineMap indexingMap,
                                ArrayRef<utils::IteratorType> iterators,
                                utils::IteratorType iter) {
  assert(iterators.size() == indexingMap.getNumDims());
  llvm::SmallDenseSet<int64_t> res;
  for (AffineExpr e : indexingMap.getResults()) {
    if (auto d = dyn_cast<AffineDimExpr>(e)) {
      if (iterators[d.getPosition()] == iter &&
          llvm::count_if(indexingMap.getResults(), [d](AffineExpr e) {
            return e.isFunctionOfDim(d.getPosition());
          }) == 1)
        res.insert(d.getPosition());
    }
  }
  return res;
}

namespace {
auto par = utils::IteratorType::parallel;
auto red = utils::IteratorType::reduction;
} // namespace

/// Infer the iterator types from the init affine map. This looks at which dims
/// are present in the map results, and returns an iterator types array with
/// parallel types for dims that are present, and reduction types for dims that
/// are not present.
static FailureOr<SmallVector<utils::IteratorType>>
inferIteratorsFromOutMap(AffineMap map) {
  if (!map.isProjectedPermutation())
    return failure();
  SmallVector<utils::IteratorType> iterators(map.getNumDims(), red);
  for (auto expr : map.getResults())
    if (auto dim = dyn_cast<AffineDimExpr>(expr))
      iterators[dim.getPosition()] = par;
  return iterators;
}

/// Find 2 parallel (m and n) and 1 reduction (k) dimension candidates that form
/// a matmul subcomputation within `linalgOp`. These dimensions are such that:
///   1. The m dimension is involved in an outer-product along LHS
///      (i.e. it is a permutation on RES and LHS and does not appear in RHS).
///   2. The n dimension is involved in an outer-product along RHS
///      (i.e. it is a permutation on RES and RHS and does not appear in LHS).
///   3. The k dimension appears as a permutation on LHS and RHS.
///   4. m, n and k appear only once in any given indexing.
///   5. Optional batch dimensions that appear in all operands are captured.
/// This allows e.g. detecting that some contraction is embedded within
/// `linalgOp` with some orthogonal heuristic.
static FailureOr<ContractionDimensions>
inferContractionDimsImpl(ArrayRef<AffineMap> indexingMaps,
                         ArrayRef<utils::IteratorType> iterators) {
  llvm::SmallDenseSet<int64_t> a =
      findPermutationsIndexingOperand(indexingMaps[0], iterators, par);
  llvm::SmallDenseSet<int64_t> b =
      findPermutationsIndexingOperand(indexingMaps[1], iterators, par);
  llvm::SmallDenseSet<int64_t> c =
      findPermutationsIndexingOperand(indexingMaps[2], iterators, par);

  // A & C - B are the iterators involved in an outer-product along A (the LHS).
  llvm::SmallDenseSet<int64_t> ac = a;
  llvm::set_intersect(ac, c);
  llvm::set_subtract(ac, b);
  // B & C - A are the iterators involved in an outer-product along B (the RHS).
  llvm::SmallDenseSet<int64_t> bc = b;
  llvm::set_intersect(bc, c);
  llvm::set_subtract(bc, a);
  // A & B & C are the "batch" dimensions.
  llvm::SmallDenseSet<int64_t> batches = a;
  llvm::set_intersect(batches, b);
  llvm::set_intersect(batches, c);

  // A & B red are the reduction dimensions.
  llvm::SmallDenseSet<int64_t> ra =
      findPermutationsIndexingOperand(indexingMaps[0], iterators, red);
  llvm::SmallDenseSet<int64_t> rb =
      findPermutationsIndexingOperand(indexingMaps[1], iterators, red);
  llvm::set_intersect(ra, rb);

  // Return each set in sorted order.
  ContractionDimensions dimensions{
      SmallVector<unsigned, 2>(batches.begin(), batches.end()),
      SmallVector<unsigned, 2>(ac.begin(), ac.end()),
      SmallVector<unsigned, 2>(bc.begin(), bc.end()),
      SmallVector<unsigned, 2>(ra.begin(), ra.end())};
  llvm::sort(dimensions.batch);
  llvm::sort(dimensions.m);
  llvm::sort(dimensions.n);
  llvm::sort(dimensions.k);
  return dimensions;
}

FailureOr<ContractionDimensions>
mlir::linalg::inferContractionDims(LinalgOp linalgOp) {
  if (linalgOp.getNumDpsInits() != 1 || linalgOp.getNumDpsInputs() != 2)
    return failure();
  return inferContractionDimsImpl(linalgOp.getIndexingMapsArray(),
                                  linalgOp.getIteratorTypesArray());
}

FailureOr<ContractionDimensions>
mlir::linalg::inferContractionDims(ArrayRef<AffineMap> indexingMaps) {
  if (indexingMaps.size() != 3)
    return failure();
  auto iterators = inferIteratorsFromOutMap(indexingMaps[2]);
  if (failed(iterators))
    return failure();
  return inferContractionDimsImpl(indexingMaps, iterators.value());
}

namespace mlir::linalg::detail {
enum class MatchContractionResult {
  Success = 0,
  NotLinalgOp,
  WrongNumOperands,
  NoReduction,
  NotProjectedPermutations,
  NotAddMul
};
} // namespace mlir::linalg::detail

mlir::linalg::detail::MatchContractionResult
mlir::linalg::detail::isContractionInterfaceImpl(
    Operation *op, mlir::linalg::ContractionDimensions *dimensions) {
  auto linalgOp = dyn_cast<linalg::LinalgOp>(op);
  if (!linalgOp)
    return MatchContractionResult::NotLinalgOp;
  if (linalgOp.getNumDpsInputs() != 2 || linalgOp.getNumDpsInits() != 1)
    return MatchContractionResult::WrongNumOperands;
  auto mapRange = linalgOp.getIndexingMapsArray();
  if (linalgOp.getNumReductionLoops() == 0)
    return MatchContractionResult::NoReduction;
  if (llvm::any_of(mapRange,
                   [](AffineMap m) { return !m.isProjectedPermutation(); }))
    return MatchContractionResult::NotProjectedPermutations;
  // TODO: more fields than add/mul.
  // clang-format off
  if (!::isContractionBody<
        arith::MulFOp, arith::AddFOp,
        arith::MulIOp, arith::AddIOp,
        complex::MulOp, complex::AddOp,
        arith::AndIOp, arith::OrIOp>(
      *linalgOp.getBlock())) {
    return MatchContractionResult::NotAddMul;
  }
  // clang-format on

  if (dimensions) {
    FailureOr<ContractionDimensions> res = inferContractionDims(linalgOp);
    assert(succeeded(res) && "unexpected failure to infer contraction dims");
    *dimensions = *res;
  }
  return MatchContractionResult::Success;
}

StringRef
mlir::linalg::detail::getMatchContractionMessage(MatchContractionResult res) {
  switch (res) {
  case MatchContractionResult::NotLinalgOp:
    return "expected a LinalgOp";
  case MatchContractionResult::WrongNumOperands:
    return "expected op with 2 inputs and 1 output";
  case MatchContractionResult::NoReduction:
    return "expected at least 1 reduction";
  case MatchContractionResult::NotProjectedPermutations:
    return "expected indexing maps to be projected permutations";
  case MatchContractionResult::NotAddMul:
    return "expected add/mul op in the body";
  case MatchContractionResult::Success:
    return "";
  }
  llvm_unreachable("unhandled MatchContractionResult case");
}

bool mlir::linalg::isaContractionOpInterface(LinalgOp linalgOp) {
  if (!linalgOp)
    return false;
  Operation *op = linalgOp.getOperation();
  return isa<ContractionOpInterface>(op) ||
         (mlir::linalg::detail::isContractionInterfaceImpl(op) ==
          mlir::linalg::detail::MatchContractionResult::Success);
}

/// Verify that a LinalgOp `op` is a contraction.
/// A Linalg contraction is defined in general terms:
///   1. Has 2 input and 1 output shapes.
///   2. Has at least one reduction dimension.
///   3. Has only projected permutation indexing maps.
///   4. its body computes `u5(u1(c) + u2(u3(a) * u4(b)))` on some field
///   (AddOpType, MulOpType), where u1, u2, u3, u4 and u5 represent scalar unary
///   operations that may change the type (e.g. for mixed-precision).
/// As a consequence, when vectorization of such an op occurs, the only special
/// behavior is that the (unique) MulOpType is vectorized into a
/// `vector.contract`. All other ops are handled in a generic fashion.
/// In the future, we may wish to allow more input arguments and elementwise and
/// constant operations that do not involve the reduction dimension(s).
LogicalResult mlir::linalg::detail::verifyContractionInterface(Operation *op) {
  auto res = isContractionInterfaceImpl(op);
  if (res != MatchContractionResult::Success)
    return op->emitError(getMatchContractionMessage(res));
  return success();
}

//===----------------------------------------------------------------------===//
// ConvolutionOpInterface implementation
//===----------------------------------------------------------------------===//

/// Of the given two expressions returns one that is of type T (`lhs` gets
/// preference over `rhs`)
template <typename T>
static T getAffineExprOfType(AffineExpr lhs, AffineExpr rhs) {
  return isa<T>(lhs) ? cast<T>(lhs) : (isa<T>(rhs) ? cast<T>(rhs) : nullptr);
}

namespace {
/// Walk the indexing expressions for input of a convolution operation to verify
/// its of the right form, either
/// - AffineDimExpr
/// - AffineDimExpr (`*` (AffineSymbolExpr | AffineConstantExpr))?
///      (`+` AffineDimExpr (`*` (AffineSymbolExpr | AffineConstantExpr))?)*
///
/// classifies the AffineDimExpr as convolved dimensions or unconvolved
/// dimensions and verifies each dimension occurs only once.
struct ConvAccessExprWalker
    : public AffineExprVisitor<ConvAccessExprWalker, LogicalResult> {
  // Stores dimensions used in expressions of the above form.
  llvm::SmallDenseSet<int64_t> convolvedDims;
  // Stores the dual mapping between LHS and RHS of convolution exprs.
  llvm::SmallDenseMap<int64_t, int64_t> convolvedDimMapping;
  // Stores single use dimensions used by an AffineDimExpr.
  llvm::SmallDenseSet<int64_t> unConvolvedDims;
  // Stores a mapping from convolved dims to their coefficient.
  llvm::SmallDenseMap<int64_t, AffineExpr> strideAndDilationMapping;

  // Removes dims with multiple uses in the source input map from dimension
  // sets tracked by this walker.
  void clearMultiUseDims(AffineMap map) {
    for (int dimPos = 0, e = map.getNumDims(); dimPos < e; ++dimPos) {
      if (llvm::count_if(map.getResults(), [dimPos](AffineExpr e) {
            return e.isFunctionOfDim(dimPos);
          }) > 1) {
        convolvedDims.erase(dimPos);
        unConvolvedDims.erase(dimPos);
        // If a duplicate dim is marked as convolved, the pair of the duplicate
        // dim must be removed from the map as well.
        auto it = convolvedDimMapping.find(dimPos);
        if (it != convolvedDimMapping.end()) {
          int64_t pairedDim = it->second;
          convolvedDims.erase(pairedDim);
          unConvolvedDims.erase(pairedDim);
          strideAndDilationMapping.erase(pairedDim);
          convolvedDimMapping.erase(dimPos);
          convolvedDimMapping.erase(pairedDim);
        }
      }
    }
  }

  LogicalResult visitDimExpr(AffineDimExpr dimExpr) {
    unsigned position = dimExpr.getPosition();
    if (unConvolvedDims.count(position) || convolvedDims.count(position)) {
      return failure();
    }
    unConvolvedDims.insert(position);
    return success();
  }

  LogicalResult visitSymbolExpr(AffineSymbolExpr expr) { return failure(); }

  LogicalResult visitConstantExpr(AffineConstantExpr expr) { return failure(); }

  LogicalResult visitAffineBinaryOpExpr(AffineBinaryOpExpr binaryExpr) {
    // In pre-order visit, top level op has to be an add op.
    if (binaryExpr.getKind() != AffineExprKind::Add)
      return failure();
    auto lhsDimPos = getDimExprOrMulExprDimPos(binaryExpr.getLHS());
    auto rhsDimPos = getDimExprOrMulExprDimPos(binaryExpr.getRHS());
    if (failed(lhsDimPos) || failed(rhsDimPos))
      return failure();
    convolvedDimMapping[*lhsDimPos] = *rhsDimPos;
    convolvedDimMapping[*rhsDimPos] = *lhsDimPos;
    return success();
  }

  FailureOr<int64_t> getDimExprOrMulExprDimPos(AffineExpr expr) {
    if (auto dimExpr = dyn_cast<AffineDimExpr>(expr)) {
      int64_t dim = dimExpr.getPosition();
      if (convolvedDims.count(dim) || unConvolvedDims.count(dim))
        return failure();
      // Stride/dilation for this dim is implicitly 1.
      strideAndDilationMapping[dim] =
          getAffineConstantExpr(1, expr.getContext());
      convolvedDims.insert(dim);
      return dim;
    }
    if (auto symbolMulExpr = dyn_cast<AffineBinaryOpExpr>(expr)) {
      if (symbolMulExpr.getKind() != AffineExprKind::Mul)
        return failure();
      auto lhsExpr = symbolMulExpr.getLHS();
      auto rhsExpr = symbolMulExpr.getRHS();
      // Check for symbol expression.
      AffineExpr mulExpr =
          getAffineExprOfType<AffineSymbolExpr>(lhsExpr, rhsExpr);
      // If there was no symbol expr, check for constant expression.
      if (!mulExpr) {
        mulExpr = getAffineExprOfType<AffineConstantExpr>(lhsExpr, rhsExpr);
      }
      auto dimExpr = getAffineExprOfType<AffineDimExpr>(lhsExpr, rhsExpr);
      if (!mulExpr || !dimExpr)
        return failure();
      int64_t dim = dimExpr.getPosition();
      if (convolvedDims.count(dim) || unConvolvedDims.count(dim))
        return failure();
      strideAndDilationMapping[dim] = mulExpr;
      convolvedDims.insert(dim);
      return dim;
    }
    return failure();
  }
};
} // namespace

static llvm::SmallDenseSet<int64_t> getPreservedDims(AffineMap map) {
  assert(map.isProjectedPermutation() &&
         "expected map to have projected permutations");
  llvm::SmallDenseSet<int64_t> preservedDims;
  for (auto expr : map.getResults())
    preservedDims.insert(cast<AffineDimExpr>(expr).getPosition());
  return preservedDims;
}

static SmallVector<int64_t, 2>
getConstantsFromExprList(const SmallVector<AffineExpr, 2> &exprs) {
  SmallVector<int64_t, 2> vals;
  for (auto e : exprs) {
    auto constantExpr = dyn_cast<AffineConstantExpr>(e);
    assert(constantExpr && "Found non-constant stride/dilation");
    vals.push_back(constantExpr.getValue());
  }
  return vals;
}

/// Classifies dimensions in the `linalgOp` used by a convolution
/// subcomputation, as captured by `inputExprWalker`. If
/// `allowEmptyConvolvedDims` is not set this this will fail if there is not
/// at least convolved dimension pair (output image + filter loop). Convolution
/// dimensions are specified in sorted order, and strides match the order of
/// the filter loop dimensions, while the dilations match the order of the
/// output image dimensions.
static FailureOr<ConvolutionDimensions>
inferConvolutionDimsImpl(LinalgOp linalgOp,
                         ConvAccessExprWalker &inputExprWalker,
                         bool allowEmptyConvolvedDims) {
  auto filterMap =
      linalgOp.getMatchingIndexingMap(linalgOp.getDpsInputOperand(1));
  auto outputMap =
      linalgOp.getMatchingIndexingMap(linalgOp.getDpsInitOperand(0));
  llvm::SmallDenseSet<int64_t> filterDims = findPermutationsIndexingOperand(
      filterMap, linalgOp.getIteratorTypesArray(), par);
  llvm::SmallDenseSet<int64_t> outputDims = findPermutationsIndexingOperand(
      outputMap, linalgOp.getIteratorTypesArray(), par);

  // unConvolvedDims & outputDims - filterDims are the batch iterators.
  llvm::SmallDenseSet<int64_t> batch = inputExprWalker.unConvolvedDims;
  llvm::set_intersect(batch, outputDims);
  llvm::set_subtract(batch, filterDims);

  // convolvedDims & outputDims are the output image iterators.
  llvm::SmallDenseSet<int64_t> oi = inputExprWalker.convolvedDims;
  llvm::set_intersect(oi, outputDims);

  // filterDims & outputDims - unConvolvedDims are the output channel iterators.
  llvm::SmallDenseSet<int64_t> oc = filterDims;
  llvm::set_intersect(oc, outputDims);
  llvm::set_subtract(oc, inputExprWalker.unConvolvedDims);

  // filterDims & outputDims & unConvolvedDims are the depth iterators.
  llvm::SmallDenseSet<int64_t> depth = filterDims;
  llvm::set_intersect(depth, outputDims);
  llvm::set_intersect(depth, inputExprWalker.unConvolvedDims);

  llvm::SmallDenseSet<int64_t> filterReducedDims =
      findPermutationsIndexingOperand(filterMap,
                                      linalgOp.getIteratorTypesArray(), red);

  // convolvedDims & filterReducedDims are the filter loop iterators.
  llvm::SmallDenseSet<int64_t> fl = inputExprWalker.convolvedDims;
  llvm::set_intersect(fl, filterReducedDims);

  // unConvolvedDims & filterReducedDims are the input channel iterators.
  llvm::SmallDenseSet<int64_t> ic = inputExprWalker.unConvolvedDims;
  llvm::set_intersect(ic, filterReducedDims);

  if (oi.empty() && !allowEmptyConvolvedDims)
    return failure();

  // Return each set in sorted order.
  ConvolutionDimensions dimensions{
      SmallVector<unsigned, 2>(batch.begin(), batch.end()),
      SmallVector<unsigned, 2>(oi.begin(), oi.end()),
      SmallVector<unsigned, 2>(oc.begin(), oc.end()),
      SmallVector<unsigned, 2>(fl.begin(), fl.end()),
      SmallVector<unsigned, 2>(ic.begin(), ic.end()),
      SmallVector<unsigned, 2>(depth.begin(), depth.end()),
      /*strides=*/SmallVector<int64_t, 2>{},
      /*dilations=*/SmallVector<int64_t, 2>{}};
  llvm::sort(dimensions.batch);
  llvm::sort(dimensions.outputImage);
  llvm::sort(dimensions.outputChannel);
  llvm::sort(dimensions.filterLoop);
  llvm::sort(dimensions.inputChannel);
  llvm::sort(dimensions.depth);

  // Use the op carried strides/dilations attribute if present.
  auto nativeStrides = linalgOp->getAttrOfType<DenseIntElementsAttr>("strides");
  if (!nativeStrides) {
    SmallVector<AffineExpr, 2> strideExprs;
    for (unsigned oiDim : dimensions.outputImage)
      strideExprs.push_back(inputExprWalker.strideAndDilationMapping[oiDim]);
    dimensions.strides = getConstantsFromExprList(strideExprs);
  } else {
    dimensions.strides = llvm::to_vector<2>(nativeStrides.getValues<int64_t>());
  }
  auto nativeDilations =
      linalgOp->getAttrOfType<DenseIntElementsAttr>("dilations");
  if (!nativeDilations) {
    SmallVector<AffineExpr, 2> dilationExprs;
    for (unsigned flDim : dimensions.filterLoop)
      dilationExprs.push_back(inputExprWalker.strideAndDilationMapping[flDim]);
    dimensions.dilations = getConstantsFromExprList(dilationExprs);
  } else {
    dimensions.dilations =
        llvm::to_vector<2>(nativeDilations.getValues<int64_t>());
  }
  return dimensions;
}

/// Find at least 1 parallel (output_image) and reduction (filter_loop)
/// dimension candidates that form a convolution subcomputation within
/// `linalgOp`. The LHS is assumed to be the convolution input while the
/// RHS is assumed as the filter.
/// These dimensions are such that:
///   1. Optional batch dimensions that appear in the input and filter.
///   2. The output_image dimension is involved in a cross-correlation along LHS
///      (i.e. it is a permutation on RES and LHS and has an associated
///      filter_loop in RHS).
///   3. Optional output_channel dimension is involved in an outer-product along
///      RHS (i.e. it is a permutation on RES and RHS and does not appear in
///      LHS).
///   4. Optional input_channel dimension appears as a permutation on LHS and
///      RHS.
///   5. The filter_loop dimension appears as a permutation on the RHS and
///      represents the shape of the kernel cross-correlated along a
///      corresponding output_image dim.
///   6. The input_channel dimension appears as a permutation on LHS and RHS.
///   7. All dimensions appear only once in any given indexing map.
/// This allows e.g. detecting that some convolution is embedded within
/// `linalgOp` with some orthogonal heuristic.
/// When multiple dimension occurrences exist that match any classification
/// indices are returned in sorted order.
/// Returns a failure if `output_image` (and implicitly `filter_loop`) is empty.
FailureOr<ConvolutionDimensions>
mlir::linalg::inferConvolutionDims(LinalgOp linalgOp) {
  if (linalgOp.getNumDpsInits() != 1 || linalgOp.getNumDpsInputs() != 2)
    return failure();

  auto indexingMaps = linalgOp.getIndexingMapsArray();

  // Check the input indexing map has the right form.
  ConvAccessExprWalker inputExprWalker;
  for (AffineExpr expr : indexingMaps[0].getResults())
    (void)inputExprWalker.visit(expr);
  inputExprWalker.clearMultiUseDims(indexingMaps[0]);

  return inferConvolutionDimsImpl(linalgOp, inputExprWalker,
                                  /*allowEmptyConvolvedDims=*/false);
}

namespace mlir::linalg::detail {
enum class MatchConvolutionResult {
  Success = 0,
  NotLinalgOp,
  WrongNumOperands,
  WrongInputIndexingMap,
  NotProjectedPermutations,
  NonConvolutionLoop,
  OutputDimsNotParallel,
  NonOutputDimNotReduction,
  EmptyConvolvedDims
};
} // namespace mlir::linalg::detail

mlir::linalg::detail::MatchConvolutionResult
mlir::linalg::detail::isConvolutionInterfaceImpl(
    Operation *op, ConvolutionDimensions *dimensions,
    bool allowEmptyConvolvedDims) {
  auto linalgOp = dyn_cast<linalg::LinalgOp>(op);
  if (!linalgOp)
    return MatchConvolutionResult::NotLinalgOp;
  if (linalgOp.getNumDpsInputs() < 2 || linalgOp.getNumDpsInits() != 1)
    return MatchConvolutionResult::WrongNumOperands;

  auto indexingMaps = linalgOp.getIndexingMapsArray();

  // Check the input indexing map has the right form.
  ConvAccessExprWalker inputExprWalker;
  if (llvm::any_of(indexingMaps[0].getResults(),
                   [&inputExprWalker](AffineExpr expr) {
                     return failed(inputExprWalker.visit(expr));
                   })) {
    return MatchConvolutionResult::WrongInputIndexingMap;
  }

  // Filter and output maps must be projected permutation.
  if (!indexingMaps[1].isProjectedPermutation() ||
      !indexingMaps.back().isProjectedPermutation())
    return MatchConvolutionResult::NotProjectedPermutations;

  auto iteratorTypes = linalgOp.getIteratorTypesArray();

  llvm::SmallDenseSet<int64_t> outputDims =
      getPreservedDims(indexingMaps.back());
  llvm::SmallDenseSet<int64_t> filterDims = getPreservedDims(indexingMaps[1]);
  // Make sure all loops are characterized as one of:
  // - Batch loop : present in output, as non-convolved in input, not present in
  //   filter.
  // - Output image dimension : present in output, convolved dims in input, not
  //   present in filter.
  // - Output channel dimension : present in output, not present in input,
  //   present in filter.
  // - Filter loop dimension : present in filter, convolved in input, not
  //   present in output.
  // - Input channel dimension : unconvolved in input, not present in output,
  //   present in filter.
  // - Depth multiplier : unconvolved in input, present in output, present in
  //   filter.
  llvm::SmallDenseSet<int64_t> allLoopDims;
  for (auto outputExpr : indexingMaps.back().getResults()) {
    int64_t outputDim = cast<AffineDimExpr>(outputExpr).getPosition();
    if (inputExprWalker.unConvolvedDims.count(outputDim) &&
        !filterDims.count(outputDim)) {
      // Batch dimension.
      if (iteratorTypes[outputDim] != utils::IteratorType::parallel)
        return MatchConvolutionResult::OutputDimsNotParallel;
      allLoopDims.insert(outputDim);
      continue;
    }
    if (inputExprWalker.convolvedDims.count(outputDim) &&
        !filterDims.count(outputDim)) {
      // Output image Loop dimension.
      if (iteratorTypes[outputDim] != utils::IteratorType::parallel)
        return MatchConvolutionResult::OutputDimsNotParallel;
      allLoopDims.insert(outputDim);
      continue;
    }
    if (!inputExprWalker.convolvedDims.count(outputDim) &&
        !inputExprWalker.unConvolvedDims.count(outputDim) &&
        filterDims.count(outputDim)) {
      // Output channel dimension.
      if (iteratorTypes[outputDim] != utils::IteratorType::parallel)
        return MatchConvolutionResult::OutputDimsNotParallel;
      allLoopDims.insert(outputDim);
      continue;
    }
    if (inputExprWalker.unConvolvedDims.count(outputDim) &&
        filterDims.count(outputDim)) {
      // Depth multiplier.
      if (iteratorTypes[outputDim] != utils::IteratorType::parallel)
        return MatchConvolutionResult::OutputDimsNotParallel;
      allLoopDims.insert(outputDim);
      continue;
    }
    return MatchConvolutionResult::NonConvolutionLoop;
  }
  for (auto filterExpr : indexingMaps[1].getResults()) {
    int64_t filterDim = cast<AffineDimExpr>(filterExpr).getPosition();
    if (outputDims.count(filterDim) &&
        !inputExprWalker.unConvolvedDims.count(filterDim) &&
        !inputExprWalker.convolvedDims.count(filterDim)) {
      // Output channel dimension. This is already seen, continue;
      continue;
    }
    if (inputExprWalker.convolvedDims.count(filterDim) &&
        !outputDims.count(filterDim)) {
      // Filter loop dimension.
      if (iteratorTypes[filterDim] != utils::IteratorType::reduction)
        return MatchConvolutionResult::NonOutputDimNotReduction;
      if (allLoopDims.count(filterDim))
        return MatchConvolutionResult::NonConvolutionLoop;
      allLoopDims.insert(filterDim);
      continue;
    }
    if (inputExprWalker.unConvolvedDims.count(filterDim) &&
        !outputDims.count(filterDim)) {
      // Input channel dimension.
      if (iteratorTypes[filterDim] != utils::IteratorType::reduction)
        return MatchConvolutionResult::NonOutputDimNotReduction;
      if (allLoopDims.count(filterDim))
        return MatchConvolutionResult::NonConvolutionLoop;
      allLoopDims.insert(filterDim);
      continue;
    }
    if (inputExprWalker.unConvolvedDims.count(filterDim) &&
        outputDims.count(filterDim)) {
      // Depthwise loop. Already seen.
      continue;
    }
    return MatchConvolutionResult::NonConvolutionLoop;
  }
  // All loops must be covered now.
  if (allLoopDims.size() != linalgOp.getNumLoops())
    return MatchConvolutionResult::NonConvolutionLoop;

  if (!allowEmptyConvolvedDims && inputExprWalker.convolvedDims.empty())
    return MatchConvolutionResult::EmptyConvolvedDims;

  if (dimensions) {
    FailureOr<ConvolutionDimensions> res = inferConvolutionDimsImpl(
        linalgOp, inputExprWalker, allowEmptyConvolvedDims);
    assert(succeeded(res) && "unexpected failure to infer convolution dims");
    *dimensions = *res;
  }

  return MatchConvolutionResult::Success;
}

StringRef
mlir::linalg::detail::getMatchConvolutionMessage(MatchConvolutionResult res) {
  switch (res) {
  case MatchConvolutionResult::NotLinalgOp:
    return "expected a LinalgOp";
  case MatchConvolutionResult::WrongNumOperands:
    return "expected op with 2 inputs and 1 output";
  case MatchConvolutionResult::WrongInputIndexingMap:
    return "unexpected input index map for convolutions";
  case MatchConvolutionResult::NotProjectedPermutations:
    return "expected output/filter indexing maps to be projected permutations";
  case MatchConvolutionResult::NonConvolutionLoop:
    return "unexpected loop dimension for convolution op";
  case MatchConvolutionResult::OutputDimsNotParallel:
    return "expected all iterators used to access outputs to be parallel";
  case MatchConvolutionResult::NonOutputDimNotReduction:
    return "expected all iterators not used to access outputs to be reduction";
  case MatchConvolutionResult::EmptyConvolvedDims:
    return "expected convolved dim to be non-empty";
  case MatchConvolutionResult::Success:
    return "";
  }
  llvm_unreachable("unhandled MatchConvolutionResult case");
}

bool mlir::linalg::isaConvolutionOpInterface(LinalgOp linalgOp,
                                             bool allowEmptyConvolvedDims) {
  return linalg::detail::isConvolutionInterfaceImpl(
             linalgOp.getOperation(), nullptr, allowEmptyConvolvedDims) ==
         linalg::detail::MatchConvolutionResult::Success;
}

LogicalResult mlir::linalg::detail::verifyConvolutionInterface(Operation *op) {
  MatchConvolutionResult res = isConvolutionInterfaceImpl(op);
  if (res != MatchConvolutionResult::Success)
    return op->emitError(getMatchConvolutionMessage(res));
  return success();
}

//===----------------------------------------------------------------------===//
// FillOpInterface implementation
//===----------------------------------------------------------------------===//

enum class MatchFillResult {
  Success = 0,
  NotLinalgOp,
  WrongNumOperands,
  NotScalarInput
};

static MatchFillResult isFillInterfaceImpl(Operation *op) {
  auto linalgOp = dyn_cast<linalg::LinalgOp>(op);
  if (!linalgOp)
    return MatchFillResult::NotLinalgOp;
  if (linalgOp.getNumDpsInputs() != 1 || linalgOp.getNumDpsInits() != 1)
    return MatchFillResult::WrongNumOperands;

  OpOperand *value = linalgOp.getDpsInputOperand(0);
  if (!linalgOp.isScalar(value))
    return MatchFillResult::NotScalarInput;

  return MatchFillResult::Success;
}

LogicalResult mlir::linalg::detail::verifyFillInterface(Operation *op) {
  auto res = isFillInterfaceImpl(op);
  if (res == MatchFillResult::NotLinalgOp)
    return op->emitError("expected a LinalgOp");
  if (res == MatchFillResult::WrongNumOperands)
    return op->emitError("expected op with 1 input and 1 output");
  if (res == MatchFillResult::NotScalarInput)
    return op->emitError("expected op with scalar input");

  return success();
}

//===----------------------------------------------------------------------===//
// StructuredOpInterface implementation
//===----------------------------------------------------------------------===//

SmallVector<OpFoldResult> LinalgOp::createFlatListOfOperandDims(OpBuilder &b,
                                                                Location loc) {
  SmallVector<OpFoldResult> res;
  for (OpOperand &opOperand : getOperation()->getOpOperands()) {
    for (int64_t i = 0, e = getRank(&opOperand); i < e; ++i)
      res.push_back(createFoldedDimOp(b, loc, opOperand.get(), i));
  }
  return res;
}

SmallVector<int64_t, 4> LinalgOp::createFlatListOfOperandStaticDims() {
  SmallVector<int64_t, 4> res;
  assert(!hasDynamicShape() && "expected operands to have static shapes");
  for (OpOperand &opOperand : getOperation()->getOpOperands())
    llvm::append_range(res, getShape(&opOperand));
  return res;
}

SmallVector<Range, 4> LinalgOp::createLoopRanges(OpBuilder &b, Location loc) {
  AffineMap map = getLoopsToShapesMap();
  unsigned numDims = map.getNumDims(), numRes = map.getNumResults();
  auto viewSizes = createFlatListOfOperandDims(b, loc);
  SmallVector<Range, 4> res(numDims);
  for (unsigned idx = 0; idx < numRes; ++idx) {
    auto result = map.getResult(idx);
    if (auto d = dyn_cast<AffineDimExpr>(result)) {
      if (res[d.getPosition()].offset)
        continue;
      res[d.getPosition()] =
          Range{b.getIndexAttr(0), viewSizes[idx], b.getIndexAttr(1)};
    }
  }
  return res;
}

/// Visitor to check if any of the given set of positions from AffineDimExprs
/// are used within an AffineExpr.
struct HasAffineDimExprVisitor
    : public AffineExprVisitor<HasAffineDimExprVisitor, bool> {
  HasAffineDimExprVisitor(llvm::SmallBitVector positions)
      : positions(std::move(positions)) {}

  bool visitAffineBinaryOpExpr(AffineBinaryOpExpr binaryOpExpr) {
    return visit(binaryOpExpr.getLHS()) || visit(binaryOpExpr.getRHS());
  }

  bool visitDimExpr(AffineDimExpr dimExpr) {
    return positions.test(dimExpr.getPosition());
  }

  bool visitConstantExpr(AffineConstantExpr constExpr) { return false; }

  bool visitSymbolExpr(AffineSymbolExpr symbolExpr) { return false; }

private:
  llvm::SmallBitVector positions;
};

static std::pair<int64_t, int64_t>
getResultsPositionInLoopsToShapeMap(LinalgOp &op) {
  int64_t inputRankSum = 0;
  int64_t outputRankSum = 0;
  for (OpOperand *input : op.getDpsInputOperands())
    inputRankSum += op.getRank(input);
  for (OpOperand &output : op.getDpsInitsMutable())
    outputRankSum += op.getRank(&output);
  return {inputRankSum, inputRankSum + outputRankSum};
}

LogicalResult
LinalgOp::reifyResultShapes(OpBuilder &b,
                            ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  // An example that helps understand the logic below.
  // Consider the following expression O(i+j, j) += A(i,k) * B(k, j)
  // We want to express the shape of dim 0 of O in terms of shape of the inputs.
  // This is achieved as follows.
  //   loopsToShapesMap = (d0, d1, d2) -> (d0, d2, d2, d1, d0 + d1, d1)
  //   subMapOfResultShapes = (d0, d1, d2) -> (d0 + d1, d1)
  //   shapesToLoopsMap = (d0, d2, d2, d3, d4, d5) -> (d0, d3, d2)
  //   resultShapesFromInputShapes = subMapOfResultDim.compose(shapesToLoopMap)
  //     = (d0, d1, d2, d3, d4, d5) -> (d0 + d1, d1)
  AffineMap loopsToShapesMap = getLoopsToShapesMap();

  // Find the position in the above map that represents the shape of the
  // result:dim being inferred.
  auto resultShapesSubMapPos = getResultsPositionInLoopsToShapeMap(*this);

  /// From loopsToShapesMap extract the submap that represents the shape of the
  /// (resultIdx, dim) needed.
  AffineMap loopToResultsShapeMap = loopsToShapesMap.getSliceMap(
      resultShapesSubMapPos.first,
      resultShapesSubMapPos.second - resultShapesSubMapPos.first);
  AffineMap resultShapesFromInputShapesMap =
      loopToResultsShapeMap.compose(getShapesToLoopsMap());

  // Check that the result dim map does not contain the positions corresponding
  // to the outputs.
  llvm::SmallBitVector outputDims(resultShapesFromInputShapesMap.getNumDims());
  outputDims.set(resultShapesSubMapPos.first, resultShapesSubMapPos.second);
  HasAffineDimExprVisitor checkDimExpr(std::move(outputDims));
  Location loc = getOperation()->getLoc();
  IRRewriter rewriter(b);
  SmallVector<OpFoldResult> allResultDimValues =
      affine::makeComposedFoldedMultiResultAffineApply(
          rewriter, loc, resultShapesFromInputShapesMap,
          createFlatListOfOperandDims(b, loc));
  int64_t pos = 0;
  ArrayRef<AffineExpr> shapeExprs = resultShapesFromInputShapesMap.getResults();
  for (OpOperand &opOperand : getDpsInitsMutable()) {
    SmallVector<OpFoldResult> shapes;
    for (int64_t dim : llvm::seq<int64_t>(0, getRank(&opOperand))) {
      auto shapedType = llvm::cast<ShapedType>(opOperand.get().getType());
      if (!shapedType.isDynamicDim(dim)) {
        // Static dim: Return IntegerAttr.
        shapes.push_back(b.getIndexAttr(shapedType.getDimSize(dim)));
      } else {
        // Dynamic dim: Return Value.
        OpFoldResult ofr = checkDimExpr.visit(shapeExprs[pos])
                               ? createOrFoldDimOp(b, loc, opOperand.get(), dim)
                               : allResultDimValues[pos];
        shapes.push_back(getValueOrCreateConstantIndexOp(b, loc, ofr));
      }
      pos++;
    }
    reifiedReturnShapes.emplace_back(std::move(shapes));
  }
  return success();
}

/// Return the index in the indexingMaps vector that corresponds to this
/// `opOperand`.
int64_t LinalgOp::getIndexingMapIndex(OpOperand *opOperand) {
  auto operandNumber = opOperand->getOperandNumber();
  auto dpsIface = cast<DestinationStyleOpInterface>(*this->getOperation());
  if (!dpsIface.isDpsInput(opOperand))
    return operandNumber;
  unsigned start = dpsIface.getDpsInits().getBeginOperandIndex();
  assert(!dpsIface.isDpsInit(opOperand));
  // Account for potential inputs that are not DPS and may not appear in
  // `indexingMaps`.
  return cast<DestinationStyleOpInterface>(*this->getOperation())
             .getNumDpsInputs() +
         operandNumber - start;
}

LogicalResult mlir::linalg::detail::verifyStructuredOpInterface(Operation *op) {
  LinalgOp linalgOp = cast<LinalgOp>(op);
  // Mixed tensor/buffer operands are not allowed.
  if (!linalgOp.hasPureTensorSemantics() &&
      !linalgOp.hasPureBufferSemantics() && op->getNumOperands() > 0)
    return op->emitOpError("expected to have pure tensor or buffer semantics");

  // Before checking indexing maps, we need to make sure the attributes
  // referenced by it are valid.
  if (linalgOp.hasDynamicIndexingMaps())
    if (failed(linalgOp.verifyIndexingMapRequiredAttributes()))
      return failure();

  // Delayed calling of IndexingMapOpInterface::verifyImpl.
  if (failed(cast<IndexingMapOpInterface>(op).verifyImpl()))
    return failure();

  // Set this flag if this op has user defined maps. This is required to guard
  // the below error condition which assume default indexing maps.
  for (OpOperand &opOperand : linalgOp->getOpOperands()) {
    AffineMap indexingMap = linalgOp.getMatchingIndexingMap(&opOperand);
    // Domain must be consistent.
    unsigned numLoops = linalgOp.getNumLoops();
    if (indexingMap.getNumDims() != numLoops)
      return op->emitOpError("expected indexing_map #")
             << opOperand.getOperandNumber() << " to have " << numLoops
             << " dim(s) to match the number of loops";
  }
  SmallVector<unsigned> redDims;
  linalgOp.getReductionDims(redDims);

  if (!linalgOp.getShapesToLoopsMap())
    return op->emitOpError("expected the shape-to-loops map to be non-null");

  // Check the region has exactly one block.
  if (linalgOp->getNumRegions() != 1 || !linalgOp->getRegion(0).hasOneBlock())
    return op->emitOpError("expects to have 1 region with 1 block");

  // Simplifying assumption: bbargs match 1-1 with shape operands elemental
  // types.
  // TODO: once ranked shape types are plugged in, we may want to drop the
  // corresponding bbargs, that can never be read from. This will be subject to
  // consistency discussions (i.e. what to do with output tensors whose bbarg is
  // not used).
  Block &block = linalgOp->getRegion(0).front();

  if (linalgOp.getOpOperandsMatchingBBargs().size() != block.getNumArguments())
    return op->emitOpError("expected as many non-induction variable region "
                           "arguments as the number of input/output operands");

  for (OpOperand *opOperand : linalgOp.getOpOperandsMatchingBBargs()) {
    Type elementType = opOperand->get().getType();
    if (isa<MemRefType, RankedTensorType>(elementType))
      elementType = getElementTypeOrSelf(opOperand->get().getType());
    Type argType = block.getArgument(opOperand->getOperandNumber()).getType();
    if (elementType != argType)
      return op->emitOpError("expected type of bb argument #")
             << opOperand->getOperandNumber() << " (" << argType << ")"
             << " to match element or self type of the corresponding operand ("
             << elementType << ")";
  }

  return success();
}
