/**************************************************************************/
/*  audio_driver_pipewire.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "audio_driver_pipewire.h"

#include "core/error/error_list.h"
#include "core/error/error_macros.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"
#include "pipewire/keys.h"
#include "servers/audio_server.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/latency.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <spa/utils/type.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

void AudioDriverPipeWire::_pw_core_on_done(void *p_user_data, uint32_t p_id, int p_seq) {
	AudioDriverPipeWire *self = (AudioDriverPipeWire *)p_user_data;

	if (p_id == PW_ID_CORE && p_seq == self->pending) {
		self->synced.set();
	}
}

void AudioDriverPipeWire::_pw_core_on_ping(void *p_user_data, uint32_t p_id, int p_seq) {
	AudioDriverPipeWire *self = (AudioDriverPipeWire *)p_user_data;
	pw_core_pong(self->core, p_id, p_seq);
}

void AudioDriverPipeWire::_pw_core_on_error(void *p_user_data, uint32_t p_id, int p_seq, int p_res, const char *p_message) {
	ERR_PRINT(vformat("PipeWire Error: %s", p_message));
}

//TODO: id -> name or name -> id?
void AudioDriverPipeWire::_pw_registry_on_global(void *p_user_data, uint32_t p_id, uint32_t p_permissions, const char *p_type, uint32_t p_version, const struct spa_dict *p_props) {
	AudioDriverPipeWire *self = (AudioDriverPipeWire *)p_user_data;

	if (p_props == nullptr) {
		return;
	}

	String name = "Default";
	const struct spa_dict_item *item;
	spa_dict_for_each(item, p_props) {
		if (strcmp(item->key, PW_KEY_NODE_NAME) == 0) {
			name = item->value;
		}

		if (strcmp(item->key, PW_KEY_MEDIA_CLASS) == 0) {
			if (strcmp(item->value, "Audio/Sink") == 0) {
				self->sinks.insert(p_id, name);
			} else if (strcmp(item->value, "Audio/Source") == 0) {
				self->sources.insert(p_id, name);
			}

			break;
		}
	}
}

void AudioDriverPipeWire::_pw_registry_on_global_remove(void *p_user_data, uint32_t p_id) {
	AudioDriverPipeWire *self = (AudioDriverPipeWire *)p_user_data;

	self->sinks.erase(p_id);
	self->sources.erase(p_id);
}

void AudioDriverPipeWire::_pw_stream_on_state_changed(void *p_user_data, pw_stream_state p_prev_state, pw_stream_state p_curr_state, const char *p_err) {
	StreamState *self = (StreamState *)p_user_data;

	self->paused = p_curr_state != pw_stream_state::PW_STREAM_STATE_STREAMING;
}

//TODO actually use these values
void AudioDriverPipeWire::_pw_stream_on_param_changed(void *p_user_data, uint32_t p_id, const struct spa_pod *p_param) {
	StreamState *self = (StreamState *)p_user_data;

	if (p_param == nullptr) {
		print_line("p_param is null");
		return;
	}

	struct spa_pod_object *object = (spa_pod_object *)p_param;
	struct spa_pod_prop *field;
	if (p_id == SPA_PARAM_Props) {
		print_line("negotiating SPA_PARAM_Props");

		SPA_POD_OBJECT_FOREACH(object, field) {
			uint32_t key = field->key;
			if (key == SPA_PROP_volume) {
				float volume = SPA_POD_VALUE(struct spa_pod_float, &field->value);
				print_line("Volume: ", volume);
			} else if (key == SPA_PROP_mute) {
				bool muted = SPA_POD_VALUE(struct spa_pod_bool, &field->value);
				print_line(vformat("muted: %s", muted));
			} else if (key == SPA_PROP_channelVolumes) {
				print_line("Channel Volumes");
				//TODO
			} else if (key == SPA_PROP_channelMap) {
				print_line("Channel Map");
				//TODO
			} else {
				WARN_PRINT(vformat("Unknown prop in SPA_PARAM_Props: %s", key));
			}
		}
	} else if (p_id == SPA_PARAM_Format) {
		print_line("negotiating SPA_PARAM_Format");

		SPA_POD_OBJECT_FOREACH(object, field) {
			uint32_t key = field->key;
			if (key == SPA_FORMAT_mediaType) {
				spa_media_type media = spa_media_type(SPA_POD_VALUE(struct spa_pod_int, &field->value));
				print_line("Media Type: ", media);
			} else if (key == SPA_FORMAT_mediaSubtype) {
				spa_media_subtype media = spa_media_subtype(SPA_POD_VALUE(struct spa_pod_int, &field->value));
				print_line("Media Subtype: ", media);
			} else if (key == SPA_FORMAT_AUDIO_format) {
				spa_audio_format format = spa_audio_format(SPA_POD_VALUE(struct spa_pod_int, &field->value));
				print_line("Format (raw): ", format);
			} else if (key == SPA_FORMAT_AUDIO_rate) {
				int rate = SPA_POD_VALUE(struct spa_pod_int, &field->value);
				print_line("Audio rate: ", rate);
			} else if (key == SPA_FORMAT_AUDIO_channels) {
				int channels = SPA_POD_VALUE(struct spa_pod_int, &field->value);
				print_line("Channels: ", channels);
			} else if (key == SPA_FORMAT_AUDIO_position) {
				print_line("Position");
				//TODO
			} else {
				WARN_PRINT(vformat("Unknown prop in SPA_PARAM_Format: %s", key));
			}
		}
	} else if (p_id == SPA_PARAM_Latency) {
		print_line("negotiating SPA_PARAM_Latency");

		SPA_POD_OBJECT_FOREACH(object, field) {
			uint32_t key = field->key;
			if (key == SPA_PARAM_LATENCY_direction) {
				spa_direction direction = spa_direction(SPA_POD_VALUE(struct spa_pod_int, &field->value));
				print_line("Direction: ", direction);
			} else if (key == SPA_PARAM_LATENCY_minQuantum) {
				float min_quantum = SPA_POD_VALUE(struct spa_pod_float, &field->value);
				print_line("Min Quantum: ", min_quantum);
			} else if (key == SPA_PARAM_LATENCY_maxQuantum) {
				float max_quantum = SPA_POD_VALUE(struct spa_pod_float, &field->value);
				print_line("Max Quantum: ", max_quantum);
			} else if (key == SPA_PARAM_LATENCY_minRate) {
				int min_rate = SPA_POD_VALUE(struct spa_pod_int, &field->value);
				print_line("Min Rate: ", min_rate);
			} else if (key == SPA_PARAM_LATENCY_maxRate) {
				int max_rate = SPA_POD_VALUE(struct spa_pod_int, &field->value);
				print_line("Max Rate: ", max_rate);
			} else if (key == SPA_PARAM_LATENCY_minNs) {
				long min_ns = SPA_POD_VALUE(struct spa_pod_long, &field->value);
				print_line("Min Ns: ", min_ns);
			} else if (key == SPA_PARAM_LATENCY_maxNs) {
				long max_ns = SPA_POD_VALUE(struct spa_pod_long, &field->value);
				print_line("Max Ns: ", max_ns);
			} else {
				WARN_PRINT(vformat("Unknown prop in SPA_PARAM_Latency: %s", key));
			}
		}
	} else {
		print_line("Unknown paramteter ", p_id);
	}

	return;

	//TODO is this necessary?
	uint8_t buffer2[1024];
	struct spa_pod_builder builder{};
	spa_pod_builder_init(&builder, buffer2, sizeof(buffer2));

	struct spa_pod_frame frame{};
	spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
	spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_buffers, 0);
	spa_pod_builder_int(&builder, 1);
	spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_blocks, 0);
	spa_pod_builder_int(&builder, 1);
	spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_size, 0);
	spa_pod_builder_int(&builder, 1024 * 2 * sizeof(int16_t));
	spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_stride, 0);
	spa_pod_builder_int(&builder, 2 * sizeof(int16_t));
	spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_align, 0);
	spa_pod_builder_int(&builder, 0);
	//spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_dataType, 1 << SPA_DATA_MemFd);
	//spa_pod_builder_int(&builder, 1);
	//spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_metaType, 1);
	//spa_pod_builder_int(&builder, 1);
	spa_pod *buffer_format = (spa_pod *)spa_pod_builder_pop(&builder, &frame);

	const struct spa_pod *buffer_formats[1];
	buffer_formats[0] = buffer_format;

	//pw_stream_update_params(self->stream, buffer_formats, 1);
}

//TODO I think allocating our own memory would be cool
void AudioDriverPipeWire::_pw_stream_on_add_buffer(void *p_user_data, struct pw_buffer *p_buffer) {
	StreamState *self = (StreamState *)p_user_data;

	print_line("Negotiating buffer");

	self->buffer_frames = p_buffer->buffer->datas[0].maxsize / self->stride;
	self->buffer.resize(self->buffer_frames);
	self->msdelay = 1000 * self->buffer_frames / self->mix_rate;
}

//TODO how can we make use of this?
void AudioDriverPipeWire::_pw_stream_on_remove_buffer(void *p_user_data, struct pw_buffer *p_buffer) {
}

void AudioDriverPipeWire::_pw_stream_on_process(void *p_user_data) {
	StreamState *self = (StreamState *)p_user_data;

	WARN_PRINT("PROCESS");

	struct pw_buffer *buffer_wrapper = pw_stream_dequeue_buffer(self->stream);
	if (buffer_wrapper == nullptr) {
		ERR_FAIL_MSG("Exhausted buffers");
	}

	struct spa_buffer *buffer = buffer_wrapper->buffer;

	uint64_t frame_count = self->buffer_frames;
	if (buffer_wrapper->requested != 0) {
		frame_count = MIN(frame_count, buffer_wrapper->requested);
		WARN_PRINT(vformat("requested %d", buffer_wrapper->requested));
	}

	buffer->datas[0].chunk->offset = 0;
	buffer->datas[0].chunk->stride = self->stride;
	buffer->datas[0].chunk->size = self->stride * frame_count;

	int16_t *dst = (int16_t *)buffer->datas[0].data;
	if (dst == nullptr) {
		ERR_FAIL_MSG("????");
	}

	for (uint64_t i = self->buffer_offset; i < self->buffer_offset + frame_count; i++) {
		dst[i] = self->buffer[i] >> 16; //TODO
	}

	if (self->buffer_offset + frame_count != self->buffer_frames) {
		self->buffer_offset += frame_count;
	} else {
		self->buffer_offset = 0;
	}

	pw_stream_queue_buffer(self->stream, buffer_wrapper);
}

Error AudioDriverPipeWire::init() {
	pw_init(nullptr, nullptr);

	String library_version = pw_get_library_version();
	print_verbose(vformat("Linked against PipeWire %s.", library_version));
	String headers_version = pw_get_headers_version();
	print_verbose(vformat("Compiled against PipeWire %s headers.", headers_version));

	mix_rate = _get_configured_mix_rate();
	current_sink.mix_rate = mix_rate;
	current_source.mix_rate = mix_rate;

	struct spa_dict_item loop_items[1];
	loop_items[0] = SPA_DICT_ITEM_INIT(PW_KEY_LOOP_NAME, "Godot Game Editor");
	struct spa_dict loop_props = SPA_DICT_INIT(loop_items, 1);

	loop = pw_loop_new(&loop_props);
	ERR_FAIL_NULL_V_MSG(loop, ERR_CANT_OPEN, "Failed to create PipeWire main_loop");

	context = pw_context_new(loop, nullptr, 0);
	ERR_FAIL_NULL_V_MSG(context, ERR_CANT_OPEN, "Failed to create PipeWire context");

	core = pw_context_connect(context, nullptr, 0);
	ERR_FAIL_NULL_V_MSG(core, ERR_CANT_OPEN, "Failed to connect to a PipeWire core object");

	pw_core_add_listener(core, &core_listener, &core_callbacks, this);

	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	ERR_FAIL_NULL_V_MSG(core, ERR_CANT_OPEN, "Failed to obtain a PipeWire registry");

	pw_registry_add_listener(registry, &registry_listener, &registry_callbacks, this);

	//TODO add more formats
	default_format_buffer.resize(1024);
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(default_format_buffer.ptr(), 1024 * 8);
	struct spa_audio_info_raw format_info = {
		.format = SPA_AUDIO_FORMAT_S16,
		.flags = 0,
		.rate = mix_rate,
		.channels = 2,
		.position = { 0 },
	};

	const struct spa_pod *format = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format_info);
	formats.push_back(format);

	struct pw_properties *sink_props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Music",
			NULL);

	current_sink.stream = pw_stream_new(core, "godot-output", sink_props);
	pw_stream_add_listener(current_sink.stream, &current_sink.stream_listener, &stream_callbacks, &current_sink);
	pw_stream_connect(current_sink.stream, SPA_DIRECTION_OUTPUT, PW_ID_ANY, stream_flags, formats.ptr(), formats.size());

	struct pw_properties *source_props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Game",
			NULL);

	current_source.stream = pw_stream_new(core, "godot-record", source_props);
	pw_stream_add_listener(current_source.stream, &current_source.stream_listener, &stream_callbacks, &current_source);

	return OK;
}

void AudioDriverPipeWire::start() {
	thread.start(thread_loop, this);
}

int AudioDriverPipeWire::get_mix_rate() const {
	return mix_rate;
}

//TODO
int AudioDriverPipeWire::get_input_mix_rate() const {
	return AudioDriverManager::DEFAULT_MIX_RATE;
}

//TODO
AudioDriver::SpeakerMode AudioDriverPipeWire::get_speaker_mode() const {
	return SpeakerMode::SPEAKER_MODE_STEREO;
}

//TODO
float AudioDriverPipeWire::get_latency() {
	return 11.0;
}

void AudioDriverPipeWire::lock() {
	mutex.lock();
}

void AudioDriverPipeWire::unlock() {
	mutex.unlock();
}

void AudioDriverPipeWire::finish() {
	exit_thread.set();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}

	if (current_sink.stream) {
		pw_stream_disconnect(current_sink.stream);
		pw_stream_destroy(current_sink.stream);
		spa_hook_remove(&current_sink.stream_listener);
		current_sink.stream = nullptr;
	}

	if (current_source.stream) {
		pw_stream_disconnect(current_source.stream);
		pw_stream_destroy(current_source.stream);
		spa_hook_remove(&current_source.stream_listener);
		current_source.stream = nullptr;
	}

	if (registry) {
		pw_proxy_destroy((struct pw_proxy *)registry);
		spa_hook_remove(&registry_listener);
		registry = nullptr;
	}

	if (core) {
		pw_core_disconnect(core);
		spa_hook_remove(&core_listener);
		core = nullptr;
	}

	if (context) {
		pw_context_destroy(context);
		context = nullptr;
	}

	if (loop) {
		pw_loop_destroy(loop);
		loop = nullptr;
	}

	pw_deinit();
}

PackedStringArray AudioDriverPipeWire::get_output_device_list() {
	PackedStringArray outputs;
	outputs.push_back("Default");

	for (const KeyValue<uint32_t, String> &pair : sinks) {
		outputs.push_back(pair.value);
	}

	return outputs;
}

String AudioDriverPipeWire::get_output_device() {
	return current_sink.name;
}

void AudioDriverPipeWire::set_output_device(const String &p_name) {
	if (current_sink.name == p_name) {
		return;
	}

	current_sink.name = p_name;
	sink_dirty = true;
}

Error AudioDriverPipeWire::input_start() {
	if (recording) {
		return OK;
	}

	recording = true;
	source_dirty = true;
	return OK;
}

Error AudioDriverPipeWire::input_stop() {
	if (!recording) {
		return OK;
	}

	recording = false;
	source_dirty = true;
	return OK;
}

PackedStringArray AudioDriverPipeWire::get_input_device_list() {
	PackedStringArray inputs;
	inputs.push_back("Default");

	for (const KeyValue<uint32_t, String> &pair : sources) {
		inputs.push_back(pair.value);
	}

	return inputs;
}

String AudioDriverPipeWire::get_input_device() {
	return current_source.name;
}

void AudioDriverPipeWire::set_input_device(const String &p_name) {
	if (current_source.name == p_name) {
		return;
	}

	current_source.name = p_name;
	source_dirty = true;
}

void AudioDriverPipeWire::thread_loop(void *p_user_data) {
	AudioDriverPipeWire *self = static_cast<AudioDriverPipeWire *>(p_user_data);

	// Force a roundtrip to init sinks/sources
	self->pending = pw_core_sync(self->core, PW_ID_CORE, 0);

	pw_loop_enter(self->loop);

	int err;
	while (!self->synced.is_set()) {
		err = pw_loop_iterate(self->loop, 0);
		if (err < 0) {
			ERR_PRINT(vformat("PipeWire Error: %d", err));
		}
	}

	//TODO input streams

	while (!self->exit_thread.is_set()) {
		if (self->current_sink.paused) {
			err = pw_loop_iterate(self->loop, 0);
			if (err < 0) {
				ERR_PRINT(vformat("PipeWire Error: %d", err));
			}
			continue;
		}

		if (self->current_sink.buffer_offset == 0) {
			self->lock();
			self->start_counting_ticks();

			self->audio_server_process(self->current_sink.buffer_frames, self->current_sink.buffer.ptr());

			self->stop_counting_ticks();
			self->unlock();
		}

		self->lock();
		self->start_counting_ticks();

		if (self->sink_dirty) {
			print_line("updating dirty sink state");
			pw_stream_disconnect(self->current_sink.stream);

			spa_dict_item items[1];
			items[0] = SPA_DICT_ITEM_INIT(PW_KEY_TARGET_OBJECT, self->current_sink.name.utf8().get_data());

			spa_dict props = SPA_DICT_INIT(items, 1);
			err = pw_stream_update_properties(self->current_sink.stream, &props);
			if (err < 0) {
				ERR_PRINT(vformat("pw_stream_update_properties error: %s", err));
			}

			pw_stream_connect(self->current_sink.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, stream_flags, self->formats.ptr(), self->formats.size());

			self->sink_dirty = false;
		}

		if (self->source_dirty) {
			print_line("updating dirty source state");
			pw_stream_disconnect(self->current_source.stream);

			spa_dict_item items[1];
			items[0] = SPA_DICT_ITEM_INIT(PW_KEY_TARGET_OBJECT, self->current_source.name.utf8().get_data());

			spa_dict props = SPA_DICT_INIT(items, 1);
			err = pw_stream_update_properties(self->current_source.stream, &props);
			if (err < 0) {
				ERR_PRINT(vformat("pw_stream_update_properties error: %s", err));
			}

			pw_stream_connect(self->current_source.stream, PW_DIRECTION_INPUT, PW_ID_ANY, stream_flags, self->formats.ptr(), self->formats.size());

			self->source_dirty = false;
		}

		err = pw_loop_iterate(self->loop, -1);
		if (err < 0) {
			ERR_PRINT(vformat("PipeWire Error: %d", err));
		}

		self->stop_counting_ticks();
		self->unlock();
	}

	pw_loop_leave(self->loop);
}

AudioDriverPipeWire::AudioDriverPipeWire() {
	exit_thread.clear();
	synced.clear();

	spa_zero(core_listener);
	spa_zero(registry_listener);
	spa_zero(current_sink.stream_listener);
	spa_zero(current_source.stream_listener);

	sinks.clear();
	sources.clear();
}
