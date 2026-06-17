// Copyright 2015 The Shaderc Authors. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal shaderc C API header — only types and declarations needed by PCSX2.
// The actual library is loaded at runtime via dlopen (shaderc_shared.so.1).

#ifndef SHADERC_SHADERC_H_
#define SHADERC_SHADERC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shaderc_compiler* shaderc_compiler_t;
typedef struct shaderc_compile_options* shaderc_compile_options_t;
typedef struct shaderc_compilation_result* shaderc_compilation_result_t;

typedef enum {
	shaderc_glsl_vertex_shader = 0,
	shaderc_glsl_fragment_shader = 1,
	shaderc_glsl_compute_shader = 2,
	shaderc_glsl_geometry_shader = 3,
	shaderc_glsl_tess_control_shader = 4,
	shaderc_glsl_tess_evaluation_shader = 5,
	shaderc_glsl_infer_from_source = 6,
	shaderc_glsl_default_vertex_shader = 7,
	shaderc_glsl_default_fragment_shader = 8,
	shaderc_glsl_default_compute_shader = 9,
	shaderc_glsl_default_geometry_shader = 10,
	shaderc_glsl_default_tess_control_shader = 11,
	shaderc_glsl_default_tess_evaluation_shader = 12,
	shaderc_spirv_assembly = 13,
	shaderc_glsl_raygen_shader = 14,
	shaderc_glsl_anyhit_shader = 15,
	shaderc_glsl_closesthit_shader = 16,
	shaderc_glsl_miss_shader = 17,
	shaderc_glsl_intersection_shader = 18,
	shaderc_glsl_callable_shader = 19,
	shaderc_glsl_default_raygen_shader = 20,
	shaderc_glsl_default_anyhit_shader = 21,
	shaderc_glsl_default_closesthit_shader = 22,
	shaderc_glsl_default_miss_shader = 23,
	shaderc_glsl_default_intersection_shader = 24,
	shaderc_glsl_default_callable_shader = 25,
	shaderc_glsl_task_shader = 26,
	shaderc_glsl_mesh_shader = 27,
	shaderc_glsl_default_task_shader = 28,
	shaderc_glsl_default_mesh_shader = 29,
} shaderc_shader_kind;

typedef enum {
	shaderc_source_language_glsl,
	shaderc_source_language_hlsl,
} shaderc_source_language;

typedef enum {
	shaderc_target_env_vulkan = 0,
	shaderc_target_env_opengl = 1,
	shaderc_target_env_opengl_compat = 2,
	shaderc_target_env_webgpu = 3,
	shaderc_target_env_default = shaderc_target_env_vulkan,
} shaderc_target_env;

typedef enum {
	shaderc_env_version_vulkan_1_0 = (1u << 22),
	shaderc_env_version_vulkan_1_1 = (1u << 22) | (1u << 12),
	shaderc_env_version_vulkan_1_2 = (1u << 22) | (2u << 12),
	shaderc_env_version_vulkan_1_3 = (1u << 22) | (3u << 12),
	shaderc_env_version_opengl_4_5 = 450,
	shaderc_env_version_webgpu = 0,
} shaderc_env_version;

typedef enum {
	shaderc_optimization_level_zero = 0,
	shaderc_optimization_level_size = 1,
	shaderc_optimization_level_performance = 2,
} shaderc_optimization_level;

typedef enum {
	shaderc_compilation_status_success = 0,
	shaderc_compilation_status_invalid_stage = 1,
	shaderc_compilation_status_compilation_error = 2,
	shaderc_compilation_status_internal_error = 3,
	shaderc_compilation_status_null_result_object = 4,
	shaderc_compilation_status_invalid_assembly = 5,
	shaderc_compilation_status_validation_error = 6,
	shaderc_compilation_status_transformation_error = 7,
	shaderc_compilation_status_configuration_error = 8,
} shaderc_compilation_status;

typedef enum {
	shaderc_profile_none,
	shaderc_profile_core,
	shaderc_profile_compatibility,
	shaderc_profile_es,
} shaderc_profile;

typedef enum {
	shaderc_uniform_kind_image,
	shaderc_uniform_kind_sampler,
	shaderc_uniform_kind_texture,
	shaderc_uniform_kind_buffer,
	shaderc_uniform_kind_storage_buffer,
	shaderc_uniform_kind_unordered_access_view,
} shaderc_uniform_kind;

shaderc_compiler_t shaderc_compiler_initialize(void);
void shaderc_compiler_release(shaderc_compiler_t);

shaderc_compile_options_t shaderc_compile_options_initialize(void);
shaderc_compile_options_t shaderc_compile_options_clone(const shaderc_compile_options_t options);
void shaderc_compile_options_release(shaderc_compile_options_t options);

void shaderc_compile_options_add_macro_definition(shaderc_compile_options_t options,
	const char* name, size_t name_length, const char* value, size_t value_length);
void shaderc_compile_options_set_source_language(shaderc_compile_options_t options,
	shaderc_source_language lang);
void shaderc_compile_options_set_generate_debug_info(shaderc_compile_options_t options);
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t options,
	shaderc_optimization_level level);
void shaderc_compile_options_set_forced_version_profile(shaderc_compile_options_t options,
	int version, shaderc_profile profile);
void shaderc_compile_options_set_suppress_warnings(shaderc_compile_options_t options);
void shaderc_compile_options_set_target_env(shaderc_compile_options_t options,
	shaderc_target_env target, uint32_t version);
void shaderc_compile_options_set_warnings_as_errors(shaderc_compile_options_t options);

shaderc_compilation_result_t shaderc_compile_into_spv(shaderc_compiler_t compiler,
	const char* source_text, size_t source_text_size, shaderc_shader_kind shader_kind,
	const char* input_file_name, const char* entry_point_name,
	const shaderc_compile_options_t additional_options);

shaderc_compilation_result_t shaderc_compile_into_spv_assembly(shaderc_compiler_t compiler,
	const char* source_text, size_t source_text_size, shaderc_shader_kind shader_kind,
	const char* input_file_name, const char* entry_point_name,
	const shaderc_compile_options_t additional_options);

shaderc_compilation_result_t shaderc_assemble_into_spv(shaderc_compiler_t compiler,
	const char* source_assembly, size_t source_assembly_size,
	const shaderc_compile_options_t additional_options);

void shaderc_result_release(shaderc_compilation_result_t result);
size_t shaderc_result_get_length(const shaderc_compilation_result_t result);
size_t shaderc_result_get_num_warnings(const shaderc_compilation_result_t result);
size_t shaderc_result_get_num_errors(const shaderc_compilation_result_t result);
shaderc_compilation_status shaderc_result_get_compilation_status(const shaderc_compilation_result_t);
const char* shaderc_result_get_bytes(const shaderc_compilation_result_t result);
const char* shaderc_result_get_error_message(const shaderc_compilation_result_t result);

void shaderc_get_spv_version(unsigned int* version, unsigned int* revision);
int shaderc_parse_version_profile(const char* str, int* version, shaderc_profile* profile);

#ifdef __cplusplus
}
#endif

#endif  // SHADERC_SHADERC_H_
