add_subdirectory(fuse_optimizer_ops_pass)
add_subdirectory(memory_optimize_pass)
add_subdirectory(multi_devices_graph_pass)
if(NOT APPLE
   AND NOT WIN32
   AND (WITH_GPU OR WITH_ROCM))
  add_subdirectory(fusion_group)
endif()

unset(INFER_IR_PASSES CACHE) # clear the global variable
cc_library(
  node
  SRCS node.cc
  DEPS proto_desc)
cc_library(
  graph
  SRCS graph.cc
  DEPS node pretty_log)
cc_library(
  graph_helper
  SRCS graph_helper.cc
  DEPS graph program_utils scale_loss_grad_op_handle
       grad_merge_all_reduce_op_handle)
cc_library(
  pass
  SRCS pass.cc
  DEPS graph node graph_helper)
cc_library(
  graph_traits
  SRCS graph_traits.cc
  DEPS graph)
cc_library(
  cost_model
  SRCS cost_model.cc
  DEPS executor graph profiler proto_desc phi_device_tracer)

set(GRAPH_PATTERN_DETECTOR_DEPS graph graph_helper graph_traits)
if(WITH_TESTING)
  set(GRAPH_PATTERN_DETECTOR_DEPS ${GRAPH_PATTERN_DETECTOR_DEPS} gtest)
endif()
cc_library(
  graph_pattern_detector
  SRCS graph_pattern_detector.cc
  DEPS ${GRAPH_PATTERN_DETECTOR_DEPS})

cc_library(
  op_compat_sensible_pass
  SRCS op_compat_sensible_pass.cc
  DEPS graph_pattern_detector op_def_api pass)
cc_library(
  subgraph_detector
  SRCS subgraph_detector.cc
  DEPS graph_pattern_detector executor)
cc_library(
  fuse_pass_base
  SRCS fuse_pass_base.cc
  DEPS op_compat_sensible_pass)
cc_library(
  placement_pass_base
  SRCS placement_pass_base.cc
  DEPS pass)

cc_library(
  coalesce_grad_tensor_pass
  SRCS coalesce_grad_tensor_pass.cc
  DEPS graph graph_helper)

pass_library(graph_to_program_pass base)
pass_library(graph_viz_pass base)
pass_library(lock_free_optimize_pass base DEPS string_helper)
pass_library(fc_fuse_pass inference)
pass_library(attention_lstm_fuse_pass inference)
pass_library(vit_attention_fuse_pass inference)
pass_library(fc_lstm_fuse_pass inference)
pass_library(embedding_fc_lstm_fuse_pass inference)
pass_library(fc_gru_fuse_pass inference)
pass_library(seq_concat_fc_fuse_pass inference)
pass_library(multi_batch_merge_pass base)
pass_library(map_op_to_another_pass inference)
pass_library(conv_bn_fuse_pass inference)
pass_library(seqconv_eltadd_relu_fuse_pass inference)
pass_library(seqpool_concat_fuse_pass inference)
pass_library(seqpool_cvm_concat_fuse_pass inference)
pass_library(repeated_fc_relu_fuse_pass inference)
pass_library(squared_mat_sub_fuse_pass inference)
pass_library(is_test_pass base)
pass_library(conv_elementwise_add_act_fuse_pass inference)
pass_library(conv_elementwise_add2_act_fuse_pass inference)
pass_library(conv_elementwise_add_fuse_pass inference)
pass_library(transpose_flatten_concat_fuse_pass inference)
pass_library(inplace_op_var_pass inference)
pass_library(identity_scale_op_clean_pass base)
pass_library(sync_batch_norm_pass base)
pass_library(runtime_context_cache_pass base)
pass_library(quant_conv2d_dequant_fuse_pass inference)
pass_library(shuffle_channel_detect_pass inference)
pass_library(delete_quant_dequant_op_pass inference)
pass_library(delete_quant_dequant_filter_op_pass inference)
pass_library(trt_delete_weight_dequant_linear_op_pass inference)
pass_library(delete_op_device_pass inference)
pass_library(delete_weight_dequant_linear_op_pass inference)
pass_library(delete_quant_dequant_linear_op_pass inference)
pass_library(delete_dropout_op_pass inference)
pass_library(delete_c_identity_op_pass inference)
pass_library(preln_residual_bias_fuse_pass inference)
pass_library(delete_fill_constant_op_pass inference)
pass_library(constant_folding_pass inference)
pass_library(auto_mixed_precision_pass inference)
pass_library(conv2d_fusion_layout_transfer_pass inference)
pass_library(silu_fuse_pass inference)
pass_library(simplify_with_basic_ops_pass base)
pass_library(fc_elementwise_layernorm_fuse_pass base)
pass_library(skip_layernorm_fuse_pass base)
pass_library(multihead_matmul_fuse_pass inference)
pass_library(multihead_matmul_roformer_fuse_pass inference)
pass_library(fused_multi_transformer_encoder_pass inference)
pass_library(fused_multi_transformer_decoder_pass inference)
pass_library(fuse_multi_transformer_layer_pass inference)
pass_library(adaptive_pool2d_convert_global_pass inference)
pass_library(unsqueeze2_eltwise_fuse_pass inference)
pass_library(yolo_box_fuse_pass inference)
pass_library(layer_norm_fuse_pass inference)
pass_library(add_support_int8_pass inference)
pass_library(matmul_scale_fuse_pass inference)
pass_library(gpu_cpu_map_matmul_to_mul_pass inference)
pass_library(dense_fc_to_sparse_pass inference)
pass_library(dense_multihead_matmul_to_sparse_pass inference)
pass_library(generate_pass DEPS pass_desc_proto)
target_link_libraries(generate_pass pass_desc_proto)

if(WITH_TENSORRT)
  pass_library(trt_map_matmul_to_mul_pass inference)
  pass_library(trt_multihead_matmul_fuse_pass inference)
  pass_library(trt_flash_multihead_matmul_fuse_pass inference)
  pass_library(trt_cross_multihead_matmul_fuse_pass inference)
  pass_library(trt_skip_layernorm_fuse_pass inference)
  pass_library(merge_layernorm_fuse_pass inference)
  pass_library(preln_skip_layernorm_fuse_pass inference)
  pass_library(set_transformer_input_convert_pass inference)
  pass_library(remove_padding_recover_padding_pass inference)
  pass_library(delete_remove_padding_recover_padding_pass inference)
  pass_library(layernorm_shift_partition_fuse_pass inference)
  pass_library(reverse_roll_fuse_pass inference)
  pass_library(elementwiseadd_transpose_pass inference)
  pass_library(preln_layernorm_x_fuse_pass inference)
  pass_library(trt_support_nhwc_pass inference)
  pass_library(elementwise_groupnorm_act_pass inference)
  pass_library(preln_elementwise_groupnorm_act_pass inference)
  pass_library(groupnorm_act_pass inference)
  pass_library(trans_layernorm_fuse_pass inference)
  pass_library(trt_embedding_eltwise_layernorm_fuse_pass inference)
  pass_library(preln_embedding_eltwise_layernorm_fuse_pass inference)
endif()

if(WITH_GPU OR WITH_ROCM)
  pass_library(cudnn_placement_pass base DEPS placement_pass_base)
  pass_library(embedding_eltwise_layernorm_fuse_pass inference)
endif()

if(WITH_MKLDNN)
  pass_library(mkldnn_placement_pass base DEPS placement_pass_base DIR mkldnn)
  pass_library(depthwise_conv_mkldnn_pass base DIR mkldnn)
  pass_library(conv_affine_channel_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(conv_bias_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(conv_activation_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(conv_elementwise_add_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(int8_scale_calculation_mkldnn_pass inference DIR mkldnn)
  pass_library(params_quantization_mkldnn_pass inference DIR mkldnn)
  pass_library(fc_elementwise_add_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(scale_matmul_fuse_pass inference DIR mkldnn)
  pass_library(cpu_bfloat16_placement_pass inference DIR mkldnn)
  pass_library(cpu_bfloat16_pass inference DIR mkldnn)
  pass_library(fc_mkldnn_pass inference DIR mkldnn)
  pass_library(interpolate_mkldnn_pass inference DIR mkldnn)
  pass_library(softplus_activation_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(shuffle_channel_mkldnn_detect_pass inference DIR mkldnn)
  pass_library(fc_act_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(elt_act_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(matmul_elementwise_add_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(matmul_activation_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(layer_norm_onednn_optimization_pass inference DIR mkldnn)
  pass_library(operator_scale_onednn_fuse_pass inference DIR mkldnn)
  pass_library(quant_transpose2_dequant_onednn_fuse_pass inference DIR mkldnn)
  pass_library(squeeze2_transpose2_onednn_fuse_pass inference DIR mkldnn)
  pass_library(operator_unsqueeze2_onednn_fuse_pass inference DIR mkldnn)
  pass_library(operator_reshape2_onednn_fuse_pass inference DIR mkldnn)
  pass_library(cpu_quantize_placement_pass base DIR mkldnn)
  pass_library(cpu_quantize_pass inference DIR mkldnn)
  pass_library(cpu_quantize_squash_pass inference DIR mkldnn)
  pass_library(reshape_transpose_matmul_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(matmul_transpose_reshape_mkldnn_fuse_pass inference DIR mkldnn)
  pass_library(batch_norm_act_fuse_pass inference DIR mkldnn)
  pass_library(multi_gru_fuse_pass inference DIR mkldnn)
  pass_library(multi_gru_seq_fuse_pass inference DIR mkldnn)
  pass_library(quant_dequant_mkldnn_pass inference DIR mkldnn)
  pass_library(compute_propagate_scales_mkldnn_pass inference DIR mkldnn)
endif()

if(WITH_IPU)
  pass_library(forward_graph_extract_pass base DIR ipu)
  pass_library(optimizer_extract_pass base DIR ipu)
  pass_library(optimizer_state_align_pass base DIR ipu)
  pass_library(ipu_graph_builder_pass base DIR ipu)
  pass_library(ipu_runtime_replacer_pass base DIR ipu)
  pass_library(inference_process_pass base DIR ipu)
  pass_library(inference_postprocess_pass base DIR ipu)
  pass_library(popart_canonicalization_pass base DIR ipu)
  pass_library(ipu_inplace_pass base DIR ipu)
  pass_library(infer_shape_pass base DIR ipu)
  pass_library(delete_scale_op_pass base DIR ipu)
  pass_library(avg_shard_pass base DIR ipu)
  pass_library(inference_dtype_transfer_pass base DIR ipu)
endif()

if(WITH_XPU)
  cc_library(
    xpu_quant_utils
    SRCS xpu/quant_utils.cc
    DEPS pass phi)
  cc_library(
    xpu_pass_utils
    SRCS xpu/pass_utils.cc
    DEPS pass xpu_quant_utils)
  set(XPU_PASS_DEPS xpu_quant_utils xpu_pass_utils)
  pass_library(embedding_with_eltwise_add_xpu_fuse_pass inference DIR xpu DEPS
               ${XPU_PASS_DEPS})
  pass_library(fc_xpu_fuse_pass inference DIR xpu DEPS ${XPU_PASS_DEPS})
  pass_library(multi_encoder_xpu_fuse_pass inference DIR xpu DEPS
               ${XPU_PASS_DEPS})
  pass_library(multi_encoder_xpu_slice_fuse_pass inference DIR xpu DEPS
               ${XPU_PASS_DEPS})
  pass_library(generate_sequence_xpu_fuse_pass inference DIR xpu DEPS
               ${XPU_PASS_DEPS})
  pass_library(link_xpu_op_max_pass inference DIR xpu DEPS ${XPU_PASS_DEPS})
  pass_library(delete_isolated_node_pass inference DIR xpu DEPS
               ${XPU_PASS_DEPS})
endif()

cc_library(
  fuse_bn_act_pass
  SRCS fuse_bn_act_pass.cc
  DEPS pass graph_pattern_detector)
cc_library(
  fuse_bn_add_act_pass
  SRCS fuse_bn_add_act_pass.cc
  DEPS pass graph_pattern_detector)
cc_library(
  fuse_elewise_add_act_pass
  SRCS fuse_elewise_add_act_pass.cc
  DEPS pass graph_pattern_detector)
cc_library(
  fuse_gemm_epilogue_pass
  SRCS fuse_gemm_epilogue_pass.cc
  DEPS pass graph_pattern_detector)
cc_library(
  fused_attention_pass
  SRCS fused_attention_pass.cc
  DEPS pass graph_pattern_detector)
cc_library(
  fuse_relu_depthwise_conv_pass
  SRCS fuse_relu_depthwise_conv_pass.cc
  DEPS pass graph_pattern_detector)

set(GLOB_PASS_LIB
    ${INFER_IR_PASSES}
    CACHE INTERNAL "Global PASS library")

cc_library(
  pass_builder
  SRCS pass_builder.cc
  DEPS pass)
cc_library(
  pass_test_util
  SRCS pass_test_util.cc
  DEPS graph pass)

cc_test(
  node_test
  SRCS node_test.cc
  DEPS node)
cc_test(
  pass_test
  SRCS pass_test.cc
  DEPS graph pass graph_helper)
cc_test(
  graph_test
  SRCS graph_test.cc
  DEPS graph graph_helper op_registry)
cc_test(
  graph_helper_test
  SRCS graph_helper_test.cc
  DEPS graph graph_helper op_registry)
cc_test(
  graph_to_program_pass_test
  SRCS graph_to_program_pass_test.cc
  DEPS graph_to_program_pass)
cc_test(
  cost_model_test
  SRCS cost_model_test.cc
  DEPS cost_model op_registry)
cc_test(
  test_graph_pattern_detector
  SRCS graph_pattern_detector_tester.cc
  DEPS graph_pattern_detector)
cc_test(
  test_op_compat_sensible_pass
  SRCS op_compat_sensible_pass_tester.cc
  DEPS op_compat_sensible_pass)
cc_test(
  test_fc_fuse_pass_cc
  SRCS fc_fuse_pass_tester.cc
  DEPS fc_fuse_pass framework_proto)
cc_test(
  test_fc_lstm_fuse_pass_cc
  SRCS fc_lstm_fuse_pass_tester.cc
  DEPS fc_lstm_fuse_pass framework_proto)
cc_test(
  test_fc_gru_fuse_pass_cc
  SRCS fc_gru_fuse_pass_tester.cc
  DEPS fc_gru_fuse_pass framework_proto)
cc_test(
  test_seqpool_concat_fuse_pass
  SRCS seqpool_concat_fuse_pass_tester.cc
  DEPS seqpool_concat_fuse_pass framework_proto)
cc_test(
  test_seqpool_cvm_concat_fuse_pass
  SRCS seqpool_cvm_concat_fuse_pass_tester.cc
  DEPS seqpool_cvm_concat_fuse_pass framework_proto)
cc_test(
  test_repeated_fc_relu_fuse_pass_cc
  SRCS repeated_fc_relu_fuse_pass_tester.cc
  DEPS repeated_fc_relu_fuse_pass framework_proto)
cc_test(
  test_is_test_pass
  SRCS is_test_pass_tester.cc
  DEPS is_test_pass)
cc_test(
  test_simplify_with_basic_ops_pass
  SRCS simplify_with_basic_ops_pass_tester.cc
  DEPS simplify_with_basic_ops_pass)
cc_test(
  test_fc_elementwise_layernorm_fuse_pass_cc
  SRCS fc_elementwise_layernorm_fuse_pass_tester.cc
  DEPS fc_elementwise_layernorm_fuse_pass)
cc_test(
  test_skip_layernorm_fuse_pass
  SRCS skip_layernorm_fuse_pass_tester.cc
  DEPS skip_layernorm_fuse_pass)
cc_test(
  test_multihead_matmul_fuse_pass
  SRCS multihead_matmul_fuse_pass_tester.cc
  DEPS multihead_matmul_fuse_pass)
cc_test(
  test_fused_multi_transformer_encoder_pass
  SRCS fused_multi_transformer_encoder_pass_tester.cc
  DEPS fused_multi_transformer_encoder_pass)
cc_test(
  test_fused_multi_transformer_decoder_pass
  SRCS fused_multi_transformer_decoder_pass_tester.cc
  DEPS fused_multi_transformer_decoder_pass)
cc_test(
  test_fuse_multi_transformer_layer_pass
  SRCS fuse_multi_transformer_layer_pass_tester.cc
  DEPS fuse_multi_transformer_layer_pass)
cc_test(
  test_conv_bn_fuse_pass_cc
  SRCS conv_bn_fuse_pass_tester.cc
  DEPS conv_bn_fuse_pass)
cc_test(
  test_adaptive_pool2d_convert_global_pass
  SRCS adaptive_pool2d_convert_global_pass_tester.cc
  DEPS adaptive_pool2d_convert_global_pass)
cc_test(
  test_unsqueeze2_eltwise_fuse_pass_cc
  SRCS unsqueeze2_eltwise_fuse_pass_tester.cc
  DEPS unsqueeze2_eltwise_fuse_pass)
cc_test(
  test_generate_pass_cc
  SRCS generate_pass_tester.cc
  DEPS generate_pass pass_desc_proto)
cc_test(
  test_delete_op_device_pass
  SRCS delete_op_device_pass_test.cc
  DEPS delete_op_device_pass)
cc_test(
  test_delete_dropout_pass_cc
  SRCS delete_dropout_op_pass_test.cc
  DEPS delete_dropout_op_pass)
cc_test(
  test_delete_dequant_weight_linear_op_pass
  SRCS delete_weight_dequant_linear_op_pass_tester.cc
  DEPS delete_weight_dequant_linear_op_pass)
if(WITH_GPU OR WITH_ROCM)
  cc_test(
    test_embedding_eltwise_layernorm_fuse_pass
    SRCS embedding_eltwise_layernorm_fuse_pass_tester.cc
    DEPS embedding_eltwise_layernorm_fuse_pass)
  cc_test(
    test_cudnn_placement_pass
    SRCS cudnn_placement_pass_tester.cc
    DEPS cudnn_placement_pass)
endif()
if(NOT WIN32)
  cc_test(
    test_sync_batch_norm_pass
    SRCS sync_batch_norm_pass_tester.cc
    DEPS sync_batch_norm_pass)
  cc_test(
    test_dense_fc_to_sparse_pass_cc
    SRCS dense_fc_to_sparse_pass_tester.cc
    DEPS fc_fuse_pass dense_fc_to_sparse_pass framework_proto)
  cc_test(
    test_dense_multihead_matmul_to_sparse_pass
    SRCS dense_multihead_matmul_to_sparse_pass_tester.cc
    DEPS multihead_matmul_fuse_pass dense_multihead_matmul_to_sparse_pass)
endif()
if(WITH_MKLDNN)
  cc_test(
    test_depthwise_conv_mkldnn_pass
    SRCS mkldnn/depthwise_conv_mkldnn_pass_tester.cc
    DEPS depthwise_conv_mkldnn_pass)
  cc_test_old(
    test_int8_scale_calculation_mkldnn_pass SRCS
    mkldnn/int8_scale_calculation_mkldnn_pass_tester.cc DEPS
    int8_scale_calculation_mkldnn_pass pass_test_util)
  cc_test_old(
    test_params_quantization_mkldnn_pass SRCS
    mkldnn/params_quantization_mkldnn_pass_tester.cc DEPS
    params_quantization_mkldnn_pass)
  set(TEST_CONV_BN_PASS_DEPS
      conv_bn_fuse_pass
      graph_to_program_pass
      conv_op
      conv_transpose_op
      math_function
      im2col
      vol2col
      batch_norm_op
      generated_op
      activation_op
      elementwise_add_op
      concat_and_split
      naive_executor
      device_context
      eigen_function)
  if(WITH_GPU OR WITH_ROCM)
    set(TEST_CONV_BN_PASS_DEPS ${TEST_CONV_BN_PASS_DEPS} depthwise_conv)
  endif()
  cc_test(
    test_mkldnn_placement_pass
    SRCS mkldnn/mkldnn_placement_pass_tester.cc
    DEPS mkldnn_placement_pass)
  cc_test(
    test_compute_propagate_scales_mkldnn_pass
    SRCS mkldnn/compute_propagate_scales_mkldnn_pass_tester.cc
    DEPS compute_propagate_scales_mkldnn_pass naive_executor)

  if(WITH_ONNXRUNTIME AND WIN32)
    # Copy onnxruntime for some c++ test in Windows, since the test will
    # be build only in CI, so suppose the generator in Windows is Ninja.
    copy_onnx(test_compute_propagate_scales_mkldnn_pass)
  endif()

  cc_test(
    test_cpu_quantize_placement_pass
    SRCS mkldnn/cpu_quantize_placement_pass_tester.cc
    DEPS cpu_quantize_placement_pass)
  cc_test(
    test_cpu_quantize_pass
    SRCS mkldnn/cpu_quantize_pass_tester.cc
    DEPS cpu_quantize_pass naive_executor)
  cc_test(
    test_cpu_quantize_squash_pass
    SRCS mkldnn/cpu_quantize_squash_pass_tester.cc
    DEPS cpu_quantize_squash_pass naive_executor)
  cc_test(
    test_shuffle_channel_mkldnn_detect_pass
    SRCS mkldnn/shuffle_channel_mkldnn_detect_pass_tester.cc
    DEPS shuffle_channel_mkldnn_detect_pass)
  cc_test(
    test_cpu_bfloat16_placement_pass
    SRCS mkldnn/cpu_bfloat16_placement_pass_tester.cc
    DEPS cpu_bfloat16_placement_pass)
  cc_test(
    test_cpu_bfloat16_pass
    SRCS mkldnn/cpu_bfloat16_pass_tester.cc
    DEPS cpu_bfloat16_pass)
endif()

if(WITH_XPU)
  cc_test(
    test_delete_isolated_node_pass
    SRCS xpu/delete_isolated_node_pass_test.cc
    DEPS delete_isolated_node_pass)
endif()
