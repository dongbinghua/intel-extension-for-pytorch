#include "utils.h"

namespace torch_ipex {
namespace jit {
namespace fuser {
namespace onednn {
namespace utils {

using namespace torch::jit;

bool isViewOp(Node* n) {
  switch (n->kind()) {
    case aten::view:
    case aten::permute:
    case aten::transpose:
      return true;
    default:
      return false;
  }
}

bool isEltwiseOp(Node* n) {
  if (n->kind() == Symbol::aten("relu") ||
      n->kind() == Symbol::aten("sigmoid") ||
      n->kind() == Symbol::aten("quantize_per_tensor") ||
      n->kind() == Symbol::aten("quantize_per_channel") ||
      n->kind() == aten::to) {
    return true;
  } else {
    return false;
  }
}

bool isSupportedAsInputToDequant(torch::jit::Node* n) {
  if (n->kind() == prim::Constant ||
      n->kind() == Symbol::aten("quantize_per_tensor") ||
      n->kind() == Symbol::aten("quantize_per_channel")) {
    return true;
  } else {
    return false;
  }
}

} // namespace utils
} // namespace onednn
} // namespace fuser
} // namespace jit
} // namespace torch_ipex
