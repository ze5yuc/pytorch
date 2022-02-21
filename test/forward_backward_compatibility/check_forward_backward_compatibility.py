import argparse
import datetime
import re
import sys
import warnings
from collections import defaultdict

import torch
from torch._C import parse_schema


# The date specifies how long the allowlist exclusion should apply to.
#
#   - If we NEVER give BC guarantee for an operator, you can put the
#     date arbitrarily far in the future.
#   - Otherwise, pick a date that is far enough in the future that you
#     believe you can land your diff before then.
#
# Allowlist entries can be removed after the date listed on them passes.
#
# Allowlist item format:
# [
#   0: function name regex
#   1: date until which the allowlist entry is valid
#   2: (optional) function argument regex
# ]
#
# NB: function name DOES NOT include overload name!
ALLOW_LIST = [
    ("c10_experimental", datetime.date(2222, 1, 1)),
    # Internal
    ("static", datetime.date(9999, 1, 1)),
    ("prim::ModuleDictIndex", datetime.date(9999, 1, 1)),
    ("prim::MKLDNNRelu6", datetime.date(9999, 1, 1)),
    ("prim::MKLDNNRelu6_", datetime.date(9999, 1, 1)),
    ("prim::Concat", datetime.date(9999, 1, 1)),
    # Internal, profiler-specific ops
    ("profiler::_call_end_callbacks_on_jit_fut*", datetime.date(9999, 1, 1)),
    ("profiler::_record_function_enter", datetime.date(9999, 1, 1)),
    ("aten::linalg_matrix_rank", datetime.date(2021, 10, 30)),
    ("aten::linalg_pinv", datetime.date(2021, 10, 30)),
    ("aten::_cholesky_helper", datetime.date(9999, 1, 1)),
    ("aten::_lstsq_helper", datetime.date(9999, 1, 1)),
    ("aten::_syevd_helper", datetime.date(9999, 1, 1)),
    ("aten::_linalg_solve_out_helper_", datetime.date(9999, 1, 1)),
    ("aten::select_backward", datetime.date(9999, 1, 1)),
    ("aten::slice_backward", datetime.date(9999, 1, 1)),
    ("aten::diagonal_backward", datetime.date(9999, 1, 1)),
    ("aten::rowwise_prune", datetime.date(9999, 1, 1)),
    ("aten::adaptive_avg_pool3d_backward", datetime.date(9999, 1, 1)),
    ("aten::_embedding_bag_dense_backward", datetime.date(9999, 1, 1)),
    ("aten::randperm", datetime.date(9999, 1, 1)),
    ("aten::gelu", datetime.date(2022, 3, 1)),
    ("aten::gelu_backward", datetime.date(2022, 3, 1)),
    ("aten::cudnn_convolution_backward", datetime.date(2022, 1, 31)),
    ("aten::cudnn_convolution_backward_input", datetime.date(2022, 1, 31)),
    ("aten::cudnn_convolution_backward_weight", datetime.date(2022, 1, 31)),
    ("aten::cudnn_convolution_transpose_backward", datetime.date(2022, 1, 31)),
    ("aten::cudnn_convolution_transpose_backward_input", datetime.date(2022, 1, 31)),
    ("aten::cudnn_convolution_transpose_backward_weight", datetime.date(2022, 1, 31)),
    ("aten::mkldnn_convolution_backward", datetime.date(2022, 1, 31)),
    ("aten::mkldnn_convolution_backward_input", datetime.date(2022, 1, 31)),
    ("aten::mkldnn_convolution_backward_weights", datetime.date(2022, 1, 31)),
    ("aten::_nnpack_spatial_convolution_backward", datetime.date(2022, 1, 31)),
    ("aten::_nnpack_spatial_convolution_backward_input", datetime.date(2022, 1, 31)),
    ("aten::_nnpack_spatial_convolution_backward_weight", datetime.date(2022, 1, 31)),
    ("aten::_slow_conv2d_forward", datetime.date(2022, 1, 31)),
    ("aten::_slow_conv2d_backward", datetime.date(2022, 1, 31)),
    ("aten::slow_conv3d_forward", datetime.date(2022, 1, 31)),
    ("aten::slow_conv3d_backward", datetime.date(2022, 1, 31)),
    ("aten::slow_conv_dilated2d_backward", datetime.date(2022, 1, 31)),
    ("aten::slow_conv_dilated3d_backward", datetime.date(2022, 1, 31)),
    ("aten::slow_conv_transpose2d", datetime.date(2022, 1, 31)),
    ("aten::slow_conv_transpose2d_backward", datetime.date(2022, 1, 31)),
    ("aten::slow_conv_transpose3d", datetime.date(2022, 1, 31)),
    ("aten::slow_conv_transpose3d_backward", datetime.date(2022, 1, 31)),
    ("aten::_index_copy_", datetime.date(2022, 5, 31)),
    ("aten::_svd_helper", datetime.date(2022, 3, 31)),
    ("aten::linalg_svdvals", datetime.date(2022, 3, 31)),
    ("aten::linalg_svdvals_out", datetime.date(2022, 3, 31)),
    ("aten::linalg_svd", datetime.date(2022, 3, 31)),
    ("aten::linalg_svd_out", datetime.date(2022, 3, 31)),
    ("aten::_max_pool1d_cpu_forward", datetime.date(2022, 2, 8)),
    ("aten::_convolution_nogroup", datetime.date(9999, 1, 1)),
    ("aten::miopen_convolution_backward", datetime.date(9999, 1, 1)),
    ("aten::miopen_convolution_backward_bias", datetime.date(9999, 1, 1)),
    ("aten::miopen_convolution_backward_input", datetime.date(9999, 1, 1)),
    ("aten::miopen_convolution_backward_weight", datetime.date(9999, 1, 1)),
    ("aten::miopen_convolution_transpose_backward", datetime.date(9999, 1, 1)),
    ("aten::miopen_convolution_transpose_backward_input", datetime.date(9999, 1, 1)),
    ("aten::miopen_convolution_transpose_backward_weight", datetime.date(9999, 1, 1)),
    ("aten::miopen_depthwise_convolution_backward", datetime.date(9999, 1, 1)),
    ("aten::miopen_depthwise_convolution_backward_input", datetime.date(9999, 1, 1)),
    ("aten::miopen_depthwise_convolution_backward_weight", datetime.date(9999, 1, 1)),
    ("caffe2::", datetime.date(2021, 10, 23)),
    ("prepacked::unpack_prepacked_sizes_conv2d", datetime.date(9999, 1, 1)),
    ("prepacked::unpack_prepacked_sizes_linear", datetime.date(9999, 1, 1)),
    ("q::_FloatToBfloat16Quantized", datetime.date(2021, 12, 21)),
    ("q::_Bfloat16QuantizedToFloat", datetime.date(2021, 12, 21)),
    ("aten::_inverse_helper", datetime.date(2021, 12, 31)),
    ("aten::softplus_backward", datetime.date(2022, 1, 31)),
    ("aten::softplus_backward.grad_input", datetime.date(2022, 1, 31)),
    ("aten::quantile", datetime.date(2022, 9, 30)),
    ("aten::nanquantile", datetime.date(2022, 9, 30)),
    ("aten::_convolution_double_backward", datetime.date(2022, 3, 31)),
    ("aten::_scatter_reduce", datetime.date(2022, 1, 31)),
    ("aten::scatter_reduce.two", datetime.date(2022, 3, 15)),
    ("aten::native_multi_head_self_attention", datetime.date(9999, 1, 1)),
    ("aten::_native_multi_head_self_attention", datetime.date(9999, 1, 1)),
    ("aten::_transform_bias_rescale_qkv", datetime.date(9999, 1, 1)),
]

ALLOW_LIST_COMPILED = [
    (
        re.compile(item[0]),
        item[1],
        re.compile(item[2]) if len(item) > 2 else None,
    ) for item in ALLOW_LIST if item[1] >= datetime.date.today()
]

def allow_listed(schema):
    for item in ALLOW_LIST_COMPILED:
        if item[0].search(str(schema)):
            if len(item) > 2 and item[2] is not None:
                # if arguments regex is present, use it
                return bool(item[2].search(str(schema)))
            return True
    return False


# The nightly will fail to parse newly added syntax to schema declarations
# Add new schemas that will fail the nightly here
dont_parse_list = [
    ("_TorchScriptTesting.*", datetime.date(2099, 9, 17)),
    ("test_backend", datetime.date(2099, 9, 17)),
    ("dist_c10d", datetime.date(2099, 9, 17)),
]

def has_valid_upgraders(schema, version_map):
    # we want to parse through the map to find if
    # the schema has valid upgraders. Since the
    # version map has entry for each overload
    # we need to do some ugly parsing.

    # the name of the operator
    schema_name = schema.name

    if schema_name not in version_map:
        return False

    entries = version_map[schema_name]

    possible_overloads = []
    possible_schemas = []
    for key, upgrader_schema_entries in entries.items():
        possible_overloads.append(key)
        possible_schemas.extend(upgrader_schema_entries)

    # let's make sure this existing schema is part of possible
    # schemas
    for old_schema in possible_schemas:
        if old_schema == schema:
            return True

    return False

def dont_parse(schema_line):
    for item in dont_parse_list:
        if item[1] < datetime.date.today():
            continue
        regexp = re.compile(item[0])
        if regexp.search(schema_line):
            return True
    return False

def load_schemas_to_dict():
    new_schemas = torch._C._jit_get_all_schemas()
    new_schemas += torch._C._jit_get_custom_class_schemas()
    new_schema_dict = defaultdict(list)
    for s in new_schemas:
        new_schema_dict[s.name].append(s)
    return new_schema_dict

def process_version_map(version_map):
    # version map maps full schema name to
    # list of upgraders. Since we only have
    # the name of the schema (aka no overload)
    # we want to first process the map to make
    # the key lookup easier. After this it will be:
    # Dict[schema_name, Dict[overload, List[schema]]]

    output = defaultdict(dict)
    for (key, entries) in version_map.items():
        operator_name = key.split(".")[0]
        schema_entries = [parse_schema(entry.old_schema) for entry in entries]
        output[operator_name][key] = schema_entries
    return output

def check_bc(existing_schemas):
    new_schema_dict = load_schemas_to_dict()
    version_map = process_version_map(torch._C._get_operator_version_map())
    is_bc = True
    broken_ops = []
    for existing_schema in existing_schemas:
        if allow_listed(existing_schema):
            print("schema: ", str(existing_schema), " found on allowlist, skipping")
            continue
        if has_valid_upgraders(existing_schema, version_map):
            print("schema: ", str(existing_schema), " has valid upgrader, skipping")
            continue
        print("processing existing schema: ", str(existing_schema))
        matching_new_schemas = new_schema_dict.get(existing_schema.name, [])
        found = False
        for matching_new_schema in matching_new_schemas:
            if matching_new_schema.is_backward_compatible_with(existing_schema):
                found = True
                break
        if not found:
            print(
                "Can NOT find backward compatible schemas after changes "
                "for schema {} from the following candidates:\n[\n{}\n]".format(
                    str(existing_schema),
                    "\n\t".join(str(s) for s in matching_new_schemas),
                )
            )
            # TODO Print out more details about why candidates don't match.
            broken_ops.append(str(existing_schema))
            is_bc = False
    if is_bc:
        print("Found backward compatible schemas for all existing schemas")
    else:
        print(
            "The PR is introducing backward incompatible changes to the "
            "operator library. Please contact PyTorch team to confirm "
            "whether this change is wanted or not. \n\nBroken ops: "
            "[\n\t{}\n]".format("\n\t".join(broken_ops))
        )
    return is_bc

def check_fc(existing_schemas):
    new_schema_dict = load_schemas_to_dict()
    is_fc = True
    broken_ops = []
    for existing_schema in existing_schemas:
        if allow_listed(existing_schema):
            print("schema: ", str(existing_schema), " found on allowlist, skipping")
            continue
        print("processing existing schema: ", str(existing_schema))
        matching_new_schemas = new_schema_dict.get(existing_schema.name, [])
        found = False
        possible_failure_reasons = []
        for matching_new_schema in matching_new_schemas:
            is_compatible, reason = matching_new_schema.check_forward_compatible_with(existing_schema)
            if is_compatible:
                found = True
                break
            if reason != "":
                possible_failure_reasons.append(reason)
        if not found:
            print(
                "Can NOT find forward compatible schemas after changes "
                "for schema {} from the following candidates:\n[\n{}\n]".format(
                    str(existing_schema),
                    "\n\t".join(str(s) for s in matching_new_schemas),
                )
            )
            print(
                "Refer to following reasons for failure "
                "to find FC schema:\n[\n{}\n]".format(
                    "\n\t".join(str(r) for r in possible_failure_reasons)
                )
            )
            broken_ops.append(str(existing_schema))
            is_fc = False
    if is_fc:
        print("Found forward compatible schemas for all existing schemas")
    else:
        warnings.warn(
            "The PR is introducing a potentially forward incompatible changes to the "
            "operator library. Please contact PyTorch team to confirm "
            "whether this change is wanted or not. \n\nBroken ops: "
            "[\n\t{}\n]".format("\n\t".join(broken_ops))
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Process some integers.")
    parser.add_argument(
        "--existing-schemas",
        help="filename to load existing schemas",
        type=str,
        default="schemas.txt",
    )
    args = parser.parse_args()
    existing_schema_dict = dict()
    slist = []
    with open(args.existing_schemas, "r") as f:
        while True:
            line = f.readline()
            if not line:
                break

            if dont_parse(line.strip()):
                print("Not parsing schema line: ", line.strip())
                continue
            s = parse_schema(line.strip())
            slist.append(s)

    # TODO in case there is FC breaking changes,
    # we just warn for now until there is a policy.
    check_fc(slist)

    if not check_bc(slist):
        sys.exit(1)
