#include <c10/util/irange.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/ir_builder.h>
#include <torch/csrc/jit/codegen/cuda/mutator.h>

#include <vector>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

void OptOutMutator::mutate(Statement* s) {
  Statement::mutatorDispatch(this, s);
}

void OptOutMutator::mutate(Expr* e) {
  Expr::mutatorDispatch(this, e);
}

void OptOutMutator::mutate(Val* v) {
  Val::mutatorDispatch(this, v);
}

void OptOutMutator::registerMutation(Val* val, Val* mutation) {
  bool val_is_ns = val->vtype() == ValType::NamedScalar;
  bool mutation_is_ns = mutation->vtype() == ValType::NamedScalar;
  bool val_is_scalar = val->vtype() == ValType::Scalar;
  bool mutation_is_scalar = mutation->vtype() == ValType::Scalar;
  TORCH_INTERNAL_ASSERT(
      mutation->dtype() == val->dtype() &&
          (mutation->vtype() == val->vtype() ||
           ((val_is_ns && mutation_is_scalar) ||
            (mutation_is_ns && val_is_scalar))),
      "Mutations are not allowed to change types, tried to go from: (",
      val->vtype(),
      ", ",
      val->dtype(),
      ") to: (",
      mutation->vtype(),
      ", ",
      mutation->dtype(),
      ")");
  mutations[val] = mutation;
}

void OptOutMutator::mutate(Bool* b) {}

void OptOutMutator::mutate(Double* d) {}

void OptOutMutator::mutate(Int* i) {}

void OptOutMutator::mutate(NamedScalar* ns) {}

void OptOutMutator::mutate(IterDomain* id) {
  Val* start = maybeMutated(id->start());
  Val* extent = maybeMutated(id->extent());
  Val* stop_offset = maybeMutated(id->stopOffset());
  if (start->sameAs(id->start()) && extent->sameAs(id->extent()) &&
      stop_offset->sameAs(id->stopOffset())) {
    return;
  }

  Val* mutated_val = IrBuilder::create<IterDomain>(
      id->container(),
      start,
      extent,
      stop_offset,
      id->getParallelType(),
      id->getIterType(),
      id->isRFactorProduct());
  if (id->hasPaddingToMultipleOfWarp()) {
    mutated_val->as<IterDomain>()->padToMultipleOfWarp(
        id->getMaybeSizeAfterPadding());
  }
  registerMutation(id, mutated_val);
}

void OptOutMutator::mutate(TensorDomain* td) {
  bool mutated = false;

  auto updateIdVec = [&](const std::vector<IterDomain*>& ids) {
    std::vector<IterDomain*> updated_ids;
    for (auto id : ids) {
      auto updated_id = maybeMutated(id)->as<IterDomain>();
      updated_ids.push_back(updated_id);
      if (!updated_id->sameAs(id)) {
        mutated = true;
      }
    }
    return updated_ids;
  };

  std::vector<IterDomain*> root_dom = updateIdVec(td->getRootDomain());
  std::vector<IterDomain*> rfactor_dom = td->hasRFactor()
      ? updateIdVec(td->getMaybeRFactorDomain())
      : std::vector<IterDomain*>();
  std::vector<IterDomain*> domain = updateIdVec(td->domain());

  if (!mutated) {
    return;
  }

  Val* mutated_val = IrBuilder::create<TensorDomain>(
      td->container(), root_dom, rfactor_dom, domain, td->contiguity());
  registerMutation(td, mutated_val);
}

void OptOutMutator::mutate(TensorView* tv) {
  TensorDomain* td = maybeMutated(tv->domain())->as<TensorDomain>();
  if (!tv->domain()->sameAs(td)) {
    tv->setDomain(td);
  }
  // Don't register tv mutations as we just want to update the TD
}

void OptOutMutator::mutate(kir::Predicate*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}

void OptOutMutator::mutate(kir::TensorIndex*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}

// MUTATE FUNCTIONS FOR EXPRESSIONS.
void OptOutMutator::mutate(UnaryOp* uop) {
  Val* out = maybeMutated(uop->out());
  Val* in = maybeMutated(uop->in());

  if (out->sameAs(uop->out()) && in->sameAs(uop->in())) {
    return;
  }
  auto container = uop->container();
  auto uop_type = uop->getUnaryOpType();
  container->removeExpr(uop);
  IrBuilder::create<UnaryOp>(container, uop_type, out, in);
}

void OptOutMutator::mutate(BinaryOp* bop) {
  Val* out = maybeMutated(bop->out());
  Val* lhs = maybeMutated(bop->lhs());
  Val* rhs = maybeMutated(bop->rhs());

  if (out == bop->out() && lhs == bop->lhs() && rhs == bop->rhs()) {
    return;
  }

  auto container = bop->container();
  auto bop_type = bop->getBinaryOpType();
  container->removeExpr(bop);
  IrBuilder::create<BinaryOp>(container, bop_type, out, lhs, rhs);
}

void OptOutMutator::mutate(TernaryOp* top) {
  Val* out = maybeMutated(top->out());
  Val* in1 = maybeMutated(top->in1());
  Val* in2 = maybeMutated(top->in2());
  Val* in3 = maybeMutated(top->in3());

  if (out == top->out() && in1 == top->in1() && in2 == top->in2() &&
      in3 == top->in3()) {
    return;
  }

  auto container = top->container();
  auto top_type = top->getTernaryOpType();
  container->removeExpr(top);
  IrBuilder::create<TernaryOp>(container, top_type, out, in1, in2, in3);
}

void OptOutMutator::mutate(ReductionOp* rop) {
  Val* out = maybeMutated(rop->out());
  Val* in = maybeMutated(rop->in());
  Val* init = rop->init();
  if (out->sameAs(rop->out()) && in->sameAs(rop->in()) &&
      init->sameAs(rop->init())) {
    return;
  }

  auto container = rop->container();
  auto rop_type = rop->getReductionOpType();
  container->removeExpr(rop);
  IrBuilder::create<ReductionOp>(container, rop_type, init, out, in);
}

namespace {
inline bool compareOptional(Val* a, Val* b) {
  if (!a || !b) {
    return (!a && !b);
  }
  return a->sameAs(b);
}

} // namespace

void OptOutMutator::mutate(WelfordOp* wop) {
  Val* out_avg = maybeMutated(wop->outAvg());
  Val* out_var = maybeMutated(wop->outVar());
  Val* out_N = maybeMutated(wop->outN());

  Val* in_avg = maybeMutated(wop->inAvg());
  Val* in_var = wop->inVar() ? maybeMutated(wop->inVar()) : nullptr;
  Val* in_N = maybeMutated(wop->inN());

  Val* init_avg = wop->initAvg() ? maybeMutated(wop->initAvg()) : nullptr;
  Val* init_var = wop->initVar() ? maybeMutated(wop->initVar()) : nullptr;
  Val* init_N = maybeMutated(wop->initN());

  const bool out_compare = out_avg->sameAs(wop->outAvg()) &&
      out_var->sameAs(wop->outVar()) && out_N->sameAs(wop->outN());
  const bool in_compare = in_avg->sameAs(wop->inAvg()) &&
      compareOptional(in_var, wop->inVar()) && in_N->sameAs(wop->inN());
  const bool init_compare = compareOptional(init_avg, wop->initAvg()) &&
      compareOptional(init_var, wop->initVar()) && init_N->sameAs(wop->initN());

  if (out_compare && init_compare && in_compare) {
    return;
  }

  auto container = wop->container();
  container->removeExpr(wop);
  IrBuilder::create<WelfordOp>(
      container,
      out_avg,
      out_var,
      out_N,
      init_avg,
      init_var,
      init_N,
      in_avg,
      in_var,
      in_N);
}

void OptOutMutator::mutate(BroadcastOp* bop) {
  Val* out = maybeMutated(bop->out());
  Val* in = maybeMutated(bop->in());

  if (out->sameAs(bop->out()) && in->sameAs(bop->in())) {
    return;
  }

  auto container = bop->container();
  auto flags = bop->getBroadcastDimFlags();
  container->removeExpr(bop);
  IrBuilder::create<BroadcastOp>(container, out, in, flags);
}

void OptOutMutator::mutate(TransposeOp* top) {
  TensorView* out = maybeMutated(top->out())->as<TensorView>();
  TensorView* in = maybeMutated(top->in())->as<TensorView>();

  if (out->sameAs(top->out()) && in->sameAs(top->in())) {
    return;
  }

  auto container = top->container();
  auto new2old = top->new2old();
  container->removeExpr(top);
  IrBuilder::create<TransposeOp>(container, out, in, new2old);
}

void OptOutMutator::mutate(ShiftOp* sop) {
  Val* out = maybeMutated(sop->out())->asVal();
  Val* in = maybeMutated(sop->in())->asVal();

  if (out->sameAs(sop->out()) && in->sameAs(sop->in())) {
    return;
  }

  auto offsets = sop->offsets();
  auto pad_width = sop->padWidth();
  auto container = sop->container();
  container->removeExpr(sop);
  IrBuilder::create<ShiftOp>(container, out, in, offsets, pad_width);
}

void OptOutMutator::mutate(GatherOp* op) {
  Val* out = maybeMutated(op->out())->asVal();
  Val* in = maybeMutated(op->in())->asVal();

  if (out->sameAs(op->out()) && in->sameAs(op->in())) {
    return;
  }

  auto window_shape = op->windowShape();
  auto pad_width = op->padWidth();
  auto container = op->container();
  container->removeExpr(op);
  IrBuilder::create<GatherOp>(container, out, in, window_shape, pad_width);
}

void OptOutMutator::mutate(ViewOp* vop) {
  TensorView* out = maybeMutated(vop->out())->as<TensorView>();
  TensorView* in = maybeMutated(vop->in())->as<TensorView>();

  if (out->sameAs(vop->out()) && in->sameAs(vop->in())) {
    return;
  }

  auto container = vop->container();
  container->removeExpr(vop);
  IrBuilder::create<ViewOp>(container, out, in);
}

void OptOutMutator::mutate(Split* s) {
  IterDomain* ot = maybeMutated(s->outer())->as<IterDomain>();
  IterDomain* inr = maybeMutated(s->inner())->as<IterDomain>();
  IterDomain* in = maybeMutated(s->in())->as<IterDomain>();
  Val* fact = maybeMutated(s->factor())->as<Val>();
  Val* start_offset = maybeMutated(s->startOffset());
  Val* stop_offset = maybeMutated(s->stopOffset());

  if (ot->sameAs(s->outer()) && inr->sameAs(s->inner()) &&
      in->sameAs(s->in()) && areEqualScalars(fact, s->factor()) &&
      start_offset->sameAs(s->startOffset()) &&
      stop_offset->sameAs(s->stopOffset())) {
    return;
  }

  auto container = s->container();
  auto inner_split = s->innerSplit();
  container->removeExpr(s);
  auto new_node = IrBuilder::create<Split>(
      container, ot, inr, in, fact, inner_split, start_offset, stop_offset);
}

void OptOutMutator::mutate(Merge* m) {
  IterDomain* ot = maybeMutated(m->out())->as<IterDomain>();
  IterDomain* otr = maybeMutated(m->outer())->as<IterDomain>();
  IterDomain* in = maybeMutated(m->inner())->as<IterDomain>();

  if (ot->sameAs(m->out()) && otr->sameAs(m->outer()) &&
      in->sameAs(m->inner())) {
    return;
  }

  auto container = m->container();
  container->removeExpr(m);
  auto new_node = IrBuilder::create<Merge>(container, ot, otr, in);
}

void OptOutMutator::mutate(kir::Allocate*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::Sync*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::InitMagicZero*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::UpdateMagicZero*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::ForLoop*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::IfThenElse*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::GridReduction*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::GridBroadcast*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}
void OptOutMutator::mutate(kir::GridWelford*) {
  TORCH_INTERNAL_ASSERT(false, "Not implemented yet.");
}

void OptOutMutator::removeExpr(IrContainer* container, Expr* expr) {
  container->removeExpr(expr);
}
} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
