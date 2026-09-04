/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / WebM demultiplexer filter
 *  based on nestegg (https://github.com/mozilla/nestegg)
 *
 *  Follows the same "full file only" pattern as avidmx: nestegg needs
 *  seekable access (SeekHead/Cues) that a single forward pass over a
 *  GF_FilterPid cannot provide, so this filter asks the source for the
 *  full local file and opens it directly via gf_fopen/gf_fseek/gf_fread,
 *  wrapped as a nestegg_io.
 *
 *  Only a single AV1/VP8/VP9 video track (if any) is exposed on output
 *  - these are the codecs currently paired with a decoder (libaom for
 *  AV1, libvpx for VP8/VP9). Audio tracks (Vorbis/Opus) are ignored.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <nestegg/nestegg.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;

	const char *src_url;
	FILE *fp;

	Bool is_playing;

	nestegg *demux;
	unsigned int video_track;
	Bool has_video_track;
} GF_WebMDmxCtx;

static int64_t webm_io_read(void *buffer, size_t length, void *userdata)
{
	GF_WebMDmxCtx *ctx = (GF_WebMDmxCtx *)userdata;
	size_t nb_read = gf_fread(buffer, length, ctx->fp);
	if (!nb_read && length) return feof(ctx->fp) ? 0 : -1;
	return (int64_t)nb_read;
}

static int webm_io_seek(int64_t offset, int whence, void *userdata)
{
	GF_WebMDmxCtx *ctx = (GF_WebMDmxCtx *)userdata;
	return gf_fseek(ctx->fp, offset, whence);
}

static int64_t webm_io_tell(void *userdata)
{
	GF_WebMDmxCtx *ctx = (GF_WebMDmxCtx *)userdata;
	return (int64_t)gf_ftell(ctx->fp);
}

static GF_Err webmdmx_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	GF_WebMDmxCtx *ctx = (GF_WebMDmxCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		ctx->ipid = NULL;
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	if (!ctx->ipid)
	{
		GF_FilterEvent fevt;
		ctx->ipid = pid;

		/* we work with full file only, ask the source for it */
		GF_FEVT_INIT(fevt, GF_FEVT_PLAY_HINT, pid);
		fevt.play.start_range = 0;
		fevt.base.on_pid = ctx->ipid;
		fevt.play.full_file_only = GF_TRUE;
		gf_filter_pid_send_event(ctx->ipid, &fevt);
	}

	p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILEPATH);
	if (!p) return GF_NOT_SUPPORTED;
	ctx->src_url = p->value.string;

	return GF_OK;
}

static Bool webmdmx_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_WebMDmxCtx *ctx = (GF_WebMDmxCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		/* cancel play event, we work with full file */
		return GF_TRUE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err webmdmx_open(GF_Filter *filter, GF_WebMDmxCtx *ctx)
{
	nestegg_io io;
	unsigned int i, track_count;

	if (!ctx->src_url) return GF_NOT_SUPPORTED;

	ctx->fp = gf_fopen(ctx->src_url, "rb");
	if (!ctx->fp)
	{
		gf_filter_setup_failure(filter, GF_URL_ERROR);
		return GF_NOT_SUPPORTED;
	}

	io.read = webm_io_read;
	io.seek = webm_io_seek;
	io.tell = webm_io_tell;
	io.userdata = ctx;

	if (nestegg_init(&ctx->demux, io, NULL, -1) != 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CONTAINER, ("[WebMDmx] Failed to parse WebM/Matroska container\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (nestegg_track_count(ctx->demux, &track_count) != 0)
		return GF_NON_COMPLIANT_BITSTREAM;

	for (i = 0; i < track_count; i++)
	{
		int nestegg_codec;
		u32 gpac_codec;

		if (nestegg_track_type(ctx->demux, i) != NESTEGG_TRACK_VIDEO)
			continue;

		nestegg_codec = nestegg_track_codec_id(ctx->demux, i);
		switch (nestegg_codec)
		{
		case NESTEGG_CODEC_AV1: gpac_codec = GF_CODECID_AV1; break;
		case NESTEGG_CODEC_VP8: gpac_codec = GF_CODECID_VP8; break;
		case NESTEGG_CODEC_VP9: gpac_codec = GF_CODECID_VP9; break;
		default: continue;
		}

		{
			nestegg_video_params vparams;
			memset(&vparams, 0, sizeof(vparams));

			ctx->video_track = i;
			ctx->has_video_track = GF_TRUE;

			ctx->opid = gf_filter_pid_new(filter);
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(gpac_codec));
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, &PROP_BOOL(GF_FALSE));
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(1000000000));

			if (nestegg_track_video_params(ctx->demux, i, &vparams) == 0)
			{
				gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(vparams.width));
				gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(vparams.height));
			}
			break;
		}
	}

	if (!ctx->has_video_track)
	{
		GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[WebMDmx] No AV1/VP8/VP9 video track found in WebM/Matroska container\n"));
	}

	return GF_OK;
}

static GF_Err webmdmx_send_packets(GF_WebMDmxCtx *ctx)
{
	nestegg_packet *pkt;
	int res;

	while ((res = nestegg_read_packet(ctx->demux, &pkt)) > 0)
	{
		unsigned int track = 0, chunk, chunks = 0;
		uint64_t tstamp = 0;

		nestegg_packet_track(pkt, &track);
		if (track != ctx->video_track)
		{
			nestegg_free_packet(pkt);
			continue;
		}

		nestegg_packet_tstamp(pkt, &tstamp);
		nestegg_packet_count(pkt, &chunks);

		for (chunk = 0; chunk < chunks; chunk++)
		{
			u8 *chunk_data = NULL;
			size_t chunk_size = 0;
			GF_FilterPacket *dst_pck;
			u8 *output;

			if (nestegg_packet_data(pkt, chunk, &chunk_data, &chunk_size) != 0) continue;
			if (!chunk_size) continue;

			dst_pck = gf_filter_pck_new_alloc(ctx->opid, (u32)chunk_size, &output);
			if (!dst_pck) continue;

			memcpy(output, chunk_data, chunk_size);
			gf_filter_pck_set_cts(dst_pck, tstamp);
			gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
			gf_filter_pck_send(dst_pck);
		}

		nestegg_free_packet(pkt);
	}

	if (ctx->opid)
		gf_filter_pid_set_eos(ctx->opid);

	return GF_EOS;
}

static GF_Err webmdmx_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	Bool start, end;
	GF_WebMDmxCtx *ctx = (GF_WebMDmxCtx *)gf_filter_get_udta(filter);

	if (ctx->demux)
		return GF_EOS;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck) return GF_OK;

	gf_filter_pck_get_framing(pck, &start, &end);
	gf_filter_pid_drop_packet(ctx->ipid);
	if (!end) return GF_OK;

	{
		GF_Err e = webmdmx_open(filter, ctx);
		if (e) return e;
	}
	if (!ctx->has_video_track) return GF_EOS;
	return webmdmx_send_packets(ctx);
}

static void webmdmx_finalize(GF_Filter *filter)
{
	GF_WebMDmxCtx *ctx = (GF_WebMDmxCtx *)gf_filter_get_udta(filter);
	if (ctx->demux)
	{
		nestegg_destroy(ctx->demux);
		ctx->demux = NULL;
	}
	if (ctx->fp)
	{
		gf_fclose(ctx->fp);
		ctx->fp = NULL;
	}
}

static const GF_FilterCapability WebMDmxCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "webm|mkv"),
		/* Sans capacite MIME, ce demultiplexeur est injoignable via httpin : des
		 * qu'un serveur renvoie un Content-Type exploitable, gpac passe le PID en
		 * ext_not_trusted et cesse d'apparier sur l'extension (cf. filter.c,
		 * gf_filter_pid_raw_new). Les demultiplexeurs amont equivalents (dmx_avi,
		 * dmx_mpegps, dmx_ogg) declarent tous leur MIME pour cette raison. */
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/webm|audio/webm|video/x-matroska|audio/x-matroska|video/matroska|application/x-matroska"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILEPATH, "*"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_AV1),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_VP8),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_VP9),
};

GF_FilterRegister WebMDmxRegister = {
	.name = "webmdmx",
	GF_FS_SET_DESCRIPTION("WebM/Matroska demultiplexer")
		GF_FS_SET_HELP("This filter demultiplexes WebM/Matroska files using nestegg, exposing the AV1/VP8/VP9 video track (if any) framed for a downstream decoder (libaom for AV1, libvpx for VP8/VP9).")
			.private_size = sizeof(GF_WebMDmxCtx),
	SETCAPS(WebMDmxCaps),
	.configure_pid = webmdmx_configure_pid,
	.process = webmdmx_process,
	.process_event = webmdmx_process_event,
	.finalize = webmdmx_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE webmdmx_register(GF_FilterSession *session)
{
	return &WebMDmxRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_webmdmx(void) {
    gf_filter_auto_register("webmdmx", webmdmx_register);
}
