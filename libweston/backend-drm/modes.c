/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2017, 2018 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
 * Copyright (c) 2018 DisplayLink (UK) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#if HAVE_LIBDISPLAY_INFO
#include <libdisplay-info/info.h>
#endif

#include "drm-internal.h"
#include "shared/weston-drm-fourcc.h"
#include "shared/xalloc.h"

struct drm_head_info {
	char *make; /* The monitor make (PNP ID or company name). */
	char *model; /* The monitor model (product name). */
	char *serial_number;

	/* The monitor supported EOTF modes, combination of
 	 * enum weston_eotf_mode bits.
	 */
	uint32_t eotf_mask;
};

static void
drm_head_info_fini(struct drm_head_info *dhi)
{
	free(dhi->make);
	free(dhi->model);
	free(dhi->serial_number);
	*dhi = (struct drm_head_info){};
}

static const char *const aspect_ratio_as_string[] = {
	[WESTON_MODE_PIC_AR_NONE] = "",
	[WESTON_MODE_PIC_AR_4_3] = " 4:3",
	[WESTON_MODE_PIC_AR_16_9] = " 16:9",
	[WESTON_MODE_PIC_AR_64_27] = " 64:27",
	[WESTON_MODE_PIC_AR_256_135] = " 256:135",
};

/*
 * Get the aspect-ratio from drmModeModeInfo mode flags.
 *
 * @param drm_mode_flags- flags from drmModeModeInfo structure.
 * @returns aspect-ratio as encoded in enum 'weston_mode_aspect_ratio'.
 */
static enum weston_mode_aspect_ratio
drm_to_weston_mode_aspect_ratio(uint32_t drm_mode_flags)
{
	switch (drm_mode_flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_4_3:
		return WESTON_MODE_PIC_AR_4_3;
	case DRM_MODE_FLAG_PIC_AR_16_9:
		return WESTON_MODE_PIC_AR_16_9;
	case DRM_MODE_FLAG_PIC_AR_64_27:
		return WESTON_MODE_PIC_AR_64_27;
	case DRM_MODE_FLAG_PIC_AR_256_135:
		return WESTON_MODE_PIC_AR_256_135;
	case DRM_MODE_FLAG_PIC_AR_NONE:
	default:
		return WESTON_MODE_PIC_AR_NONE;
	}
}

static const char *
aspect_ratio_to_string(enum weston_mode_aspect_ratio ratio)
{
	if (ratio < 0 || ratio >= ARRAY_LENGTH(aspect_ratio_as_string) ||
	    !aspect_ratio_as_string[ratio])
		return " (unknown aspect ratio)";

	return aspect_ratio_as_string[ratio];
}

static int
drm_subpixel_to_wayland(int drm_value)
{
	switch (drm_value) {
	default:
	case DRM_MODE_SUBPIXEL_UNKNOWN:
		return WL_OUTPUT_SUBPIXEL_UNKNOWN;
	case DRM_MODE_SUBPIXEL_NONE:
		return WL_OUTPUT_SUBPIXEL_NONE;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;
	case DRM_MODE_SUBPIXEL_VERTICAL_RGB:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;
	case DRM_MODE_SUBPIXEL_VERTICAL_BGR:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;
	}
}

int
drm_mode_ensure_blob(struct drm_device *device, struct drm_mode *mode)
{
	struct drm_backend *backend = device->backend;
	int ret;

	if (mode->blob_id)
		return 0;

	ret = drmModeCreatePropertyBlob(device->drm.fd,
					&mode->mode_info,
					sizeof(mode->mode_info),
					&mode->blob_id);
	if (ret != 0)
		weston_log("failed to create mode property blob: %s\n",
			   strerror(errno));

	drm_debug(backend, "\t\t\t[atomic] created new mode blob %lu for %s\n",
		  (unsigned long) mode->blob_id, mode->mode_info.name);

	return ret;
}

static bool
check_non_desktop(struct drm_connector *connector, drmModeObjectPropertiesPtr props)
{
	struct drm_property_info *non_desktop_info =
		&connector->props[WDRM_CONNECTOR_NON_DESKTOP];

	return drm_property_get_value(non_desktop_info, props, 0);
}

static uint32_t
get_panel_orientation(struct drm_connector *connector, drmModeObjectPropertiesPtr props)
{
	struct drm_property_info *orientation =
		&connector->props[WDRM_CONNECTOR_PANEL_ORIENTATION];
	uint64_t kms_val =
		drm_property_get_value(orientation, props,
				       WDRM_PANEL_ORIENTATION_NORMAL);

	switch (kms_val) {
	case WDRM_PANEL_ORIENTATION_NORMAL:
		return WL_OUTPUT_TRANSFORM_NORMAL;
	case WDRM_PANEL_ORIENTATION_UPSIDE_DOWN:
		return WL_OUTPUT_TRANSFORM_180;
	case WDRM_PANEL_ORIENTATION_LEFT_SIDE_UP:
		return WL_OUTPUT_TRANSFORM_90;
	case WDRM_PANEL_ORIENTATION_RIGHT_SIDE_UP:
		return WL_OUTPUT_TRANSFORM_270;
	default:
		assert(!"unknown property value in get_panel_orientation");
	}
}

static int
parse_modeline(const char *s, drmModeModeInfo *mode)
{
	char hsync[16];
	char vsync[16];
	float fclock;

	memset(mode, 0, sizeof *mode);

	mode->type = DRM_MODE_TYPE_USERDEF;
	mode->hskew = 0;
	mode->vscan = 0;
	mode->vrefresh = 0;
	mode->flags = 0;

	if (sscanf(s, "%f %hd %hd %hd %hd %hd %hd %hd %hd %15s %15s",
		   &fclock,
		   &mode->hdisplay,
		   &mode->hsync_start,
		   &mode->hsync_end,
		   &mode->htotal,
		   &mode->vdisplay,
		   &mode->vsync_start,
		   &mode->vsync_end,
		   &mode->vtotal, hsync, vsync) != 11)
		return -1;

	mode->clock = fclock * 1000;
	if (strcasecmp(hsync, "+hsync") == 0)
		mode->flags |= DRM_MODE_FLAG_PHSYNC;
	else if (strcasecmp(hsync, "-hsync") == 0)
		mode->flags |= DRM_MODE_FLAG_NHSYNC;
	else
		return -1;

	if (strcasecmp(vsync, "+vsync") == 0)
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	else if (strcasecmp(vsync, "-vsync") == 0)
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	else
		return -1;

	snprintf(mode->name, sizeof mode->name, "%dx%d@%.3f",
		 mode->hdisplay, mode->vdisplay, fclock);

	return 0;
}

#if HAVE_LIBDISPLAY_INFO

static void
drm_head_info_from_edid(struct drm_head_info *dhi,
			const uint8_t *data,
			size_t length)
{
	struct di_info *di_ctx;
	const char *msg;

	di_ctx = di_info_parse_edid(data, length);
	if (!di_ctx)
		return;

	msg = di_info_get_failure_msg(di_ctx);
	if (msg)
		weston_log("DRM: EDID for the following head fails conformity:\n%s\n", msg);

	dhi->make = di_info_get_make(di_ctx);
	dhi->model = di_info_get_model(di_ctx);
	dhi->serial_number = di_info_get_serial(di_ctx);

	di_info_destroy(di_ctx);

	/* TODO: parse this from EDID */
	dhi->eotf_mask = WESTON_EOTF_MODE_ALL_MASK;
}

#else /* HAVE_LIBDISPLAY_INFO */

struct drm_edid {
	char eisa_id[13];
	char monitor_name[13];
	char pnp_id[5];
	char serial_number[13];
};

static void
edid_parse_string(const uint8_t *data, char text[])
{
	int i;
	int replaced = 0;

	/* this is always 12 bytes, but we can't guarantee it's null
	 * terminated or not junk. */
	strncpy(text, (const char *) data, 12);

	/* guarantee our new string is null-terminated */
	text[12] = '\0';

	/* remove insane chars */
	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] == '\n' ||
		    text[i] == '\r') {
			text[i] = '\0';
			break;
		}
	}

	/* ensure string is printable */
	for (i = 0; text[i] != '\0'; i++) {
		if (!isprint(text[i])) {
			text[i] = '-';
			replaced++;
		}
	}

	/* if the string is random junk, ignore the string */
	if (replaced > 4)
		text[0] = '\0';
}

#define EDID_DESCRIPTOR_ALPHANUMERIC_DATA_STRING	0xfe
#define EDID_DESCRIPTOR_DISPLAY_PRODUCT_NAME		0xfc
#define EDID_DESCRIPTOR_DISPLAY_PRODUCT_SERIAL_NUMBER	0xff
#define EDID_OFFSET_DATA_BLOCKS				0x36
#define EDID_OFFSET_LAST_BLOCK				0x6c
#define EDID_OFFSET_PNPID				0x08
#define EDID_OFFSET_SERIAL				0x0c

static int
edid_parse(struct drm_edid *edid, const uint8_t *data, size_t length)
{
	int i;
	uint32_t serial_number;

	/* check header */
	if (length < 128)
		return -1;
	if (data[0] != 0x00 || data[1] != 0xff)
		return -1;

	/* decode the PNP ID from three 5 bit words packed into 2 bytes
	 * /--08--\/--09--\
	 * 7654321076543210
	 * |\---/\---/\---/
	 * R  C1   C2   C3 */
	edid->pnp_id[0] = 'A' + ((data[EDID_OFFSET_PNPID + 0] & 0x7c) / 4) - 1;
	edid->pnp_id[1] = 'A' + ((data[EDID_OFFSET_PNPID + 0] & 0x3) * 8) + ((data[EDID_OFFSET_PNPID + 1] & 0xe0) / 32) - 1;
	edid->pnp_id[2] = 'A' + (data[EDID_OFFSET_PNPID + 1] & 0x1f) - 1;
	edid->pnp_id[3] = '\0';

	/* maybe there isn't a ASCII serial number descriptor, so use this instead */
	serial_number = (uint32_t) data[EDID_OFFSET_SERIAL + 0];
	serial_number += (uint32_t) data[EDID_OFFSET_SERIAL + 1] * 0x100;
	serial_number += (uint32_t) data[EDID_OFFSET_SERIAL + 2] * 0x10000;
	serial_number += (uint32_t) data[EDID_OFFSET_SERIAL + 3] * 0x1000000;
	if (serial_number > 0)
		sprintf(edid->serial_number, "%lu", (unsigned long) serial_number);

	/* parse EDID data */
	for (i = EDID_OFFSET_DATA_BLOCKS;
	     i <= EDID_OFFSET_LAST_BLOCK;
	     i += 18) {
		/* ignore pixel clock data */
		if (data[i] != 0)
			continue;
		if (data[i+2] != 0)
			continue;

		/* any useful blocks? */
		if (data[i+3] == EDID_DESCRIPTOR_DISPLAY_PRODUCT_NAME) {
			edid_parse_string(&data[i+5],
					  edid->monitor_name);
		} else if (data[i+3] == EDID_DESCRIPTOR_DISPLAY_PRODUCT_SERIAL_NUMBER) {
			edid_parse_string(&data[i+5],
					  edid->serial_number);
		} else if (data[i+3] == EDID_DESCRIPTOR_ALPHANUMERIC_DATA_STRING) {
			edid_parse_string(&data[i+5],
					  edid->eisa_id);
		}
	}
	return 0;
}

static void
drm_head_info_from_edid(struct drm_head_info *dhi,
			const uint8_t *data,
			size_t length)
{
	struct drm_edid edid = {};
	int rc;

	rc = edid_parse(&edid, data, length);
	if (rc == 0) {
		if (edid.pnp_id[0] != '\0')
			dhi->make = xstrdup(edid.pnp_id);
		if (edid.monitor_name[0] != '\0')
			dhi->model = xstrdup(edid.monitor_name);
		if (edid.serial_number[0] != '\0')
			dhi->serial_number = xstrdup(edid.serial_number);
	}

	/* This ad hoc code will never parse HDR data. */
	dhi->eotf_mask = WESTON_EOTF_MODE_SDR;
}

#endif /* HAVE_LIBDISPLAY_INFO else */

/** Parse monitor make, model and serial from EDID
 *
 * \param head The head whose \c drm_edid to fill in.
 * \param props The DRM connector properties to get the EDID from.
 * \param[out] dhi Receives information from EDID.
 *
 * \c *dhi must be drm_head_info_fini()'d by the caller.
 */
static void
find_and_parse_output_edid(struct drm_head *head,
			   drmModeObjectPropertiesPtr props,
			   struct drm_head_info *dhi)
{
	struct drm_device *device = head->connector.device;
	drmModePropertyBlobPtr edid_blob = NULL;
	uint32_t blob_id;

	blob_id =
		drm_property_get_value(
			&head->connector.props[WDRM_CONNECTOR_EDID],
			props, 0);
	if (!blob_id)
		return;

	edid_blob = drmModeGetPropertyBlob(device->drm.fd, blob_id);
	if (!edid_blob)
		return;

	drm_head_info_from_edid(dhi, edid_blob->data, edid_blob->length);

	drmModeFreePropertyBlob(edid_blob);
}

static void
prune_eotf_modes_by_kms_support(struct drm_head *head, uint32_t *eotf_mask)
{
	const struct drm_property_info *info;

	/* Without the KMS property, cannot do anything but SDR. */

	info = &head->connector.props[WDRM_CONNECTOR_HDR_OUTPUT_METADATA];
	if (!head->connector.device->atomic_modeset || info->prop_id == 0)
		*eotf_mask = WESTON_EOTF_MODE_SDR;
}

static uint32_t
drm_refresh_rate_mHz(const drmModeModeInfo *info)
{
	uint64_t refresh;

	/* Calculate higher precision (mHz) refresh rate */
	refresh = (info->clock * 1000000LL / info->htotal +
		   info->vtotal / 2) / info->vtotal;

	if (info->flags & DRM_MODE_FLAG_INTERLACE)
		refresh *= 2;
	if (info->flags & DRM_MODE_FLAG_DBLSCAN)
		refresh /= 2;
	if (info->vscan > 1)
	    refresh /= info->vscan;

	return refresh;
}

/**
 * Add a mode to output's mode list
 *
 * Copy the supplied DRM mode into a Weston mode structure, and add it to the
 * output's mode list.
 *
 * @param output DRM output to add mode to
 * @param info DRM mode structure to add
 * @returns Newly-allocated Weston/DRM mode structure
 */
static struct drm_mode *
drm_output_add_mode(struct drm_output *output, const drmModeModeInfo *info)
{
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct drm_mode *mode;

	mode = malloc(sizeof *mode);
	if (mode == NULL)
		return NULL;

	if (output->base.fixed_size) {
		mode->base.width = output->base.width;
		mode->base.height = output->base.height;
	} else if (b->virtual_width && b->virtual_height) {
		mode->base.width = b->virtual_width;
		mode->base.height = b->virtual_height;
	} else {
		mode->base.width = info->hdisplay;
		mode->base.height = info->vdisplay;
	}

	mode->base.flags = 0;

	mode->base.refresh = drm_refresh_rate_mHz(info);
	mode->mode_info = *info;
	mode->blob_id = 0;

	if (info->type & DRM_MODE_TYPE_PREFERRED)
		mode->base.flags |= WL_OUTPUT_MODE_PREFERRED;

	mode->base.aspect_ratio = drm_to_weston_mode_aspect_ratio(info->flags);

	wl_list_insert(output->base.mode_list.prev, &mode->base.link);

	return mode;
}

/**
 * Destroys a mode, and removes it from the list.
 */
static void
drm_output_destroy_mode(struct drm_device *device, struct drm_mode *mode)
{
	if (mode->blob_id)
		drmModeDestroyPropertyBlob(device->drm.fd, mode->blob_id);
	wl_list_remove(&mode->base.link);
	free(mode);
}

/** Destroy a list of drm_modes
 *
 * @param device The device for releasing mode property blobs.
 * @param mode_list The list linked by drm_mode::base.link.
 */
void
drm_mode_list_destroy(struct drm_device *device, struct wl_list *mode_list)
{
	struct drm_mode *mode, *next;

	wl_list_for_each_safe(mode, next, mode_list, base.link)
		drm_output_destroy_mode(device, mode);
}

void
drm_output_print_modes(struct drm_output *output)
{
	struct weston_mode *m;
	struct drm_mode *dm;
	const char *aspect_ratio;
	bool virtual_size = false;

	wl_list_for_each(m, &output->base.mode_list, link) {
		dm = to_drm_mode(m);

		aspect_ratio = aspect_ratio_to_string(m->aspect_ratio);
		weston_log_continue(STAMP_SPACE "%s@%.1f%s%s%s, %.1f MHz\n",
				    dm->mode_info.name,
				    m->refresh / 1000.0,
				    aspect_ratio,
				    m->flags & WL_OUTPUT_MODE_PREFERRED ?
				    ", preferred" : "",
				    m->flags & WL_OUTPUT_MODE_CURRENT ?
				    ", current" : "",
				    dm->mode_info.clock / 1000.0);

		if(m->flags & WL_OUTPUT_MODE_CURRENT &&
		   (dm->mode_info.hdisplay != m->width ||
		    dm->mode_info.vdisplay != m->height))
			virtual_size = true;
	}

	if (virtual_size)
		weston_log("Output %s: using virtual size %dx%d\n",
			   output->base.name,
			   output->base.current_mode->width,
			   output->base.current_mode->height);
}


/**
 * Find the closest-matching mode for a given target
 *
 * Given a target mode, find the most suitable mode amongst the output's
 * current mode list to use, preferring the current mode if possible, to
 * avoid an expensive mode switch.
 *
 * @param output DRM output
 * @param target_mode Mode to attempt to match
 * @returns Pointer to a mode from the output's mode list
 */
struct drm_mode *
drm_output_choose_mode(struct drm_output *output,
		       struct weston_mode *target_mode)
{
	struct drm_mode *tmp_mode = NULL, *mode_fall_back = NULL, *mode;
	enum weston_mode_aspect_ratio src_aspect = WESTON_MODE_PIC_AR_NONE;
	enum weston_mode_aspect_ratio target_aspect = WESTON_MODE_PIC_AR_NONE;
	struct drm_device *device;

#define WESTON_MODE_NAME(mode) \
	to_drm_mode(mode)->mode_info.name

	device = output->device;

	target_aspect = target_mode->aspect_ratio;
	src_aspect = output->base.current_mode->aspect_ratio;
	if (!strcmp(WESTON_MODE_NAME(output->base.current_mode),
		    WESTON_MODE_NAME(target_mode)) &&
	    (output->base.current_mode->refresh == target_mode->refresh ||
	     target_mode->refresh == 0)) {
		if (!device->aspect_ratio_supported || src_aspect == target_aspect)
			return to_drm_mode(output->base.current_mode);
	}

	wl_list_for_each(mode, &output->base.mode_list, base.link) {

		src_aspect = mode->base.aspect_ratio;
		if (!strcmp(mode->mode_info.name,
			    WESTON_MODE_NAME(target_mode))) {
			if (mode->base.refresh == target_mode->refresh ||
			    target_mode->refresh == 0) {
				if (!device->aspect_ratio_supported ||
				    src_aspect == target_aspect)
					return mode;
				else if (!mode_fall_back)
					mode_fall_back = mode;
			} else if (!tmp_mode) {
				tmp_mode = mode;
			}
		}
	}

	if (mode_fall_back)
		return mode_fall_back;

	return tmp_mode;
}

void
update_head_from_connector(struct drm_head *head)
{
	struct drm_connector *connector = &head->connector;
	drmModeObjectProperties *props = connector->props_drm;
	drmModeConnector *conn = connector->conn;
	struct drm_head_info dhi = { .eotf_mask = WESTON_EOTF_MODE_SDR };

	find_and_parse_output_edid(head, props, &dhi);

	weston_head_set_monitor_strings(&head->base, dhi.make ?: "unknown",
					dhi.model ?: "unknown",
					dhi.serial_number ?: "unknown");

	prune_eotf_modes_by_kms_support(head, &dhi.eotf_mask);
	weston_head_set_supported_eotf_mask(&head->base, dhi.eotf_mask);
	weston_head_set_non_desktop(&head->base,
				    check_non_desktop(connector, props));
	weston_head_set_subpixel(&head->base,
				 drm_subpixel_to_wayland(conn->subpixel));

	weston_head_set_physical_size(&head->base, conn->mmWidth, conn->mmHeight);

	weston_head_set_transform(&head->base,
				  get_panel_orientation(connector, props));

	/* Unknown connection status is assumed disconnected. */
	weston_head_set_connection_status(&head->base,
				conn->connection == DRM_MODE_CONNECTED);

	drm_head_info_fini(&dhi);
}

/**
 * Choose suitable mode for an output
 *
 * Find the most suitable mode to use for initial setup (or reconfiguration on
 * hotplug etc) for a DRM output.
 *
 * @param device the DRM device
 * @param output DRM output to choose mode for
 * @param mode Strategy and preference to use when choosing mode
 * @param modeline Manually-entered mode (may be NULL)
 * @param current_mode Mode currently being displayed on this output
 * @returns A mode from the output's mode list, or NULL if none available
 */
struct drm_mode *
drm_output_choose_initial_mode(struct drm_device *device,
			       struct drm_output *output,
			       enum weston_drm_backend_output_mode mode,
			       const char *modeline,
			       const drmModeModeInfo *current_mode)
{
	struct drm_mode *preferred = NULL;
	struct drm_mode *current = NULL;
	struct drm_mode *configured = NULL;
	struct drm_mode *config_fall_back = NULL;
	struct drm_mode *best = NULL;
	struct drm_mode *drm_mode;
	drmModeModeInfo drm_modeline;
	char name[16] = {0};
	int32_t width = 0;
	int32_t height = 0;
	uint32_t refresh = 0;
	uint32_t aspect_width = 0;
	uint32_t aspect_height = 0;
	enum weston_mode_aspect_ratio aspect_ratio = WESTON_MODE_PIC_AR_NONE;
	int n;

	if (mode == WESTON_DRM_BACKEND_OUTPUT_PREFERRED && modeline) {
		sscanf(modeline, "%12[^@pP]", name);

		n = sscanf(modeline, "%dx%d%*[^0-9]%d %u:%u", &width, &height,
			   &refresh, &aspect_width, &aspect_height);
		if (device->aspect_ratio_supported && n == 5) {
			if (aspect_width == 4 && aspect_height == 3)
				aspect_ratio = WESTON_MODE_PIC_AR_4_3;
			else if (aspect_width == 16 && aspect_height == 9)
				aspect_ratio = WESTON_MODE_PIC_AR_16_9;
			else if (aspect_width == 64 && aspect_height == 27)
				aspect_ratio = WESTON_MODE_PIC_AR_64_27;
			else if (aspect_width == 256 && aspect_height == 135)
				aspect_ratio = WESTON_MODE_PIC_AR_256_135;
			else
				weston_log("Invalid modeline \"%s\" for output %s\n",
					   modeline, output->base.name);
		}
		if (n != 2 && n != 3 && n != 5) {
			width = -1;

			if (parse_modeline(modeline, &drm_modeline) == 0) {
				configured = drm_output_add_mode(output, &drm_modeline);
				if (!configured)
					return NULL;
			} else {
				weston_log("Invalid modeline \"%s\" for output %s\n",
					   modeline, output->base.name);
			}
		}
	}

	wl_list_for_each_reverse(drm_mode, &output->base.mode_list, base.link) {
		if (!strcmp(name, drm_mode->mode_info.name) &&
		    (refresh == 0 || refresh == drm_mode->mode_info.vrefresh)) {
			if (!device->aspect_ratio_supported ||
			    aspect_ratio == drm_mode->base.aspect_ratio)
				configured = drm_mode;
			else
				config_fall_back = drm_mode;
		}

		if (memcmp(current_mode, &drm_mode->mode_info,
			   sizeof *current_mode) == 0)
			current = drm_mode;

		if (drm_mode->base.flags & WL_OUTPUT_MODE_PREFERRED)
			preferred = drm_mode;

		best = drm_mode;
	}

	if (current == NULL && current_mode->clock != 0) {
		current = drm_output_add_mode(output, current_mode);
		if (!current)
			return NULL;
	}

	if (mode == WESTON_DRM_BACKEND_OUTPUT_CURRENT)
		configured = current;

	if (configured)
		return configured;

	if (config_fall_back)
		return config_fall_back;

	if (preferred)
		return preferred;

	if (current)
		return current;

	if (best)
		return best;

	weston_log("no available modes for %s\n", output->base.name);
	return NULL;
}

static uint32_t
u32distance(uint32_t a, uint32_t b)
{
	if (a < b)
		return b - a;
	else
		return a - b;
}

/** Choose equivalent mode
 *
 * If the two modes are not equivalent, return NULL.
 * Otherwise return the mode that is more likely to work in place of both.
 *
 * None of the fuzzy matching criteria in this function have any justification.
 *
 * typedef struct _drmModeModeInfo {
 *         uint32_t clock;
 *         uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
 *         uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
 *
 *         uint32_t vrefresh;
 *
 *         uint32_t flags;
 *         uint32_t type;
 *         char name[DRM_DISPLAY_MODE_LEN];
 * } drmModeModeInfo, *drmModeModeInfoPtr;
 */
static const drmModeModeInfo *
drm_mode_pick_equivalent(const drmModeModeInfo *a, const drmModeModeInfo *b)
{
	uint32_t refresh_a, refresh_b;

	if (a->hdisplay != b->hdisplay || a->vdisplay != b->vdisplay)
		return NULL;

	if (a->flags != b->flags)
		return NULL;

	/* kHz */
	if (u32distance(a->clock, b->clock) > 500)
		return NULL;

	refresh_a = drm_refresh_rate_mHz(a);
	refresh_b = drm_refresh_rate_mHz(b);
	if (u32distance(refresh_a, refresh_b) > 50)
		return NULL;

	if ((a->type ^ b->type) & DRM_MODE_TYPE_PREFERRED) {
		if (a->type & DRM_MODE_TYPE_PREFERRED)
			return a;
		else
			return b;
	}

	return a;
}

/* If the given mode info is not already in the list, add it.
 * If it is in the list, either keep the existing or replace it,
 * depending on which one is "better".
 */
static int
drm_output_try_add_mode(struct drm_output *output, const drmModeModeInfo *info)
{
	struct weston_mode *base;
	struct drm_mode *mode = NULL;
	struct drm_device *device = output->device;
	const drmModeModeInfo *chosen = NULL;

	assert(info);

	wl_list_for_each(base, &output->base.mode_list, link) {
		mode = to_drm_mode(base);
		chosen = drm_mode_pick_equivalent(&mode->mode_info, info);
		if (chosen)
			break;
	}

	if (chosen == info) {
		assert(mode);
		drm_output_destroy_mode(device, mode);
		chosen = NULL;
	}

	if (!chosen) {
		mode = drm_output_add_mode(output, info);
		if (!mode)
			return -1;
	}
	/* else { the equivalent mode is already in the list } */

	return 0;
}

/** Rewrite the output's mode list
 *
 * @param output The output.
 * @return 0 on success, -1 on failure.
 *
 * Destroy all existing modes in the list, and reconstruct a new list from
 * scratch, based on the currently attached heads.
 *
 * On failure the output's mode list may contain some modes.
 */
static int
drm_output_update_modelist_from_heads(struct drm_output *output)
{
	struct drm_device *device = output->device;
	struct weston_head *head_base;
	struct drm_head *head;
	drmModeConnector *conn;
	int i;
	int ret;

	assert(!output->base.enabled);

	drm_mode_list_destroy(device, &output->base.mode_list);

	wl_list_for_each(head_base, &output->base.head_list, output_link) {
		head = to_drm_head(head_base);
		conn = head->connector.conn;
		for (i = 0; i < conn->count_modes; i++) {
			ret = drm_output_try_add_mode(output, &conn->modes[i]);
			if (ret < 0)
				return -1;
		}
	}

	return 0;
}

int
drm_output_set_mode(struct weston_output *base,
		    enum weston_drm_backend_output_mode mode,
		    const char *modeline)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_device *device = output->device;
	struct drm_head *head = to_drm_head(weston_output_get_first_head(base));

	struct drm_mode *current;

	if (output->virtual)
		return -1;

	if (drm_output_update_modelist_from_heads(output) < 0)
		return -1;

	current = drm_output_choose_initial_mode(device, output, mode, modeline,
						 &head->inherited_mode);
	if (!current)
		return -1;

	output->base.current_mode = &current->base;
	output->base.current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	/* Set native_ fields, so weston_output_mode_switch_to_native() works */
	output->base.native_mode = output->base.current_mode;
	output->base.native_scale = output->base.current_scale;

	return 0;
}
