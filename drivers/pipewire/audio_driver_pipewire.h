/**************************************************************************/
/*  audio_driver_pipewire.h                                               */
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

#pragma once

#include "core/os/mutex.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "servers/audio_server.h"

#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>
#include <cstdint>

class AudioDriverPipeWire : public AudioDriver {
private:
	static constexpr pw_stream_flags stream_flags = pw_stream_flags(
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS);

	Thread thread;
	Mutex mutex;
	SafeFlag exit_thread;

	int pending = 0;
	SafeFlag synced;

	uint32_t mix_rate = AudioDriverManager::DEFAULT_MIX_RATE;

	LocalVector<const struct spa_pod *> formats;
	LocalVector<uint8_t> default_format_buffer;

	struct StreamState {
		struct pw_stream *stream = nullptr;
		struct spa_hook stream_listener{};

		String name;
		bool paused = true;
		uint32_t mix_rate = AudioDriverManager::DEFAULT_MIX_RATE;
		uint64_t msdelay = 0;

		int stride = sizeof(int16_t) * 2;
		LocalVector<int32_t> buffer;
		uint64_t buffer_frames = 0;
		size_t buffer_offset = 0;
	};

	HashMap<uint32_t, String> sinks;
	HashMap<uint32_t, String> sources;
	StreamState current_sink;
	StreamState current_source;
	bool sink_dirty = false;
	bool source_dirty = false;

	bool recording = false;

	// Have no significant events for us to bind to.
	struct pw_loop *loop = nullptr;
	struct pw_context *context = nullptr;

	struct pw_core *core = nullptr;
	struct spa_hook core_listener{};

	static void _pw_core_on_done(void *p_user_data, uint32_t p_id, int p_seq);
	static void _pw_core_on_ping(void *p_user_data, uint32_t p_id, int p_seq);
	static void _pw_core_on_error(void *p_user_data, uint32_t p_id, int p_seq, int p_res, const char *p_message);

	static constexpr pw_core_events core_callbacks = {
		.version = PW_VERSION_CORE_EVENTS,
		.info = nullptr,
		.done = &_pw_core_on_done,
		.ping = &_pw_core_on_ping,
		.error = &_pw_core_on_error,
		.remove_id = nullptr,
		.bound_id = nullptr,
		.add_mem = nullptr,
		.remove_mem = nullptr,
		.bound_props = nullptr
	};

	struct pw_registry *registry = nullptr;
	struct spa_hook registry_listener{};

	static void _pw_registry_on_global(void *p_user_data, uint32_t p_id, uint32_t p_permissions, const char *p_type, uint32_t p_version, const struct spa_dict *p_props);
	static void _pw_registry_on_global_remove(void *p_user_data, uint32_t p_id);

	static constexpr pw_registry_events registry_callbacks = {
		.version = PW_VERSION_REGISTRY_EVENTS,
		.global = &_pw_registry_on_global,
		.global_remove = &_pw_registry_on_global_remove
	};

	static void _pw_stream_on_state_changed(void *p_user_data, pw_stream_state p_prev_state, pw_stream_state p_curr_state, const char *p_err);
	static void _pw_stream_on_param_changed(void *p_user_data, uint32_t p_id, const struct spa_pod *p_param);
	static void _pw_stream_on_add_buffer(void *p_user_data, struct pw_buffer *p_buffer);
	static void _pw_stream_on_remove_buffer(void *p_user_data, struct pw_buffer *p_buffer);
	static void _pw_stream_on_process(void *p_user_data);

	static constexpr struct pw_stream_events stream_callbacks = {
		.version = PW_VERSION_STREAM_EVENTS,
		.destroy = nullptr,
		.state_changed = &_pw_stream_on_state_changed,
		.control_info = nullptr,
		.io_changed = nullptr,
		.param_changed = &_pw_stream_on_param_changed,
		.add_buffer = &_pw_stream_on_add_buffer,
		.remove_buffer = &_pw_stream_on_remove_buffer,
		.process = &_pw_stream_on_process,
		.drained = nullptr,
		.command = nullptr,
		.trigger_done = nullptr
	};

public:
	AudioDriverPipeWire();
	~AudioDriverPipeWire() {}

	virtual const char *get_name() const override {
		return "PipeWire";
	}

	virtual Error init() override;
	virtual void start() override;
	virtual int get_mix_rate() const override;
	virtual int get_input_mix_rate() const override;
	virtual SpeakerMode get_speaker_mode() const override;
	virtual float get_latency() override;

	virtual void lock() override;
	virtual void unlock() override;
	virtual void finish() override;

	virtual PackedStringArray get_output_device_list() override;
	virtual String get_output_device() override;
	virtual void set_output_device(const String &p_name) override;

	virtual Error input_start() override;
	virtual Error input_stop() override;

	virtual PackedStringArray get_input_device_list() override;
	virtual String get_input_device() override;
	virtual void set_input_device(const String &p_name) override;

private:
	static void thread_loop(void *p_user_data);
};
