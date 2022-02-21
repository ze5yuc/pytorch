#include <torch/csrc/jit/codegen/cuda/lower2device.h>

#include <ATen/cuda/CUDAContext.h>
#include <torch/csrc/jit/codegen/cuda/expr_evaluator.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/instrumentation.h>
#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/lower_alias_memory.h>
#include <torch/csrc/jit/codegen/cuda/lower_allocation.h>
#include <torch/csrc/jit/codegen/cuda/lower_double_buffer.h>
#include <torch/csrc/jit/codegen/cuda/lower_expr_sort.h>
#include <torch/csrc/jit/codegen/cuda/lower_fusion_simplifier.h>
#include <torch/csrc/jit/codegen/cuda/lower_index.h>
#include <torch/csrc/jit/codegen/cuda/lower_insert_syncs.h>
#include <torch/csrc/jit/codegen/cuda/lower_loops.h>
#include <torch/csrc/jit/codegen/cuda/lower_magic_zero.h>
#include <torch/csrc/jit/codegen/cuda/lower_misaligned_vectorization.h>
#include <torch/csrc/jit/codegen/cuda/lower_predicate.h>
#include <torch/csrc/jit/codegen/cuda/lower_replace_size.h>
#include <torch/csrc/jit/codegen/cuda/lower_shift.h>
#include <torch/csrc/jit/codegen/cuda/lower_trivial_reductions.h>
#include <torch/csrc/jit/codegen/cuda/lower_unroll.h>
#include <torch/csrc/jit/codegen/cuda/lower_utils.h>
#include <torch/csrc/jit/codegen/cuda/lower_validation.h>
#include <torch/csrc/jit/codegen/cuda/lower_warp_reduce.h>

#include <list>
#include <unordered_map>
#include <unordered_set>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

thread_local GpuLower* active_gpu_lower = nullptr; // NOLINT
namespace {

class KIRCleaner : public OptOutDispatch {
 public:
  //! Remove nop IR nodes
  static std::vector<Expr*> cleanUp(const std::vector<Expr*>& loop_nests) {
    KIRCleaner cleaner;
    std::vector<Expr*> out_loop_nests;
    for (auto loop_nest : loop_nests) {
      cleaner.handle(loop_nest);
      // No need to keep the loop nest if it's determined to be nop
      if (!cleaner.is_nop_) {
        out_loop_nests.push_back(loop_nest);
      }
    }
    return out_loop_nests;
  }

 private:
  using OptOutDispatch::handle;
  void handle(Expr* expr) final {
    if (expr->isA<kir::ForLoop>() || expr->isA<kir::IfThenElse>()) {
      OptOutDispatch::handle(expr);
    } else {
      // Any non-scoping expr is not considered nop
      is_nop_ = false;
    }
  }

  void handle(kir::ForLoop* fl) final {
    auto exprs = fl->body().exprs();
    fl->body().clear();
    for (auto expr : exprs) {
      handle(expr);
      // Add the expr to the loop body only when the expr is not nop
      if (!is_nop_) {
        fl->body().push_back(expr);
      }
    }
    // The loop is nop when no expr exists in the body
    is_nop_ = fl->body().empty();
  }

  void handle(kir::IfThenElse* ite) final {
    const auto conditional = ite->predicate()->value();

    // Visit the then block
    auto then_exprs = ite->thenBody().exprs();
    ite->thenBody().clear();
    if (!conditional->isConst() || conditional->value().value()) {
      for (auto expr : then_exprs) {
        handle(expr);
        if (!is_nop_) {
          ite->thenBody().push_back(expr);
        }
      }
    }

    const bool then_nop = ite->thenBody().empty();

    // Visit the else block
    auto else_exprs = ite->elseBody().exprs();
    ite->elseBody().clear();
    if (!conditional->isConst() || !conditional->value().value()) {
      for (auto expr : else_exprs) {
        handle(expr);
        if (!is_nop_) {
          ite->elseBody().push_back(expr);
        }
      }
    }

    const bool else_nop = ite->elseBody().empty();

    // If the then block is nop but the else is not, invert the
    // conditional and move the exprs in the else block to the then
    // block.
    if (then_nop && !else_nop) {
      Bool* pred = ite->predicate()->value();
      Bool* not_pred = SimplifyingIrBuilder::notExpr(pred)->as<Bool>();
      ite->predicate()->setValue(not_pred);
      for (auto expr : ite->elseBody().exprs()) {
        ite->thenBody().push_back(expr);
      }
      ite->elseBody().clear();
    }

    // This IfThenElse is nop if both the then and else blocks are nop
    is_nop_ = then_nop && else_nop;
  }

 private:
  //! True if the last visited expr is nop
  bool is_nop_ = false;
};

} // namespace

void GpuLower::collectPaddedParallelDims() {
  ExpressionEvaluator ee(fusion_);
  bool can_be_single_warp = true;

  auto warp_size = at::cuda::warp_size();

  auto used_vals = fusion_->usedMathVals();
  for (auto tv : ir_utils::filterByType<TensorView>(used_vals)) {
    for (auto id : tv->domain()->domain()) {
      if (tv->definition()) {
        if (auto reduction = dynamic_cast<ReductionOp*>(tv->definition())) {
          if (ir_utils::getMaybeWarpReductionDim(reduction).has_value()) {
            warp_pad_info_.has_warp_reduction = true;
          }
        }
      }

      // Check ifi TIDx is padded in this kernel
      if (id->hasPaddingToMultipleOfWarp()) {
        TORCH_INTERNAL_ASSERT(
            id->getParallelType() == ParallelType::TIDx,
            "Padded types supported only on TIDx");
        warp_pad_info_.is_tidx_padded = true;
      }

      // Check all possible bindings of TIDx to see
      //  if TIDx will eventually be bound to a single warp.
      if (id->getParallelType() == ParallelType::TIDx) {
        auto eval_dim = ee.evaluate(id->extent());
        auto size_after_padding = id->getMaybeSizeAfterPadding();
        bool padding_to_single_warp = size_after_padding.has_value() &&
            size_after_padding.value() == warp_size;

        if ((!eval_dim.has_value() || eval_dim.value() > warp_size) &&
            !padding_to_single_warp) {
          // If we see any other TIDx binding that's larger than
          //  a warp or unknown, we shouldn't lower warp reduce
          //  to a single warp type.
          can_be_single_warp = false;
          warp_pad_info_.is_tidx_single_warp = false;
        } else if (can_be_single_warp) {
          if (padding_to_single_warp ||
              (eval_dim.has_value() && eval_dim.value() == warp_size)) {
            warp_pad_info_.is_tidx_single_warp = true;
          }
        }
      }
    }
  }
}

void GpuLower::lower(Fusion* fusion) {
  FUSER_PERF_SCOPE("GpuLower::lower");
  TORCH_INTERNAL_ASSERT(fusion != nullptr);
  TORCH_INTERNAL_ASSERT(
      active_gpu_lower == nullptr, "Nested lowering passes are not supported");

  struct LowerGuard {
    LowerGuard(GpuLower* gpu_lower) {
      active_gpu_lower = gpu_lower;
    }
    ~LowerGuard() {
      active_gpu_lower = nullptr;
    }
  } lower_guard(this);
  // Copy fusion into a new kernel for processing
  kernel_ = std::make_unique<kir::Kernel>(fusion);
  // Alias the fusion kernel caries around as a view of itself.
  fusion_ = kernel_.get();

  FusionGuard fg(fusion_);
  // prepare for lowering
  validateIr(fusion_);

  collectPaddedParallelDims();

  replaceSymbolicSizes(fusion_);

  trivial_reduction_info_.build(fusion_);
  trivialReductionReplacement(fusion_, trivialReductionInfo());

  // In the future we may directly use this map, but for now it will propagate
  // and validate (to some extent) the parallelization strategy.
  // This is the first time nodes will be lowered to kir nodes. Since for now we
  // propagate the parallel strategy in some instances, we need to do it before
  // lowering.
  ca_parallel_map_ = ComputeAtMap(ComputeAtMap::MappingMode::PARALLEL);
  ca_parallel_map_.build(fusion_, current());

  // Want to run this after parallel map is created
  validateVectorize(fusion_);

  // Generate mappings to generate indices
  ca_index_map_ = ComputeAtMap(ComputeAtMap::MappingMode::INDEX);
  ca_index_map_.build(fusion_, current());

  // Generate mappings to generate and map to loop nests
  ca_loop_map_ = ComputeAtMap(ComputeAtMap::MappingMode::LOOP);
  ca_loop_map_.build(fusion_, current());

  parallelDimensionMap().build(fusion_);
  if (isDebugDumpEnabled(DebugDumpOption::ParallelDimensions)) {
    std::cout << "Parallel dimension map:" << std::endl;
    std::cout << parallel_dimension_map_.toString() << std::endl;
  }

  concretized_broadcast_domains_.build(fusion_);

  // Compute thread predicates. Depends on parallel_dimension_map_
  thread_pred_map_.build(fusion_);

  // Depends on thread_pred_map_
  validateParallelize(fusion_);

  // Scan the whole fusion and build mappings about halo extensions of
  // all IterDomains
  haloInfo().build(fusion_);

  partialSplitMap().build(fusion_);

  validatePartialSplit(fusion_);

  // Detects all exprssions that don't need predicates
  predicateElimination().build(fusion_);

  nonDivisibleSplitInfo().build(fusion_);

  doubleBufferInfo().build(fusion_);

  // Run our passes keeping the lowered expressions and forwarding
  // them

  // Reorder expressions for loop-nest generation respecting computeAt
  // relationships
  const auto exprs_sorted = reorderExprsForComputeAt();

  // Generate loop-nests and place each expression at its
  // corresponding loop
  const auto exprs_lowered = LoopNestGenerator::loweredExprs(exprs_sorted);

  // Replace trivial reductions, Transpose, Shift, Gather, and View ops with
  // unary ops since they're not separately processed in lowering.
  const auto exprs_unary_replaced = unarySetOpInserter(exprs_lowered);

  // Insert allocations
  const auto exprs_alloced = insertAllocations(exprs_unary_replaced);

  // Insert read after write smem syncs
  const auto exprs_raw_sync = insertRawThreadSynchronization(exprs_alloced);

  // Reuse memory locations
  const auto exprs_reuse_mem = reuseMemoryAllocations(exprs_raw_sync);

  // Insert SyncThreads at end of for-loop to avoid WAR race condition
  const auto exprs_war_sync = insertWarThreadSynchronization(exprs_reuse_mem);

  const auto exprs_double_buffered = DoubleBufferPass::run(exprs_war_sync);

  // This pass inserts predicates as well as branches in the code. Up until now
  // the code is explicitly single shot for loop based. Need to be careful in
  // later passes when doing any kind of insertions in loop nest structure as
  // insertions could be on if then or else instead of directly on a for loop.
  const auto exprs_unrolled_loops =
      UnrollPass::runPass(fusion_, exprs_double_buffered);

  const auto exprs_unrolled_mv_loops =
      processMisalignedVectorization(exprs_unrolled_loops);

  const auto exprs_indexed_loops =
      IndexLowering::getIndexedExprs(exprs_unrolled_mv_loops);

  // TODO: It seems this type of optimization would be far easier to implement
  // on fusion ir than kernel ir. We should likely refactor this to at least run
  // before allocation insertion.
  const auto exprs_with_fused_broadcast = fuseWarpReduce(exprs_indexed_loops);

  const auto exprs_conditional_loops =
      generateConditionalFromPredicate(exprs_with_fused_broadcast);

  // Insert fake zero updates to make sure nvrtc doesn't blow out register use
  // on index and predicate reuse
  const auto exprs_register_adjusted = insertMagicZero(exprs_conditional_loops);

  const auto exprs_cleaned_up_loops =
      KIRCleaner::cleanUp(exprs_register_adjusted);

  // We now have the lowered expressions, finalize the kernel IR
  kernel_->finalize(exprs_cleaned_up_loops);
}

kir::Kernel* GpuLower::kernel() const {
  TORCH_CHECK(kernel_);
  return kernel_.get();
}

GpuLower* GpuLower::current() {
  TORCH_INTERNAL_ASSERT(
      active_gpu_lower != nullptr, "No active GpuLower available");
  return active_gpu_lower;
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
