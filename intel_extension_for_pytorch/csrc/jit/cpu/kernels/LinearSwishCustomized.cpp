#include "LinearSwishCustomized.h"
#include "csrc/aten/cpu/AddSwish.h"

#include <ATen/Context.h>
#include <ATen/InferSize.h>
#include <c10/util/Exception.h>
#include <c10/util/Logging.h>
#include <torch/csrc/autograd/function.h>

#include <limits>

#include "csrc/cpu/ideep/ideep.hpp"

namespace torch_ipex {
namespace cpu {

at::Tensor dil_linear_swish_customized(
    at::Tensor& x,
    const at::Tensor& weight,
    const at::Tensor& bias) {
#if defined(IPEX_PROFILE_OP)
  RECORD_FUNCTION(
      "dil_linear_swish_customized", c10::ArrayRef<c10::IValue>({}));
#endif

  // at::linear w/o bias
  auto linear_res = at::linear(x, weight);
  return AddSwish(x, linear_res, weight, bias);
}

} // namespace cpu
} // namespace torch_ipex
