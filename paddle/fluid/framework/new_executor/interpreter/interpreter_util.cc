// Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/framework/new_executor/interpreter/interpreter_util.h"

#include <algorithm>

#include "paddle/fluid/distributed/auto_parallel/dist_attr.h"
#include "paddle/fluid/framework/details/nan_inf_utils.h"
#include "paddle/fluid/framework/executor_gc_helper.h"
#include "paddle/fluid/framework/new_executor/interpreter/data_transfer.h"
#include "paddle/fluid/framework/new_executor/interpreter/execution_config.h"
#include "paddle/fluid/memory/stats.h"
#include "paddle/fluid/operators/controlflow/conditional_block_op_helper.h"
#include "paddle/fluid/operators/controlflow/recurrent_op_helper.h"
#include "paddle/fluid/operators/controlflow/while_op_helper.h"
#include "paddle/fluid/operators/ops_extra_info.h"
#include "paddle/phi/core/distributed/comm_context_manager.h"
#include "paddle/phi/core/kernel_context.h"
#include "paddle/phi/core/kernel_factory.h"

#ifdef PADDLE_WITH_MKLDNN
#include "paddle/fluid/platform/mkldnn_helper.h"
#endif

PADDLE_DEFINE_EXPORTED_bool(
    new_executor_log_memory_stats,
    false,
    "Log memory stats after each op runs, just used for debug.");

DECLARE_bool(use_mkldnn);
DECLARE_bool(check_nan_inf);

namespace paddle {
namespace framework {
namespace interpreter {

using VariableIdMap = std::map<std::string, std::vector<int>>;

// These Op needs set output dtype when register phi kernel, but they didn't
static std::set<std::string> OpsNeedSetOutputDtypeWhenRegisterPhiKernel = {
    "abs",
    "adam",
    "adamw",
    "all_close",
    "all_raw",
    "any_raw",
    "arg_sort",
    "atan2",
    "auc",
    "clip_by_norm",
    "complex",
    "conv3d_coo",
    "distribute_fpn_proposals",
    "eig",
    "eig_grad",
    "eigh",
    "ftt_c2r",
    "ftt_r2c",
    "fused_matmul",
    "generate_proposals",
    "graph_sample_neighbors",
    "group_norm",
    "histogram",
    "instance_norm",
    "is_empty",
    "kthvalue",
    "lamb",
    "layer_norm",
    "layer_norm_grad",
    "less_equal",
    "less_than",
    "merged_adam",
    "mode",
    "momentum",
    "multiclass_nms3",
    "multinomial",
    "nanmedian",
    "rnn",
    "search_sort",
    "select",
    "send_recv",
    "send_ue_recv",
    "sync_batch_norm_grad",
    "unique",
    "unique_consecutive_flattened_tensor",
    "unique_raw",
    "viterbi_devode"};

// These Ops can use InferMeta to infer the output dtype
static std::set<std::string> OpsWithAvailablePhiInferMeta = {
    "abs", "adam", "adamw", "layer_norm", "layer_norm_grad", "merged_adam"};

// Cannot static analysis these Ops' output dtype or backend because their
// kernels have not moved to PHI yet.
static std::set<std::string> OpsWithFluidKernelNeedMoveToPhi = {
    "fused_batch_norm_act", "fused_batch_norm_act_grad"};

// NOTE(Ruibiao): SingleStreamGuard make some multi-strem op (i.e.,
// c_allreduce_sum) run in single stream. It is dedicated to BuildOpFuncList
// which run kernel without stream synchronization.
class SingleStreamGuard {
 public:
  explicit SingleStreamGuard(std::shared_ptr<OperatorBase>& op) : op_(op) {
    if (op_->Type() == "c_allreduce_sum" &&
        op_->Attr<bool>("use_calc_stream") == false) {
      VLOG(6) << "Set c_allredce_sum's attr use_calc_stream to true";
      op_->SetAttr("use_calc_stream", true);
      is_changed = true;
    }
  }

  ~SingleStreamGuard() {
    if (!is_changed) {
      return;
    }

    if (op_->Type() == "c_allreduce_sum") {
      op_->SetAttr("use_calc_stream", false);
      VLOG(6) << "Set c_allredce_sum's attr use_calc_stream to false";
    }
  }

  DISABLE_COPY_AND_ASSIGN(SingleStreamGuard);

 private:
  bool is_changed{false};
  std::shared_ptr<OperatorBase> op_;
};

const std::vector<WorkQueueOptions> ConstructWorkQueueOptions(
    size_t host_num_threads, size_t device_num_threads, EventsWaiter* waiter) {
  std::vector<WorkQueueOptions> group_options;
  // for execute host Kernel
  group_options.emplace_back(/*name*/ "HostTasks",
                             /*num_threads*/ host_num_threads,
                             /*allow_spinning*/ true,
                             /*always_spinning*/ false,
                             /*track_task*/ false,
                             /*detached*/ true,
                             /*events_waiter*/ waiter);
  // for launch device Kernel
  group_options.emplace_back(/*name*/ "DeviceKernelLaunch",
                             /*num_threads*/ device_num_threads,
                             /*allow_spinning*/ true,
                             /*always_spinning*/ false,
                             /*track_task*/ false,
                             /*detached*/ true,
                             /*events_waiter*/ waiter);
  return group_options;
}

AsyncWorkQueue::AsyncWorkQueue(size_t host_num_threads,
                               size_t device_num_threads,
                               EventsWaiter* waiter)
    : host_num_thread_(host_num_threads) {
  queue_group_ = CreateWorkQueueGroup(
      ConstructWorkQueueOptions(host_num_threads, device_num_threads, waiter));
}

void AsyncWorkQueue::AddTask(const OpFuncType& op_func_type,
                             std::function<void()> fn) {
  // queue_idx=0 : kCpuSync or kGpuSync
  // queue_idx=1 : kGPUAsync
  queue_group_->AddTask(op_func_type == OpFuncType::kGpuAsync, std::move(fn));
}

bool BlockCanBeStaticBuilt(const framework::BlockDesc& block) {
  // has_fluid_kernel = (kernelCode >> 3) & 1
  // has_structed_kernel = (kernelCode >> 2) & 1
  // need_move_to_phi = (kernelCode >> 1) & 1
  // need_set_dtype =  KernelCode & 1
  using KernelCode = int8_t;
  std::set<std::pair<std::string, KernelCode>> invalid_ops;
  for (auto& op : block.AllOps()) {
    auto op_type = op->Type();
    bool has_fluid_kernel = OperatorWithKernel::AllOpKernels().count(op_type);
    bool has_structured_kernel =
        phi::KernelFactory::Instance().HasStructuredKernel(op_type);
    bool need_move_to_phi = (has_fluid_kernel || has_structured_kernel) &&
                            OpsWithFluidKernelNeedMoveToPhi.count(op_type);
    bool need_set_dtype =
        !has_fluid_kernel && !has_structured_kernel &&
        OpsNeedSetOutputDtypeWhenRegisterPhiKernel.count(op_type) &&
        !OpsWithAvailablePhiInferMeta.count(op_type);

    KernelCode kernel_code = (has_fluid_kernel << 3) +
                             (has_structured_kernel << 2) +
                             (need_move_to_phi << 1) + need_set_dtype;
    if (need_move_to_phi || need_set_dtype) {
      invalid_ops.insert(std::make_pair(op_type, kernel_code));
    }
  }

  if (!invalid_ops.empty()) {
    std::stringstream ss;
    ss << "The following OPs are unable to static build:\n";
    for (auto& item : invalid_ops) {
      ss << item.first << " [has_fluid_kernel = " << (item.second >> 3 & 1)
         << ", has_structed_kerenl = " << (item.second >> 2 & 1)
         << ", need_move_to_phi = " << (item.second >> 1 & 1)
         << ", need_set_dtype = " << (item.second & 1) << "]\n";
    }
    VLOG(0) << ss.str();
  }

  return invalid_ops.empty();
}

bool IsCommunicationOp(const std::string& op_name) {
  const std::set<std::string> special_comm_op_set = {
      "send",
      "recv",
      "send_v2",
      "recv_v2",
  };
  const std::string communication_op_prefix = "c_";
  if (op_name.find(communication_op_prefix) != std::string::npos ||
      special_comm_op_set.count(op_name)) {
    return true;
  }
  return false;
}

bool IsCommunicationOp(const Instruction& instr) {
  return IsCommunicationOp(instr.OpBase()->Type());
}

bool IsCpuOp(const Instruction& instr) {
  return platform::is_cpu_place(instr.DeviceContext().GetPlace());
}

bool IsGradOp(const std::string& op_name) {
  return paddle::string::ends_with(op_name, "_grad");
}

bool IsSupportedHeterPlace(const phi::Place& place) {
  return platform::is_gpu_place(place) || platform::is_npu_place(place) ||
         platform::is_xpu_place(place) || platform::is_ipu_place(place) ||
         platform::is_custom_place(place);
}

bool IsMemcpyD2H(const Instruction& instr) {
  return instr.OpBase()->Type() == kMemcpyD2H;
}

bool IsMemcpyH2D(const Instruction& instr) {
  return instr.OpBase()->Type() == kMemcpyH2D;
}

bool IsMemcpyOp(const Instruction& instr) {
  return IsMemcpyD2H(instr) || IsMemcpyH2D(instr);
}

void AddFetch(const std::vector<std::string>& fetch_names,
              framework::BlockDesc* block) {
  auto* fetch_holder = block->Var(kFetchVarName);
  fetch_holder->SetType(proto::VarType::FETCH_LIST);
  fetch_holder->SetPersistable(true);

  int i = 0;
  for (auto& fetch_name : fetch_names) {
    // append fetch op
    auto* op = block->AppendOp();
    op->SetType("fetch_v2");
    op->SetInput("X", {fetch_name});
    op->SetOutput("Out", {kFetchVarName});
    op->SetAttr("col", {static_cast<int>(i)});
    op->CheckAttrs();
    i++;
  }
}

bool var_can_be_deleted(const std::string& name, const BlockDesc& block) {
  auto* var_desc = block.FindVar(name);
  if (var_desc == nullptr || var_desc->Persistable()) {
    return false;
  }

  auto type = var_desc->Proto()->type().type();

  return type == proto::VarType::LOD_TENSOR ||
         type == proto::VarType::SELECTED_ROWS ||
         type == proto::VarType::LOD_TENSOR_ARRAY;
}

std::unordered_map<const paddle::framework::OperatorBase*,
                   std::vector<std::string>>
GetUnusedVars(const BlockDesc& block,
              const std::vector<std::shared_ptr<OperatorBase>>& ops) {
  std::unordered_map<std::string, size_t> var_op_idx_map;

  for (size_t i = 0; i < ops.size(); ++i) {
    const auto& op = ops[i];

    OpInOutInfo info;
    for (auto& name_pair : op->Inputs()) {
      for (auto& name : name_pair.second) {
        if (!var_can_be_deleted(name, block)) {
          continue;
        }

        // var can be gc-ed
        if (!info.IsBuilt()) {
          info.Build(op.get());
        }

        if (info.IsInArgBufferNeeded(name)) {
          // Update the last living op of variable to current op
          var_op_idx_map[name] = i;
        } else {
          VLOG(10) << "Skip reference count computing of variable "
                   << name_pair.first << "(" << name << ") in Operator "
                   << op->Type();
        }
      }
    }

    for (auto& name_pair : op->Outputs()) {
      for (auto& name : name_pair.second) {
        if (var_can_be_deleted(name, block)) {
          // Update the last living op of variable to current op
          var_op_idx_map[name] = i;
        }
      }
    }
  }

  std::unordered_map<const OperatorBase*, std::vector<std::string>> result;
  for (auto& name_op_idx_pair : var_op_idx_map) {
    auto& name = name_op_idx_pair.first;
    size_t op_idx = name_op_idx_pair.second;
    auto op = ops[op_idx].get();
    result[op].emplace_back(name);
    VLOG(4) << op->Type() << " " << name;
  }
  VLOG(4) << "gc map size:" << result.size();
  return result;
}

OpFuncType AnalyseOpFuncType(const OpFuncNode& op_func_node,
                             const platform::Place& place) {
  if (platform::is_cpu_place(place)) {
    return OpFuncType::kCpuSync;
  }

  PADDLE_ENFORCE_EQ(IsSupportedHeterPlace(place),
                    true,
                    phi::errors::Fatal("Unsupported current place %s", place));

  // Some GPU OPs do not launch CUDA Kernel, but spend a lot of time on CPU
  // computing. They execute serially in device thread and block CUDA kernel
  // launching in other GPU OPs. To improve performance, set them as kGpuSync
  // and so that they would be dispatched to host thread.
  std::shared_ptr<OperatorBase> op = op_func_node.operator_base_;
  if (op->Type() == kCoalesceTensor &&
      op->Attr<bool>("set_constant") == false &&
      op->Attr<bool>("copy_data") == false) {
    return OpFuncType::kGpuSync;
  }

  // for memcpy explicitly called by user
  if (platform::is_gpu_place(place) && op->Type() == interpreter::kMemcpyD2H) {
    return OpFuncType::kGpuSync;
  }

  if (op->Type() == "shape") {
    return OpFuncType::kGpuSync;
  }
  return OpFuncType::kGpuAsync;
}

void CreateAllOps(const framework::BlockDesc& block,
                  std::vector<std::unique_ptr<OperatorBase>>* ops) {
  for (auto& op : block.AllOps()) {
    auto op_type = op->Type();
    VLOG(8) << "CreateOp from : " << op_type;

    auto& info = OpInfoMap::Instance().Get(op_type);

    const VariableNameMap& inputs_names = op->Inputs();
    const VariableNameMap& outputs_names = op->Outputs();

    AttributeMap op_attr_map = op->GetAttrMap();
    AttributeMap op_runtime_attr_map = op->GetRuntimeAttrMap();

    if (info.Checker() != nullptr) {
      info.Checker()->Check(&op_attr_map);
    }

    const auto& extra_attr_checkers =
        operators::ExtraInfoUtils::Instance().GetExtraAttrsChecker(op_type);
    for (const auto& checker : extra_attr_checkers) {
      checker(&op_runtime_attr_map, false);
    }

    auto op_base =
        info.Creator()(op_type, inputs_names, outputs_names, op_attr_map);
    op_base->SetRuntimeAttributeMap(op_runtime_attr_map);

#ifdef PADDLE_WITH_MKLDNN
    if (FLAGS_use_mkldnn) {
      if (op->HasAttr("use_mkldnn")) {
        VLOG(4) << "Set use_mkldnn=True for " << op_base->Type();
        op_base->SetAttr("use_mkldnn", true);
      }
    }
#endif

    ops->emplace_back(std::unique_ptr<OperatorBase>(op_base));
  }
}

std::tuple<VariableValueMap, VariableIdMap> BuildVariableMap(
    const VariableNameMap& var_name_map,
    VariableScope* var_scope,
    Scope* local_scope,
    bool find_var_recursively = false,
    bool allow_var_not_in_scope = false) {
  VariableValueMap name2var;
  VariableIdMap name2id;
  for (auto& item : var_name_map) {
    std::vector<Variable*> vars;
    std::vector<int> ids;
    vars.reserve(item.second.size());

    for (auto& var_name : item.second) {
      auto* var = local_scope->FindVar(var_name);

      if (!var_scope->HasVar(var_name)) {
        if (find_var_recursively && var) {
          VLOG(3) << "Add " << var_name << " to var_scope";
          var_scope->AddVar(var_name, nullptr);
        } else if (allow_var_not_in_scope) {
          VLOG(4) << var_name << " don't exist in variable scope, skip it!";
          continue;
        }
      }
      auto var_id = var_scope->VarId(var_name);
      vars.push_back(var);
      ids.push_back(var_id);
    }
    name2var[item.first] = std::move(vars);
    name2id[item.first] = std::move(ids);
  }
  return std::make_tuple(name2var, name2id);
}

void ApplyDeviceGuard(const OperatorBase* op_base,
                      const platform::Place& place,
                      OpKernelType* expected_kernel_key) {
  bool need_change_place =
      (op_base->HasAttr("op_device") &&
       (op_base->Attr<std::string>("op_device").length() > 0));
  if (need_change_place) {
    auto& op_device = op_base->Attr<std::string>("op_device");
    if (op_device == "cpu" || platform::is_cpu_place(place)) {
      VLOG(3) << "Switch into CPUPlace by device_guard.";
      expected_kernel_key->place_ = platform::CPUPlace();
    } else if (op_device.find("gpu") != std::string::npos &&
               platform::is_gpu_place(place)) {
      // when the Op that does not have GPUKernel is assigned to GPU, the
      // CPUKernel will be executed and a warning will be given at the same
      // time.
      if (op_base->SupportGPU()) {
        expected_kernel_key->place_ = place;
      } else {
        expected_kernel_key->place_ = platform::CPUPlace();
        LOG_FIRST_N(WARNING, 1)
            << "Op(" << op_base->Type()
            << ") has no CUDA implementation. It will be assigned to CPUPlace.";
      }
      VLOG(3) << "Switch into " << expected_kernel_key->place_
              << " by device_guard.";
    } else if (op_device.find("npu") != std::string::npos &&
               platform::is_npu_place(place)) {
      // when the Op that does not have NPUKernel is assigned to NPU, the
      // CPUKernel will be executed and a warning will be given at the same
      // time.
      if (op_base->SupportNPU()) {
        expected_kernel_key->place_ = place;
      } else {
        expected_kernel_key->place_ = platform::CPUPlace();
        LOG_FIRST_N(WARNING, 1)
            << "Op(" << op_base->Type()
            << ") has no NPU implementation. It will be assigned to CPUPlace.";
      }
      VLOG(3) << "Switch into " << expected_kernel_key->place_
              << " by device_guard.";
    } else if (op_device.find("xpu") != std::string::npos &&
               platform::is_xpu_place(place)) {
      // when the Op that does not have XPUKernel is assigned to XPU, the
      // CPUKernel will be executed and a warning will be given at the same
      // time.
      if (op_base->SupportXPU()) {
        expected_kernel_key->place_ = place;
      } else {
        expected_kernel_key->place_ = platform::CPUPlace();
        LOG_FIRST_N(WARNING, 1)
            << "Op(" << op_base->Type()
            << ") has no XPU implementation. It will be assigned to CPUPlace.";
      }
      VLOG(3) << "Switch into " << expected_kernel_key->place_
              << " by device_guard.";
    } else {
      PADDLE_THROW(
          platform::errors::Fatal("Unsupported current place %s", op_device));
    }
  }
}

void HandleOperatorBase(const platform::Place& place,
                        const VariableScope* var_scope,
                        std::shared_ptr<OperatorBase> op_base,
                        OpFuncNode* op_func_node,
                        Scope* local_scope) {
  platform::DeviceContextPool& pool = platform::DeviceContextPool::Instance();
  auto* dev_ctx = pool.Get(place);
  // input, output is prepared. set the other attributes.
  op_func_node->operator_base_ = op_base;
  op_func_node->type_ = AnalyseOpFuncType(*op_func_node, place);
  op_func_node->kernel_func_ = nullptr;
  op_base->Run(*local_scope, place);  // Run without data transformer.
  op_func_node->dev_ctx_ = dev_ctx;
}

void BuildOpFuncList(const platform::Place& place,
                     const framework::BlockDesc& block,
                     const std::set<std::string>& skip_gc_vars,
                     std::vector<OpFuncNode>* vec_func_list,
                     VariableScope* var_scope,
                     const ExecutionConfig& execution_config,
                     bool use_local_scope,
                     bool static_build) {
  Scope* local_scope = use_local_scope ? var_scope->GetMutableLocalScope()
                                       : var_scope->GetMutableScope();
  std::vector<std::unique_ptr<OperatorBase>>
      ops_unique;  // its elements will be moved to vec_func_list
  // Step 1: create all ops for current block.
  CreateAllOps(block, &ops_unique);

  VLOG(4) << "Static build: " << static_build;

  if (!execution_config.used_for_jit) {
    // If gc is enabled and block size > 1
    const ProgramDesc& main_program = *block.Program();
    operators::PrepareSafeEagerDeletionOnConditionalOpAndConditionalGradOp(
        main_program, block.ID(), ops_unique);
    operators::PrepareSafeEagerDeletionOnWhileOpAndWhileGradOp(
        main_program, block.ID(), ops_unique);
    operators::PrepareSafeEagerDeletionOnRecurrentOpAndRecurrentGradOp(
        main_program, block.ID(), ops_unique);
  }

#ifdef PADDLE_WITH_MKLDNN
  platform::RegisterModelLayout(ops_unique, place);
#endif
  // its elements will be moved to vec_func_list
  std::vector<std::shared_ptr<OperatorBase>> ops;
  for (auto& op_unique : ops_unique) {
    ops.emplace_back(std::move(op_unique));
  }
  auto unused_var_map = GetUnusedVars(block, ops);

  bool flag_log_is_printed = false;
  for (size_t i = 0; i < ops.size(); ++i) {
    auto op = ops[i].get();
    const std::string& op_type = op->Type();

    VLOG(6) << "Build OpFuncNode from : " << op_type;

    // Print new executor log if grad op is used.
    // It's only for test and will be removed later.
    if (!flag_log_is_printed && op_type.find("_grad") != std::string::npos) {
      LOG_FIRST_N(INFO, 1) << "Standalone Executor is Used.";
      flag_log_is_printed = true;
    }

    // Hot fix for variables used in dataloader, like
    // 'lod_tensor_blocking_queue_0'. These variables may be created in scope,
    // and it is not existed as variable in program.
    const std::set<std::string> ops_with_var_not_in_program = {
        "create_py_reader"};
    const std::set<std::string> ops_with_var_not_in_scope = {
        "conditional_block",
        "conditional_block_grad",
        "recurrent_grad",
        "rnn_memory_helper",
        "rnn_memory_helper_grad",
        "while",
        "while_grad"};
    bool allow_var_not_in_program = ops_with_var_not_in_program.count(op_type);
    bool allow_var_not_in_scope = ops_with_var_not_in_scope.count(op_type);

    // ops in the control flow block may not find its inputs or outputs
    // in VariableScope of the sub-block, so we need search it in parent scope.

    framework::VariableNameMap& input_name_map = op->Inputs();
    VariableValueMap ins_map;
    VariableIdMap ins_name2id;
    std::tie(ins_map, ins_name2id) = BuildVariableMap(
        input_name_map,
        var_scope,
        local_scope,
        execution_config.used_for_control_flow_op || allow_var_not_in_program,
        allow_var_not_in_scope);

    framework::VariableNameMap& output_name_map = op->Outputs();
    VariableValueMap outs_map;
    VariableIdMap outs_name2id;
    std::tie(outs_map, outs_name2id) =
        BuildVariableMap(output_name_map,
                         var_scope,
                         local_scope,
                         execution_config.used_for_control_flow_op,
                         allow_var_not_in_scope);

    // step 1: build OpFuncNode
    OpFuncNode op_func_node;
    op_func_node.operator_base_ = ops[i];
    op_func_node.input_index = ins_name2id;
    op_func_node.output_index = outs_name2id;

    const OperatorDistAttr* dist_attr = block.Op(i)->DistAttr();
    if (dist_attr) {
      if (dist_attr->execution_stream() !=
          distributed::auto_parallel::kDefault) {
        op_func_node.execution_stream_ = dist_attr->execution_stream();
      }
      op_func_node.stream_priority_ = dist_attr->stream_priority();
      op_func_node.scheduling_priority_ = dist_attr->scheduling_priority();
    } else {
      if (interpreter::IsCommunicationOp(op_type)) {
        // NOTE(Ruibiao): Dispatching computation before communication improves
        // multi-stream overlap when the time cost of communication less than
        // that of the calculation (e.g., ResNet50_bs128_pure_fp16 N4C32
        // training).
        op_func_node.scheduling_priority_ = 1;
      }
    }

    VLOG(6) << op_type
            << " : [execution_stream, stream_priority, scheduling_priority] = ["
            << op_func_node.execution_stream_ << ", "
            << op_func_node.stream_priority_ << ", "
            << op_func_node.scheduling_priority_ << "]";

    SingleStreamGuard single_stream_guard(ops[i]);

    VLOG(4) << "Start run " << place << " " << op->DebugStringEx(local_scope);

#ifdef PADDLE_WITH_ASCEND_CL
    // NOTE(wangxi): nan/inf cannot be detected on NPU by checking the variable
    // values, but only through special `float_status` to checks whether
    // the operation is overflow. More about `float_status`, see:
    // https://gitee.com/ascend/modelzoo/issues/I3NF8V?from=project-issue
    if (FLAGS_check_nan_inf) {
      framework::details::NPUAllocAndClearFloatStatus(*op, *local_scope, place);
    }
#endif

    try {
      if (dynamic_cast<framework::OperatorWithKernel*>(op) == nullptr) {
        VLOG(4) << "HandleOperatorBase";
        // op is not a operatorwithkernel, so direcly run OperatorBase::Run()
        HandleOperatorBase(
            place, var_scope, ops[i], &op_func_node, local_scope);
        vec_func_list->emplace_back(op_func_node);
      } else {
        VLOG(4) << "OP is not null";
        auto op_with_kernel = const_cast<framework::OperatorWithKernel*>(
            static_cast<const framework::OperatorWithKernel*>(op));
        VLOG(4) << "get op_with_kernel";
        // construct RuntimeContext and analysis KernelType
        RuntimeContext runtime_context({}, {});
        runtime_context.inputs.swap(ins_map);
        runtime_context.outputs.swap(outs_map);
        VLOG(4) << "get RuntimeContext";

        Scope scope, *runtime_scope = &scope;
        // NOTE(Ruibiao): We do not encourage directly using scope in OP kernel.
        // But some OPs do have such behavior (e.g., cinn_launch OP). Here
        // special treatment for them.
        if (op_with_kernel->Type() == "cinn_launch" ||
            op_with_kernel->Type() == "cinn_instruction_run") {
          VLOG(6) << "OP(" << op_with_kernel->Type()
                  << ") use scope in kernel, "
                     "so pass a real scope to "
                     "ExecutionContext";
          runtime_scope = local_scope;
        }

        auto& pool = platform::DeviceContextPool::Instance();
        auto* dev_ctx = pool.Get(place);
        SetDeviceCommContext(op, dev_ctx);
        auto exec_ctx = ExecutionContext(
            *op_with_kernel, *runtime_scope, *dev_ctx, runtime_context);
        auto expected_kernel_key = framework::TransPhiKernelKeyToOpKernelType(
            op_with_kernel->GetExpectedKernelType(exec_ctx));
#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
        if (op_with_kernel->CanCUDNNBeUsed(exec_ctx,
                                           expected_kernel_key.data_type_)) {
          expected_kernel_key.library_type_ = framework::LibraryType::kCUDNN;
        }
#endif
        VLOG(4) << "expected_kernel_key : " << expected_kernel_key;
        // change device by the device_guard()
        ApplyDeviceGuard(op, place, &expected_kernel_key);
        if (platform::places_are_same_class(exec_ctx.GetPlace(),
                                            expected_kernel_key.place_)) {
          expected_kernel_key.place_ = exec_ctx.GetPlace();
        }

        // step 2. select op kernel
        auto run_phi_kernel = false;
        if (phi::KernelFactory::Instance().HasCompatiblePhiKernel(
                op_with_kernel->Type())) {
          auto phi_kernel_key = op_with_kernel->ChoosePhiKernel(exec_ctx);
          auto phi_kernel_name = op_with_kernel->PhiKernelSignature()->name;
          bool in_custom_back_list = false;
#ifdef PADDLE_WITH_CUSTOM_DEVICE
          in_custom_back_list =
              phi::backends::custom_device::is_in_custom_black_list(
                  phi_kernel_name);
#endif
          if (op_with_kernel->PhiKernel()->IsValid() && !in_custom_back_list) {
            run_phi_kernel = true;
          } else {
            if ((!op_with_kernel->SupportsKernelType(expected_kernel_key,
                                                     exec_ctx)) ||
                in_custom_back_list) {
              std::string info = in_custom_back_list ? "fluid in black list "
                                                     : "fluid missing ";
              VLOG(3) << info << phi_kernel_key
                      << " kernel: " << phi_kernel_name;
              auto phi_cpu_kernel_key =
                  FallBackToCpu(phi_kernel_key, *op_with_kernel);
              op_with_kernel->ResetPhiKernel(
                  new phi::Kernel(phi::KernelFactory::Instance().SelectKernel(
                      phi_kernel_name, phi_cpu_kernel_key)));
              if (op_with_kernel->PhiKernel()->IsValid()) {
                VLOG(6) << "Static graph mode PrepareImpl - kernel name: "
                        << phi_kernel_name
                        << " | kernel key: " << phi_cpu_kernel_key
                        << " | kernel: " << *(op_with_kernel->PhiKernel());
                op_with_kernel->ResetKernelType(new OpKernelType(
                    TransPhiKernelKeyToOpKernelType(phi_cpu_kernel_key)));
                run_phi_kernel = true;
              }
            }
          }
        }

        VLOG(4) << "if run phi kernel? : " << run_phi_kernel;
        if (!run_phi_kernel) {
          op_with_kernel->ChooseKernel(exec_ctx);
          op_func_node.kernel_func_ = *op_with_kernel->kernel_func();
        } else {
          op_func_node.phi_kernel_ = op_with_kernel->PhiKernel();
        }
        auto kernel_type = *(op_with_kernel->kernel_type());
        if (kernel_type.place_ != dev_ctx->GetPlace()) {
          dev_ctx = pool.Get(kernel_type.place_);
        }
        op_func_node.dev_ctx_ = dev_ctx;
        op_func_node.type_ =
            AnalyseOpFuncType(op_func_node, kernel_type.place_);

        VLOG(3) << op_with_kernel->Type()
                << " : finally selected kernel_key: " << kernel_type;

        // step 3. data transform
        VariableValueMap& ins_map_temp = runtime_context.inputs;
        VariableValueMap& outs_map_temp = runtime_context.outputs;
        ApplyDataTransform(kernel_type,
                           place,
                           &ins_map_temp,
                           &outs_map_temp,
                           var_scope,
                           &op_func_node,
                           vec_func_list,
                           use_local_scope,
                           static_build);
        VLOG(4) << "apply data transform done. ";
        // step 4. infershape, see OperatorWithKernel::RunImpl in operator.cc
        // for why.
        if (!(op->HasAttr(kAllKernelsMustComputeRuntimeShape) &&
              op->Attr<bool>(kAllKernelsMustComputeRuntimeShape))) {
          VLOG(4) << "infer shape";
          RuntimeInferShapeContext infer_shape_ctx(*op, runtime_context);
          // TODO(Aurelius84): In case of control flow ops, they are NOT
          // inheritted from OperatorWithKernel.
          op_with_kernel->Info().infer_shape_(&infer_shape_ctx);
        }

        // step 5. run kernel
        if (run_phi_kernel &&
            op_func_node.phi_kernel_->GetKernelRegisteredType() ==
                phi::KernelRegisteredType::FUNCTION) {
          VLOG(6) << op_type << " run function kernel";
          if (static_build) {
            FakeInitializeOutputsForFunctionKernel(
                *(op_func_node.phi_kernel_),
                *(op_with_kernel->PhiKernelSignature()),
                runtime_context,
                *dev_ctx);
          } else {
            phi::KernelContext phi_kernel_context;
            op_with_kernel->BuildPhiKernelContext(
                runtime_context, dev_ctx, &phi_kernel_context);
            (*op_func_node.phi_kernel_)(&phi_kernel_context);
          }
        } else if (run_phi_kernel &&
                   op_func_node.phi_kernel_->GetKernelRegisteredType() ==
                       phi::KernelRegisteredType::STRUCTURE) {
          VLOG(6) << op_type << " run structure kernel";
          ExecutionContext execution_context(
              *op_with_kernel, *runtime_scope, *dev_ctx, runtime_context);
          if (static_build) {
            FakeInitializeOutputsForStructureKernel(kernel_type,
                                                    &execution_context);
          } else {
            (*op_func_node.phi_kernel_)(&execution_context);
          }
        } else {
          VLOG(6) << op_type << " run fluid kernel";
          // the place of exec_ctx maybe has changed.
          ExecutionContext execution_context(
              *op_with_kernel, *runtime_scope, *dev_ctx, runtime_context);
          if (static_build) {
            FakeInitializeOutputsForStructureKernel(kernel_type,
                                                    &execution_context);
          } else {
            op_func_node.kernel_func_(execution_context);
          }
        }

        // for debug nan/inf
        if (FLAGS_check_nan_inf) {
          VLOG(4) << "Check nan/inf";
          framework::details::CheckOpHasNanOrInf(*op, *runtime_scope, place);
        }

        vec_func_list->emplace_back(op_func_node);

        if (!op_func_node.inplace_back_map.empty()) {
          auto& m = op_func_node.inplace_back_map;
          // NOTE(zhiqiu): same logic as TransferInplaceVarsBack() in
          // operator.cc
          for (auto& p : m) {
            auto* transformed_tensor =
                GetMutableLoDTensorOrSelectedRowsValueFromVar(
                    local_scope->FindVar(var_scope->GetNameById(p.first)));
            auto* original_tensor =
                GetMutableLoDTensorOrSelectedRowsValueFromVar(
                    local_scope->FindVar(var_scope->GetNameById(p.second)));
            original_tensor->ShareDataWith(*transformed_tensor);
            VLOG(4) << "Transfer inplace variable back form "
                    << var_scope->GetNameById(p.first) << " to "
                    << var_scope->GetNameById(p.second);
          }
        }

        // post-process grad_op.outputs if need cast complex grad into real
        // grad.
        // NOTE(Aurelius84): insert a transfer_dtype_op inplacely to cast it.
        if (IsGradOp(op_type) &&
            framework::IsComplexType(kernel_type.data_type_)) {
          interpreter::HandleComplexGradToRealGrad(op_func_node,
                                                   place,
                                                   output_name_map,
                                                   &runtime_context.outputs,
                                                   var_scope,
                                                   vec_func_list,
                                                   local_scope,
                                                   static_build);
        }
      }
    } catch (platform::EnforceNotMet& ex) {
      framework::InsertCallStackInfo(op_type, op->Attrs(), &ex);
      throw std::move(ex);
    } catch (platform::EOFException&) {
      std::rethrow_exception(std::current_exception());
    } catch (std::exception& ex) {
      LOG(WARNING) << op_type << " raises an exception "
                   << platform::demangle(typeid(ex).name()) << ", "
                   << ex.what();
      std::rethrow_exception(std::current_exception());
    } catch (...) {
      LOG(WARNING) << op_type << " raises an unknown exception";
      std::rethrow_exception(std::current_exception());
    }

    VLOG(4) << "End run " << place << " "
            << op_func_node.operator_base_->DebugStringEx(local_scope);

    // gc---------------------------------------------
    auto iter = unused_var_map.find(op);
    if (iter == unused_var_map.end()) {
      interpreter::LogDeviceMemoryStats(place);
      continue;
    }

    auto& delete_vars = iter->second;
    std::deque<std::shared_ptr<memory::Allocation>>* garbages =
        new std::deque<std::shared_ptr<memory::Allocation>>();

    for (auto& var_name : delete_vars) {
      auto* var = local_scope->FindVar(var_name);
      if (var == nullptr || skip_gc_vars.find(var_name) != skip_gc_vars.end()) {
        continue;
      }

      VLOG(6) << "Erase variable " << var_name;
      if (var->IsType<phi::DenseTensor>()) {
        garbages->emplace_back(
            var->GetMutable<phi::DenseTensor>()->MoveMemoryHolder());
      }
    }
    delete garbages;  // free mem

    interpreter::LogDeviceMemoryStats(place);
  }
}

void BuildVariableScope(const framework::BlockDesc& block,
                        const ExecutionConfig& execution_config,
                        VariableScope* var_scope) {
  VLOG(3) << "Creating Variables";
  auto inner_scope = var_scope->GetMutableScope();

  // NOTE(zhiqiu): if create_local_scope_ is true, the persistable is
  // created in var_scope.scope_ , and other scope is created in local scope.
  Scope* local_scope = execution_config.create_local_scope
                           ? var_scope->GetMutableLocalScope()
                           : var_scope->GetMutableScope();

  for (auto& var_desc : block.AllVars()) {
    auto var_name = var_desc->Name();
    // TODO(xiongkun): user may create a variable with name that exists before.
    // under such circumstances, we should raise a error. Currently we can't
    // get the var_desc of startup_program, so leave it later.
    if (var_name == framework::kEmptyVarName) {
      continue;
    }

    if (var_desc->Persistable() ||
        execution_config.force_root_scope_vars.count(var_name)) {
      // In principle, we should put all trainable parameters in global scope,
      // which means the root of the scope tree. Some cases like quantization
      // will look up these parameters in global scope.
      const Scope* ancestor_scope = inner_scope;
      while (ancestor_scope->parent()) {
        ancestor_scope = ancestor_scope->parent();
      }
      auto* ptr = const_cast<Scope*>(ancestor_scope)->Var(var_name);

      // NOTE(zhiqiu): if var exists in scope and the type is right,
      // InitializeVariable will not create a new variable.
      InitializeVariable(ptr, var_desc->GetType());
      VLOG(3) << "Create Variable " << var_name << " global, which pointer is "
              << ptr << " type is " << static_cast<int>(var_desc->GetType());
    } else {
      auto* ptr = local_scope->Var(var_name);
      InitializeVariable(ptr, var_desc->GetType());
      VLOG(3) << "Create Variable " << var_name << " locally, which pointer is "
              << ptr << " type is " << static_cast<int>(var_desc->GetType());
    }
    var_scope->AddVar(var_name, var_desc);
  }
}

phi::TensorBase* GetTensorFormVar(framework::Variable* var) {
  if (var) {
    if (var->template IsType<phi::DenseTensor>()) {
      return var->template GetMutable<phi::DenseTensor>();
    } else if (var->template IsType<phi::SelectedRows>()) {
      return var->template GetMutable<phi::SelectedRows>();
    } else if (var->template IsType<phi::SparseCooTensor>()) {
      return var->template GetMutable<phi::SparseCooTensor>();
    } else if (var->template IsType<framework::LoDTensorArray>()) {
      return var->template GetMutable<framework::LoDTensorArray>();
    } else if (var->template IsType<framework::Strings>()) {
      return var->template GetMutable<framework::Strings>();
    } else if (var->template IsType<paddle::framework::RawTensor>()) {
      return var->template GetMutable<paddle::framework::RawTensor>();
    } else if (!var->IsInitialized()) {
      // The following is for RAW type of var
      return var->template GetMutable<paddle::framework::RawTensor>();
    } else {
      PADDLE_THROW(platform::errors::Unimplemented(
          "Unsupported `%s` type when get tensor.",
          framework::ToTypeName(var->Type())));
    }
  } else {
    VLOG(4) << "Var is nullptr";
    return nullptr;
  }
}

void FakeInitializeTensor(const platform::DeviceContext& dev_ctx,
                          const phi::DataType& dtype,
                          const phi::Place& place,
                          phi::TensorBase* tensor) {
  PADDLE_ENFORCE_NOT_NULL(
      tensor,
      phi::errors::InvalidArgument(
          "The tensor to fake intialize should not be null."));
  if (place == phi::CPUPlace()) {
    dev_ctx.HostAlloc(tensor,
                      dtype,
                      /*requested_size=*/0,
                      /*fake_alloc=*/true);
  } else {
    PADDLE_ENFORCE_EQ(
        place,
        dev_ctx.GetPlace(),
        phi::errors::Unavailable("The place %s for fack alloc is not equal to "
                                 "the place %s of DeviceContext.",
                                 place,
                                 dev_ctx.GetPlace()));
    dev_ctx.Alloc(tensor,
                  dtype,
                  /*requested_size=*/0,
                  /*pinned=*/false,
                  /*fake_alloc=*/true);
  }
}

void FakeInitializeOutputsForFunctionKernel(
    const phi::Kernel& phi_kernel,
    const phi::KernelSignature& kernel_sig,
    const RuntimeContext& ctx,
    const platform::DeviceContext& dev_ctx) {
  std::string op_name = std::string(kernel_sig.name);
  if (OpsNeedSetOutputDtypeWhenRegisterPhiKernel.count(op_name)) {
    PADDLE_ENFORCE_GT(
        OpsWithAvailablePhiInferMeta.count(op_name),
        0,
        phi::errors::Unavailable(
            "Cannot static build for op %s because it did not set output dtype "
            "in phi kernel register. Please set its output dtype and remove it "
            "from OpsNeedSetOutputDtypeWhenRegisterPhiKernel set, or add it to "
            " OpsWithAvailablePhiInferMeta set if its InferMeta is available.",
            op_name));
  }

  auto output_names = kernel_sig.output_names;
  auto output_defs = phi_kernel.args_def().output_defs();
  PADDLE_ENFORCE_EQ(output_names.size(),
                    output_defs.size(),
                    platform::errors::InvalidArgument(
                        "The size of outputs_args names (%d) must be equal to "
                        "the size of kernel output_defs (%d).",
                        output_names.size(),
                        output_defs.size()));

  size_t start_idx = 0;
  for (size_t i = 0; i < output_names.size(); ++i) {
    auto it = ctx.outputs.find(output_names[i]);

    // Deal with the case that some outputs are not found or be NULL when run
    // the kernel. For example : the outputs of matmul_grad are dx and dy,
    // sometimes dx or dy may be NULL.
    if (it == ctx.outputs.end() || it->second.empty()) {
      VLOG(4) << "Output " << output_names[i] << " not found";
      ++start_idx;
      continue;
    }

    auto& outs_vector = it->second;
    for (size_t offset = 0; offset < outs_vector.size(); ++offset) {
      phi::TensorBase* out_tensor = GetTensorFormVar(outs_vector[offset]);
      if (out_tensor && !out_tensor->initialized()) {
        phi::TensorArgDef& tensor_arg_def = output_defs[start_idx + offset];
        phi::DataType dtype = tensor_arg_def.dtype;
        phi::Place place = tensor_arg_def.backend == phi::Backend::CUSTOM
                               ? dev_ctx.GetPlace()
                               : phi::TransToPhiPlace(tensor_arg_def.backend);

        if (dtype == DataType::UNDEFINED ||
            OpsNeedSetOutputDtypeWhenRegisterPhiKernel.count(
                std::string(kernel_sig.name))) {
          VLOG(4) << "Get dtype result from InferMeta";
          dtype = out_tensor->dtype();  // dtype from InferMeta
        }

        VLOG(4) << output_names[i] << " fake alloc with type " << dtype
                << " on place " << place << " " << out_tensor;

        FakeInitializeTensor(dev_ctx, dtype, place, out_tensor);
      }
    }
    start_idx += outs_vector.size();
  }
}

void FakeInitializeOutputsForStructureKernel(
    const framework::OpKernelType& op_kernel_type,
    ExecutionContext* execution_context) {
  const std::string& op_type = execution_context->Type();
  if (op_type == "fetch_v2") {
    return;
  }

  const VariableNameMap& outputs = execution_context->GetOp().Outputs();
  for (auto& item : outputs) {
    const std::string& parameter_name = item.first;
    auto multi_output_var = execution_context->MultiOutputVar(parameter_name);
    for (Variable* var : multi_output_var) {
      phi::TensorBase* out_tensor = GetTensorFormVar(var);
      if (out_tensor && !out_tensor->initialized()) {
        phi::DataType dtype =
            phi::TransToPhiDataType(op_kernel_type.data_type_);
        phi::Place place = execution_context->GetPlace();

        VLOG(4) << parameter_name << " fake alloc with type " << dtype
                << " on place " << place << " " << out_tensor;

        FakeInitializeTensor(
            execution_context->device_context(), dtype, place, out_tensor);
      }
    }
  }
}

void LogDeviceMemoryStats(const platform::Place& place) {
  if (FLAGS_new_executor_log_memory_stats && platform::is_gpu_place(place)) {
    VLOG(0) << "memory_allocated: "
            << static_cast<double>(memory::DeviceMemoryStatCurrentValue(
                   "Allocated", place.device)) /
                   1024 / 1024
            << " MB";
    VLOG(0) << "max_memory_allocated: "
            << static_cast<double>(memory::DeviceMemoryStatPeakValue(
                   "Allocated", place.device)) /
                   1024 / 1024
            << " MB";
  }
}

void SetDeviceCommContext(framework::OperatorBase* operator_base,
                          platform::DeviceContext* dev_ctx) {
  if (operator_base->HasAttr("ring_id")) {
    int ring_id = operator_base->Attr<int>("ring_id");
    const auto& comm_context_manager =
        phi::distributed::CommContextManager::GetInstance();
    if (comm_context_manager.Has(ring_id)) {
      auto comm_context = comm_context_manager.Get(ring_id);
      if (!dev_ctx->GetCommContext()) {
        dev_ctx->SetCommContext(comm_context);
      }
    } else {
      LOG(WARNING) << "op: " << operator_base->Type()
                   << ", ring_id: " << ring_id << ", get comm_context failed!";
    }
  }
}

}  // namespace interpreter
}  // namespace framework
}  // namespace paddle
