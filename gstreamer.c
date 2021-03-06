/*
 * obs-gstreamer-source. OBS Studio source plugin.
 * Copyright (C) 2018 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-gstreamer-source.
 *
 * obs-gstreamer-source is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-gstreamer-source is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-gstreamer-source. If not, see <http://www.gnu.org/licenses/>.
 */

#include <obs/obs-module.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>

OBS_DECLARE_MODULE()

typedef struct {
	GstElement* pipe;
	obs_source_t* source;
	obs_data_t* settings;
	gint64 frame_count;
	guint timeout_id;
} data_t;

static gboolean start_pipe(gpointer user_data)
{
	data_t* data = user_data;

	data->timeout_id = 0;
	gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	return FALSE;
}

static gboolean bus_callback(GstBus* bus, GstMessage* message, gpointer user_data)
{
	data_t* data = user_data;

	switch (GST_MESSAGE_TYPE(message)) {
		case GST_MESSAGE_EOS:
			if (obs_data_get_bool(data->settings, "restart_on_eos"))
				gst_element_seek_simple(data->pipe, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
			break;
		case GST_MESSAGE_ERROR:
			{
				GError* err;
				gst_message_parse_error(message, &err, NULL);
				blog(LOG_ERROR, "%s", err->message);
				g_error_free(err);
			}
			gst_element_set_state(data->pipe, GST_STATE_NULL);
			if (obs_data_get_bool(data->settings, "restart_on_error") && data->timeout_id == 0)
				data->timeout_id = g_timeout_add_seconds(5, start_pipe, data);
			break;
		default:
			break;
	}

	return TRUE;
}

static GstFlowReturn video_new_sample(GstAppSink* appsink, gpointer user_data)
{
	data_t* data = user_data;
	GstSample* sample = gst_app_sink_pull_sample(appsink);
	GstBuffer* buffer = gst_sample_get_buffer(sample);
	GstCaps* caps = gst_sample_get_caps(sample);
	GstStructure* structure = gst_caps_get_structure(caps, 0);
	GstMapInfo info;
	gint width;
	gint height;

	gst_structure_get_int(structure, "width", &width);
	gst_structure_get_int(structure, "height", &height);
	const gchar* format = gst_structure_get_string(structure, "format");

	gst_buffer_map(buffer, &info, GST_MAP_READ);

	struct obs_source_frame frame = {};

	frame.width = width;
	frame.height = height;
	frame.timestamp = obs_data_get_bool(data->settings, "use_timestamps") ? GST_BUFFER_PTS(buffer) : data->frame_count++;
	frame.data[0] = info.data;

	video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_DEFAULT, frame.color_matrix, frame.color_range_min, frame.color_range_max);

	switch (gst_video_format_from_string(format))
	{
		case GST_VIDEO_FORMAT_I420:
			frame.format = VIDEO_FORMAT_I420;
			frame.linesize[0] = width;
			frame.linesize[1] = width / 2;
			frame.linesize[2] = width / 2;
			frame.data[1] = frame.data[0] + width * height;
			frame.data[2] = frame.data[1] + width * height / 4;
			break;
		case GST_VIDEO_FORMAT_NV12:
			frame.format = VIDEO_FORMAT_NV12;
			frame.linesize[0] = width;
			frame.linesize[1] = width;
			frame.data[1] = frame.data[0] + width * height;
			break;
		case GST_VIDEO_FORMAT_BGRA:
			frame.format = VIDEO_FORMAT_BGRX;
			frame.linesize[0] = width * 4;
			break;
		case GST_VIDEO_FORMAT_RGBA:
			frame.format = VIDEO_FORMAT_RGBA;
			frame.linesize[0] = width * 4;
			break;
		case GST_VIDEO_FORMAT_UYVY:
			frame.format = VIDEO_FORMAT_UYVY;
			frame.linesize[0] = width * 2;
			break;
		case GST_VIDEO_FORMAT_YUY2:
			frame.format = VIDEO_FORMAT_YUY2;
			frame.linesize[0] = width * 2;
			break;
		case GST_VIDEO_FORMAT_YVYU:
			frame.format = VIDEO_FORMAT_YVYU;
			frame.linesize[0] = width * 2;
			break;
		default:
			frame.format = VIDEO_FORMAT_NONE;
			blog(LOG_ERROR, "Unknown video format: %s", format);
			break;
	}

	obs_source_output_video(data->source, &frame);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

static GstFlowReturn audio_new_sample(GstAppSink* appsink, gpointer user_data)
{
	data_t* data = user_data;
	GstSample* sample = gst_app_sink_pull_sample(appsink);
	GstBuffer* buffer = gst_sample_get_buffer(sample);
	GstCaps* caps = gst_sample_get_caps(sample);
	GstStructure* structure = gst_caps_get_structure(caps, 0);
	GstMapInfo info;
	gint channels;
	gint rate;

	gst_structure_get_int(structure, "channels", &channels);
	gst_structure_get_int(structure, "rate", &rate);
	const gchar* format = gst_structure_get_string(structure, "format");

	gst_buffer_map(buffer, &info, GST_MAP_READ);

	struct obs_source_audio audio = {};

	audio.frames = info.size / channels;
	audio.samples_per_sec = rate;
	audio.timestamp = obs_data_get_bool(data->settings, "use_timestamps") ? GST_BUFFER_PTS(buffer) : 0;
	audio.data[0] = info.data;

	switch (channels) {
		case 1:
			audio.speakers = SPEAKERS_MONO;
			break;
		case 2:
			audio.speakers = SPEAKERS_STEREO;
			break;
		case 3:
			audio.speakers = SPEAKERS_2POINT1;
			break;
		case 4:
			audio.speakers = SPEAKERS_4POINT0;
			break;
		case 5:
			audio.speakers = SPEAKERS_4POINT1;
			break;
		case 6:
			audio.speakers = SPEAKERS_5POINT1;
			break;
		case 8:
			audio.speakers = SPEAKERS_7POINT1;
			break;
		default:
			audio.speakers = SPEAKERS_UNKNOWN;
			blog(LOG_ERROR, "Unsupported channel count: %d", channels);
			break;
	}

	switch (gst_audio_format_from_string(format))
	{
		case GST_AUDIO_FORMAT_U8:
			audio.format = AUDIO_FORMAT_U8BIT;
			break;
		case GST_AUDIO_FORMAT_S16LE:
			audio.format = AUDIO_FORMAT_16BIT;
			audio.frames /= 2;
			break;
		case GST_AUDIO_FORMAT_S32LE:
			audio.format = AUDIO_FORMAT_32BIT;
			audio.frames /= 4;
			break;
		case GST_AUDIO_FORMAT_F32LE:
			audio.format = AUDIO_FORMAT_FLOAT;
			audio.frames /= 4;
			break;
		default:
			audio.format = AUDIO_FORMAT_UNKNOWN;
			blog(LOG_ERROR, "Unknown audio format: %s", format);
			break;
	}

	obs_source_output_audio(data->source, &audio);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

static const char* get_name(void* type_data)
{
	return "GStreamer Source";
}

static void start(data_t* data)
{
	GError* err = NULL;

	g_autofree gchar* pipeline = g_strdup_printf(
		"videoconvert name=video ! video/x-raw, format={I420,NV12,BGRA,RGBA,YUY2,YVYU,UYVY} ! appsink name=video_appsink "
		"audioconvert name=audio ! audioresample ! audio/x-raw, format={U8,S16LE,S32LE,F32LE}, channels={1,2,3,4,5,6,8} ! appsink name=audio_appsink "
		"%s",
		obs_data_get_string(data->settings, "pipeline"));

	data->pipe = gst_parse_launch(pipeline, &err);
	if (err != NULL)
	{
		blog(LOG_ERROR, "Cannot start GStreamer: %s", err->message);
		g_error_free(err);

		return;
	}

	GstAppSinkCallbacks video_cbs = {
		NULL,
		NULL,
		video_new_sample
	};

	GstElement* appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "video_appsink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &video_cbs, data, NULL);

	if(!obs_data_get_bool(data->settings, "sync_appsinks"))
		g_object_set(appsink, "sync", FALSE, NULL);

	// check if connected and remove if not
	GstElement* sink = gst_bin_get_by_name(GST_BIN(data->pipe), "video");
	GstPad* pad = gst_element_get_static_pad(sink, "sink");
	if (!gst_pad_is_linked(pad))
		gst_bin_remove(GST_BIN(data->pipe), appsink);
	gst_object_unref(pad);
	gst_object_unref(sink);

	gst_object_unref(appsink);

	GstAppSinkCallbacks audio_cbs = {
		NULL,
		NULL,
		audio_new_sample
	};

	appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio_appsink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &audio_cbs, data, NULL);

	if (!obs_data_get_bool(data->settings, "sync_appsinks"))
		g_object_set(appsink, "sync", FALSE, NULL);

	// check if connected and remove if not
	sink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio");
	pad = gst_element_get_static_pad(sink, "sink");
	if (!gst_pad_is_linked(pad))
		gst_bin_remove(GST_BIN(data->pipe), appsink);
	gst_object_unref(pad);
	gst_object_unref(sink);

	gst_object_unref(appsink);

	data->frame_count = 0;

	GstBus* bus = gst_element_get_bus(data->pipe);
	gst_bus_add_watch(bus, bus_callback, data);
	gst_object_unref(bus);

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);
}

static void* create(obs_data_t* settings, obs_source_t* source)
{
	data_t* data = g_new0(data_t, 1);

	data->source = source;
	data->settings = settings;

	return data;
}

static void stop(data_t* data)
{
	if (data->timeout_id != 0)
	{
		g_source_remove(data->timeout_id);
		data->timeout_id = 0;
	}

	if (data->pipe == NULL)
	{
		return;
	}

	gst_element_set_state(data->pipe, GST_STATE_NULL);
	gst_object_unref(data->pipe);
	data->pipe = NULL;
}

static void destroy(void* data)
{
	stop(data);
	g_free(data);
}

static void get_defaults(obs_data_t* settings)
{
	obs_data_set_default_string(settings, "pipeline",
		"videotestsrc is-live=true ! video/x-raw, framerate=30/1, width=960, height=540 ! video. "
		"audiotestsrc wave=ticks is-live=true ! audio.");
	obs_data_set_default_bool(settings, "use_timestamps", false);
	obs_data_set_default_bool(settings, "sync_appsinks", true);
	obs_data_set_default_bool(settings, "restart_on_eos", true);
	obs_data_set_default_bool(settings, "restart_on_error", false);
}

static obs_properties_t* get_properties(void* data)
{
	obs_properties_t* props = obs_properties_create();

	obs_property_t* prop = obs_properties_add_text(props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop, "Use \"video\" and \"audio\" as names for the media sinks.");
	obs_properties_add_bool(props, "use_timestamps", "Use pipeline time stamps");
	obs_properties_add_bool(props, "sync_appsinks", "Sync appsinks to clock");
	obs_properties_add_bool(props, "restart_on_eos", "Try to restart when end of stream is reached");
	obs_properties_add_bool(props, "restart_on_error", "Try to restart after pipeline encountered an error");

	return props;
}

static void update(void* data, obs_data_t* settings)
{
	if (((data_t*)data)->pipe == NULL)
	{
		return;
	}

	stop(data);
	start(data);
}

static void show(void* data)
{
	start(data);
}

static void hide(void* data)
{
	stop(data);
}

bool obs_module_load(void)
{
	struct obs_source_info info = {
		.id = "gstreamer-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,

		.get_name = get_name,
		.create = create,
		.destroy = destroy,

		.get_defaults = get_defaults,
		.get_properties = get_properties,
		.update = update,
		.show = show,
		.hide = hide,
	};

	obs_register_source(&info);

	gst_init(NULL, NULL);

	return true;
}
