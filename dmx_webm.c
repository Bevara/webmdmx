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
 *  One output PID is declared per exposed track: the first AV1/VP8/VP9
 *  video track, plus every Vorbis/Opus audio track - these are the codecs
 *  currently paired with a decoder (libaom for AV1, libvpx for VP8/VP9,
 *  vorbis for Vorbis, libopus for Opus). Tracks in any other codec are
 *  skipped.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/bitstream.h>
#include <string.h>

#include <nestegg/nestegg.h>

typedef struct
{
	GF_FilterPid *ipid;

	const char *src_url;
	FILE *fp;

	Bool is_playing;

	nestegg *demux;
	/* one output PID per exposed track, indexed by nestegg track number;
	 * NULL for tracks we skip. Packets are routed with this table. */
	GF_FilterPid **opids;
	u32 nb_tracks;
	Bool has_video_track, has_track;
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
		if (ctx->opids)
		{
			u32 i;
			for (i = 0; i < ctx->nb_tracks; i++)
			{
				if (ctx->opids[i]) gf_filter_pid_remove(ctx->opids[i]);
			}
			gf_free(ctx->opids);
			ctx->opids = NULL;
			ctx->nb_tracks = 0;
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

	/* nestegg lit un fichier : il faut un chemin local. Sur une source HTTP il
	 * n'apparait qu'une fois le telechargement termine, en reponse au
	 * PLAY_HINT ci-dessus. Refuser ici (GF_NOT_SUPPORTED) mettait le filtre sur
	 * liste noire avant meme que la source ait pu repondre ; on accepte donc la
	 * connexion et on relit la propriete au moment d'ouvrir (webmdmx_process
	 * n'ouvre de toute facon qu'a la fin du flux d'entree). */
	p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILEPATH);
	if (p) ctx->src_url = p->value.string;

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

	ctx->nb_tracks = track_count;
	ctx->opids = gf_malloc(sizeof(GF_FilterPid *) * (track_count ? track_count : 1));
	if (!ctx->opids) return GF_OUT_OF_MEM;
	memset(ctx->opids, 0, sizeof(GF_FilterPid *) * (track_count ? track_count : 1));

	for (i = 0; i < track_count; i++)
	{
		int track_type = nestegg_track_type(ctx->demux, i);
		int nestegg_codec = nestegg_track_codec_id(ctx->demux, i);
		u32 gpac_codec;
		GF_FilterPid *opid;

		if (track_type == NESTEGG_TRACK_VIDEO)
		{
			nestegg_video_params vparams;

			//only the first video track is exposed
			if (ctx->has_video_track) continue;

			switch (nestegg_codec)
			{
			case NESTEGG_CODEC_AV1: gpac_codec = GF_CODECID_AV1; break;
			case NESTEGG_CODEC_VP8: gpac_codec = GF_CODECID_VP8; break;
			case NESTEGG_CODEC_VP9: gpac_codec = GF_CODECID_VP9; break;
			default: continue;
			}

			opid = gf_filter_pid_new(filter);
			if (!opid) continue;
			gf_filter_pid_set_property(opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
			gf_filter_pid_set_property(opid, GF_PROP_PID_CODECID, &PROP_UINT(gpac_codec));
			gf_filter_pid_set_property(opid, GF_PROP_PID_UNFRAMED, &PROP_BOOL(GF_FALSE));
			gf_filter_pid_set_property(opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(1000000000));

			memset(&vparams, 0, sizeof(vparams));
			if (nestegg_track_video_params(ctx->demux, i, &vparams) == 0)
			{
				gf_filter_pid_set_property(opid, GF_PROP_PID_WIDTH, &PROP_UINT(vparams.width));
				gf_filter_pid_set_property(opid, GF_PROP_PID_HEIGHT, &PROP_UINT(vparams.height));
			}
			ctx->has_video_track = GF_TRUE;
		}
		else if (track_type == NESTEGG_TRACK_AUDIO)
		{
			nestegg_audio_params aparams;

			switch (nestegg_codec)
			{
			case NESTEGG_CODEC_VORBIS: gpac_codec = GF_CODECID_VORBIS; break;
			case NESTEGG_CODEC_OPUS: gpac_codec = GF_CODECID_OPUS; break;
			default: continue;
			}

			opid = gf_filter_pid_new(filter);
			if (!opid) continue;
			gf_filter_pid_set_property(opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
			gf_filter_pid_set_property(opid, GF_PROP_PID_CODECID, &PROP_UINT(gpac_codec));
			gf_filter_pid_set_property(opid, GF_PROP_PID_UNFRAMED, &PROP_BOOL(GF_FALSE));
			gf_filter_pid_set_property(opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(1000000000));

			memset(&aparams, 0, sizeof(aparams));
			if (nestegg_track_audio_params(ctx->demux, i, &aparams) == 0)
			{
				if (aparams.rate > 0)
					gf_filter_pid_set_property(opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT((u32)aparams.rate));
				if (aparams.channels)
					gf_filter_pid_set_property(opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(aparams.channels));
			}

			/* Vorbis needs its three Xiph setup headers as decoder config, in
			 * the layout dec_vorbis.c reads back: a series of "u16 size +
			 * payload" blocks. nestegg has already split the CodecPrivate
			 * lacing into one item per header, so we just re-emit them.
			 * Opus needs none: its decoder only uses SAMPLE_RATE/NUM_CHANNELS
			 * (see the header comment of libopus/dec_opus.c). */
			if (gpac_codec == GF_CODECID_VORBIS)
			{
				unsigned int nb_items = 0, item;
				if ((nestegg_track_codec_data_count(ctx->demux, i, &nb_items) == 0) && nb_items)
				{
					GF_BitStream *bs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
					u8 *dsi = NULL;
					u32 dsi_size = 0;
					for (item = 0; item < nb_items; item++)
					{
						unsigned char *hdr = NULL;
						size_t hdr_size = 0;
						if (nestegg_track_codec_data(ctx->demux, i, item, &hdr, &hdr_size) != 0) continue;
						if (!hdr_size || (hdr_size > 0xFFFF)) continue;
						gf_bs_write_u16(bs, (u32)hdr_size);
						gf_bs_write_data(bs, (const u8 *)hdr, (u32)hdr_size);
					}
					gf_bs_get_content(bs, &dsi, &dsi_size);
					gf_bs_del(bs);
					if (dsi_size)
						gf_filter_pid_set_property(opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA_NO_COPY(dsi, dsi_size));
					else if (dsi)
						gf_free(dsi);
				}
			}
		}
		else continue;

		ctx->opids[i] = opid;
		ctx->has_track = GF_TRUE;
	}

	if (!ctx->has_track)
	{
		GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[WebMDmx] No AV1/VP8/VP9 video nor Vorbis/Opus audio track found in WebM/Matroska container\n"));
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
		GF_FilterPid *opid;

		nestegg_packet_track(pkt, &track);
		opid = (track < ctx->nb_tracks) ? ctx->opids[track] : NULL;
		if (!opid)
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

			dst_pck = gf_filter_pck_new_alloc(opid, (u32)chunk_size, &output);
			if (!dst_pck) continue;

			memcpy(output, chunk_data, chunk_size);
			gf_filter_pck_set_cts(dst_pck, tstamp);
			gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
			gf_filter_pck_send(dst_pck);
		}

		nestegg_free_packet(pkt);
	}

	{
		u32 i;
		for (i = 0; i < ctx->nb_tracks; i++)
		{
			if (ctx->opids[i]) gf_filter_pid_set_eos(ctx->opids[i]);
		}
	}

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

	if (!ctx->src_url) {
		const GF_PropertyValue *p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILEPATH);
		if (!p) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("[WebMDmx] No local file for source, cannot demultiplex\n"));
			return GF_NOT_SUPPORTED;
		}
		ctx->src_url = p->value.string;
	}
	{
		GF_Err e = webmdmx_open(filter, ctx);
		if (e) return e;
	}
	if (!ctx->has_track) return GF_EOS;
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
	if (ctx->opids)
	{
		gf_free(ctx->opids);
		ctx->opids = NULL;
		ctx->nb_tracks = 0;
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
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_AV1),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_VP8),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_VP9),
		{0},
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "webm|mkv"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/webm|audio/webm|video/x-matroska|audio/x-matroska|video/matroska|application/x-matroska"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_VORBIS),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_OPUS),
};

GF_FilterRegister WebMDmxRegister = {
	.name = "webmdmx",
	GF_FS_SET_DESCRIPTION("WebM/Matroska demultiplexer")
		GF_FS_SET_HELP("This filter demultiplexes WebM/Matroska files using nestegg, exposing the first AV1/VP8/VP9 video track and every Vorbis/Opus audio track, framed for downstream decoders (libaom for AV1, libvpx for VP8/VP9, vorbis for Vorbis, libopus for Opus).")
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
