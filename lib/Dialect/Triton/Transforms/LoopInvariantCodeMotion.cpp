#include "mlir/Transforms/LoopInvariantCodeMotionUtils.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/Utility.h"
#include "triton/Dialect/Triton/Transforms/Passes.h"
#include "llvm/Support/Debug.h"

namespace mlir::triton {

#define GEN_PASS_DEF_TRITONLOOPINVARIANTCODEMOTION
#include "triton/Dialect/Triton/Transforms/Passes.h.inc"

#define DEBUG_TYPE "triton-licm"
#define DBGS() (llvm::dbgs() << "[" DEBUG_TYPE "]: ")
#define LDBG(X) LLVM_DEBUG(DBGS() << X << "\n")

class LoopInvariantCodeMotionPass
    : public impl::TritonLoopInvariantCodeMotionBase<
          LoopInvariantCodeMotionPass> {

  DenseMap<LoopLikeOpInterface, bool> isLoopMemoryEffectFreeOrOnlyRead;

  bool isMemoryEffectFreeOrOnlyRead(Operation *op) {
    std::optional<SmallVector<MemoryEffects::EffectInstance>> effects =
        getEffectsRecursively(op);
    if (!effects)
      return false;
    return llvm::all_of(*effects,
                        [&](const MemoryEffects::EffectInstance &effect) {
                          return isa<MemoryEffects::Read>(effect.getEffect());
                        });
  }

  void runOnOperation() override {
    // Walk through all loops in a function in innermost-loop-first order.
    // This way, we first LICM from the inner loop, and place the ops in the
    // outer loop, which in turn can be further LICM'ed.
    getOperation()->walk([&](LoopLikeOpInterface loopLike) {
      moveLoopInvariantCode(
          loopLike.getLoopRegions(),
          // isDefinedOutsideOfRegion
          [&](Value value, Region *region) {
            return loopLike.isDefinedOutsideOfLoop(value);
          },
          // shouldMoveOutOfRegion
          [&](Operation *op, Region *region) {
            if (!isa<LoadOp>(op))
              return isSpeculatable(op) && isMemoryEffectFree(op);
            if (!isLoopMemoryEffectFreeOrOnlyRead.contains(loopLike))
              isLoopMemoryEffectFreeOrOnlyRead[loopLike] =
                  isMemoryEffectFreeOrOnlyRead(loopLike);
            return isMemoryEffectFreeOrOnlyRead(op) &&
                   isLoopMemoryEffectFreeOrOnlyRead[loopLike];
          },
          // moveOutOfRegion
          [&](Operation *op, Region *) {
            // Create the new mask for load op.
            if (auto loadOp = dyn_cast<LoadOp>(op)) {
              Value mask = loadOp.getMask();
              IRRewriter rewriter(loopLike);
              Location loc = loopLike->getLoc();
              Value cond;
              if (auto forOp = dyn_cast<scf::ForOp>(loopLike.getOperation())) {
                cond = rewriter.create<arith::CmpIOp>(
                    loc, arith::CmpIPredicate::slt, forOp.getLowerBound(),
                    forOp.getUpperBound());
              } else if (auto whileOp =
                             dyn_cast<scf::WhileOp>(loopLike.getOperation())) {
                // TODO: Support Load Op hoisting for while loop.
                return;
              } else {
                return;
              }
              Value newMask = getPredMask(rewriter, loadOp.getPtr().getType(),
                                          loadOp.getMask(), cond);
              loadOp.getMaskMutable().assign(newMask);
            }
            loopLike.moveOutOfLoop(op);
          });
    });
  }
};

} // namespace mlir::triton
