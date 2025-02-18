#include <ATen/code_template.h>
#include "csrc/cpu/ideep/ideep.hpp"
#include "csrc/jit/cpu/passes/utils.h"

#include "graph_rewrite.h"
#include "graph_rewrite_utils.h"

namespace torch_ipex {
namespace jit {
namespace graph_rewrite {

using namespace at::jit;
using namespace torch_ipex::cpu;
using namespace torch::jit;

void replaceFrozenIPEXLinearWithAtenLinear(
    Block* b,
    std::vector<Node*>& get_data_handle_nodes,
    const bool& use_mkl_sgemm) {
  for (Node* n : b->nodes()) {
    for (Block* block : n->blocks()) {
      replaceFrozenIPEXLinearWithAtenLinear(
          block, get_data_handle_nodes, use_mkl_sgemm);
    }
    if (n->kind() == Symbol::fromQualString("torch_ipex::ipex_linear") ||
        n->kind() == Symbol::fromQualString("torch_ipex::ipex_MKLSGEMM")) {
      TORCH_CHECK(
          !(c10::GradMode::is_enabled()),
          "Detect the Grad Mode! Please make sure torch.no_grad() is set priori to JIT trace");
      if (!(constant_as<at::Tensor>(n->namedInput("weight")).has_value())) {
        continue;
      }

      auto input_size_option = n->inputs()
                                   .at(0)
                                   ->type()
                                   ->cast<TensorType>()
                                   ->sizes()
                                   .concrete_sizes();
      if (!(input_size_option.has_value() &&
            input_size_option.value().size() >= 2)) {
        continue;
      }
      auto prepack_node = n->inputs().at(3)->node()->inputs().at(0);
      // For graph before "freeze", cannot get custom class to repack
      if (!toIValue(prepack_node).has_value())
        continue;
      at::Tensor weight_tensor;
      auto weight_dtype =
          n->inputs().at(1)->type()->cast<TensorType>()->scalarType().value();
      if (use_mkl_sgemm && weight_dtype != at::ScalarType::BFloat16) {
        auto linear_op_ctx =
            toIValue(prepack_node).value().toCustomClass<MKLOpContext>();
        weight_tensor = linear_op_ctx->to_public(
            constant_as<at::Tensor>(n->namedInput("weight")).value());
      } else {
        auto linear_op_ctx =
            toIValue(prepack_node).value().toCustomClass<LinearOpContext>();
        weight_tensor = linear_op_ctx->to_public(
            constant_as<at::Tensor>(n->namedInput("weight")).value());
      }
      WithInsertPoint guard(n);
      auto graph = n->owningGraph();

      auto aten_linear = graph->insertNode(graph->create(aten::linear));
      aten_linear->addInput(n->inputs().at(0));
      IValue weight_value(weight_tensor);
      auto weight = graph->insertConstant(weight_value);
      aten_linear->addInput(weight);
      aten_linear->addInput(n->inputs().at(2));
      aten_linear->output()->setType(n->output()->type()->cast<TensorType>());
      n->output()->replaceAllUsesWith(aten_linear->output());
      get_data_handle_nodes.emplace_back(n->inputs().at(3)->node());
    }
  }
  EliminateDeadCode(b);
}

void replaceFrozenIPEXLinearWithAtenLinear(
    std::shared_ptr<Graph>& graph,
    const bool& use_mkl_sgemm) {
  std::vector<Node*> get_data_handle_nodes;
  replaceFrozenIPEXLinearWithAtenLinear(
      graph->block(), get_data_handle_nodes, use_mkl_sgemm);
  for (auto& n : get_data_handle_nodes) {
    n->destroy();
  }
  EliminateDeadCode(graph);
}

void insertPrePackedLinearOp(
    Block* b,
    std::unordered_set<Node*>& aten_linear,
    const bool& use_mkl_sgemm) {
  for (Node* n : b->nodes()) {
    for (Block* block : n->blocks()) {
      insertPrePackedLinearOp(block, aten_linear, use_mkl_sgemm);
    }
    if (n->kind() != aten::linear)
      continue;
    WithInsertPoint guard(n);
    auto graph = n->owningGraph();
    auto input_size_option =
        n->inputs().at(0)->type()->cast<TensorType>()->sizes().concrete_sizes();
    if (!(input_size_option.has_value() &&
          input_size_option.value().size() >= 2)) {
      continue;
    }
    auto input_size = input_size_option.value();
    int64_t b_size = std::accumulate(
                         input_size.begin(),
                         input_size.end(),
                         1,
                         std::multiplies<double>()) /
        input_size[input_size.size() - 1];
    IValue batch_size_value(b_size);
    auto batch_size = graph->insertConstant(batch_size_value);
    auto tt = n->inputs().at(1)->type()->cast<TensorType>();
    auto weight_size_option = tt->sizes().concrete_sizes();
    if (!(weight_size_option.has_value() &&
          weight_size_option.value().size() == 2)) {
      continue;
    }
    auto weight_dtype_option = tt->scalarType();
    if (!(weight_dtype_option.has_value() &&
              (weight_dtype_option.value() == at::ScalarType::BFloat16) &&
              ideep::has_bf16_type_support() ||
          aten_linear.find(n) == aten_linear.end())) {
      continue;
    }
    auto weight_size = weight_size_option.value();

    // Note that once creating a graph node, make sure it is also inserted into
    // the graph, for: PyTorch (when disabled TE) has a check on the graph node,
    // pointing out that every mutable value in the system has a corresponding
    // element. So if creating a graph node but not inserted, it will not pass
    // the check since its graph element is not initialized. Details please
    // refer to
    // https://github.com/pytorch/pytorch/blob/master/torch/csrc/jit/ir/alias_analysis.cpp#L1956
    auto use_mkl_sgemm_ = use_mkl_sgemm &&
        weight_dtype_option.value() != at::ScalarType::BFloat16;
    auto prepack_node = graph->create(
        use_mkl_sgemm_
            ? Symbol::fromQualString("ipex_prepack::mkl_sgemm_prepack")
            : Symbol::fromQualString("ipex_prepack::linear_prepack"),
        1);
    for (auto i = 1; i < n->inputs().size(); ++i) {
      Value* v = n->inputs().at(i);
      prepack_node->addInput(v);
    }
    prepack_node->addInput(batch_size);
    prepack_node->output()->setType(
        use_mkl_sgemm_
            ? getCustomClass(
                  "__torch__.torch.classes.ipex_prepack.MKLOpContext")
            : getCustomClass(
                  "__torch__.torch.classes.ipex_prepack.LinearOpContext"));
    graph->insertNode(prepack_node);
    auto prepack_linear = graph->insertNode(graph->create(
        use_mkl_sgemm_ ? Symbol::fromQualString("ipex_prepack::mkl_sgemm_run")
                       : Symbol::fromQualString("ipex_prepack::linear_run"),
        1));
    prepack_linear->addInput(n->inputs().at(0));
    prepack_linear->addInput(prepack_node->output());
    prepack_linear->output()->setType(n->output()->type()->cast<TensorType>());
    auto v = n->outputs().at(0);
    n->output()->replaceAllUsesWith(prepack_linear->output());
  }
  EliminateDeadCode(b);
}

void insertPrePackedLinearOp(
    std::shared_ptr<Graph>& graph,
    std::unordered_set<Node*>& aten_linear,
    const bool& use_mkl_sgemm) {
  insertPrePackedLinearOp(graph->block(), aten_linear, use_mkl_sgemm);
}

void RecordAtenLinearNodes(
    Block* b,
    std::unordered_set<Node*>& aten_linear,
    bool& use_mkl_sgemm) {
  for (Node* n : b->nodes()) {
    for (Block* block : n->blocks()) {
      RecordAtenLinearNodes(block, aten_linear, use_mkl_sgemm);
    }
    if (n->kind() == aten::linear) {
      aten_linear.insert(n);
    }
    if (n->kind() == Symbol::fromQualString("torch_ipex::ipex_MKLSGEMM")) {
      use_mkl_sgemm = true;
    }
  }
  EliminateDeadCode(b);
}

void RecordAtenLinearNodes(
    std::shared_ptr<Graph>& graph,
    std::unordered_set<Node*>& aten_linear,
    bool& use_mkl_sgemm) {
  RecordAtenLinearNodes(graph->block(), aten_linear, use_mkl_sgemm);
  EliminateDeadCode(graph);
}

void fuseLinearWithEltwise(std::shared_ptr<Graph>& graph) {
  SubgraphRewriter rewriter_swish;
  std::array<std::string, 2> sigmoid_operators = {"sigmoid", "sigmoid_"};
  std::array<std::string, 2> mul_operators = {"mul", "mul_"};

  // For unary post OPs:
  auto linear_op_rstring = at::jit::CodeTemplate(R"(
     graph(%input, %packed_weight):
        %x : Tensor = ipex_prepack::linear_run(%input, %packed_weight)
        %res = ${op}(%x)
        return (%res))");

  auto linear_op_fused_rstring = at::jit::CodeTemplate(R"(
    graph(%input, %packed_weight):
        %res = ipex_prepack::linear_${op}_run(%input, %packed_weight)
        return (%res))");

  for (auto const& it : utils::supported_unary_post_op_fusion_set()) {
    std::string op = it.first;
    std::string ipex_op_name = it.second.ipex_op_name;

    at::jit::TemplateEnv env;
    env.s("op", op);

    at::jit::TemplateEnv env_fused;
    env_fused.s("op", ipex_op_name);

    SubgraphRewriter rewriter;
    rewriter.RegisterRewritePattern(
        linear_op_rstring.format(env),
        linear_op_fused_rstring.format(env_fused));

    auto filters = it.second.filters;
    rewriter.runOnGraph(graph, filters);
  }

  // For non-unary post OPs:
  auto linear_op_non_unary_rstring = at::jit::CodeTemplate(R"(
     graph(%input, %packed_weight, ${op_input_str}):
        %x : Tensor = ipex_prepack::linear_run(%input, %packed_weight)
        %res = ${op}(%x, ${op_input_str})
        return (%res))");

  auto linear_op_non_unary_fused_rstring = at::jit::CodeTemplate(R"(
    graph(%input, %packed_weight, ${op_input_str}):
        %res = ipex_prepack::linear_${op}_run(%input, ${op_input_str}, %packed_weight)
        return (%res))");

  for (auto const& it : utils::supported_non_unary_post_op_fusion_set()) {
    std::string op = it.first;
    std::string ipex_op_name = it.second.ipex_op_name;
    std::vector<std::string> op_input_list = it.second.op_input_list;
    std::string op_input_str = c10::Join(", ", op_input_list);

    at::jit::TemplateEnv env;
    env.s("op", op);
    env.s("op_input_str", op_input_str);

    at::jit::TemplateEnv env_fused;
    env_fused.s("op", ipex_op_name);
    env_fused.s("op_input_str", op_input_str);

    SubgraphRewriter rewriter;
    rewriter.RegisterRewritePattern(
        linear_op_non_unary_rstring.format(env),
        linear_op_non_unary_fused_rstring.format(env_fused));

    auto filters = it.second.filters;
    rewriter.runOnGraph(graph, filters);
  }

  auto linear_sigmoid_mul_rstring = CodeTemplate(R"(
    graph(%input, %packed_weight):
        %x = ipex_prepack::linear_run(%input, %packed_weight)
        %y = aten::${sigmoid}(%x)
        %res = aten::${mul}(%x, %y)
        return (%res))");

  std::string linear_swish_fused = R"(
    graph(%input, %packed_weight):
        %res = ipex_prepack::linear_swish_run(%input, %packed_weight)
        return (%res))";

  for (const auto& sigmoid : sigmoid_operators) {
    TemplateEnv env;
    env.s("sigmoid", sigmoid);
    for (const auto& mul : mul_operators) {
      env.s("mul", mul);
      rewriter_swish.RegisterRewritePattern(
          linear_sigmoid_mul_rstring.format(env), linear_swish_fused);
    }
  }
  rewriter_swish.runOnGraph(graph);
}

void fuseLinearAddRelu(std::shared_ptr<Graph>& graph) {
  SubgraphRewriter rewriter_add_accumu_on_the_right,
      rewriter_add_accumu_on_the_left, rewriter_add_relu;
  std::array<std::string, 2> add_operators = {"add", "add_"};
  std::array<std::string, 2> relu_operators = {"relu", "relu_"};

  // linear   Y
  //   \   /
  //    add
  // output = linear_output + alpha*Y
  auto linear_add_accumu_on_the_right_rstring = CodeTemplate(R"(
    graph(%input, %accumu, %alpha, %packed_weight):
        %x = ipex_prepack::linear_run(%input, %packed_weight)
        %res = aten::${add}(%x, %accumu, %alpha)
        return (%res))");

  //  Y     linear
  //   \   /
  //    add
  // output = Y + alpha*linear_output, alpha need to one or none.
  auto linear_add_accumu_on_the_left_rstring = CodeTemplate(R"(
    graph(%input, %accumu, %alpha, %packed_weight):
        %x = ipex_prepack::linear_run(%input, %packed_weight)
        %res = aten::${add}(%accumu, %x, %alpha)
        return (%res))");

  std::string linear_add_fused = R"(
    graph(%input, %accumu, %alpha, %packed_weight):
        %res = ipex_prepack::linear_add_run(%input, %accumu, %alpha, %packed_weight)
        return (%res))";

  auto linear_add_relu_rstring = CodeTemplate(R"(
    graph(%input, %accumu, %alpha, %packed_weight):
        %x = ipex_prepack::linear_add_run(%input, %accumu, %alpha, %packed_weight)
        %res = aten::${relu}(%x)
        return (%res))");

  std::string linear_add_relu_fused = R"(
    graph(%input, %accumu, %alpha, %packed_weight):
        %res = ipex_prepack::linear_add_relu_run(%input, %accumu, %alpha, %packed_weight)
        return (%res))";

  // linear + add
  for (const auto& add : add_operators) {
    TemplateEnv env;
    env.s("add", add);
    rewriter_add_accumu_on_the_right.RegisterRewritePattern(
        linear_add_accumu_on_the_right_rstring.format(env), linear_add_fused);
    rewriter_add_accumu_on_the_left.RegisterRewritePattern(
        linear_add_accumu_on_the_left_rstring.format(env), linear_add_fused);
  }

  // linear + add + relu
  for (const auto& relu : relu_operators) {
    TemplateEnv env;
    env.s("relu", relu);
    rewriter_add_relu.RegisterRewritePattern(
        linear_add_relu_rstring.format(env), linear_add_relu_fused);
  }

  rewriter_add_accumu_on_the_right.runOnGraph(
      graph, fuse_add_filter_accumu_on_the_right);
  rewriter_add_accumu_on_the_left.runOnGraph(
      graph, fuse_add_filter_accumu_on_the_left);
  rewriter_add_relu.runOnGraph(graph);
}

} // namespace graph_rewrite
} // namespace jit
} // namespace torch_ipex
