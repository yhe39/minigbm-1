/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_I915

#include <errno.h>
#include <i915_drm.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "drv_priv.h"
#include "helpers.h"
#include "util.h"
#include "i915_private.h"

#define I915_CACHELINE_SIZE 64
#define I915_CACHELINE_MASK (I915_CACHELINE_SIZE - 1)

static const uint32_t render_target_formats[] = { DRM_FORMAT_ABGR8888,    DRM_FORMAT_ARGB1555,
						  DRM_FORMAT_ARGB8888,    DRM_FORMAT_RGB565,
						  DRM_FORMAT_XBGR2101010, DRM_FORMAT_XBGR8888,
						  DRM_FORMAT_XRGB1555,    DRM_FORMAT_XRGB2101010,
						  DRM_FORMAT_XRGB8888 };

static const uint32_t tileable_texture_source_formats[] = { DRM_FORMAT_GR88, DRM_FORMAT_R8,
							    DRM_FORMAT_UYVY, DRM_FORMAT_YUYV,
							    DRM_FORMAT_YVYU, DRM_FORMAT_VYUY };

static const uint32_t texture_source_formats[] = { DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID,
						   DRM_FORMAT_NV12 };

struct i915_device
{
	uint32_t gen;
	int32_t has_llc;
	uint64_t cursor_width;
	uint64_t cursor_height;
};

static uint32_t i915_get_gen(int device_id)
{
	const uint16_t gen3_ids[] = { 0x2582, 0x2592, 0x2772, 0x27A2, 0x27AE,
				      0x29C2, 0x29B2, 0x29D2, 0xA001, 0xA011 };
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(gen3_ids); i++)
		if (gen3_ids[i] == device_id)
			return 3;

	return 4;
}

static int i915_add_kms_item(struct driver *drv, const struct kms_item *item)
{
	uint32_t i;
	struct combination *combo;

	/*
	 * Older hardware can't scanout Y-tiled formats. Newer devices can, and
	 * report this functionality via format modifiers.
	 */
	for (i = 0; i < drv->combos.size; i++) {
		combo = &drv->combos.data[i];
		if (combo->format != item->format)
			continue;

		if (item->modifier == DRM_FORMAT_MOD_INVALID &&
		    combo->metadata.tiling == I915_TILING_X) {
			/*
			 * FIXME: drv_query_kms() does not report the available modifiers
			 * yet, but we know that all hardware can scanout from X-tiled
			 * buffers, so let's add this to our combinations, except for
			 * cursor, which must not be tiled.
			 */
			combo->use_flags |= item->use_flags & ~BO_USE_CURSOR;
		}

		if (combo->metadata.modifier == item->modifier)
			combo->use_flags |= item->use_flags;
	}

	return 0;
}

static int i915_add_combinations(struct driver *drv)
{
	int ret;
	uint32_t i, num_items;
	struct kms_item *items;
	struct format_metadata metadata;
	uint64_t render_use_flags, texture_use_flags;

	render_use_flags = BO_USE_RENDER_MASK;
	texture_use_flags = BO_USE_TEXTURE_MASK;

	metadata.tiling = I915_TILING_NONE;
	metadata.priority = 1;
	metadata.modifier = DRM_FORMAT_MOD_LINEAR;

	ret = drv_add_combinations(drv, render_target_formats, ARRAY_SIZE(render_target_formats),
				   &metadata, render_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, texture_source_formats, ARRAY_SIZE(texture_source_formats),
				   &metadata, texture_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, tileable_texture_source_formats,
				   ARRAY_SIZE(tileable_texture_source_formats), &metadata,
				   texture_use_flags);
	if (ret)
		return ret;

	drv_modify_combination(drv, DRM_FORMAT_XRGB8888, &metadata, BO_USE_CURSOR | BO_USE_SCANOUT);
	drv_modify_combination(drv, DRM_FORMAT_ARGB8888, &metadata, BO_USE_CURSOR | BO_USE_SCANOUT);

	/* IPU3 camera ISP supports only NV12 output. */
	drv_modify_combination(drv, DRM_FORMAT_NV12, &metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	/*
	 * R8 format is used for Android's HAL_PIXEL_FORMAT_BLOB and is used for JPEG snapshots
	 * from camera.
	 */
	drv_modify_combination(drv, DRM_FORMAT_R8, &metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

	render_use_flags &= ~BO_USE_RENDERSCRIPT;
	render_use_flags &= ~BO_USE_SW_WRITE_OFTEN;
	render_use_flags &= ~BO_USE_SW_READ_OFTEN;
	render_use_flags &= ~BO_USE_LINEAR;

	texture_use_flags &= ~BO_USE_RENDERSCRIPT;
	texture_use_flags &= ~BO_USE_SW_WRITE_OFTEN;
	texture_use_flags &= ~BO_USE_SW_READ_OFTEN;
	texture_use_flags &= ~BO_USE_LINEAR;

	metadata.tiling = I915_TILING_X;
	metadata.priority = 2;
	metadata.modifier = I915_FORMAT_MOD_X_TILED;

	ret = drv_add_combinations(drv, render_target_formats, ARRAY_SIZE(render_target_formats),
				   &metadata, render_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, tileable_texture_source_formats,
				   ARRAY_SIZE(tileable_texture_source_formats), &metadata,
				   texture_use_flags);
	if (ret)
		return ret;

	metadata.tiling = I915_TILING_Y;
	metadata.priority = 3;
	metadata.modifier = I915_FORMAT_MOD_Y_TILED;

	ret = drv_add_combinations(drv, render_target_formats, ARRAY_SIZE(render_target_formats),
				   &metadata, render_use_flags);
	if (ret)
		return ret;

	ret = drv_add_combinations(drv, tileable_texture_source_formats,
				   ARRAY_SIZE(tileable_texture_source_formats), &metadata,
				   texture_use_flags);
	if (ret)
		return ret;

	i915_private_add_combinations(drv);

	items = drv_query_kms(drv, &num_items);
	if (!items || !num_items)
		return 0;

	for (i = 0; i < num_items; i++) {
		ret = i915_add_kms_item(drv, &items[i]);
		if (ret) {
			free(items);
			return ret;
		}
	}

	free(items);
	return 0;
}

static int i915_align_dimensions(struct bo *bo, uint32_t tiling, uint64_t modifier,
				 uint32_t *stride, uint32_t *aligned_height)
{
	struct i915_device *i915 = bo->drv->priv;
	uint32_t horizontal_alignment = 4;
	uint32_t vertical_alignment = 4;

	switch (tiling) {
	default:
	case I915_TILING_NONE:
		horizontal_alignment = 64;
		if (modifier == I915_FORMAT_MOD_Yf_TILED ||
		    modifier == I915_FORMAT_MOD_Yf_TILED_CCS) {
			horizontal_alignment = 128;
			vertical_alignment = 32;
		}

		break;

	case I915_TILING_X:
		horizontal_alignment = 512;
		vertical_alignment = 8;
		break;

	case I915_TILING_Y:
		if (i915->gen == 3) {
			horizontal_alignment = 512;
			vertical_alignment = 8;
		} else {
                       horizontal_alignment = 128;
                       vertical_alignment = 32;
		}
		break;
	}

	/*
	 * The alignment calculated above is based on the full size luma plane and to have chroma
	 * planes properly aligned with subsampled formats, we need to multiply luma alignment by
	 * subsampling factor.
	 */
	switch (bo->format) {
	case DRM_FORMAT_YVU420_ANDROID:
	case DRM_FORMAT_YVU420:
		horizontal_alignment *= 2;
	/* Fall through */
	case DRM_FORMAT_NV12:
		vertical_alignment *= 2;
		break;
	}

	i915_private_align_dimensions(bo->format, &vertical_alignment);

	*aligned_height = ALIGN(bo->height, vertical_alignment);
	if (i915->gen > 3) {
		*stride = ALIGN(*stride, horizontal_alignment);
	} else {
		while (*stride > horizontal_alignment)
			horizontal_alignment <<= 1;

		*stride = horizontal_alignment;
	}

	if (i915->gen <= 3 && *stride > 8192)
		return -EINVAL;

	return 0;
}

static void i915_clflush(void *start, size_t size)
{
	void *p = (void *)(((uintptr_t)start) & ~I915_CACHELINE_MASK);
	void *end = (void *)((uintptr_t)start + size);

	__builtin_ia32_mfence();
	while (p < end) {
		__builtin_ia32_clflush(p);
		p = (void *)((uintptr_t)p + I915_CACHELINE_SIZE);
	}
}

static int i915_init(struct driver *drv)
{
	int ret;
	int device_id;
	struct i915_device *i915;
	drm_i915_getparam_t get_param;

	i915 = calloc(1, sizeof(*i915));
	if (!i915)
		return -ENOMEM;

	memset(&get_param, 0, sizeof(get_param));
	get_param.param = I915_PARAM_CHIPSET_ID;
	get_param.value = &device_id;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_GETPARAM, &get_param);
	if (ret) {
		fprintf(stderr, "drv: Failed to get I915_PARAM_CHIPSET_ID\n");
		free(i915);
		return -EINVAL;
	}

	i915->gen = i915_get_gen(device_id);

	memset(&get_param, 0, sizeof(get_param));
	get_param.param = I915_PARAM_HAS_LLC;
	get_param.value = &i915->has_llc;
	ret = drmIoctl(drv->fd, DRM_IOCTL_I915_GETPARAM, &get_param);
	if (ret) {
		fprintf(stderr, "drv: Failed to get I915_PARAM_HAS_LLC\n");
		free(i915);
		return -EINVAL;
	}

	drv->priv = i915;

	i915_private_init(drv, &i915->cursor_width, &i915->cursor_height);

	return i915_add_combinations(drv);
}

static int i915_bo_create_for_modifier(struct bo *bo, uint32_t width, uint32_t height,
				       uint32_t format, uint64_t modifier)
{
	int ret;
	size_t plane;
	uint32_t stride;
	struct drm_i915_gem_create gem_create;
	struct drm_i915_gem_set_tiling gem_set_tiling;
	struct i915_device *i915_dev = (struct i915_device *)bo->drv->priv;

	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		bo->tiling = I915_TILING_NONE;
		break;
	case I915_FORMAT_MOD_X_TILED:
		bo->tiling = I915_TILING_X;
		break;
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_Y_TILED_CCS:
        case I915_FORMAT_MOD_Yf_TILED:
        case I915_FORMAT_MOD_Yf_TILED_CCS:
		bo->tiling = I915_TILING_Y;
		break;
	}

	stride = drv_stride_from_format(format, width, 0);

	/*
	 * Align cursor width and height to values expected by Intel
	 * HW.
	 */
	if (bo->use_flags & BO_USE_CURSOR) {
		width = ALIGN(width, i915_dev->cursor_width);
		height = ALIGN(height, i915_dev->cursor_height);
		stride = drv_stride_from_format(format, width, 0);
	} else {
		ret = i915_align_dimensions(bo, bo->tiling, modifier, &stride, &height);
		if (ret)
			return ret;
	}

	/*
	 * HAL_PIXEL_FORMAT_YV12 requires the buffer height not be aligned, but we need to keep
	 * total size as with aligned height to ensure enough padding space after each plane to
	 * satisfy GPU alignment requirements.
	 *
	 * We do it by first calling drv_bo_from_format() with aligned height and
	 * DRM_FORMAT_YVU420, which allows height alignment, saving the total size it calculates
	 * and then calling it again with requested parameters.
	 *
	 * This relies on the fact that i965 driver uses separate surfaces for each plane and
	 * contents of padding bytes is not affected, as it is only used to satisfy GPU cache
	 * requests.
	 *
	 * This is enforced by Mesa in src/intel/isl/isl_gen8.c, inside
	 * isl_gen8_choose_image_alignment_el(), which is used for GEN9 and GEN8.
	 */
	if (format == DRM_FORMAT_YVU420_ANDROID) {
		uint32_t unaligned_height = bo->height;
		size_t total_size;

		drv_bo_from_format(bo, stride, height, DRM_FORMAT_YVU420);
		total_size = bo->total_size;
		drv_bo_from_format(bo, stride, unaligned_height, format);
		bo->total_size = total_size;
	} else {

		drv_bo_from_format(bo, stride, height, format);
	}

	if (modifier == I915_FORMAT_MOD_Y_TILED_CCS || modifier == I915_FORMAT_MOD_Yf_TILED_CCS) {
		/*
		 * For compressed surfaces, we need a color control surface
		 * (CCS). Color compression is only supported for Y tiled
		 * surfaces, and for each 32x16 tiles in the main surface we
		 * need a tile in the control surface.  Y tiles are 128 bytes
		 * wide and 32 lines tall and we use that to first compute the
		 * width and height in tiles of the main surface. stride and
		 * height are already multiples of 128 and 32, respectively:
		 */
		uint32_t width_in_tiles = stride / 128;
		uint32_t height_in_tiles = height / 32;

		/*
		 * Now, compute the width and height in tiles of the control
		 * surface by dividing and rounding up.
		 */
		uint32_t ccs_width_in_tiles = DIV_ROUND_UP(width_in_tiles, 32);
		uint32_t ccs_height_in_tiles = DIV_ROUND_UP(height_in_tiles, 16);
		uint32_t ccs_size = ccs_width_in_tiles * ccs_height_in_tiles * 4096;

		/*
		 * With stride and height aligned to y tiles, bo->total_size
		 * is already a multiple of 4096, which is the required
		 * alignment of the CCS.
		 */
		bo->num_planes = 2;
		bo->strides[1] = ccs_width_in_tiles * 128;
		bo->sizes[1] = ccs_size;
		bo->offsets[1] = bo->total_size;
		bo->total_size += ccs_size;
	}

	/*
	 * Quoting Mesa ISL library:
	 *
	 *    - For linear surfaces, additional padding of 64 bytes is required at
	 *      the bottom of the surface. This is in addition to the padding
	 *      required above.
	 */
	if (bo->tiling == I915_TILING_NONE)
		bo->total_size += 64;

        /*
         * Ensure we pass aligned width/height.
         */
        bo->width = width;
        bo->height = height;

	memset(&gem_create, 0, sizeof(gem_create));
	gem_create.size = bo->total_size;

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_CREATE, &gem_create);
	if (ret) {
		fprintf(stderr, "drv: DRM_IOCTL_I915_GEM_CREATE failed (size=%llu)\n",
			gem_create.size);
		return ret;
	}

	for (plane = 0; plane < bo->num_planes; plane++)
		bo->handles[plane].u32 = gem_create.handle;

	memset(&gem_set_tiling, 0, sizeof(gem_set_tiling));
	gem_set_tiling.handle = bo->handles[0].u32;
	gem_set_tiling.tiling_mode = bo->tiling;
	gem_set_tiling.stride = bo->strides[0];

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_SET_TILING, &gem_set_tiling);
	if (ret) {
		struct drm_gem_close gem_close;
		memset(&gem_close, 0, sizeof(gem_close));
		gem_close.handle = bo->handles[0].u32;
		drmIoctl(bo->drv->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);

		fprintf(stderr, "drv: DRM_IOCTL_I915_GEM_SET_TILING failed with %d", errno);
		return -errno;
	}

	return 0;
}

static int i915_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			  uint64_t use_flags)
{
	struct combination *combo;

	combo = drv_get_combination(bo->drv, format, use_flags);
	if (!combo)
		return -EINVAL;

	return i915_bo_create_for_modifier(bo, width, height, format, combo->metadata.modifier);
}

static int i915_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height,
					 uint32_t format, const uint64_t *modifiers, uint32_t count)
{
	static const uint64_t modifier_order[] = {
		I915_FORMAT_MOD_Y_TILED,      I915_FORMAT_MOD_Yf_TILED, I915_FORMAT_MOD_Y_TILED_CCS,
		I915_FORMAT_MOD_Yf_TILED_CCS, I915_FORMAT_MOD_X_TILED,  DRM_FORMAT_MOD_LINEAR,
	};
	uint64_t modifier;

	modifier = drv_pick_modifier(modifiers, count, modifier_order, ARRAY_SIZE(modifier_order));

	bo->format_modifiers[0] = modifier;

	return i915_bo_create_for_modifier(bo, width, height, format, modifier);
}

static void i915_close(struct driver *drv)
{
	free(drv->priv);
	drv->priv = NULL;
}

static int i915_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	int ret;
	struct drm_i915_gem_get_tiling gem_get_tiling;

	ret = drv_prime_bo_import(bo, data);
	if (ret)
		return ret;

	/* TODO(gsingh): export modifiers and get rid of backdoor tiling. */
	memset(&gem_get_tiling, 0, sizeof(gem_get_tiling));
	gem_get_tiling.handle = bo->handles[0].u32;

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_GET_TILING, &gem_get_tiling);
	if (ret) {
		drv_gem_bo_destroy(bo);
		fprintf(stderr, "drv: DRM_IOCTL_I915_GEM_GET_TILING failed.");
		return ret;
	}

	bo->tiling = gem_get_tiling.tiling_mode;
	return 0;
}

static void *i915_bo_map(struct bo *bo, struct map_info *data, size_t plane, uint32_t map_flags)
{
	int ret;
	void *addr;

	if (bo->tiling == I915_TILING_NONE) {
		struct drm_i915_gem_mmap gem_map;
		memset(&gem_map, 0, sizeof(gem_map));

		if ((bo->use_flags & BO_USE_SCANOUT) && !(bo->use_flags & BO_USE_RENDERSCRIPT))
			gem_map.flags = I915_MMAP_WC;

		gem_map.handle = bo->handles[0].u32;
		gem_map.offset = 0;
		gem_map.size = bo->total_size;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP, &gem_map);
		if (ret) {
			fprintf(stderr, "drv: DRM_IOCTL_I915_GEM_MMAP failed\n");
			return MAP_FAILED;
		}

		addr = (void *)(uintptr_t)gem_map.addr_ptr;
	} else {
		struct drm_i915_gem_mmap_gtt gem_map;
		memset(&gem_map, 0, sizeof(gem_map));

		gem_map.handle = bo->handles[0].u32;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &gem_map);
		if (ret) {
			fprintf(stderr, "drv: DRM_IOCTL_I915_GEM_MMAP_GTT failed\n");
			return MAP_FAILED;
		}

		addr = mmap(0, bo->total_size, drv_get_prot(map_flags), MAP_SHARED, bo->drv->fd,
			    gem_map.offset);
	}

	if (addr == MAP_FAILED) {
		fprintf(stderr, "drv: i915 GEM mmap failed\n");
		return addr;
	}

	data->length = bo->total_size;
	return addr;
}

static int i915_bo_invalidate(struct bo *bo, struct map_info *data)
{
	int ret;
	struct drm_i915_gem_set_domain set_domain;

	memset(&set_domain, 0, sizeof(set_domain));
	set_domain.handle = bo->handles[0].u32;
	if (bo->tiling == I915_TILING_NONE) {
		set_domain.read_domains = I915_GEM_DOMAIN_CPU;
		if (data->map_flags & BO_MAP_WRITE)
			set_domain.write_domain = I915_GEM_DOMAIN_CPU;
	} else {
		set_domain.read_domains = I915_GEM_DOMAIN_GTT;
		if (data->map_flags & BO_MAP_WRITE)
			set_domain.write_domain = I915_GEM_DOMAIN_GTT;
	}

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain);
	if (ret) {
		fprintf(stderr, "drv: DRM_IOCTL_I915_GEM_SET_DOMAIN with %d\n", ret);
		return ret;
	}

	return 0;
}

static int i915_bo_flush(struct bo *bo, struct map_info *data)
{
	struct i915_device *i915 = bo->drv->priv;
	if (!i915->has_llc && bo->tiling == I915_TILING_NONE)
		i915_clflush(data->addr, data->length);

	return 0;
}

static uint32_t i915_resolve_format(uint32_t format, uint64_t use_flags)
{
	uint32_t resolved_format;
	if (i915_private_resolve_format(format, use_flags, &resolved_format)) {
	    return resolved_format;
	}

	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* KBL camera subsystem requires NV12. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE))
			return DRM_FORMAT_NV12;
		/*HACK: See b/28671744 */
		return DRM_FORMAT_XBGR8888;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		/*
		 * KBL camera subsystem requires NV12. Our other use cases
		 * don't care:
		 * - Hardware video supports NV12,
		 * - USB Camera HALv3 supports NV12,
		 * - USB Camera HALv1 doesn't use this format.
		 * Moreover, NV12 is preferred for video, due to overlay
		 * support on SKL+.
		 */
		return DRM_FORMAT_NV12;
	default:
		return format;
	}
}

struct backend backend_i915 = {
	.name = "i915",
	.init = i915_init,
	.close = i915_close,
	.bo_create = i915_bo_create,
	.bo_create_with_modifiers = i915_bo_create_with_modifiers,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = i915_bo_import,
	.bo_map = i915_bo_map,
	.bo_unmap = drv_bo_munmap,
	.bo_invalidate = i915_bo_invalidate,
	.bo_flush = i915_bo_flush,
	.resolve_format = i915_resolve_format,
};

#endif
