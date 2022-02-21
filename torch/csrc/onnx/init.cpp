#include <onnx/onnx_pb.h>
#include <torch/csrc/onnx/init.h>
#include <torch/csrc/onnx/onnx.h>
#include <torch/version.h>
#include <torch/csrc/jit/passes/onnx.h>
#include <torch/csrc/jit/passes/onnx/cast_all_constant_to_floating.h>
#include <torch/csrc/jit/passes/onnx/constant_fold.h>
#include <torch/csrc/jit/passes/onnx/deduplicate_initializers.h>
#include <torch/csrc/jit/passes/onnx/eliminate_unused_items.h>
#include <torch/csrc/jit/passes/onnx/eval_peephole.h>
#include <torch/csrc/jit/passes/onnx/fixup_onnx_controlflow.h>
#include <torch/csrc/jit/passes/onnx/function_extraction.h>
#include <torch/csrc/jit/passes/onnx/function_substitution.h>
#include <torch/csrc/jit/passes/onnx/list_model_parameters.h>
#include <torch/csrc/jit/passes/onnx/pattern_conversion/pattern_conversion.h>
#include <torch/csrc/jit/passes/onnx/pattern_conversion/pattern_encapsulation.h>
#include <torch/csrc/jit/passes/onnx/peephole.h>
#include <torch/csrc/jit/passes/onnx/prepare_division_for_onnx.h>
#include <torch/csrc/jit/passes/onnx/preprocess_for_onnx.h>
#include <torch/csrc/jit/passes/onnx/remove_inplace_ops_for_onnx.h>
#include <torch/csrc/jit/passes/onnx/scalar_type_analysis.h>
#include <torch/csrc/jit/passes/onnx/shape_type_inference.h>
#include <torch/csrc/jit/passes/onnx/unpack_quantized_weights.h>
#include <torch/csrc/jit/serialization/export.h>

namespace torch {
namespace onnx {

using namespace torch::jit;

void initONNXBindings(PyObject* module) {
  auto m = py::handle(module).cast<py::module>();

  // ONNX specific passes
  m.def("_jit_pass_onnx_remove_print", RemovePrintOps)
      .def("_jit_pass_onnx_preprocess_caffe2", PreprocessCaffe2Ops)
      .def("_jit_pass_onnx", ToONNX)
      .def(
          "_jit_pass_onnx_assign_output_shape",
          [](std::shared_ptr<Graph>& graph,
             const std::vector<at::Tensor>& tensors,
             const python::IODescriptor& desc,
             bool onnx_shape_inference = false) {
            ONNXAssignOutputShape(graph, tensors, desc, onnx_shape_inference);
          })
      .def("_jit_pass_onnx_function_substitution", ONNXFunctionCallSubstitution)
      .def(
          "_jit_pass_onnx_peephole",
          [](std::shared_ptr<Graph>& graph,
             int opset_version,
             bool fixed_batch_size) {
            return PeepholeOptimizeONNX(graph, opset_version, fixed_batch_size);
          })
      .def("_jit_pass_onnx_preprocess", PreprocessForONNX)
      .def(
          "_jit_pass_onnx_eval_peephole",
          [](std::shared_ptr<Graph>& graph,
             std::map<std::string, IValue>& paramsDict) {
            EvalPeepholeONNX(graph, paramsDict);
            return paramsDict;
          },
          pybind11::return_value_policy::move)
      .def(
          "_jit_pass_onnx_cast_all_constant_to_floating",
          CastAllConstantToFloating)
      .def(
          "_jit_pass_onnx_constant_fold",
          [](std::shared_ptr<Graph>& graph,
             std::map<std::string, IValue>& paramsDict,
             int opset_version) {
            ConstantFoldONNX(
                graph,
                paramsDict,
                opset_version); // overload resolution
            return paramsDict;
          },
          pybind11::return_value_policy::move)
      .def(
          "_jit_pass_onnx_eliminate_unused_items",
          [](std::shared_ptr<Graph>& graph,
             std::map<std::string, IValue>& paramsDict) {
            EliminateUnusedItemsONNX(
                graph->block(),
                paramsDict); // overload resolution
            return paramsDict;
          },
          pybind11::return_value_policy::move)
      .def(
          "_jit_pass_onnx_scalar_type_analysis",
          [](std::shared_ptr<Graph>& graph,
             bool lowprecision_cast,
             int opset_version) {
            return ScalarTypeAnalysisForONNX(
                graph, lowprecision_cast, opset_version);
          },
          py::arg("graph"),
          py::arg("lowprecision_cast") = true,
          py::arg("opset_version"))
      .def(
          "_jit_pass_onnx_remove_inplace_ops_for_onnx", RemoveInplaceOpsForONNX)
      .def(
          "_jit_pass_onnx_node_shape_type_inference",
          [](Node* n,
             std::map<std::string, IValue>& params_dict,
             int opset_version) {
            ONNXShapeTypeInference(n, params_dict, opset_version);
          })
      .def(
          "_jit_pass_onnx_graph_shape_type_inference",
          [](std::shared_ptr<Graph>& graph,
             std::map<std::string, IValue>& params_dict,
             int opset_version) {
            ONNXShapeTypeInference(graph, params_dict, opset_version);
          })
      .def("_jit_pass_onnx_set_dynamic_input_shape", ONNXSetDynamicInputShape)
      .def("_jit_pass_onnx_lint", ONNXLintGraph)
      .def("_jit_pass_onnx_function_extraction", torch::jit::onnx::ONNXFunctionExtraction)
      .def("_jit_pass_onnx_block", BlockToONNX)
      .def(
          "_jit_pass_onnx_unpack_quantized_weights",
          [](std::shared_ptr<Graph>& graph,
             std::map<std::string, IValue>& paramsDict,
             bool caffe2) {
            UnpackQuantizedWeights(graph, paramsDict, caffe2);
            return paramsDict;
          },
          pybind11::return_value_policy::move)
      .def(
          "_jit_pass_onnx_quantization_insert_permutes",
          [](std::shared_ptr<Graph>& graph,
             std::map<std::string, IValue>& paramsDict) {
            insertPermutes(graph, paramsDict);
            return paramsDict;
          },
          pybind11::return_value_policy::move)
      .def(
          "_jit_onnx_list_model_parameters",
          [](Module& module) { return list_module_parameters(module); })
      .def("_jit_pass_prepare_division_for_onnx", PrepareDivisionForONNX)
      .def(
          "_jit_onnx_convert_pattern_from_subblock", ConvertPatternFromSubblock)
      .def("_jit_pass_fixup_onnx_controlflow_node", FixupONNXControlflowNode)
      .def(
          "_jit_pass_onnx_deduplicate_initializers",
          [](std::shared_ptr<Graph>& graph,
             std::map<std::string, IValue> params_dict,
             bool is_train) {
            DeduplicateInitializers(graph, params_dict, is_train);
            return params_dict;
          },
          pybind11::return_value_policy::move);

  m.def(
      "_check_onnx_proto",
      [](const std::string& proto_string, bool full_check) { check_onnx_proto(proto_string, full_check); },
      py::arg("proto_string"),
      py::arg("full_check") = false);

  auto onnx = m.def_submodule("_onnx");
  py::enum_<::ONNX_NAMESPACE::TensorProto_DataType>(onnx, "TensorProtoDataType")
      .value("UNDEFINED", ::ONNX_NAMESPACE::TensorProto_DataType_UNDEFINED)
      .value("FLOAT", ::ONNX_NAMESPACE::TensorProto_DataType_FLOAT)
      .value("UINT8", ::ONNX_NAMESPACE::TensorProto_DataType_UINT8)
      .value("INT8", ::ONNX_NAMESPACE::TensorProto_DataType_INT8)
      .value("UINT16", ::ONNX_NAMESPACE::TensorProto_DataType_UINT16)
      .value("INT16", ::ONNX_NAMESPACE::TensorProto_DataType_INT16)
      .value("INT32", ::ONNX_NAMESPACE::TensorProto_DataType_INT32)
      .value("INT64", ::ONNX_NAMESPACE::TensorProto_DataType_INT64)
      .value("STRING", ::ONNX_NAMESPACE::TensorProto_DataType_STRING)
      .value("BOOL", ::ONNX_NAMESPACE::TensorProto_DataType_BOOL)
      .value("FLOAT16", ::ONNX_NAMESPACE::TensorProto_DataType_FLOAT16)
      .value("DOUBLE", ::ONNX_NAMESPACE::TensorProto_DataType_DOUBLE)
      .value("UINT32", ::ONNX_NAMESPACE::TensorProto_DataType_UINT32)
      .value("UINT64", ::ONNX_NAMESPACE::TensorProto_DataType_UINT64)
      .value("COMPLEX64", ::ONNX_NAMESPACE::TensorProto_DataType_COMPLEX64)
      .value("COMPLEX128", ::ONNX_NAMESPACE::TensorProto_DataType_COMPLEX128)
      .value("BFLOAT16", ::ONNX_NAMESPACE::TensorProto_DataType_BFLOAT16);

  py::enum_<OperatorExportTypes>(onnx, "OperatorExportTypes")
      .value("ONNX", OperatorExportTypes::ONNX)
      .value("ONNX_ATEN", OperatorExportTypes::ONNX_ATEN)
      .value("ONNX_ATEN_FALLBACK", OperatorExportTypes::ONNX_ATEN_FALLBACK)
      .value("ONNX_FALLTHROUGH", OperatorExportTypes::ONNX_FALLTHROUGH);

  py::enum_<TrainingMode>(onnx, "TrainingMode")
      .value("EVAL", TrainingMode::EVAL)
      .value("PRESERVE", TrainingMode::PRESERVE)
      .value("TRAINING", TrainingMode::TRAINING);

  onnx.attr("PRODUCER_VERSION") = py::str(TORCH_VERSION);

#ifdef BUILD_CAFFE2
  onnx.attr("_CAFFE2_ATEN_FALLBACK") = true;
#else
  onnx.attr("_CAFFE2_ATEN_FALLBACK") = false;
#endif
}
} // namespace onnx
} // namespace torch
