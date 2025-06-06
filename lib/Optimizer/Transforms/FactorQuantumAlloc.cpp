/*******************************************************************************
 * Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "PassDetails.h"
#include "cudaq/Optimizer/Builder/Factory.h"
#include "cudaq/Optimizer/Dialect/Quake/QuakeOps.h"
#include "cudaq/Optimizer/Transforms/Passes.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"

namespace cudaq::opt {
#define GEN_PASS_DEF_FACTORQUANTUMALLOCATIONS
#include "cudaq/Optimizer/Transforms/Passes.h.inc"
} // namespace cudaq::opt

#define DEBUG_TYPE "factor-quantum-alloc"

using namespace mlir;

namespace {
class AllocaPat : public OpRewritePattern<quake::AllocaOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  /// If we are here, then all uses of \p allocOp are either an ExtractRefOp
  /// with a constant index or a DeallocOp. Any other user is assumed to block
  /// the factoring of the allocation.
  LogicalResult matchAndRewrite(quake::AllocaOp allocOp,
                                PatternRewriter &rewriter) const override {
    auto veqTy = cast<quake::VeqType>(allocOp.getType());
    auto loc = allocOp.getLoc();
    std::size_t size = veqTy.getSize();
    SmallVector<quake::AllocaOp> newAllocs;
    auto *ctx = rewriter.getContext();
    auto refTy = quake::RefType::get(ctx);

    // 1. Split the aggregate veq into a sequence of distinct alloca of ref.
    for (std::size_t i = 0; i < size; ++i)
      newAllocs.emplace_back(rewriter.create<quake::AllocaOp>(loc, refTy));

    std::function<LogicalResult(Operation *, std::int64_t)> rewriteOpAndUsers =
        [&](Operation *op, std::int64_t start) -> LogicalResult {
      // First handle the users. Note that this can recurse.
      for (auto *user : op->getUsers()) {
        if (auto dealloc = dyn_cast<quake::DeallocOp>(user)) {
          rewriter.setInsertionPoint(dealloc);
          auto deloc = dealloc.getLoc();
          for (std::size_t i = 0; i < size - 1; ++i)
            rewriter.create<quake::DeallocOp>(deloc, newAllocs[i]);
          rewriter.replaceOpWithNewOp<quake::DeallocOp>(dealloc,
                                                        newAllocs[size - 1]);
          continue;
        }
        if (auto subveq = dyn_cast<quake::SubVeqOp>(user)) {
          auto lowInt = [&]() -> std::optional<std::int32_t> {
            if (subveq.hasConstantLowerBound())
              return {subveq.getConstantLowerBound()};
            return cudaq::opt::factory::getIntIfConstant(subveq.getLower());
          }();
          if (!lowInt)
            return failure();
          for (auto *subUser : subveq->getUsers())
            if (failed(rewriteOpAndUsers(subUser, *lowInt)))
              return failure();
          rewriter.eraseOp(subveq);
          continue;
        }
        if (auto ext = dyn_cast<quake::ExtractRefOp>(user)) {
          auto index = ext.getConstantIndex();
          rewriter.replaceOp(ext, newAllocs[start + index].getResult());
        }
      }
      // Now handle the base operation.
      if (isa<quake::SubVeqOp>(op))
        rewriter.eraseOp(op);
      else if (auto ext = dyn_cast<quake::ExtractRefOp>(op)) {
        auto index = ext.getConstantIndex();
        rewriter.replaceOp(ext, newAllocs[start + index].getResult());
      }
      return success();
    };

    // 2. Visit all users and replace them accordingly.
    if (failed(rewriteOpAndUsers(allocOp, 0)))
      return failure();

    // 3. Remove the original alloca operation.
    rewriter.eraseOp(allocOp);

    return success();
  }
};

class DeallocPat : public OpRewritePattern<quake::DeallocOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(quake::DeallocOp dealloc,
                                PatternRewriter &rewriter) const override {
    auto veq = dealloc.getReference();
    auto veqTy = cast<quake::VeqType>(veq.getType());
    auto loc = dealloc.getLoc();
    assert(veqTy.hasSpecifiedSize());
    std::size_t size = veqTy.getSize();

    // 1. Split the aggregate veq into a sequence of distinct dealloc of ref.
    for (std::size_t i = 0; i < size; ++i) {
      Value r = rewriter.create<quake::ExtractRefOp>(loc, veq, i);
      rewriter.create<quake::DeallocOp>(loc, r);
    }

    // 2. Remove the original dealloc operation.
    rewriter.eraseOp(dealloc);
    return success();
  }
};

class FactorQuantumAllocationsPass
    : public cudaq::opt::impl::FactorQuantumAllocationsBase<
          FactorQuantumAllocationsPass> {
public:
  using FactorQuantumAllocationsBase::FactorQuantumAllocationsBase;

  void runOnOperation() override {
    func::FuncOp func = getOperation();
    LLVM_DEBUG(llvm::dbgs() << "Function before factoring quake alloca:\n"
                            << func << "\n\n");

    // 1) Factor (expand) any deallocations that are veqs of constant size.
    if (failed(factorDeallocations()))
      return;

    // 2) Run an analysis to find the allocations to factor (expand).
    SmallVector<quake::AllocaOp> allocations;
    if (failed(runAnalysis(allocations)))
      return;

    // 3) Factor (expand) any allocations that are veqs of constant size.
    factorAllocations(allocations);
  }

  LogicalResult factorDeallocations() {
    auto *ctx = &getContext();
    func::FuncOp func = getOperation();
    RewritePatternSet patterns(ctx);
    patterns.insert<DeallocPat>(ctx);
    ConversionTarget target(*ctx);
    target.addLegalDialect<quake::QuakeDialect>();
    target.addDynamicallyLegalOp<quake::DeallocOp>([](quake::DeallocOp d) {
      if (d.getReference().getDefiningOp<quake::InitializeStateOp>())
        return true;
      if (auto ty = dyn_cast<quake::VeqType>(d.getReference().getType()))
        return !ty.hasSpecifiedSize();
      return true;
    });
    if (failed(applyPartialConversion(func.getOperation(), target,
                                      std::move(patterns)))) {
      signalPassFailure();
      return failure();
    }
    return success();
  }

  void factorAllocations(const SmallVector<quake::AllocaOp> &allocations) {
    auto *ctx = &getContext();
    func::FuncOp func = getOperation();
    RewritePatternSet patterns(ctx);
    patterns.insert<AllocaPat>(ctx);
    ConversionTarget target(*ctx);
    target.addLegalDialect<quake::QuakeDialect>();
    target.addDynamicallyLegalOp<quake::AllocaOp>([&](quake::AllocaOp alloc) {
      return std::find(allocations.begin(), allocations.end(), alloc) ==
             allocations.end();
    });
    target.addDynamicallyLegalOp<quake::DeallocOp>([](quake::DeallocOp d) {
      if (d.getReference().getDefiningOp<quake::InitializeStateOp>())
        return true;
      if (auto ty = dyn_cast<quake::VeqType>(d.getReference().getType()))
        return !ty.hasSpecifiedSize();
      return true;
    });
    if (failed(applyPartialConversion(func.getOperation(), target,
                                      std::move(patterns)))) {
      func.emitOpError("factoring quantum allocations failed");
      signalPassFailure();
    }

    LLVM_DEBUG(llvm::dbgs() << "Function after factoring quake alloca:\n"
                            << func << "\n\n");
  }

  LogicalResult runAnalysis(SmallVector<quake::AllocaOp> &allocations) {
    auto func = getOperation();
    std::function<bool(Operation *)> isUseConvertible =
        [&](Operation *op) -> bool {
      if (isa<quake::DeallocOp>(op))
        return true;
      if (auto ext = dyn_cast<quake::ExtractRefOp>(op))
        if (ext.hasConstantIndex())
          return true;
      if (auto sub = dyn_cast<quake::SubVeqOp>(op)) {
        auto lowInt = [&]() -> std::optional<std::int32_t> {
          if (sub.hasConstantLowerBound())
            return {sub.getConstantLowerBound()};
          return cudaq::opt::factory::getIntIfConstant(sub.getLower());
        }();
        auto upInt = [&]() -> std::optional<std::int32_t> {
          if (sub.hasConstantUpperBound())
            return {sub.getConstantUpperBound()};
          return cudaq::opt::factory::getIntIfConstant(sub.getUpper());
        }();
        if (!(lowInt && upInt))
          return false;
        for (auto *subUser : sub->getUsers())
          if (!isUseConvertible(subUser))
            return false;
        return true;
      }
      return false;
    };
    func.walk([&](quake::AllocaOp alloc) {
      if (!allocaOfVeq(alloc) || allocaOfUnspecifiedSize(alloc) ||
          alloc.hasInitializedState())
        return;
      bool usesAreConvertible = [&]() {
        for (auto *users : alloc->getUsers()) {
          if (isUseConvertible(users))
            continue;
          return false;
        }
        return true;
      }();
      if (usesAreConvertible)
        allocations.push_back(alloc);
    });
    if (allocations.empty())
      return failure();
    return success();
  }

  static bool allocaOfVeq(quake::AllocaOp alloc) {
    return isa<quake::VeqType>(alloc.getType());
  }

  static bool allocaOfUnspecifiedSize(quake::AllocaOp alloc) {
    if (auto veqTy = dyn_cast<quake::VeqType>(alloc.getType()))
      return !veqTy.hasSpecifiedSize();
    return false;
  }
};
} // namespace
