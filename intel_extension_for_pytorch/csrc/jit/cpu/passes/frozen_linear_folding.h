#pragma once

#include <torch/csrc/jit/ir/ir.h>
#include "graph_rewrite.h"
#include "graph_rewrite_utils.h"

namespace torch_ipex {
namespace jit {
namespace graph_rewrite {

// Fuses Linear -> Add/Sub into a single Linear by
// folding add constant tensor into linear weights.
// This pass only works on Frozen Graphs; otherwise it is a No-Op.
bool FoldFrozenLinearAddOrSub(std::shared_ptr<torch::jit::Graph>& graph);

// Fuses Linear -> Mul/Div into a single Linear by
// folding add constant tensor into linear weights.
// This pass only works on Frozen Graphs; otherwise it is a No-Op.
bool FoldFrozenLinearMulOrDiv(std::shared_ptr<torch::jit::Graph>& graph);

// Call FoldFrozenLinearAddOrSub and FoldFrozenLinearMulOrDiv multiple times
void FrozenLinearFolding(std::shared_ptr<torch::jit::Graph>& graph);

} // namespace graph_rewrite
} // namespace jit
} // namespace torch_ipex
