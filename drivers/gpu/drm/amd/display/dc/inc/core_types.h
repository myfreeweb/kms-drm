/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#ifndef _CORE_TYPES_H_
#define _CORE_TYPES_H_

#include "dc.h"
#include "dce_calcs.h"
#include "dcn_calcs.h"
#include "ddc_service_types.h"
#include "dc_bios_types.h"
#include "mem_input.h"
#include "mpc.h"

#define MAX_CLOCK_SOURCES 7

void enable_surface_flip_reporting(struct dc_plane_state *plane_state,
		uint32_t controller_id);

#include "grph_object_id.h"
#include "link_encoder.h"
#include "stream_encoder.h"
#include "clock_source.h"
#include "audio.h"
#include "hw_sequencer_types.h"


/************ link *****************/
struct link_init_data {
	const struct core_dc *dc;
	struct dc_context *ctx; /* TODO: remove 'dal' when DC is complete. */
	uint32_t connector_index; /* this will be mapped to the HPD pins */
	uint32_t link_index; /* this is mapped to DAL display_index
				TODO: remove it when DC is complete. */
};

struct dc_link *link_create(const struct link_init_data *init_params);
void link_destroy(struct dc_link **link);

enum dc_status dc_link_validate_mode_timing(
		const struct dc_stream_state *stream,
		struct dc_link *link,
		const struct dc_crtc_timing *timing);

void core_link_resume(struct dc_link *link);

void core_link_enable_stream(struct pipe_ctx *pipe_ctx);

void core_link_disable_stream(struct pipe_ctx *pipe_ctx);

void core_link_set_avmute(struct pipe_ctx *pipe_ctx, bool enable);
/********** DAL Core*********************/
#include "display_clock.h"
#include "transform.h"

struct resource_pool;
struct validate_context;
struct resource_context;

struct resource_funcs {
	void (*destroy)(struct resource_pool **pool);
	struct link_encoder *(*link_enc_create)(
			const struct encoder_init_data *init);
	enum dc_status (*validate_with_context)(
					const struct core_dc *dc,
					const struct dc_validation_set set[],
					int set_count,
					struct validate_context *context,
					struct validate_context *old_context);

	enum dc_status (*validate_guaranteed)(
					const struct core_dc *dc,
					struct dc_stream_state *stream,
					struct validate_context *context);

	bool (*validate_bandwidth)(
					const struct core_dc *dc,
					struct validate_context *context);

	struct pipe_ctx *(*acquire_idle_pipe_for_layer)(
			struct validate_context *context,
			const struct resource_pool *pool,
			struct dc_stream_state *stream);
	enum dc_status (*validate_plane)(const struct dc_plane_state *plane_state);
};

struct audio_support{
	bool dp_audio;
	bool hdmi_audio_on_dongle;
	bool hdmi_audio_native;
};

#define NO_UNDERLAY_PIPE -1

struct resource_pool {
	struct mem_input *mis[MAX_PIPES];
	struct input_pixel_processor *ipps[MAX_PIPES];
	struct transform *transforms[MAX_PIPES];
	struct output_pixel_processor *opps[MAX_PIPES];
	struct timing_generator *timing_generators[MAX_PIPES];
	struct stream_encoder *stream_enc[MAX_PIPES * 2];
#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	struct mpc *mpc;
#endif

	unsigned int pipe_count;
	unsigned int underlay_pipe_index;
	unsigned int stream_enc_count;
	unsigned int ref_clock_inKhz;

	/*
	 * reserved clock source for DP
	 */
	struct clock_source *dp_clock_source;

	struct clock_source *clock_sources[MAX_CLOCK_SOURCES];
	unsigned int clk_src_count;

	struct audio *audios[MAX_PIPES];
	unsigned int audio_count;
	struct audio_support audio_support;

	struct display_clock *display_clock;
	struct irq_service *irqs;

	struct abm *abm;
	struct dmcu *dmcu;

	const struct resource_funcs *funcs;
	const struct resource_caps *res_cap;
};

struct stream_resource {
	struct output_pixel_processor *opp;
};

struct plane_resource {
	struct scaler_data scl_data;

	struct mem_input *mi;
	struct input_pixel_processor *ipp;
	struct transform *xfm;
};

struct pipe_ctx {
	struct dc_plane_state *plane_state;
	struct dc_stream_state *stream;

	struct plane_resource plane_res;
	struct stream_resource stream_res;

	struct timing_generator *tg;

	struct stream_encoder *stream_enc;
	struct display_clock *dis_clk;
	struct clock_source *clock_source;

	struct audio *audio;

	struct pixel_clk_params pix_clk_params;
	struct pll_settings pll_settings;

	/*fmt*/
	struct encoder_info_frame encoder_info_frame;

	uint8_t pipe_idx;

	struct pipe_ctx *top_pipe;
	struct pipe_ctx *bottom_pipe;

#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	struct _vcs_dpi_display_dlg_regs_st dlg_regs;
	struct _vcs_dpi_display_ttu_regs_st ttu_regs;
	struct _vcs_dpi_display_rq_regs_st rq_regs;
	struct _vcs_dpi_display_pipe_dest_params_st pipe_dlg_param;
#endif
};

struct resource_context {
	struct pipe_ctx pipe_ctx[MAX_PIPES];
	bool is_stream_enc_acquired[MAX_PIPES * 2];
	bool is_audio_acquired[MAX_PIPES];
	uint8_t clock_source_ref_count[MAX_CLOCK_SOURCES];
	uint8_t dp_clock_source_ref_count;
};

struct dce_bw_output {
	bool cpuc_state_change_enable;
	bool cpup_state_change_enable;
	bool stutter_mode_enable;
	bool nbp_state_change_enable;
	bool all_displays_in_sync;
	struct dce_watermarks urgent_wm_ns[MAX_PIPES];
	struct dce_watermarks stutter_exit_wm_ns[MAX_PIPES];
	struct dce_watermarks nbp_state_change_wm_ns[MAX_PIPES];
	int sclk_khz;
	int sclk_deep_sleep_khz;
	int yclk_khz;
	int dispclk_khz;
	int blackout_recovery_time_us;
};

#ifdef CONFIG_DRM_AMD_DC_DCN1_0
struct dcn_bw_clocks {
	int dispclk_khz;
	bool dppclk_div;
	int dcfclk_khz;
	int dcfclk_deep_sleep_khz;
	int fclk_khz;
	int dram_ccm_us;
	int min_active_dram_ccm_us;
};

struct dcn_bw_output {
	struct dcn_bw_clocks cur_clk;
	struct dcn_bw_clocks calc_clk;
	struct dcn_watermark_set watermarks;
};
#endif

union bw_context {
#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	struct dcn_bw_output dcn;
#endif
	struct dce_bw_output dce;
};

struct validate_context {
	struct dc_stream_state *streams[MAX_PIPES];
	struct dc_stream_status stream_status[MAX_PIPES];
	uint8_t stream_count;

	struct resource_context res_ctx;

	/* The output from BW and WM calculations. */
	union bw_context bw;

	/* Note: these are big structures, do *not* put on stack! */
	struct dm_pp_display_configuration pp_display_cfg;
#ifdef CONFIG_DRM_AMD_DC_DCN1_0
	struct dcn_bw_internal_vars dcn_bw_vars;
#endif

	int ref_count;
};

#endif /* _CORE_TYPES_H_ */
