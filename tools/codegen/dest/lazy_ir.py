from abc import ABC, abstractmethod
from typing import List, Union
from dataclasses import dataclass
from tools.codegen.context import method_with_native_function
from tools.codegen.model import (BackendIndex, NativeFunction,
                                 NativeFunctionsGroup)
from tools.codegen.api.types import (BaseCType, OptionalCType, NamedCType,
                                     VectorCType, kernel_signature)
import tools.codegen.api.dispatcher as dispatcher
from tools.codegen.api.lazy import LazyIrSchema, isValueType
from tools.codegen.dest.lazy_ts_lowering import ts_lowering_body

def node_ctor_arg_rvalue_string(arg: NamedCType, schema: LazyIrSchema) -> str:
    """
    Given a NamedCType from a lazy IR schema,
    generate a c++ string for materializing an rvalue of that arg for passing into
    a lazy Node constructor.
    """

    if isValueType(arg.type):
        if isinstance(arg.type, BaseCType):
            if arg.name in schema.wrapped_scalar_names:
                return f"torch::lazy::LazyGraphExecutor::Get()->GetIrValueForScalarFromCodegen({arg.name})"
            return f"lazy_{arg.name}.GetIrValue()"
        elif isinstance(arg.type, OptionalCType):
            if arg.name in schema.wrapped_scalar_names:
                return f"{arg.name} ? " \
                    f"c10::make_optional(torch::lazy::LazyGraphExecutor::Get()->GetIrValueForScalarFromCodegen(*{arg.name})) : " \
                    "c10::nullopt"
            return f"lazy_{arg.name} ? " \
                   f"c10::make_optional(lazy_{arg.name}.GetIrValue()) : " \
                   "c10::nullopt"
        else:
            raise AssertionError("TODO not sure if there are other valid types to handle here")
    else:
        if isinstance(arg.type, VectorCType) and isinstance(arg.type.elem, BaseCType):
            return f"std::vector<{arg.type.elem.type}>({arg.name}.begin(), {arg.name}.end())"
        elif (isinstance(arg.type, OptionalCType) and
                isinstance(arg.type.elem, VectorCType) and
                isinstance(arg.type.elem.elem, BaseCType)):
            return f"torch::lazy::ToOptionalVector<{arg.type.elem.elem.type}>({arg.name})"
        else:
            return f"{arg.name}"

def node_ctor_inputs(schema: LazyIrSchema) -> str:
    """
    Produce a formatted string with the arguments as passed into the constructor of a node class.
    """
    node_ctor_values = [node_ctor_arg_rvalue_string(arg, schema) for arg in schema.filtered_types()]
    return ",\n                              ".join(node_ctor_values)

def gen_fallback_code(schema: LazyIrSchema, overload_name: str) -> str:
    """
    Generate code that falls back to eager conditioned on a predicate
    """
    fallback_args = ",\n                ".join([str(arg.name) for arg in schema.filtered_types()])
    if len(overload_name):
        aten_op_str = f"ATEN_OP2({schema.aten_name}, {overload_name})"
    else:
        aten_op_str = f"ATEN_OP({schema.aten_name})"
    return f"""
        if (force_eager_fallback({aten_symbol(schema)})) {{
            return at::native::call_fallback_fn<&ltc_eager_fallback, {aten_op_str}>::call(
                {fallback_args}
            );
        }}
"""

def aten_symbol(schema: LazyIrSchema) -> str:
    missing_interned_strings = {
        'sigmoid_backward',
    }
    if schema.aten_name in missing_interned_strings:
        return f'c10::Symbol::fromQualString("aten::{schema.aten_name}")'
    return f'at::aten::{schema.aten_name}'

@dataclass(frozen=True)
class LazyIR(ABC):
    backend_index: BackendIndex
    node_base: str
    lowering_function_type: str = ""
    lowering_context_type: str = ""
    lowering_return_type: str = ""

    @method_with_native_function
    def __call__(self, f: Union[NativeFunctionsGroup, NativeFunction]) -> List[str]:
        func = f.functional.func if isinstance(f, NativeFunctionsGroup) else f.func
        return self.gen(f)

    @abstractmethod
    def lowering_body(self, f: Union[NativeFunctionsGroup, NativeFunction]) -> str:
        pass

    def gen(self, f: Union[NativeFunctionsGroup, NativeFunction]) -> List[str]:
        # for now, we just want one IR class decl and soon after also the method defs
        # and we use the functional version not out/inplace.
        func = f.functional.func if isinstance(f, NativeFunctionsGroup) else f.func
        schema = LazyIrSchema(func)
        all_types = schema.filtered_types()
        value_types = schema.filtered_types(values=True, scalars=False)
        scalar_types = schema.filtered_types(values=False, scalars=True)

        node_ctor_args = ", ".join([f"const {i.cpp_type()}& {i.name}" for i in all_types])
        scalar_initializers = ",\n        ".join([f"{t.name}({t.name})" for t in scalar_types])
        comma_if_scalar_initializers = ",\n" if len(scalar_initializers) else ""
        scalar_decls = "\n  ".join([f"{t.cpp_type()} {t.name};" for t in scalar_types])
        scalar_hashes = ", ".join([f"{f.name}" for f in scalar_types])
        base_ctor_value_args_list = []
        optional_values = []
        for t in value_types:
            if isinstance(t.type, BaseCType):
                base_ctor_value_args_list.append(f"{t.name}")
            elif isinstance(t.type, OptionalCType):
                base_ctor_value_args_list.append(f"{t.name}.value_or(kNullValue)")
                optional_values.append(t.name)
            else:
                raise AssertionError("TODO not sure if there are other valid types to handle here")
        base_ctor_value_args = ", ".join(base_ctor_value_args_list)
        has_optional_decls = "\n  ".join([f"bool has_{value}: 1;" for value in optional_values])
        has_optional_defs = "\n    ".join([f"has_{value} = !!{value};" for value in optional_values])
        members_to_string = []
        for t in scalar_types:
            if isinstance(t.type, OptionalCType):
                members_to_string.append(f"""if ({t.name}.has_value()) {{
    ss << ", {t.name}=" << {t.name}.value();
}} else {{
    ss << ", {t.name}=null";
}}""")
            else:
                members_to_string.append(f'ss << ", {t.name}=" << {t.name};')
        members_to_string_str = "\n    ".join(members_to_string)

        return [f"""\
class {schema.node_name} : public {self.node_base} {{
 public:
  {schema.node_name}({node_ctor_args}, std::vector<Shape>&& shapes)
      : {self.node_base}(torch::lazy::OpKind({aten_symbol(schema)}),
              {{{base_ctor_value_args}}}, std::move(shapes),
              /* num_outputs */ {len(func.returns)},
              torch::lazy::MHash({scalar_hashes})){comma_if_scalar_initializers}
        {scalar_initializers}

  {{
    {has_optional_defs}
  }}

  std::string ToString() const override {{
    std::stringstream ss;
    ss << {self.node_base}::ToString();
    {members_to_string_str}
    return ss.str();
  }}

  {self.lowering_return_type} Lower({self.lowering_function_type} function,
                   {self.lowering_context_type} loctx) const override {{
    {self.lowering_body(f)}
  }}

  {scalar_decls}
  {has_optional_decls}

}};

""", ]


@dataclass(frozen=True)
class TSLazyIR(LazyIR):
    lowering_function_type: str = "std::shared_ptr<torch::jit::GraphFunction>"
    lowering_context_type: str = "torch::lazy::TSLoweringContext*"
    lowering_return_type: str = "torch::lazy::TSOpVector"

    def lowering_body(self, f: Union[NativeFunctionsGroup, NativeFunction]) -> str:
        return ts_lowering_body(f)


def lazy_tensor_decls(value_types: List[NamedCType], tensor_class: str, schema: LazyIrSchema) -> str:
    lazy_tensor_decls: List[str] = []
    for t in value_types:
        if t.name in schema.wrapped_scalar_names:
            # no lazy tensor wrapper for scalars that are promoted to IR values
            continue
        if isinstance(t.type, BaseCType):
            lazy_tensor_decls.append(
                f"{tensor_class} lazy_{t.name} = "
                f"torch::lazy::GetLtcTensorOrCreateForWrappedNumber({t.name}, *common_device);")
        elif isinstance(t.type, OptionalCType):
            # TODO(alanwaketan): Maybe we want to apply GetLtcTensorOrCreateForWrappedNumber here, but hold it
            # until we encounter a real world example.
            lazy_tensor_decls.append(
                f"    {tensor_class} lazy_{t.name} = torch::lazy::TryGetLtcTensor({t.name}.value_or(at::Tensor()));")
        else:
            raise AssertionError("TODO not sure if there are other valid types to handle here")
    return ("\n        ").join(lazy_tensor_decls)

@dataclass(frozen=True)
class GenLazyNativeFuncDefinition:
    class_method_name: str
    backend_index: BackendIndex
    tensor_class: str

    @method_with_native_function
    def __call__(self, func: NativeFunction) -> List[str]:
        sig = kernel_signature(func, self.backend_index)
        metadata = self.backend_index.get_kernel(func)
        assert metadata is not None
        schema = LazyIrSchema(func.func)
        all_types = schema.filtered_types()
        value_types = schema.filtered_types(values=True, scalars=False)
        scalar_types = schema.filtered_types(values=False, scalars=True)
        returns_length = len(schema.returns)

        fallback_str = gen_fallback_code(schema, overload_name=func.func.name.overload_name)
        value_types_names = [f"{t.name}" for t in value_types if t.name not in schema.wrapped_scalar_names]
        assert len(value_types_names) > 0, "Code below assumes there is at least one tensor arg"
        get_device_str = f"""auto common_device = torch::lazy::GetBackendDevice({', '.join(value_types_names)});
        TORCH_INTERNAL_ASSERT(common_device);
        """

        lazy_tensor_decls_str = lazy_tensor_decls(value_types, self.tensor_class, schema)
        node_ctor_input_str = node_ctor_inputs(schema)

        # call the meta kernel if it exists, to compute output shape/dtype for our IR
        if func.structured or func.structured_delegate is not None:
            meta_out = """std::vector<Shape> shapes{Shape(out_meta.scalar_type(), out_meta.sizes().vec())};"""
            if returns_length > 1:
                def this_shape(i: int) -> str:
                    return f"Shape(std::get<{i}>(out_meta).scalar_type(), std::get<{i}>(out_meta).sizes().vec())"
                shapes_str = ','.join([this_shape(i) for i in range(returns_length)])
                meta_out = "std::vector<Shape> shapes{" + shapes_str + "};"

            # TODO: INTEGRATION POINT HERE:
            meta_str = f"""auto out_meta = at::meta::{schema.aten_name}({', '.join(str(t.name) for t in all_types)});
        {meta_out}"""
        else:
            shape_sig = ComputeShapeSignature(metadata.kernel, func)
            meta_str = f"""
        auto shapes = {shape_sig.shape_call};"""

        meta_str += f"""
        TORCH_INTERNAL_ASSERT(shapes.size() == {returns_length});"""

        node_str = f"""auto node = torch::lazy::MakeNode<ir::ops::{schema.node_name}>({node_ctor_input_str},
                                                                                      std::move(shapes));"""
        first_tensor_name = value_types_names[0]
        bridge_str = """auto result = torch::lazy::CreateAtenFromLtcTensor(
                torch::lazy::LazyTensor::Create(std::move(node), *common_device));"""

        if returns_length > 1:
            bridge_str = f"""std::vector<{self.tensor_class}> lazy_tensors;
        for (int i = 0; i < {returns_length}; i++) {{
            lazy_tensors.push_back(torch::lazy::LazyTensor::Create(torch::lazy::Value(node, i), *common_device));
        }}
        auto result = torch::lazy::TupleAtenFromLtcTensors<{returns_length}>(lazy_tensors);"""

        if schema.name.name.inplace or func.func.is_out_fn():
            assert returns_length == 1, "We assumed there was no such case where an op is an in-place variant " \
                                        "and has tuple outputs."
            bridge_str = f"""lazy_{first_tensor_name}.SetInPlaceIrValue(node);
        auto& result = {first_tensor_name};"""


        return [f"""\
    {sig.decl(name=f"{self.class_method_name}::{metadata.kernel}")} {{
        {fallback_str}
        TORCH_LAZY_FN_COUNTER("lazy::");
        {get_device_str}
        {lazy_tensor_decls_str}
        {meta_str}
        {node_str}
        {bridge_str}
        return result;
    }};\n
    """]

class ComputeShapeSignature:
    """
    Here we use the base name as the suffix of the signature to avoid generating for in-place variants.
    """
    def __init__(self, kernel_name: str, f: NativeFunction):
        self.__schema = LazyIrSchema(f.func)
        self.__dispatch_args = ', '.join([a.decl() for a in dispatcher.arguments(f.func)])
        self.__call_args = ", ".join([f"{t.name}" for t in self.__schema.filtered_types()])
        self.__kernel_name = kernel_name

    def __decl_suffix(self) -> str:
        return f"{self.__kernel_name}({self.__dispatch_args})"

    def __call_suffix(self) -> str:
        return f"{self.__kernel_name}({self.__call_args})"

    @property
    def shape_decl(self) -> str:
        return f"std::vector<Shape> compute_shape_{self.__decl_suffix()}"

    @property
    def shape_call(self) -> str:
        return f"torch_lazy_tensors::ir::ops::compute_shape_{self.__call_suffix()}"


@dataclass(frozen=True)
class GenLazyShapeInferenceDefinition:
    backend_index: BackendIndex
    tensor_class: str

    @method_with_native_function
    # def gen_lazy_shape_inference_decl(f: NativeFunction, backend_index: BackendIndex, tensor_class: str) -> List[str]:
    def __call__(self, f: NativeFunction) -> List[str]:
        sig = kernel_signature(f, self.backend_index)
        metadata = self.backend_index.get_kernel(f)
        assert metadata is not None
        schema = LazyIrSchema(f.func)
        value_types = schema.filtered_types(values=True, scalars=False)
        lazy_tensor_decls_str = lazy_tensor_decls(value_types, self.tensor_class, schema)
        node_ctor_input_str = node_ctor_inputs(schema)

        # Only generate shape/dtype fn for non-structured kernels,
        # since we just use the meta function for structured kernels
        if not f.structured and f.structured_delegate is None:
            shape_sig = ComputeShapeSignature(metadata.kernel, f)
            return ["\n".join([f"{shape_sig.shape_decl};"])]
        else:
            return []
