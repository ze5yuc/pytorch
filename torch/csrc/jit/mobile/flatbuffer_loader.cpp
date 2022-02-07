#include <torch/csrc/jit/mobile/flatbuffer_loader.h>

#include <ATen/ATen.h>
#include <ATen/core/dynamic_type.h>
#include <ATen/core/ivalue.h>
#include <ATen/core/qualified_name.h>
#include <c10/core/CPUAllocator.h>
#include <c10/util/Exception.h>
#include <c10/util/Optional.h>
#include <c10/util/ScopeExit.h>
#include <caffe2/serialize/inline_container.h>
#include <torch/csrc/jit/frontend/script_type_parser.h>
#include <torch/csrc/jit/mobile/import.h>
#include <torch/csrc/jit/mobile/interpreter.h>
#include <torch/csrc/jit/mobile/observer.h>
#include <torch/csrc/jit/mobile/type_parser.h>
#include <torch/csrc/jit/runtime/instruction.h>
#include <torch/csrc/jit/serialization/import_export_constants.h>
#include <torch/csrc/jit/serialization/import_read.h>
#include <torch/custom_class.h>

#include <flatbuffers/flatbuffers.h>

#if defined(HAVE_MMAP)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <string>
#include <vector>

namespace torch {
namespace jit {

using caffe2::serialize::IStreamAdapter;
using caffe2::serialize::PyTorchStreamReader;
using caffe2::serialize::ReadAdapterInterface;

static constexpr c10::string_view kCustomClassPrefix =
    "__torch__.torch.classes";
static constexpr c10::string_view kTorchPrefix = "__torch__";
static constexpr c10::string_view kJitPrefix = "torch.jit";

template <typename T, typename U>
std::vector<T> parseListNative(const U* list) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(list != nullptr);
  return {list->items()->begin(), list->items()->end()};
}

IValue parseList(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseTensor(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseTuple(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseDict(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseObject(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseIntList(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseDoubleList(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseBoolList(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseBasic(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);
IValue parseEnum(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue);

TypePtr resolveType(
    const std::string& type_string,
    std::shared_ptr<CompilationUnit> cu) {
  TypePtr type;
  c10::string_view type_str(type_string);
  if (type_str.starts_with(kCustomClassPrefix)) {
    type = getCustomClass(type_string);
    TORCH_CHECK(
        type, "The implementation of class ", type_string, " cannot be found.");
  } else if (
      type_str.starts_with(kTorchPrefix) || type_str.starts_with(kJitPrefix)) {
    c10::QualifiedName qn(type_string);
    if (cu->get_class(qn) == nullptr) {
      auto classtype = ClassType::create(qn, cu, true);
      cu->register_type(classtype);
      type = classtype;
    } else {
      type = cu->get_class(qn);
    }
  } else {
    type = c10::parseType(type_string);
  }
  return type;
}

FlatbufferLoader::FlatbufferLoader()
    : mcu_(std::make_shared<mobile::CompilationUnit>()),
      cu_(std::make_shared<CompilationUnit>()),
      ivalue_parsers_{nullptr} {
  registerIValueParser(mobile::serialization::IValueUnion::NONE, &parseBasic);
  registerIValueParser(mobile::serialization::IValueUnion::Int, &parseBasic);
  registerIValueParser(mobile::serialization::IValueUnion::Bool, &parseBasic);
  registerIValueParser(mobile::serialization::IValueUnion::Double, &parseBasic);
  registerIValueParser(
      mobile::serialization::IValueUnion::ComplexDouble, &parseBasic);
  registerIValueParser(
      mobile::serialization::IValueUnion::TensorMetadata, &parseTensor);
  registerIValueParser(mobile::serialization::IValueUnion::String, &parseBasic);
  registerIValueParser(mobile::serialization::IValueUnion::List, &parseList);
  registerIValueParser(
      mobile::serialization::IValueUnion::IntList, &parseIntList);
  registerIValueParser(
      mobile::serialization::IValueUnion::DoubleList, &parseDoubleList);
  registerIValueParser(
      mobile::serialization::IValueUnion::BoolList, &parseBoolList);
  registerIValueParser(mobile::serialization::IValueUnion::Tuple, &parseTuple);
  registerIValueParser(mobile::serialization::IValueUnion::Dict, &parseDict);
  registerIValueParser(
      mobile::serialization::IValueUnion::Object, &parseObject);
  registerIValueParser(mobile::serialization::IValueUnion::Device, &parseBasic);
  registerIValueParser(
      mobile::serialization::IValueUnion::EnumValue, &parseEnum);
  internal_registerTypeResolver(&resolveType);
}

void FlatbufferLoader::registerIValueParser(
    mobile::serialization::IValueUnion ivalue_type,
    IValueParser parser) {
  ivalue_parsers_[static_cast<uint8_t>(ivalue_type)] = parser;
}

void FlatbufferLoader::internal_registerTypeResolver(
    TypeResolver type_resolver) {
  type_resolver_ = type_resolver;
}

mobile::Module FlatbufferLoader::parseModule(
    mobile::serialization::Module* module) {
  module_ = module;
  all_ivalues_.clear();
  all_types_.clear();
  storages_.clear();
  storage_loaded_.clear();

  const auto* ivalues = module->ivalues();
  all_ivalues_.resize(ivalues->size());
  all_types_.resize(module->object_types()->size());
  storages_.resize(module->storage_data_size());
  storage_loaded_.resize(module->storage_data_size(), false);

  for (uint32_t i = 0; i < ivalues->size(); i++) {
    const auto* ival = ivalues->Get(i);
    if (const auto* func = ival->val_as_Function()) {
      auto func_ptr = parseFunction(func);
      all_functions_[i] = func_ptr.get();
      mcu_->register_function(std::move(func_ptr));
    } else {
      all_ivalues_[i] = parseIValue(ival);
    }
  }
  IValue& module_ivalue = getIValue(module->state_obj());

  // register functions
  for (const auto& f : all_functions_) {
    uint32_t class_index =
        ivalues->Get(f.first)->val_as_Function()->class_type();
    ClassTypePtr class_type = all_types_[class_index];
    class_type->addMethod(f.second);
  }

  return mobile::Module(module_ivalue.toObject(), mcu_);
}

std::unique_ptr<mobile::Function> FlatbufferLoader::parseFunction(
    const mobile::serialization::Function* method) {
  auto function = std::make_unique<mobile::Function>(
      c10::QualifiedName(method->qn()->str()));
  // TODO(qihan) add debug handle
  // const auto* debug_handle = method->debug_info()->debug_handle();
  for (const auto* inst : *method->instructions()) {
    function->append_instruction(
        static_cast<OpCode>(inst->op()), inst->x(), inst->n());
  }

  for (uint32_t i : *method->constants()) {
    function->append_constant(getIValue(i));
  }

  std::unordered_set<std::string> unsupported_op_names;
  const int64_t model_version = 0x6L;
  for (const auto* op : *method->operators()) {
    c10::optional<int> num_args = c10::nullopt;
    if (op->num_args_serialized() > -1) {
      num_args = op->num_args_serialized();
    }

    auto op_found = function->append_operator(
        op->name()->str(), op->overload_name()->str(), num_args, model_version);

    if (!op_found) {
      unsupported_op_names.emplace(
          op->name()->str() + "/" + op->overload_name()->str());
    }
  }

  AT_ASSERT(unsupported_op_names.empty());

  for (const auto i : *method->type_annotations()) {
    function->append_type(getOrCreateTypeAnnotations(i));
  }

  function->set_register_size(method->register_size());
  if (method->schema()) {
    auto parseArgList = [this](const auto* args_fb) {
      std::vector<c10::Argument> args;
      for (const auto* arg_tb : *args_fb) {
        IValue default_value = getIValue(arg_tb->default_value());
        TypePtr type_ptr = getOrCreateTypeAnnotations(arg_tb->type());
        auto arg = c10::Argument(
            arg_tb->name()->str(),
            std::move(type_ptr),
            c10::nullopt /*N*/,
            std::move(default_value));
        args.emplace_back(std::move(arg));
      }
      return args;
    };
    c10::FunctionSchema schema(
        method->qn()->str(),
        "" /*overload_name*/,
        parseArgList(method->schema()->arguments()),
        parseArgList(method->schema()->returns()),
        false /*is_varargs*/,
        false /*is_varret*/);

    function->setSchema(std::move(schema));
  }
  return function;
}

IValue parseEnum(
    FlatbufferLoader& loader,
    const mobile::serialization::IValue& ivalue) {
  const auto* enum_val = ivalue.val_as_EnumValue();
  auto enum_type = loader.getOrCreateTypeAnnotations(enum_val->type_name())
                       ->cast<c10::EnumType>();
  AT_ASSERT(
      enum_type,
      "Enum with type: " + enum_val->type_name()->str() + " not found.");
  IValue val = loader.getIValue(enum_val->value());
  for (const auto& p : enum_type->enumNamesValues()) {
    if (p.second == val) {
      auto enum_holder = c10::make_intrusive<at::ivalue::EnumHolder>(
          enum_type, p.first, p.second);
      return IValue(std::move(enum_holder));
    }
  }
  AT_ASSERT(
      false, "Enum with type: " + enum_val->type_name()->str() + " not found.");
}

IValue parseBasic(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue) {
  switch (ivalue.val_type()) {
    case mobile::serialization::IValueUnion::NONE:
      return {};
    case mobile::serialization::IValueUnion::Int:
      return ivalue.val_as_Int()->int_val();
    case mobile::serialization::IValueUnion::Bool:
      return ivalue.val_as_Bool()->bool_val();
    case mobile::serialization::IValueUnion::Double:
      return ivalue.val_as_Double()->double_val();
    case mobile::serialization::IValueUnion::ComplexDouble: {
      const auto* comp = ivalue.val_as_ComplexDouble();
      return c10::complex<double>(comp->real(), comp->imag());
    }
    case mobile::serialization::IValueUnion::String:
      return ivalue.val_as_String()->data()->str();
    case mobile::serialization::IValueUnion::Device: {
      return c10::Device(ivalue.val_as_Device()->str()->str());
    }
    default:
      return {};
  }
}

at::Tensor parseTensorFromMetadata(
    FlatbufferLoader* loader,
    const mobile::serialization::TensorMetadata* tensor_md) {
  at::ScalarType type = static_cast<at::ScalarType>(tensor_md->scalar_type());
  auto options = at::CPU(type).options();
  at::Tensor tensor;
  if (tensor_md->quantized_schema() != nullptr) {
    // is quantized
    const auto* schema = tensor_md->quantized_schema();
    auto qscheme_type = static_cast<at::QScheme>(schema->qscheme());
    switch (qscheme_type) {
      case at::kPerTensorAffine: {
        tensor = at::_empty_affine_quantized(
            {0}, options, schema->scale(), schema->zero_point());
      } break;
      case at::kPerChannelAffineFloatQParams:
      case at::kPerChannelAffine: {
        at::Tensor scales = parseTensorFromMetadata(loader, schema->scales());
        at::Tensor zero_points =
            parseTensorFromMetadata(loader, schema->zero_points());
        tensor = at::_empty_per_channel_affine_quantized(
            {0}, scales, zero_points, schema->axis(), options);
      } break;
      default:
        TORCH_CHECK(
            false,
            "Unsupported tensor quantization type in serialization ",
            toString(qscheme_type));
        break;
    }
  } else {
    tensor = at::empty({0}, options);
  }
  at::TensorImpl* impl = tensor.unsafeGetTensorImpl();

  c10::Storage storage;
  storage = loader->getStorage(tensor_md->storage_location_index());
  impl->set_storage_keep_dtype(storage);
  impl->set_storage_offset(tensor_md->storage_offset());

  std::vector<int64_t> size{
      tensor_md->sizes()->begin(), tensor_md->sizes()->end()};
  std::vector<int64_t> stride{
      tensor_md->strides()->begin(), tensor_md->strides()->end()};
  impl->set_sizes_and_strides(size, stride);
#ifndef MIN_EDGE_RUNTIME
  tensor = autograd::make_variable(tensor, tensor_md->requires_grad());
#endif
  return tensor;
}

IValue parseTensor(
    FlatbufferLoader& loader,
    const mobile::serialization::IValue& ivalue) {
  const mobile::serialization::TensorMetadata* tensor_md =
      ivalue.val_as_TensorMetadata();
  return parseTensorFromMetadata(&loader, tensor_md);
}

IValue parseList(
    FlatbufferLoader& loader,
    const mobile::serialization::IValue& ivalue) {
  const mobile::serialization::List* list = ivalue.val_as_List();
  auto res = c10::impl::GenericList(AnyType::get());
  for (int i : *list->items()) {
    res.emplace_back(loader.getIValue(i));
  }
  auto type = loader.getOrCreateTypeAnnotations(list->annotation_str());
  res.unsafeSetElementType(type->containedType(0));
  return res;
}

IValue parseIntList(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue) {
  const auto& list = ivalue.val_as_IntList();
  return parseListNative<int64_t>(list);
}

IValue parseDoubleList(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue) {
  const auto& list = ivalue.val_as_DoubleList();
  return parseListNative<double>(list);
}

IValue parseBoolList(
    FlatbufferLoader&,
    const mobile::serialization::IValue& ivalue) {
  const auto& list = ivalue.val_as_BoolList();
  std::vector<uint8_t> res = parseListNative<uint8_t>(list);
  c10::List<bool> boollist;
  for (auto x : res) {
    boollist.push_back(x);
  }
  return boollist;
}

IValue parseTuple(
    FlatbufferLoader& loader,
    const mobile::serialization::IValue& ivalue) {
  const auto& tuple = ivalue.val_as_Tuple();
  std::vector<IValue> res;
  for (int i : *tuple->items()) {
    res.emplace_back(loader.getIValue(i));
  }
  return c10::ivalue::Tuple::create(res);
}

IValue parseDict(
    FlatbufferLoader& loader,
    const mobile::serialization::IValue& ivalue) {
  const auto* dict = ivalue.val_as_Dict();
  auto result = c10::impl::GenericDict(AnyType::get(), AnyType::get());
  const auto* keys = dict->keys();
  const auto* values = dict->values();
  for (size_t i = 0; i < keys->size(); ++i) {
    uint32_t key = keys->Get(i);
    uint32_t val = values->Get(i);
    result.insert_or_assign(loader.getIValue(key), loader.getIValue(val));
  }
  auto type = loader.getOrCreateTypeAnnotations(dict->annotation_str());
  result.unsafeSetKeyType(type->containedType(0));
  result.unsafeSetValueType(type->containedType(1));
  return result;
}

ClassTypePtr FlatbufferLoader::getOrCreateClassTypeForObject(
    const mobile::serialization::Object* object) {
  auto cls = getType(object->type_index());
  const mobile::serialization::ObjectType* obj_type =
      module_->object_types()->Get(object->type_index());
  if (cls == nullptr) {
    c10::string_view qn_str(
        obj_type->type_name()->c_str(), obj_type->type_name()->size());
    if (qn_str.starts_with(kTorchPrefix) || qn_str.starts_with(kJitPrefix)) {
      c10::QualifiedName qn(obj_type->type_name()->str());
      cls = cu_->get_class(qn);
      if (cls == nullptr) {
        cls = ClassType::create(qn, cu_, true);
        cu_->register_type(cls);
      }
    } else {
      cls = c10::parseType(std::string(qn_str))->cast<ClassType>();
    }
    TORCH_CHECK(object->type_index() < all_ivalues_.size());
    all_types_[object->type_index()] = cls;

    if (obj_type->type() == mobile::serialization::TypeType::CLASS_WITH_FIELD) {
      for (uint32_t i = 0; i < object->attrs()->size(); i++) {
        IValue val = getIValue(object->attrs()->Get(i));
        // Need to use concrete object's field's type to set type of field.
        cls->addAttribute(
            obj_type->attr_names()->Get(i)->str(),
            val.type<c10::DynamicType>());
      }
    }
    initialized_types_.insert(object->type_index());
  }
  return cls;
}

IValue parseObject(
    FlatbufferLoader& loader,
    const mobile::serialization::IValue& ivalue) {
  const mobile::serialization::Object* object = ivalue.val_as_Object();
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(object != nullptr);
  const auto* cur_input = loader.getCurrentFlatbufferInput();
  const mobile::serialization::ObjectType* obj_type =
      cur_input->object_types()->Get(object->type_index());
  auto cls = loader.getOrCreateClassTypeForObject(object);
  Stack stack;
  switch (obj_type->type()) {
    case mobile::serialization::TypeType::CLASS_WITH_FIELD: {
      auto obj = c10::ivalue::Object::create(
          at::StrongTypePtr(loader.cu_, cls), object->attrs()->size());
      for (uint32_t i = 0; i < object->attrs()->size(); i++) {
        IValue val = loader.getIValue(object->attrs()->Get(i));
        obj->setSlot(i, std::move(val));
      }
      return obj;
    }
    case mobile::serialization::TypeType::CLASS_WITH_SETSTATE: {
      IValue input = loader.getIValue(object->state());
      mobile::Function* setstate = loader.getFunction(object->setstate_func());
      auto obj =
          c10::ivalue::Object::create(at::StrongTypePtr(loader.cu_, cls), 0);
      stack.push_back(obj);
      stack.emplace_back(std::move(input));
      setstate->run(stack);
      return obj;
    }
    case mobile::serialization::TypeType::CUSTOM_CLASS: {
      auto custom_class_type =
          torch::jit::getCustomClass(cls->name()->qualifiedName());
      IValue input = loader.getIValue(object->state());
      auto obj = c10::ivalue::Object::create(
          c10::StrongTypePtr(nullptr, custom_class_type), 1);
      stack.push_back(obj);
      stack.emplace_back(std::move(input));
      custom_class_type->getMethod("__setstate__").run(stack);
      return obj;
    }
    default:
      AT_ASSERT(false, "need to be object");
  }
}

IValue FlatbufferLoader::parseIValue(
    const mobile::serialization::IValue* ivalue) {
  return ivalue_parsers_[static_cast<uint32_t>(ivalue->val_type())](
      *this, *ivalue);
}

void deleteNothing2(void*);
void deleteNothing2(void*) {}

c10::Storage FlatbufferLoader::getStorage(uint32_t index) {
  TORCH_CHECK(index < storage_loaded_.size());
  TORCH_CHECK(index < storages_.size());
  if (!storage_loaded_[index]) {
    auto* storage = module_->storage_data()->GetMutableObject(index);
    size_t size = storage->data()->size();
    void* ptr = static_cast<void*>(storage->mutable_data()->data());
    at::DataPtr data(ptr, ptr, deleteNothing2, DeviceType::CPU);
    storages_[index] =
        c10::Storage(c10::Storage::use_byte_size_t(), size, std::move(data));
    storage_loaded_[index] = true;
  }
  return storages_[index];
}

TypePtr FlatbufferLoader::getOrCreateTypeAnnotations(
    const flatbuffers::String* offset) {
  auto iter = type_annotations_.find(offset);
  if (iter != type_annotations_.end()) {
    return iter->second;
  }
  TypePtr type = type_resolver_(offset->str(), cu_);
  type_annotations_[offset] = type;
  return type;
}

mobile::Module parse_and_initialize_mobile_module(
    std::shared_ptr<char> data,
    size_t,
    c10::optional<at::Device>) {
  auto* flatbuffer_module = mobile::serialization::GetMutableModule(data.get());
  mobile::Module m = FlatbufferLoader().parseModule(flatbuffer_module);
  m.set_delete_memory(std::move(data));
  return m;
}

mobile::Module initialize_mobile_module(
    mobile::serialization::Module* flatbuffer_module,
    c10::optional<at::Device>) {
  mobile::Module m = FlatbufferLoader().parseModule(flatbuffer_module);
  return m;
}

mobile::Module load_mobile_module_from_file(
    const std::string& filename,
    c10::optional<c10::Device> device) {
#if defined(HAVE_MMAP)
  int fd = open(filename.c_str(), O_RDONLY);
  struct stat statbuf {};
  fstat(fd, &statbuf);
  int size = statbuf.st_size;
  void* ptr = mmap(nullptr, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  auto deleter = [statbuf](char* ptr) { munmap(ptr, statbuf.st_size); };
  std::shared_ptr<char> data(reinterpret_cast<char*>(ptr), deleter);
#else
  FILE* f = fopen(filename.c_str(), "rb");
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::shared_ptr<char> data(static_cast<char*>(malloc(size)), free); // NOLINT
  fread(data.get(), size, 1, f);
  fclose(f);
#endif
  return parse_and_initialize_mobile_module(std::move(data), size, device);
}

} // namespace jit
} // namespace torch
