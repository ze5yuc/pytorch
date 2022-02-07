#include <torch/csrc/jit/operator_upgraders/version_map.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace torch {
namespace jit {

// this flag is used to make sure the elements in the version map
// are sorted according to when the upgraders are introduced.
static bool isVersionMapSorted = false;

// Main entry point for all operators that have valid upgraders.
// Note for developers: The list of upgraders should be SORTED
// by the version number where the upgrader is registered.
static std::unordered_map<std::string, std::vector<UpgraderEntry>> operatorVersionMap(
    {{"aten::logspace",
      {{9,
        "logspace_0_8",
        "aten::logspace(Scalar start, Scalar end, int? steps=None, float base=10.0, *, ScalarType? dtype=None, Layout? layout=None, Device? device=None, bool? pin_memory=None) -> Tensor"}}},
     {"aten::logspace.out",
      {{9,
        "logspace_out_0_8",
        "aten::logspace.out(Scalar start, Scalar end, int? steps=None, float base=10.0, *, Tensor(a!) out) -> Tensor(a!)"}}},
     {"aten::linspace",
      {{8,
        "linspace_0_7",
        "aten::linspace(Scalar start, Scalar end, int? steps=None, *, ScalarType? dtype=None, Layout? layout=None, Device? device=None, bool? pin_memory=None) -> Tensor"}}},
     {"aten::linspace.out",
      {{8,
        "linspace_out_0_7",
        "aten::linspace.out(Scalar start, Scalar end, int? steps=None, *, Tensor(a!) out) -> Tensor(a!)"}}},
     {"aten::div.Tensor",
      {{4,
        "div_Tensor_0_3",
        "aten::div.Tensor(Tensor self, Tensor other) -> Tensor"}}},
     {"aten::div.Scalar",
      {{4,
        "div_Scalar_0_3",
        "aten::div.Scalar(Tensor self, Scalar other) -> Tensor"}}},
     {"aten::div.out",
      {{4,
        "div_out_0_3",
        "aten::div.out(Tensor self, Tensor other, *, Tensor(a!) out) -> Tensor(a!)"}}},
     {"aten::div_.Tensor",
      {{4,
        "div__Tensor_0_3",
        "aten::div_.Tensor(Tensor(a!) self, Tensor other) -> Tensor(a!)"}}},
     {"aten::div_.Scalar",
      {{4,
        "div__Scalar_0_3",
        "aten::div_.Scalar(Tensor(a!) self, Scalar other) -> Tensor(a!)"}}},
     {"aten::full",
      {{5,
        "full_0_4",
        "aten::full(int[] size, Scalar fill_value, *, ScalarType? dtype=None, Layout? layout=None, Device? device=None, bool? pin_memory=None) -> Tensor"}}},
     {"aten::full.out",
      {{5,
        "full_out_0_4",
        "aten::full.out(int[] size, Scalar fill_value, *, Tensor(a!) out) -> Tensor(a!)"}}}});

const std::unordered_map<std::string, std::vector<UpgraderEntry>>&
get_operator_version_map() {
  if (!isVersionMapSorted) {
    for (auto entry : operatorVersionMap) {
      std::sort(
          entry.second.begin(),
          entry.second.end(),
          [](const auto& a, const auto& b) {
            return a.bumped_at_version > b.bumped_at_version;
          });
    }
    isVersionMapSorted = true;
  }
  return operatorVersionMap;
}

void test_only_add_entry(const std::string& op_name, UpgraderEntry entry) {
  test_only_reset_flag();
  operatorVersionMap[op_name].push_back(entry);
}

void test_only_remove_entry(const std::string& op_name) {
  test_only_reset_flag();
  operatorVersionMap.erase(op_name);
}

void test_only_reset_flag() {
  isVersionMapSorted = false;
}

} // namespace jit
} // namespace torch
