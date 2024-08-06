/*
 * Copyright (C) 2024 Patrick Gaskin <patrick@pgaskin.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// for development, can cross-compile with $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang -target aarch64-linux-android26 -shared -o aaudio.so -fPIC -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ -Werror=unguarded-availability -Wall -std=gnu11 op/aaudio.c -laaudio
// also see https://github.com/google/oboe/blob/main/docs/AndroidAudioHistory.md
// also see https://android.googlesource.com/platform/frameworks/av/+/master/media/libaaudio/examples/utils/AAudioSimplePlayer.h

#ifndef __ANDROID__
// make ide autocomplete work without using a full ndk toolchain
#define __INTRODUCED_IN(api_level)
#endif

// https://developer.android.com/ndk/guides/using-newer-apis
#define REQUIRES_API(x) __attribute__((__availability__(android,introduced=x)))
#define API_AT_LEAST(x) __builtin_available(android x, *)
#define AAUDIO_MINIMUM_API 26

#include <aaudio/AAudio.h>

#include "../op.h"
#include "../mixer.h"
#include "../sf.h"
#include "../utils.h"
#include "../xmalloc.h"

// see the AAudio.h channel mask enum
// note that this happens to match the wav channel order
static channel_position_t cmus_channel_by_aaudio[CHANNELS_MAX] = { // [AAUDIO_CHANNEL_* shift] = cmus channel
	/* AAUDIO_CHANNEL_FRONT_LEFT            = 1 <<  0 */ CHANNEL_POSITION_FRONT_LEFT,
	/* AAUDIO_CHANNEL_FRONT_RIGHT           = 1 <<  1 */ CHANNEL_POSITION_FRONT_RIGHT,
	/* AAUDIO_CHANNEL_FRONT_CENTER          = 1 <<  2 */ CHANNEL_POSITION_FRONT_CENTER,
	/* AAUDIO_CHANNEL_LOW_FREQUENCY         = 1 <<  3 */ CHANNEL_POSITION_LFE,
	/* AAUDIO_CHANNEL_BACK_LEFT             = 1 <<  4 */ CHANNEL_POSITION_REAR_LEFT,
	/* AAUDIO_CHANNEL_BACK_RIGHT            = 1 <<  5 */ CHANNEL_POSITION_REAR_RIGHT,
	/* AAUDIO_CHANNEL_FRONT_LEFT_OF_CENTER  = 1 <<  6 */ CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
	/* AAUDIO_CHANNEL_FRONT_RIGHT_OF_CENTER = 1 <<  7 */ CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
	/* AAUDIO_CHANNEL_BACK_CENTER           = 1 <<  8 */ CHANNEL_POSITION_REAR_CENTER,
	/* AAUDIO_CHANNEL_SIDE_LEFT             = 1 <<  9 */ CHANNEL_POSITION_SIDE_LEFT,
	/* AAUDIO_CHANNEL_SIDE_RIGHT            = 1 << 10 */ CHANNEL_POSITION_SIDE_RIGHT,
	/* AAUDIO_CHANNEL_TOP_CENTER            = 1 << 11 */ CHANNEL_POSITION_TOP_CENTER,
	/* AAUDIO_CHANNEL_TOP_FRONT_LEFT        = 1 << 12 */ CHANNEL_POSITION_TOP_FRONT_LEFT,
	/* AAUDIO_CHANNEL_TOP_FRONT_CENTER      = 1 << 13 */ CHANNEL_POSITION_TOP_FRONT_CENTER,
	/* AAUDIO_CHANNEL_TOP_FRONT_RIGHT       = 1 << 14 */ CHANNEL_POSITION_TOP_FRONT_RIGHT,
	/* AAUDIO_CHANNEL_TOP_BACK_LEFT         = 1 << 15 */ CHANNEL_POSITION_TOP_REAR_LEFT,
	/* AAUDIO_CHANNEL_TOP_BACK_CENTER       = 1 << 16 */ CHANNEL_POSITION_TOP_REAR_CENTER,
	/* AAUDIO_CHANNEL_TOP_BACK_RIGHT        = 1 << 17 */ CHANNEL_POSITION_TOP_REAR_RIGHT,
	/* AAUDIO_CHANNEL_TOP_SIDE_LEFT         = 1 << 18 */ CHANNEL_POSITION_INVALID,
	/* AAUDIO_CHANNEL_TOP_SIDE_RIGHT        = 1 << 19 */ CHANNEL_POSITION_INVALID,
	/* AAUDIO_CHANNEL_BOTTOM_FRONT_LEFT     = 1 << 20 */ CHANNEL_POSITION_INVALID,
	/* AAUDIO_CHANNEL_BOTTOM_FRONT_CENTER   = 1 << 21 */ CHANNEL_POSITION_INVALID,
	/* AAUDIO_CHANNEL_BOTTOM_FRONT_RIGHT    = 1 << 22 */ CHANNEL_POSITION_INVALID,
	/* AAUDIO_CHANNEL_LOW_FREQUENCY_2       = 1 << 23 */ CHANNEL_POSITION_INVALID,
	/* AAUDIO_CHANNEL_FRONT_WIDE_LEFT       = 1 << 24 */ CHANNEL_POSITION_INVALID,
	/* AAUDIO_CHANNEL_FRONT_WIDE_RIGHT      = 1 << 25 */ CHANNEL_POSITION_INVALID,
	/*                                                */ CHANNEL_POSITION_INVALID,
	/*                                                */ CHANNEL_POSITION_INVALID,
	/*                                                */ CHANNEL_POSITION_INVALID,
	/*                                                */ CHANNEL_POSITION_INVALID,
	/*                                                */ CHANNEL_POSITION_INVALID,
	/*                                                */ CHANNEL_POSITION_INVALID,
};

static AAudioStream *strm;
static int32_t strm_frame_size;
static int32_t strm_last_device;
static bool strm_errored;
static int mixer_notify_output_in, mixer_notify_output_out;

// note: all options require restarting the output stream to apply
static aaudio_performance_mode_t op_aaudio_opt_performance_mode = AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
static aaudio_allowed_capture_policy_t op_aaudio_opt_allowed_capture = AAUDIO_ALLOW_CAPTURE_BY_ALL;
static aaudio_sharing_mode_t op_aaudio_opt_sharing_mode = AAUDIO_SHARING_MODE_SHARED;
static bool op_aaudio_opt_disable_spatialization = false;

// if we ever decide to support AAUDIO_PERFORMANCE_MODE_LOW_LATENCY streams,
// note that disconnection is broken for shared low-latency streams on RQ1A
// (this doesn't affect us right now since we don't use low-latency shared mmap
// streams)
//
// https://issuetracker.google.com/issues/173928197

static int op_aaudio_set_performance_mode(const char *val)
{
	if (!strcmp(val, "none")) {
		op_aaudio_opt_performance_mode = AAUDIO_PERFORMANCE_MODE_NONE;
		return OP_ERROR_SUCCESS;
	}
	if (!strcmp(val, "power_saving")) {
		op_aaudio_opt_performance_mode = AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
		return OP_ERROR_SUCCESS;
	}
	errno = EINVAL;
	return -OP_ERROR_ERRNO;
}

static int op_aaudio_get_performance_mode(char **val)
{
	switch (op_aaudio_opt_performance_mode) {
	default:
		__attribute__((fallthrough));
	case AAUDIO_PERFORMANCE_MODE_NONE:
		*val = xstrdup("none");
		break;
	case AAUDIO_PERFORMANCE_MODE_POWER_SAVING:
		*val = xstrdup("power_saving");
		break;
	}
	return OP_ERROR_SUCCESS;
}

static int op_aaudio_set_allowed_capture(const char *val)
{
	if (!strcmp(val, "all")) {
		op_aaudio_opt_allowed_capture = AAUDIO_ALLOW_CAPTURE_BY_ALL;
		return OP_ERROR_SUCCESS;
	}
	if (!strcmp(val, "none")) {
		op_aaudio_opt_allowed_capture = AAUDIO_ALLOW_CAPTURE_BY_NONE;
		return OP_ERROR_SUCCESS;
	}
	if (!strcmp(val, "system")) {
		op_aaudio_opt_allowed_capture = AAUDIO_ALLOW_CAPTURE_BY_SYSTEM;
		return OP_ERROR_SUCCESS;
	}
	errno = EINVAL;
	return -OP_ERROR_ERRNO;
}

static int op_aaudio_get_allowed_capture(char **val)
{
	switch (op_aaudio_opt_allowed_capture) {
	default:
		__attribute__((fallthrough));
	case AAUDIO_ALLOW_CAPTURE_BY_ALL:
		*val = xstrdup("all");
		break;
	case AAUDIO_ALLOW_CAPTURE_BY_NONE:
		*val = xstrdup("none");
		break;
	case AAUDIO_ALLOW_CAPTURE_BY_SYSTEM:
		*val = xstrdup("system");
		break;
	}
	return OP_ERROR_SUCCESS;
}

static int op_aaudio_set_sharing_mode(const char *val)
{
	if (!strcmp(val, "shared")) {
		op_aaudio_opt_performance_mode = AAUDIO_SHARING_MODE_SHARED;
		return OP_ERROR_SUCCESS;
	}
	if (!strcmp(val, "exclusive")) {
		op_aaudio_opt_performance_mode = AAUDIO_SHARING_MODE_EXCLUSIVE;
		return OP_ERROR_SUCCESS;
	}
	errno = EINVAL;
	return -OP_ERROR_ERRNO;
}

static int op_aaudio_get_sharing_mode(char **val)
{
	switch (op_aaudio_opt_performance_mode) {
	default:
		__attribute__((fallthrough));
	case AAUDIO_SHARING_MODE_SHARED:
		*val = xstrdup("shared");
		break;
	case AAUDIO_SHARING_MODE_EXCLUSIVE:
		*val = xstrdup("exclusive");
		break;
	}
	return OP_ERROR_SUCCESS;
}

static int op_aaudio_set_disable_spatialization(const char *val)
{
	op_aaudio_opt_disable_spatialization = strcmp(val, "true") ? false : true;
	return OP_ERROR_SUCCESS;
}

static int op_aaudio_get_disable_spatialization(char **val)
{
	*val = xstrdup(op_aaudio_opt_disable_spatialization ? "true" : "false");
	return OP_ERROR_SUCCESS;
}

static bool aaudio_supported() {
	if (API_AT_LEAST(AAUDIO_MINIMUM_API)) {
		return !!&AAudio_createStreamBuilder;
	}
	if (API_AT_LEAST(27)) {} else {
		// don't use AAudio on API 26 due to bug causing crash on some
		// devices when closing stream
		//
		// https://github.com/google/oboe/issues/40
		return -OP_ERROR_NOT_SUPPORTED;
	}
	return false;
}

static int op_aaudio_init(void)
{
	if (!aaudio_supported()) {
		// skip the output plugin (see op_select_any)
		return -OP_ERROR_NOT_SUPPORTED;
	}

	init_pipes(&mixer_notify_output_out, &mixer_notify_output_in);

	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static aaudio_result_t aaudio_request_state_change(AAudioStream *stream, aaudio_result_t (*request)(AAudioStream *strm), aaudio_stream_state_t state, aaudio_stream_state_t state2)
{
	aaudio_result_t rc;

	if (request) {
		d_print("request state change\n");
		rc = request(stream);
		if (rc) {
			return rc;
		}
	}

	d_print("wait state change (%d:%s || %d:%s)\n", state, AAudio_convertStreamStateToText(state), state2, AAudio_convertStreamStateToText(state2));
	aaudio_stream_state_t currentState = AAudioStream_getState(stream);
	aaudio_stream_state_t inputState = currentState;
	rc = AAUDIO_OK;
	while (rc == AAUDIO_OK && currentState != state && (state2 == 0 || currentState != state2)) {
		d_print("current state change %d\r\n", currentState);
		rc = AAudioStream_waitForStateChange(stream, inputState, &currentState, INT64_MAX);
		inputState = currentState;
	}
	if (rc) {
		d_print("failed state change (%d - %s) [current=%d:%s]\n", rc, AAudio_convertResultToText(rc), currentState, AAudio_convertStreamStateToText(currentState));
	} else {
		d_print("done state change [current=%d:%s]\n", currentState, AAudio_convertStreamStateToText(currentState));
	}
	return rc;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_exit(void)
{
	close(mixer_notify_output_out);
	close(mixer_notify_output_in);

	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static void handle_error(AAudioStream *stream, void *userData, aaudio_result_t error) {
	if (error == AAUDIO_ERROR_DISCONNECTED) {
		notify_via_pipe(mixer_notify_output_in);
	}
	d_print("stream errored (%d - %s)\n", error, AAudio_convertResultToText(error));
	strm_errored = true;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_open(sample_format_t sf, const channel_position_t *channel_map)
{
	aaudio_result_t rc;
	AAudioStreamBuilder *bld;

	// create the stream builder
	rc = AAudio_createStreamBuilder(&bld);
	if (rc) {
		d_print("create stream builder failed (%d - %s)\n", rc, AAudio_convertResultToText(rc));
		return -OP_ERROR_INTERNAL;
	}

	// apply the options
	AAudioStreamBuilder_setSharingMode(bld, op_aaudio_opt_sharing_mode);
	AAudioStreamBuilder_setPerformanceMode(bld, op_aaudio_opt_performance_mode);
	if (API_AT_LEAST(28)) AAudioStreamBuilder_setContentType(bld, AAUDIO_CONTENT_TYPE_MUSIC);
	if (API_AT_LEAST(28)) AAudioStreamBuilder_setUsage(bld, AAUDIO_USAGE_MEDIA);
	if (API_AT_LEAST(29)) AAudioStreamBuilder_setAllowedCapturePolicy(bld, op_aaudio_opt_allowed_capture);
	if (API_AT_LEAST(31)) AAudioStreamBuilder_setAttributionTag(bld, "cmus");
	if (API_AT_LEAST(32)) AAudioStreamBuilder_setSpatializationBehavior(bld, op_aaudio_opt_disable_spatialization ? AAUDIO_SPATIALIZATION_BEHAVIOR_NEVER : AAUDIO_SPATIALIZATION_BEHAVIOR_AUTO);

	// apply the channel layout
	AAudioStreamBuilder_setChannelCount(bld, sf_get_channels(sf));

	// if not mono audio, validate the channel layout
	if (sf_get_channels(sf) != 1) {
		// if we have a channel map, ensure it matches the aaudio channel map
		// TODO: on api 32, we can set a channel mask to skip channels
		// TODO: maybe add code for remapping channels into the correct order if necessary
		if (channel_map && channel_map_valid(channel_map)) {
			// for each channel in the frame
			for (int i = 0; i < sf_get_channels(sf); i++) {
				// if the channel aaudio wants is not the channel the input has
				if (cmus_channel_by_aaudio[i] != channel_map[i]) {
					d_print("aaudio channel idx %d maps to cmus channel position %d, but input channel idx %d maps to cmus channel position %d (and we don't currently support channel remapping, so we can't play this channel layout)\n", i, cmus_channel_by_aaudio[i], i, channel_map[i]);
					return -OP_ERROR_SAMPLE_FORMAT;
				}
			}
		} else {
			// if we don't, assume it's in the wav ordering, which happens to match what aaudio expects
		}
	}

	// apply the sample format
	if (!sf_get_signed(sf)) {
		d_print("aaudio does not support unsigned samples\n");
		AAudioStreamBuilder_delete(bld);
		return -OP_ERROR_SAMPLE_FORMAT;
	}
	if (sf_get_bigendian(sf)) {
		d_print("aaudio does not support big-endian samples\n");
		AAudioStreamBuilder_delete(bld);
		return -OP_ERROR_SAMPLE_FORMAT;
	}
	switch (sf_get_bits(sf)) {
		case 16:
			AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
			break;
		case 24:
			AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I24_PACKED);
			break;
		case 32:
			AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I32);
			break;
		default:
			d_print("unsupported sample format bits\n");
			AAudioStreamBuilder_delete(bld);
			return -OP_ERROR_SAMPLE_FORMAT;
	}

	// apply the sample rate
	AAudioStreamBuilder_setSampleRate(bld, sf_get_rate(sf));

	// set the error callback
	AAudioStreamBuilder_setErrorCallback(bld, handle_error, NULL);

	// open the stream
	strm_frame_size = sf_get_frame_size(sf);
	strm_last_device = -1;
	strm_errored = false;
	rc = AAudioStreamBuilder_openStream(bld, &strm);
	if (rc) {
		d_print("open stream failed (%d - %s)\n", rc, AAudio_convertResultToText(rc));
		AAudioStreamBuilder_delete(bld);
		if (rc == AAUDIO_ERROR_INVALID_RATE || rc == AAUDIO_ERROR_INVALID_FORMAT) {
			return -OP_ERROR_SAMPLE_FORMAT;
		}
		return -OP_ERROR_INTERNAL;
	}
	d_print("optimal buffer frames = %d\n", AAudioStream_getFramesPerBurst(strm));
	d_print("max non-blocking buffer frames = %d\n", AAudioStream_getBufferSizeInFrames(strm));

	// cleanup the stream builder
	rc = AAudioStreamBuilder_delete(bld);
	if (rc) {
		d_print("delete stream builder failed (%d - %s)\n", rc, AAudio_convertResultToText(rc));
		AAudioStream_close(strm);
		return -OP_ERROR_INTERNAL;
	}

	// done (we don't actually start the stream until the first write)
	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_close(void)
{
	if (strm) {
		AAudioStream_close(strm);
		strm = NULL;
	}

	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_drop(void)
{
	aaudio_result_t rc;
	aaudio_stream_state_t orig_state = AAudioStream_getState(strm);

	// we can't flush if it's closing
	if (orig_state == AAUDIO_STREAM_STATE_CLOSING || orig_state == AAUDIO_STREAM_STATE_CLOSED) {
		return -OP_ERROR_NOT_OPEN;
	}

	// only flush if it isn't already flushed or closed
	if (orig_state != AAUDIO_STREAM_STATE_FLUSHED) {

		// the stream must be paused to be flushed
		if (orig_state == AAUDIO_STREAM_STATE_STARTED || orig_state == AAUDIO_STREAM_STATE_STARTING) {
			rc = aaudio_request_state_change(strm, AAudioStream_requestPause, AAUDIO_STREAM_STATE_PAUSED, 0);
			if (rc) {
				return -OP_ERROR_INTERNAL;
			}
			// the stream will be started again on the first write
		}

		// flush the stream
		rc = aaudio_request_state_change(strm, AAudioStream_requestFlush, AAUDIO_STREAM_STATE_FLUSHED, 0);
		if (rc) {
			return -OP_ERROR_INTERNAL;
		}
	}

	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_write(const char *buf, int count)
{
	aaudio_result_t rc;

	// if the stream errored, return an error so cmus restarts the output
	// plugin
	//
	// note that this isn't strictly required since AAudioStream_write will
	// return an error on stream disconnection, which will cause cmus to
	// reopen the output plugin
	//
	// https://github.com/google/oboe/wiki/TechNote_Disconnect
	if (strm_errored) {
		return -OP_ERROR_INTERNAL;
	}

	// note: this is cheap; it's just a field getter internally
	int32_t device = AAudioStream_getDeviceId(strm);
	if (strm_last_device != device) {
		if (strm_last_device != -1) {
			notify_via_pipe(mixer_notify_output_in);
		}
		strm_last_device = device;
	}

	// start the stream on the first write (rather than after opening or
	// flushing since cmus may not always use the stream and starting a
	// stream is somewhat expensive)
	//
	// note: this is cheap; it's just a atomic field getter internally
	aaudio_stream_state_t state = AAudioStream_getState(strm);
	if (state == AAUDIO_STREAM_STATE_CLOSING || state == AAUDIO_STREAM_STATE_CLOSED) {
		return -OP_ERROR_NOT_OPEN;
	}
	if (state != AAUDIO_STREAM_STATE_STARTING && state != AAUDIO_STREAM_STATE_STARTED) {
		rc = aaudio_request_state_change(strm, AAudioStream_requestStart, AAUDIO_STREAM_STATE_STARTED, AAUDIO_STREAM_STATE_STARTING);
		if (rc) {
			return -OP_ERROR_INTERNAL;
		}
	}

	// synchronously write the samples to the buffer
	rc = AAudioStream_write(strm, buf, count / strm_frame_size, INT64_MAX);
	if (rc < 0) {
		d_print("write %d = error %d - %s [device=%d] [state=%d]\n", count / strm_frame_size, rc, AAudio_convertResultToText(rc), device, state);
		return -OP_ERROR_INTERNAL;
	}
	d_print("write %d = %d (* %d bytes) [device=%d] [state=%d]\n", count / strm_frame_size, rc, strm_frame_size, device, state);

	// return the number of bytes we write
	return rc * strm_frame_size;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_pause(void)
{
	// request stream pause, wait until it completes
	return aaudio_request_state_change(strm, AAudioStream_requestPause, AAUDIO_STREAM_STATE_PAUSED, 0)
		? -OP_ERROR_INTERNAL
		: OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_unpause(void)
{
	// request stream start, wait until it starts to start (i.e., will start
	// consuming frames written to it)
	return aaudio_request_state_change(strm, AAudioStream_requestStart, AAUDIO_STREAM_STATE_STARTED, AAUDIO_STREAM_STATE_STARTING)
		? -OP_ERROR_INTERNAL
		: OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_buffer_space(void)
{
	// optimal buffer amount (anecdotally, this generally seems to be less
	// than half the max buffer amount)
	return AAudioStream_getFramesPerBurst(strm) * strm_frame_size;

	// max buffer amount (without blocking)
	// return AAudioStream_getBufferSizeInFrames(strm) * strm_frame_size;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_mixer_init(void)
{
	if (!aaudio_supported()) {
		// skip the output plugin (see op_select_any)
		return -OP_ERROR_NOT_SUPPORTED;
	}
	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_mixer_exit(void)
{
	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_mixer_open(int *volume_max)
{
	*volume_max = UINT16_MAX;

	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_mixer_close(void)
{
	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_mixer_get_fds(int what, int *fds)
{
	switch (what) {
	case MIXER_FDS_OUTPUT:
		fds[0] = mixer_notify_output_out;
		return 1;
	default:
		return 0;
	}
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_mixer_set_volume(int l, int r)
{
	return -OP_ERROR_NOT_SUPPORTED;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_mixer_get_volume(int *l, int *r)
{
	// aaudio doesn't support volume control, so say the volume is 100%
	*l = *r = UINT16_MAX;

	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
const struct output_plugin_ops op_pcm_ops = {
	.init = op_aaudio_init,
	.exit = op_aaudio_exit,
	.open = op_aaudio_open,
	.close = op_aaudio_close,
	.drop = op_aaudio_drop,
	.write = op_aaudio_write,
	.pause = op_aaudio_pause,
	.unpause = op_aaudio_unpause,
	.buffer_space = op_aaudio_buffer_space,
};

REQUIRES_API(AAUDIO_MINIMUM_API)
const struct mixer_plugin_ops op_mixer_ops = {
	.init = op_aaudio_mixer_init,
	.exit = op_aaudio_mixer_exit,
	.open = op_aaudio_mixer_open,
	.close = op_aaudio_mixer_close,
	.get_fds.abi_2 = op_aaudio_mixer_get_fds,
	.set_volume = op_aaudio_mixer_set_volume,
	.get_volume = op_aaudio_mixer_get_volume,
};

const struct output_plugin_opt op_pcm_options[] = {
	OPT(op_aaudio, performance_mode),
	OPT(op_aaudio, allowed_capture),
	OPT(op_aaudio, sharing_mode),
	OPT(op_aaudio, disable_spatialization),
	{ NULL },
};

const struct mixer_plugin_opt op_mixer_options[] = {
	{ NULL },
};

const int op_priority = -3; // higher priority than pulse (-2)
const unsigned op_abi_version = OP_ABI_VERSION;
