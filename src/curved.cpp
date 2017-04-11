#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <CL/opencl.h>

#include "curved.h"

#ifndef TILE_SIZE_DIM0
#define TILE_SIZE_DIM0 (128)
#endif//TILE_SIZE_DIM0
#ifndef TILE_SIZE_DIM1
#define TILE_SIZE_DIM1 (128)
#endif//TILE_SIZE_DIM1
#define STENCIL_DIM0 (23)
#define STENCIL_DIM1 (19)

#define min(a, b) ((a) < (b) ? (a) : (b) )
#define max(a, b) ((a) > (b) ? (a) : (b) )

inline float float_from_bits(uint32_t bits)
{
    union
    {
        uint32_t as_uint;
        float as_float;
        
    } u;
    u.as_uint = bits;
    return u.as_float;
}

inline float pow_f32(float x, float y) {return powf(x, y);}

int load_file_to_memory(const char *filename, char **result)
{ 
    uint32_t size = 0;
    FILE *f = fopen(filename, "rb");
    if (f == NULL)
    {
        *result = NULL;
        return -1;
    } 
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *result = (char *)malloc(size+1);
    if (size != fread(*result, sizeof(char), size, f))
    {
        free(*result);
        return -2;
    } 
    fclose(f);
    (*result)[size] = 0;
    return size;
}

static bool halide_rewrite_buffer(buffer_t *b, int32_t elem_size,
                                  int32_t min0, int32_t extent0, int32_t stride0,
                                  int32_t min1, int32_t extent1, int32_t stride1,
                                  int32_t min2, int32_t extent2, int32_t stride2,
                                  int32_t min3, int32_t extent3, int32_t stride3) {
    b->min[0] = min0;
    b->min[1] = min1;
    b->min[2] = min2;
    b->min[3] = min3;
    b->extent[0] = extent0;
    b->extent[1] = extent1;
    b->extent[2] = extent2;
    b->extent[3] = extent3;
    b->stride[0] = stride0;
    b->stride[1] = stride1;
    b->stride[2] = stride2;
    b->stride[3] = stride3;
    return true;
}

int halide_error_code_success = 0;
int halide_error_code_generic_error = -1;
int halide_error_code_explicit_bounds_too_small = -2;
int halide_error_code_bad_elem_size = -3;
int halide_error_code_access_out_of_bounds = -4;
int halide_error_code_buffer_allocation_too_large = -5;
int halide_error_code_buffer_extents_too_large = -6;
int halide_error_code_constraints_make_required_region_smaller = -7;
int halide_error_code_constraint_violated = -8;
int halide_error_code_param_too_small = -9;
int halide_error_code_param_too_large = -10;
int halide_error_code_out_of_memory = -11;
int halide_error_code_buffer_argument_is_null = -12;
int halide_error_code_debug_to_file_failed = -13;
int halide_error_code_copy_to_host_failed = -14;
int halide_error_code_copy_to_device_failed = -15;
int halide_error_code_device_malloc_failed = -16;
int halide_error_code_device_sync_failed = -17;
int halide_error_code_device_free_failed = -18;
int halide_error_code_no_device_interface = -19;
int halide_error_code_matlab_init_failed = -20;
int halide_error_code_matlab_bad_param_type = -21;
int halide_error_code_internal_error = -22;
int halide_error_code_device_run_failed = -23;
int halide_error_code_unaligned_host_ptr = -24;
int halide_error_code_bad_fold = -25;
int halide_error_code_fold_factor_too_small = -26;

FILE* const* error_report = &stderr;

int halide_error_bad_elem_size(void *user_context, const char *func_name,
                               const char *type_name, int elem_size_given, int correct_elem_size) {
    fprintf(*error_report, "%s has type %s but elem_size of the buffer passed in is %d instead of %d",
            func_name, type_name, elem_size_given, correct_elem_size);
    return halide_error_code_bad_elem_size;
}
int halide_error_constraint_violated(void *user_context, const char *var, int val,
                                     const char *constrained_var, int constrained_val) {
    fprintf(*error_report, "Constraint violated: %s (%d) == %s (%d)",
            var, val, constrained_var, constrained_val);
    return halide_error_code_constraint_violated;
}
int halide_error_buffer_allocation_too_large(void *user_context, const char *buffer_name, uint64_t allocation_size, uint64_t max_size) {
    fprintf(*error_report, "Total allocation for buffer %s is %lu, which exceeds the maximum size of %lu",
            buffer_name, allocation_size, max_size);
    return halide_error_code_buffer_allocation_too_large;
}
int halide_error_buffer_extents_too_large(void *user_context, const char *buffer_name, int64_t actual_size, int64_t max_size) {
    fprintf(*error_report, "Product of extents for buffer %s is %ld, which exceeds the maximum size of %ld",
            buffer_name, actual_size, max_size);
    return halide_error_code_buffer_extents_too_large;
}
int halide_error_access_out_of_bounds(void *user_context, const char *func_name, int dimension, int min_touched, int max_touched, int min_valid, int max_valid) {
    if (min_touched < min_valid) {
        fprintf(*error_report, "%s is accessed at %d, which is before the min (%d) in dimension %d", func_name, min_touched, min_valid, dimension);
    } else if (max_touched > max_valid) {
        fprintf(*error_report, "%s is acccessed at %d, which is beyond the max (%d) in dimension %d", func_name, max_touched, max_valid, dimension);
    }
return halide_error_code_access_out_of_bounds;
}

static int curved_wrapped(float var_color_temp, float var_gamma, float var_contrast, int32_t var_blackLevel, int32_t var_whiteLevel, buffer_t* var_input_buffer, buffer_t* var_m3200_buffer, buffer_t* var_m7000_buffer, buffer_t* var_processed_buffer, const char* xclbin) HALIDE_FUNCTION_ATTRS {
    uint16_t *var_input = (uint16_t *)(var_input_buffer->host);
    (void)var_input;
    const bool var_input_host_and_dev_are_null = (var_input_buffer->host == NULL) && (var_input_buffer->dev == 0);
    (void)var_input_host_and_dev_are_null;
    int32_t var_input_min_0 = var_input_buffer->min[0];
    (void)var_input_min_0;
    int32_t var_input_min_1 = var_input_buffer->min[1];
    (void)var_input_min_1;
    int32_t var_input_min_2 = var_input_buffer->min[2];
    (void)var_input_min_2;
    int32_t var_input_min_3 = var_input_buffer->min[3];
    (void)var_input_min_3;
    int32_t var_input_extent_0 = var_input_buffer->extent[0];
    (void)var_input_extent_0;
    int32_t var_input_extent_1 = var_input_buffer->extent[1];
    (void)var_input_extent_1;
    int32_t var_input_extent_2 = var_input_buffer->extent[2];
    (void)var_input_extent_2;
    int32_t var_input_extent_3 = var_input_buffer->extent[3];
    (void)var_input_extent_3;
    int32_t var_input_stride_0 = var_input_buffer->stride[0];
    (void)var_input_stride_0;
    int32_t var_input_stride_1 = var_input_buffer->stride[1];
    (void)var_input_stride_1;
    int32_t var_input_stride_2 = var_input_buffer->stride[2];
    (void)var_input_stride_2;
    int32_t var_input_stride_3 = var_input_buffer->stride[3];
    (void)var_input_stride_3;
    int32_t var_input_elem_size = var_input_buffer->elem_size;
    (void)var_input_elem_size;

    float *var_m3200 = (float *)(var_m3200_buffer->host);
    (void)var_m3200;
    const bool var_m3200_host_and_dev_are_null = (var_m3200_buffer->host == NULL) && (var_m3200_buffer->dev == 0);
    (void)var_m3200_host_and_dev_are_null;
    int32_t var_m3200_min_0 = var_m3200_buffer->min[0];
    (void)var_m3200_min_0;
    int32_t var_m3200_min_1 = var_m3200_buffer->min[1];
    (void)var_m3200_min_1;
    int32_t var_m3200_min_2 = var_m3200_buffer->min[2];
    (void)var_m3200_min_2;
    int32_t var_m3200_min_3 = var_m3200_buffer->min[3];
    (void)var_m3200_min_3;
    int32_t var_m3200_extent_0 = var_m3200_buffer->extent[0];
    (void)var_m3200_extent_0;
    int32_t var_m3200_extent_1 = var_m3200_buffer->extent[1];
    (void)var_m3200_extent_1;
    int32_t var_m3200_extent_2 = var_m3200_buffer->extent[2];
    (void)var_m3200_extent_2;
    int32_t var_m3200_extent_3 = var_m3200_buffer->extent[3];
    (void)var_m3200_extent_3;
    int32_t var_m3200_stride_0 = var_m3200_buffer->stride[0];
    (void)var_m3200_stride_0;
    int32_t var_m3200_stride_1 = var_m3200_buffer->stride[1];
    (void)var_m3200_stride_1;
    int32_t var_m3200_stride_2 = var_m3200_buffer->stride[2];
    (void)var_m3200_stride_2;
    int32_t var_m3200_stride_3 = var_m3200_buffer->stride[3];
    (void)var_m3200_stride_3;
    int32_t var_m3200_elem_size = var_m3200_buffer->elem_size;
    (void)var_m3200_elem_size;

    float *var_m7000 = (float *)(var_m7000_buffer->host);
    (void)var_m7000;
    const bool var_m7000_host_and_dev_are_null = (var_m7000_buffer->host == NULL) && (var_m7000_buffer->dev == 0);
    (void)var_m7000_host_and_dev_are_null;
    int32_t var_m7000_min_0 = var_m7000_buffer->min[0];
    (void)var_m7000_min_0;
    int32_t var_m7000_min_1 = var_m7000_buffer->min[1];
    (void)var_m7000_min_1;
    int32_t var_m7000_min_2 = var_m7000_buffer->min[2];
    (void)var_m7000_min_2;
    int32_t var_m7000_min_3 = var_m7000_buffer->min[3];
    (void)var_m7000_min_3;
    int32_t var_m7000_extent_0 = var_m7000_buffer->extent[0];
    (void)var_m7000_extent_0;
    int32_t var_m7000_extent_1 = var_m7000_buffer->extent[1];
    (void)var_m7000_extent_1;
    int32_t var_m7000_extent_2 = var_m7000_buffer->extent[2];
    (void)var_m7000_extent_2;
    int32_t var_m7000_extent_3 = var_m7000_buffer->extent[3];
    (void)var_m7000_extent_3;
    int32_t var_m7000_stride_0 = var_m7000_buffer->stride[0];
    (void)var_m7000_stride_0;
    int32_t var_m7000_stride_1 = var_m7000_buffer->stride[1];
    (void)var_m7000_stride_1;
    int32_t var_m7000_stride_2 = var_m7000_buffer->stride[2];
    (void)var_m7000_stride_2;
    int32_t var_m7000_stride_3 = var_m7000_buffer->stride[3];
    (void)var_m7000_stride_3;
    int32_t var_m7000_elem_size = var_m7000_buffer->elem_size;
    (void)var_m7000_elem_size;

    uint8_t *var_processed = (uint8_t *)(var_processed_buffer->host);
    (void)var_processed;
    const bool var_processed_host_and_dev_are_null = (var_processed_buffer->host == NULL) && (var_processed_buffer->dev == 0);
    (void)var_processed_host_and_dev_are_null;
    int32_t var_processed_min_0 = var_processed_buffer->min[0];
    (void)var_processed_min_0;
    int32_t var_processed_min_1 = var_processed_buffer->min[1];
    (void)var_processed_min_1;
    int32_t var_processed_min_2 = var_processed_buffer->min[2];
    (void)var_processed_min_2;
    int32_t var_processed_min_3 = var_processed_buffer->min[3];
    (void)var_processed_min_3;
    int32_t var_processed_extent_0 = var_processed_buffer->extent[0];
    (void)var_processed_extent_0;
    int32_t var_processed_extent_1 = var_processed_buffer->extent[1];
    (void)var_processed_extent_1;
    int32_t var_processed_extent_2 = var_processed_buffer->extent[2];
    (void)var_processed_extent_2;
    int32_t var_processed_extent_3 = var_processed_buffer->extent[3];
    (void)var_processed_extent_3;
    int32_t var_processed_stride_0 = var_processed_buffer->stride[0];
    (void)var_processed_stride_0;
    int32_t var_processed_stride_1 = var_processed_buffer->stride[1];
    (void)var_processed_stride_1;
    int32_t var_processed_stride_2 = var_processed_buffer->stride[2];
    (void)var_processed_stride_2;
    int32_t var_processed_stride_3 = var_processed_buffer->stride[3];
    (void)var_processed_stride_3;
    int32_t var_processed_elem_size = var_processed_buffer->elem_size;
    (void)var_processed_elem_size;

    if (var_processed_host_and_dev_are_null)
    {
        bool assign_0 = halide_rewrite_buffer(var_processed_buffer, 2, var_processed_min_0, var_processed_extent_0, 1, var_processed_min_1, var_processed_extent_1, var_processed_extent_0, 0, 0, 0, 0, 0, 0);
        (void)assign_0;
    } // if var_processed_host_and_dev_are_null
    if (var_input_host_and_dev_are_null)
    {
        int32_t assign_1 = var_processed_extent_0 + 2;
        int32_t assign_2 = var_processed_extent_1 + 2;
        bool assign_3 = halide_rewrite_buffer(var_input_buffer, 2, var_processed_min_0, assign_1, 1, var_processed_min_1, assign_2, assign_1, 0, 0, 0, 0, 0, 0);
        (void)assign_3;
    } // if var_input_host_and_dev_are_null
    bool assign_4 = var_processed_host_and_dev_are_null || var_input_host_and_dev_are_null;
    bool assign_5 = !(assign_4);
    if (assign_5)
    {
        bool assign_6 = var_processed_elem_size == 1;
        if (!assign_6)         {
            int32_t assign_7 = halide_error_bad_elem_size(NULL, "Output buffer curved_y", "uint8", var_processed_elem_size, 1);
            return assign_7;
        }
        bool assign_8 = var_input_elem_size == 2;
        if (!assign_8)         {
            int32_t assign_9 = halide_error_bad_elem_size(NULL, "Input buffer p0", "uint16", var_input_elem_size, 2);
            return assign_9;
        }
        bool assign_10 = var_input_min_0 <= var_processed_min_0;
        int32_t assign_11 = var_processed_min_0 + var_processed_extent_0;
        int32_t assign_12 = assign_11 - var_input_extent_0;
        int32_t assign_13 = assign_12 + 2;
        bool assign_14 = assign_13 <= var_input_min_0;
        bool assign_15 = assign_10 && assign_14;
        if (!assign_15)         {
            int32_t assign_16 = var_processed_min_0 + var_processed_extent_0;
            int32_t assign_17 = assign_16 + 1;
            int32_t assign_18 = var_input_min_0 + var_input_extent_0;
            int32_t assign_19 = assign_18 + -1;
            int32_t assign_20 = halide_error_access_out_of_bounds(NULL, "Input buffer p0", 0, var_processed_min_0, assign_17, var_input_min_0, assign_19);
            return assign_20;
        }
        bool assign_21 = var_input_min_1 <= var_processed_min_1;
        int32_t assign_22 = var_processed_min_1 + var_processed_extent_1;
        int32_t assign_23 = assign_22 - var_input_extent_1;
        int32_t assign_24 = assign_23 + 2;
        bool assign_25 = assign_24 <= var_input_min_1;
        bool assign_26 = assign_21 && assign_25;
        if (!assign_26)         {
            int32_t assign_27 = var_processed_min_1 + var_processed_extent_1;
            int32_t assign_28 = assign_27 + 1;
            int32_t assign_29 = var_input_min_1 + var_input_extent_1;
            int32_t assign_30 = assign_29 + -1;
            int32_t assign_31 = halide_error_access_out_of_bounds(NULL, "Input buffer p0", 1, var_processed_min_1, assign_28, var_input_min_1, assign_30);
            return assign_31;
        }
        bool assign_32 = var_processed_stride_0 == 1;
        if (!assign_32)         {
            int32_t assign_33 = halide_error_constraint_violated(NULL, "curved_y.stride.0", var_processed_stride_0, "1", 1);
            return assign_33;
        }
        bool assign_34 = var_input_stride_0 == 1;
        if (!assign_34)         {
            int32_t assign_35 = halide_error_constraint_violated(NULL, "p0.stride.0", var_input_stride_0, "1", 1);
            return assign_35;
        }
        int64_t assign_36 = (int64_t)(var_processed_extent_1);
        int64_t assign_37 = (int64_t)(var_processed_extent_0);
        int64_t assign_38 = assign_36 * assign_37;
        int64_t assign_39 = (int64_t)(var_input_extent_1);
        int64_t assign_40 = (int64_t)(var_input_extent_0);
        int64_t assign_41 = assign_39 * assign_40;
        int64_t assign_42 = (int64_t)(2147483647);
        bool assign_43 = assign_37 <= assign_42;
        if (!assign_43)         {
            int64_t assign_44 = (int64_t)(var_processed_extent_0);
            int64_t assign_45 = (int64_t)(2147483647);
            int32_t assign_46 = halide_error_buffer_allocation_too_large(NULL, "curved_y", assign_44, assign_45);
            return assign_46;
        }
        int64_t assign_47 = (int64_t)(var_processed_extent_1);
        int64_t assign_48 = (int64_t)(var_processed_stride_1);
        int64_t assign_49 = assign_47 * assign_48;
        int64_t assign_50 = (int64_t)(2147483647);
        bool assign_51 = assign_49 <= assign_50;
        if (!assign_51)         {
            int64_t assign_52 = (int64_t)(var_processed_extent_1);
            int64_t assign_53 = (int64_t)(var_processed_stride_1);
            int64_t assign_54 = assign_52 * assign_53;
            int64_t assign_55 = (int64_t)(2147483647);
            int32_t assign_56 = halide_error_buffer_allocation_too_large(NULL, "curved_y", assign_54, assign_55);
            return assign_56;
        }
        int64_t assign_57 = (int64_t)(2147483647);
        bool assign_58 = assign_38 <= assign_57;
        if (!assign_58)         {
            int64_t assign_59 = (int64_t)(2147483647);
            int32_t assign_60 = halide_error_buffer_extents_too_large(NULL, "curved_y", assign_38, assign_59);
            return assign_60;
        }
        int64_t assign_61 = (int64_t)(var_input_extent_0);
        int64_t assign_62 = (int64_t)(2147483647);
        bool assign_63 = assign_61 <= assign_62;
        if (!assign_63)         {
            int64_t assign_64 = (int64_t)(var_input_extent_0);
            int64_t assign_65 = (int64_t)(2147483647);
            int32_t assign_66 = halide_error_buffer_allocation_too_large(NULL, "p0", assign_64, assign_65);
            return assign_66;
        }
        int64_t assign_67 = (int64_t)(var_input_extent_1);
        int64_t assign_68 = (int64_t)(var_input_stride_1);
        int64_t assign_69 = assign_67 * assign_68;
        int64_t assign_70 = (int64_t)(2147483647);
        bool assign_71 = assign_69 <= assign_70;
        if (!assign_71)         {
            int64_t assign_72 = (int64_t)(var_input_extent_1);
            int64_t assign_73 = (int64_t)(var_input_stride_1);
            int64_t assign_74 = assign_72 * assign_73;
            int64_t assign_75 = (int64_t)(2147483647);
            int32_t assign_76 = halide_error_buffer_allocation_too_large(NULL, "p0", assign_74, assign_75);
            return assign_76;
        }
        int64_t assign_77 = (int64_t)(2147483647);
        bool assign_78 = assign_41 <= assign_77;
        if (!assign_78)         {
            int64_t assign_79 = (int64_t)(2147483647);
            int32_t assign_80 = halide_error_buffer_extents_too_large(NULL, "p0", assign_41, assign_79);
            return assign_80;
        }

        // allocate buffer for tiled input/output
        int32_t tile_num_dim0 = (var_processed_extent_0+(TILE_SIZE_DIM0)-(STENCIL_DIM0))/((TILE_SIZE_DIM0)-(STENCIL_DIM0)+1);
        int32_t tile_num_dim1 = (var_processed_extent_1+(TILE_SIZE_DIM1)-(STENCIL_DIM1))/((TILE_SIZE_DIM1)-(STENCIL_DIM1)+1);
        uint8_t* var_processed_buf = new uint8_t[tile_num_dim0*tile_num_dim1*((TILE_SIZE_DIM0)*(TILE_SIZE_DIM1)/21*64)/*channels*/];
        uint16_t* var_input_buf = new uint16_t[tile_num_dim0*tile_num_dim1*(TILE_SIZE_DIM0)*(TILE_SIZE_DIM1)];

        // tiling
        for(int32_t tile_index_dim1 = 0; tile_index_dim1 < tile_num_dim1; ++tile_index_dim1)
        {
            uint32_t actual_tile_size_dim1 = (tile_index_dim1==tile_num_dim1-1) ? var_input_extent_1-((TILE_SIZE_DIM1)-(STENCIL_DIM1)+1)*tile_index_dim1 : (TILE_SIZE_DIM1);
            for(int32_t tile_index_dim0 = 0; tile_index_dim0 < tile_num_dim0; ++tile_index_dim0)
            {
                uint32_t actual_tile_size_dim0 = (tile_index_dim0==tile_num_dim0-1) ? var_input_extent_0-((TILE_SIZE_DIM0)-(STENCIL_DIM0)+1)*tile_index_dim0 : (TILE_SIZE_DIM0);
                for(uint32_t j = 0; j < actual_tile_size_dim1;++j)
                {
                    for(uint32_t i = 0; i < actual_tile_size_dim0;++i)
                    {
                        // (x, y, z, w) is coordinates in tiled image
                        // (p, q, r, s) is coordinates in original image
                        // (i, j, k, l) is coordinates in a tile
                        //uint32_t x = tile_index_dim0*TILE_SIZE_DIM0+i;
                        //uint32_t y = tile_index_dim1*TILE_SIZE_DIM1+j;
                        uint32_t p = tile_index_dim0*((TILE_SIZE_DIM0)-(STENCIL_DIM0)+1)+i;
                        uint32_t q = tile_index_dim1*((TILE_SIZE_DIM1)-(STENCIL_DIM1)+1)+j;
                        uint32_t tiled_offset = (tile_index_dim1*tile_num_dim0+tile_index_dim0)*TILE_SIZE_DIM0*TILE_SIZE_DIM1+j*TILE_SIZE_DIM0+i;
                        uint32_t original_offset = p*var_input_stride_0+q*var_input_stride_1;
                        var_input_buf[tiled_offset] = var_input[original_offset];
                    }
                }
            }
        }

        int16_t var_matrix[12];
        // produce matrix
        for (int var_matrix_s0_y = 0; var_matrix_s0_y < 0 + 3; var_matrix_s0_y++)
        {
            for (int var_matrix_s0_v0 = 0; var_matrix_s0_v0 < 0 + 4; var_matrix_s0_v0++)
            {
                int32_t var_220 = var_matrix_s0_y * 4;
                int32_t var_221 = var_matrix_s0_v0 + var_220;
                int32_t var_222 = var_matrix_s0_y * var_m3200_stride_1;
                int32_t var_223 = var_matrix_s0_v0 + var_222;
                int32_t var_224 = var_m3200_min_1 * var_m3200_stride_1;
                int32_t var_225 = var_m3200_min_0 + var_224;
                int32_t var_226 = var_223 - var_225;
                float var_227 = var_m3200[var_226];
                float var_228 = float_from_bits(1065353216 /* 1 */) / var_color_temp;
                float var_229 = var_228 * float_from_bits(1169700325 /* 5894.74 */);
                float var_230 = float_from_bits(1072417307 /* 1.84211 */) - var_229;
                float var_231 = var_227 * var_230;
                int32_t var_232 = var_matrix_s0_y * var_m7000_stride_1;
                int32_t var_233 = var_matrix_s0_v0 + var_232;
                int32_t var_234 = var_m7000_min_1 * var_m7000_stride_1;
                int32_t var_235 = var_m7000_min_0 + var_234;
                int32_t var_236 = var_233 - var_235;
                float var_237 = var_m7000[var_236];
                float var_238 = var_229 + float_from_bits(3210187830 /* -0.842105 */);
                float var_239 = var_237 * var_238;
                float var_240 = var_231 + var_239;
                float var_241 = var_240 * float_from_bits(1132462080 /* 256 */);
                int16_t var_242 = (int16_t)(var_241);
                var_matrix[var_221] = var_242;
            } // for var_matrix_s0_v0
        } // for var_matrix_s0_y
        // consume matrix
        uint8_t var_curve[1024];
        // produce curve
        for (int var_curve_s0_v0 = 0; var_curve_s0_v0 < 0 + 1024; var_curve_s0_v0++)
        {
            int32_t var_243 = var_curve_s0_v0 - var_blackLevel;
            float var_244 = (float)(var_243);
            int32_t var_245 = var_whiteLevel - var_blackLevel;
            float var_246 = (float)(var_245);
            float var_247 = float_from_bits(1065353216 /* 1 */) / var_246;
            float var_248 = var_244 * var_247;
            float var_249 = min(var_248, float_from_bits(1065353216 /* 1 */));
            float var_250 = max(var_249, float_from_bits(0 /* 0 */));
            float var_251 = float_from_bits(1065353216 /* 1 */) / var_gamma;
            float var_252 = pow_f32(var_250, var_251);
            float var_253 = var_contrast * float_from_bits(1008981770 /* 0.01 */);
            float var_254 = pow_f32(float_from_bits(1073741824 /* 2 */), var_253);
            uint8_t var_255 = (uint8_t)(255);
            float var_256 = float_from_bits(1065353216 /* 1 */) - var_252;
            float var_257 = float_from_bits(1073741824 /* 2 */) - var_254;
            float var_258 = var_257 * float_from_bits(1073741824 /* 2 */);
            float var_259 = float_from_bits(1073741824 /* 2 */) - var_258;
            float var_260 = var_259 * var_256;
            float var_261 = var_260 - var_254;
            float var_262 = var_261 + float_from_bits(1073741824 /* 2 */);
            float var_263 = var_256 * var_262;
            float var_264 = float_from_bits(1065353216 /* 1 */) - var_263;
            float var_265 = var_259 * var_252;
            float var_266 = var_265 - var_254;
            float var_267 = var_266 + float_from_bits(1073741824 /* 2 */);
            float var_268 = var_252 * var_267;
            bool var_269 = float_from_bits(1056964608 /* 0.5 */) < var_252;
            float var_270 = (float)(var_269 ? var_264 : var_268);
            float var_271 = var_270 * float_from_bits(1132396544 /* 255 */);
            float var_272 = var_271 + float_from_bits(1056964608 /* 0.5 */);
            float var_273 = min(var_272, float_from_bits(1132396544 /* 255 */));
            float var_274 = max(var_273, float_from_bits(0 /* 0 */));
            uint8_t var_275 = (uint8_t)(var_274);
            bool var_276 = var_whiteLevel < var_curve_s0_v0;
            uint8_t var_277 = (uint8_t)(var_276 ? var_255 : var_275);
            uint8_t var_278 = (uint8_t)(0);
            bool var_279 = var_blackLevel < var_curve_s0_v0;
            uint8_t var_280 = (uint8_t)(var_279 ? var_277 : var_278);
            var_curve[var_curve_s0_v0] = var_280;
        }

        // prepare for opencl
#if defined(SDA_PLATFORM) && !defined(TARGET_DEVICE)
  #define STR_VALUE(arg)      #arg
  #define GET_STRING(name) STR_VALUE(name)
  #define TARGET_DEVICE GET_STRING(SDA_PLATFORM)
#endif
        const char *target_device_name = TARGET_DEVICE;
        int err;                            // error code returned from api calls

        cl_platform_id platforms[16];       // platform id
        cl_platform_id platform_id;         // platform id
        cl_uint platform_count;
        cl_device_id device_id;             // compute device id 
        cl_context context;                 // compute context
        cl_command_queue commands;          // compute command queue
        cl_program program;                 // compute program
        cl_kernel kernel;                   // compute kernel
       
        char cl_platform_vendor[1001];
       
        cl_mem var_input_cl;                   // device memory used for the input array
        cl_mem var_processed_cl;               // device memory used for the output array
        cl_mem var_matrix_cl;                   // device memory used for the input array
        cl_mem var_curve_cl;               // device memory used for the output array
   
        // Get all platforms and then select Xilinx platform
        err = clGetPlatformIDs(16, platforms, &platform_count);
        if (err != CL_SUCCESS)
            {
                printf("Error: Failed to find an OpenCL platform!\n");
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }
        printf("INFO: Found %d platforms\n", platform_count);

        // Find Xilinx Plaftorm
        int platform_found = 0;
        for (unsigned int iplat=0; iplat<platform_count; iplat++) {
            err = clGetPlatformInfo(platforms[iplat], CL_PLATFORM_VENDOR, 1000, (void *)cl_platform_vendor,NULL);
            if (err != CL_SUCCESS) {
                printf("Error: clGetPlatformInfo(CL_PLATFORM_VENDOR) failed!\n");
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }
            if (strcmp(cl_platform_vendor, "Xilinx") == 0) {
                printf("INFO: Selected platform %d from %s\n", iplat, cl_platform_vendor);
                platform_id = platforms[iplat];
                platform_found = 1;
            }
        }
        if (!platform_found) {
            printf("ERROR: Platform Xilinx not found. Exit.\n");
            exit(EXIT_FAILURE);
        }
      
        // Connect to a compute device
        // find all devices and then select the target device
        cl_device_id devices[16];  // compute device id 
        cl_uint device_count;
        unsigned int device_found = 0;
        char cl_device_name[1001];
        err = clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_ACCELERATOR,
                             16, devices, &device_count);
        if (err != CL_SUCCESS) {
            printf("Error: Failed to create a device group!\n");
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }

        //iterate all devices to select the target device. 
        for (unsigned i=0; i<device_count; i++) {
            err = clGetDeviceInfo(devices[i], CL_DEVICE_NAME, 1024, cl_device_name, 0);
            if (err != CL_SUCCESS) {
                printf("Error: Failed to get device name for device %d!\n", i);
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }
            //printf("CL_DEVICE_NAME %s\n", cl_device_name);
            if(strcmp(cl_device_name, target_device_name) == 0) {
                device_id = devices[i];
                device_found = 1;
                printf("INFO: Selected %s as the target device\n", cl_device_name);
            }
        }
        
        if (!device_found) {
            printf("ERROR: Target device %s not found. Exit.\n", target_device_name);
            exit(EXIT_FAILURE);
        }


        err = clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_ACCELERATOR,
                             1, &device_id, NULL);
        if (err != CL_SUCCESS)
            {
                printf("Error: Failed to create a device group!\n");
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }
      
        // Create a compute context 
        //
        context = clCreateContext(0, 1, &device_id, NULL, NULL, &err);
        if (!context)
            {
                printf("Error: Failed to create a compute context!\n");
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }

        // Create a command commands
        //
        commands = clCreateCommandQueue(context, device_id, 0, &err);
        if (!commands)
            {
                printf("Error: Failed to create a command commands!\n");
                printf("Error: code %i\n",err);
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }

        int status;

        // Create Program Objects
        //
      
        // Load binary from disk
        unsigned char *kernelbinary;
        printf("INFO: Loading %s\n", xclbin);
        int n_i = load_file_to_memory(xclbin, (char **) &kernelbinary);
        if (n_i < 0) {
            printf("failed to load kernel from xclbin: %s\n", xclbin);
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }
        size_t n = n_i;
        // Create the compute program from offline
        program = clCreateProgramWithBinary(context, 1, &device_id, &n,
                                            (const unsigned char **) &kernelbinary, &status, &err);
        if ((!program) || (err!=CL_SUCCESS)) {
            printf("Error: Failed to create compute program from binary %d!\n", err);
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }

        // Build the program executable
        //
        err = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
        if (err != CL_SUCCESS)
            {
                size_t len;
                char buffer[2048];

                printf("Error: Failed to build program executable!\n");
                clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, sizeof(buffer), buffer, &len);
                printf("%s\n", buffer);
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }

        // Create the compute kernel in the program we wish to run
        //
        kernel = clCreateKernel(program, "curved_kernel", &err);
        if (!kernel || err != CL_SUCCESS)
            {
                printf("Error: Failed to create compute kernel %d!\n", err);
                printf("Test failed\n");
                exit(EXIT_FAILURE);
            }

        // Create the input and output arrays in device memory for our calculation
        //
        var_matrix_cl    = clCreateBuffer(context,  CL_MEM_READ_ONLY, sizeof(uint16_t) * 12, NULL, NULL);
        var_curve_cl     = clCreateBuffer(context,  CL_MEM_READ_ONLY, sizeof(uint8_t) * 1024, NULL, NULL);
        var_input_cl     = clCreateBuffer(context,  CL_MEM_READ_ONLY, sizeof(uint16_t) * tile_num_dim0*tile_num_dim1*TILE_SIZE_DIM0*TILE_SIZE_DIM1, NULL, NULL);
        var_processed_cl = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(uint8_t) * tile_num_dim0*tile_num_dim1*(TILE_SIZE_DIM0*TILE_SIZE_DIM1/21*64), NULL, NULL);
        if (!var_input_cl || !var_processed_cl)
        {
            printf("Error: Failed to allocate device memory!\n");
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }
        
        // Write our data set into the input array in device memory 
        //
        timespec write_begin, write_end;
        cl_event writeevent;
        clock_gettime(CLOCK_REALTIME, &write_begin);
        err = clEnqueueWriteBuffer(commands, var_matrix_cl, CL_TRUE, 0, sizeof(uint16_t) * 12, var_matrix, 0, NULL, NULL);
        err = clEnqueueWriteBuffer(commands, var_curve_cl,  CL_TRUE, 0, sizeof(uint8_t) * 1024, var_curve, 0, NULL, NULL);
        err = clEnqueueWriteBuffer(commands, var_input_cl,  CL_TRUE, 0, sizeof(uint16_t) * tile_num_dim0*tile_num_dim1*TILE_SIZE_DIM0*TILE_SIZE_DIM1, var_input_buf, 0, NULL, &writeevent);
        if (err != CL_SUCCESS)
        {
            printf("Error: Failed to write to source array a!\n");
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }

        clWaitForEvents(1, &writeevent);
        clock_gettime(CLOCK_REALTIME, &write_end);

        // Set the arguments to our compute kernel
        //
        err = 0;

        err |= clSetKernelArg(kernel, 0, sizeof(cl_mem), &var_matrix_cl);
        err |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &var_curve_cl);
        err |= clSetKernelArg(kernel, 2, sizeof(cl_mem), &var_processed_cl);
        err |= clSetKernelArg(kernel, 3, sizeof(cl_mem), &var_input_cl);
        err |= clSetKernelArg(kernel, 4, sizeof(tile_num_dim0), &tile_num_dim0);
        err |= clSetKernelArg(kernel, 5, sizeof(tile_num_dim1), &tile_num_dim1);
        err |= clSetKernelArg(kernel, 6, sizeof(var_processed_extent_0), &var_processed_extent_0);
        err |= clSetKernelArg(kernel, 7, sizeof(var_processed_extent_1), &var_processed_extent_1);
        err |= clSetKernelArg(kernel, 8, sizeof(var_processed_min_0), &var_processed_min_0);
        err |= clSetKernelArg(kernel, 9, sizeof(var_processed_min_1), &var_processed_min_1);
        if (err != CL_SUCCESS)
        {
            printf("Error: Failed to set kernel arguments! %d\n", err);
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }
        
        cl_event execute;
        for(int i = 0; i<0; ++i)
        {
            err = clEnqueueTask(commands, kernel, 0, NULL, &execute);
            clWaitForEvents(1, &execute);
        }

        // Execute the kernel over the entire range of our 1d input data set
        // using the maximum number of work group items for this device
        //
        timespec execute_begin, execute_end;
        clock_gettime(CLOCK_REALTIME, &execute_begin);
        err = clEnqueueTask(commands, kernel, 0, NULL, &execute);
        if (err)
        {
            printf("Error: Failed to execute kernel! %d\n", err);
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }

        clWaitForEvents(1, &execute);
        clock_gettime(CLOCK_REALTIME, &execute_end);

        // Read back the results from the device to verify the output
        //
        timespec read_begin, read_end;
        cl_event readevent;
        clock_gettime(CLOCK_REALTIME, &read_begin);
        err = clEnqueueReadBuffer( commands, var_processed_cl, CL_TRUE, 0, sizeof(uint8_t) * tile_num_dim0*tile_num_dim1*(TILE_SIZE_DIM0*TILE_SIZE_DIM1/21*64), var_processed_buf, 0, NULL, &readevent );  
        if (err != CL_SUCCESS)
        {
            printf("Error: Failed to read output array! %d\n", err);
            printf("Test failed\n");
            exit(EXIT_FAILURE);
        }

        clWaitForEvents(1, &readevent);
        clock_gettime(CLOCK_REALTIME, &read_end);

        double elapsed_time = 0.;
        elapsed_time = (double(write_end.tv_sec-write_begin.tv_sec)+(write_end.tv_nsec-write_begin.tv_nsec)/1e9)*1e6;
        printf("PCIe write time: %lf us\n", elapsed_time);
        elapsed_time = (double(execute_end.tv_sec-execute_begin.tv_sec)+(execute_end.tv_nsec-execute_begin.tv_nsec)/1e9)*1e6;
        printf("Kernel run time: %lf us\n", elapsed_time);
        elapsed_time = (double(read_end.tv_sec-read_begin.tv_sec)+(read_end.tv_nsec-read_begin.tv_nsec)/1e9)*1e6;
        printf("PCIe read  time: %lf us\n", elapsed_time);

        // Shutdown and cleanup
        //
        clReleaseMemObject(var_matrix_cl);
        clReleaseMemObject(var_curve_cl);
        clReleaseMemObject(var_input_cl);
        clReleaseMemObject(var_processed_cl);
        clReleaseProgram(program);
        clReleaseKernel(kernel);
        clReleaseCommandQueue(commands);
        clReleaseContext(context);

        for(int32_t tile_index_dim1 = 0; tile_index_dim1 < tile_num_dim1; ++tile_index_dim1)
        {
            uint32_t actual_tile_size_dim1 = (tile_index_dim1==tile_num_dim1-1) ? var_processed_extent_1-((TILE_SIZE_DIM1)-(STENCIL_DIM1)+1)*tile_index_dim1 : (TILE_SIZE_DIM1)-(STENCIL_DIM1)+1;
            for(int32_t tile_index_dim0 = 0; tile_index_dim0 < tile_num_dim0; ++tile_index_dim0)
            {
                uint32_t actual_tile_size_dim0 = (tile_index_dim0==tile_num_dim0-1) ? var_processed_extent_0-((TILE_SIZE_DIM0)-(STENCIL_DIM0)+1)*tile_index_dim0 : (TILE_SIZE_DIM0)-(STENCIL_DIM0)+1;
                for(uint32_t k = 0; k < 3; ++k)
                {
                    for(uint32_t j = 0; j < actual_tile_size_dim1;++j)
                    {
                        for(uint32_t i = 0; i < actual_tile_size_dim0;++i)
                        {
                            // (x, y, z, w) is coordinates in tiled image
                            // (p, q, r, s) is coordinates in original image
                            // (i, j, k, l) is coordinates in a tile
                            //uint32_t x = tile_index_dim0*TILE_SIZE_DIM0+i;
                            //uint32_t y = tile_index_dim1*TILE_SIZE_DIM1+j;
                            //uint32_t z = k;
                            uint32_t p = tile_index_dim0*((TILE_SIZE_DIM0)-(STENCIL_DIM0)+1)+i;
                            uint32_t q = tile_index_dim1*((TILE_SIZE_DIM1)-(STENCIL_DIM1)+1)+j;
                            uint32_t r = k;
                            uint32_t tiled_offset = (tile_index_dim1*tile_num_dim0+tile_index_dim0)*((TILE_SIZE_DIM1)*(TILE_SIZE_DIM0)/21*64)+(j*(TILE_SIZE_DIM0)+i)/21*64+((j*(TILE_SIZE_DIM0)+i)%21)*3+k;
                            uint32_t original_offset = p*var_processed_stride_0+q*var_processed_stride_1+r*var_processed_stride_2;
                            var_processed[original_offset] = var_processed_buf[tiled_offset];
                        }
                    }
                }
            }
        }

        delete[] var_processed_buf;
        delete[] var_input_buf;
    } // if assign_5
    return 0;
}

int curved(float var_color_temp, float var_gamma, float var_contrast, int32_t var_blackLevel, int32_t var_whiteLevel,
           buffer_t *var_input_buffer, buffer_t *var_m3200_buffer, buffer_t *var_m7000_buffer,
           buffer_t *var_processed_buffer, const char* xclbin) HALIDE_FUNCTION_ATTRS {
    uint16_t *var_input = (uint16_t *)(var_input_buffer->host);
    (void)var_input;
    const bool var_input_host_and_dev_are_null = (var_input_buffer->host == NULL) && (var_input_buffer->dev == 0);
    (void)var_input_host_and_dev_are_null;
    int32_t var_input_min_0 = var_input_buffer->min[0];
    (void)var_input_min_0;
    int32_t var_input_min_1 = var_input_buffer->min[1];
    (void)var_input_min_1;
    int32_t var_input_min_2 = var_input_buffer->min[2];
    (void)var_input_min_2;
    int32_t var_input_min_3 = var_input_buffer->min[3];
    (void)var_input_min_3;
    int32_t var_input_extent_0 = var_input_buffer->extent[0];
    (void)var_input_extent_0;
    int32_t var_input_extent_1 = var_input_buffer->extent[1];
    (void)var_input_extent_1;
    int32_t var_input_extent_2 = var_input_buffer->extent[2];
    (void)var_input_extent_2;
    int32_t var_input_extent_3 = var_input_buffer->extent[3];
    (void)var_input_extent_3;
    int32_t var_input_stride_0 = var_input_buffer->stride[0];
    (void)var_input_stride_0;
    int32_t var_input_stride_1 = var_input_buffer->stride[1];
    (void)var_input_stride_1;
    int32_t var_input_stride_2 = var_input_buffer->stride[2];
    (void)var_input_stride_2;
    int32_t var_input_stride_3 = var_input_buffer->stride[3];
    (void)var_input_stride_3;
    int32_t var_input_elem_size = var_input_buffer->elem_size;
    (void)var_input_elem_size;

    float *var_m3200 = (float *)(var_m3200_buffer->host);
    (void)var_m3200;
    const bool var_m3200_host_and_dev_are_null = (var_m3200_buffer->host == NULL) && (var_m3200_buffer->dev == 0);
    (void)var_m3200_host_and_dev_are_null;
    int32_t var_m3200_min_0 = var_m3200_buffer->min[0];
    (void)var_m3200_min_0;
    int32_t var_m3200_min_1 = var_m3200_buffer->min[1];
    (void)var_m3200_min_1;
    int32_t var_m3200_min_2 = var_m3200_buffer->min[2];
    (void)var_m3200_min_2;
    int32_t var_m3200_min_3 = var_m3200_buffer->min[3];
    (void)var_m3200_min_3;
    int32_t var_m3200_extent_0 = var_m3200_buffer->extent[0];
    (void)var_m3200_extent_0;
    int32_t var_m3200_extent_1 = var_m3200_buffer->extent[1];
    (void)var_m3200_extent_1;
    int32_t var_m3200_extent_2 = var_m3200_buffer->extent[2];
    (void)var_m3200_extent_2;
    int32_t var_m3200_extent_3 = var_m3200_buffer->extent[3];
    (void)var_m3200_extent_3;
    int32_t var_m3200_stride_0 = var_m3200_buffer->stride[0];
    (void)var_m3200_stride_0;
    int32_t var_m3200_stride_1 = var_m3200_buffer->stride[1];
    (void)var_m3200_stride_1;
    int32_t var_m3200_stride_2 = var_m3200_buffer->stride[2];
    (void)var_m3200_stride_2;
    int32_t var_m3200_stride_3 = var_m3200_buffer->stride[3];
    (void)var_m3200_stride_3;
    int32_t var_m3200_elem_size = var_m3200_buffer->elem_size;
    (void)var_m3200_elem_size;

    float *var_m7000 = (float *)(var_m7000_buffer->host);
    (void)var_m7000;
    const bool var_m7000_host_and_dev_are_null = (var_m7000_buffer->host == NULL) && (var_m7000_buffer->dev == 0);
    (void)var_m7000_host_and_dev_are_null;
    int32_t var_m7000_min_0 = var_m7000_buffer->min[0];
    (void)var_m7000_min_0;
    int32_t var_m7000_min_1 = var_m7000_buffer->min[1];
    (void)var_m7000_min_1;
    int32_t var_m7000_min_2 = var_m7000_buffer->min[2];
    (void)var_m7000_min_2;
    int32_t var_m7000_min_3 = var_m7000_buffer->min[3];
    (void)var_m7000_min_3;
    int32_t var_m7000_extent_0 = var_m7000_buffer->extent[0];
    (void)var_m7000_extent_0;
    int32_t var_m7000_extent_1 = var_m7000_buffer->extent[1];
    (void)var_m7000_extent_1;
    int32_t var_m7000_extent_2 = var_m7000_buffer->extent[2];
    (void)var_m7000_extent_2;
    int32_t var_m7000_extent_3 = var_m7000_buffer->extent[3];
    (void)var_m7000_extent_3;
    int32_t var_m7000_stride_0 = var_m7000_buffer->stride[0];
    (void)var_m7000_stride_0;
    int32_t var_m7000_stride_1 = var_m7000_buffer->stride[1];
    (void)var_m7000_stride_1;
    int32_t var_m7000_stride_2 = var_m7000_buffer->stride[2];
    (void)var_m7000_stride_2;
    int32_t var_m7000_stride_3 = var_m7000_buffer->stride[3];
    (void)var_m7000_stride_3;
    int32_t var_m7000_elem_size = var_m7000_buffer->elem_size;
    (void)var_m7000_elem_size;

    uint16_t *var_processed = (uint16_t *)(var_processed_buffer->host);
    (void)var_processed;
    const bool var_processed_host_and_dev_are_null = (var_processed_buffer->host == NULL) && (var_processed_buffer->dev == 0);
    (void)var_processed_host_and_dev_are_null;
    int32_t var_processed_min_0 = var_processed_buffer->min[0];
    (void)var_processed_min_0;
    int32_t var_processed_min_1 = var_processed_buffer->min[1];
    (void)var_processed_min_1;
    int32_t var_processed_min_2 = var_processed_buffer->min[2];
    (void)var_processed_min_2;
    int32_t var_processed_min_3 = var_processed_buffer->min[3];
    (void)var_processed_min_3;
    int32_t var_processed_extent_0 = var_processed_buffer->extent[0];
    (void)var_processed_extent_0;
    int32_t var_processed_extent_1 = var_processed_buffer->extent[1];
    (void)var_processed_extent_1;
    int32_t var_processed_extent_2 = var_processed_buffer->extent[2];
    (void)var_processed_extent_2;
    int32_t var_processed_extent_3 = var_processed_buffer->extent[3];
    (void)var_processed_extent_3;
    int32_t var_processed_stride_0 = var_processed_buffer->stride[0];
    (void)var_processed_stride_0;
    int32_t var_processed_stride_1 = var_processed_buffer->stride[1];
    (void)var_processed_stride_1;
    int32_t var_processed_stride_2 = var_processed_buffer->stride[2];
    (void)var_processed_stride_2;
    int32_t var_processed_stride_3 = var_processed_buffer->stride[3];
    (void)var_processed_stride_3;
    int32_t var_processed_elem_size = var_processed_buffer->elem_size;
    (void)var_processed_elem_size;

    int32_t assign_81 = curved_wrapped(var_color_temp, var_gamma, var_contrast, var_blackLevel, var_whiteLevel,
                                       var_input_buffer, var_m3200_buffer, var_m7000_buffer, var_processed_buffer, xclbin);
    bool assign_82 = assign_81 == 0;
    if (!assign_82)     {
        return assign_81;
    }
    return 0;
}

