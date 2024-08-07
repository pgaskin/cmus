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

// mapping from AAUDIO_CHANNEL_* enum values to cmus channel_position_t values
//
// cat "$(find ${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk} -wholename '*/aaudio/AAudio.h' | sort -n | tail -n1)" |
// grep AAUDIO_CHANNEL | tr -d ' \n' | tr '|,' ' \n' | grep -F '<<' |
// cut -d '_' -f3- | cut -d '=' -f1 | xargs printf '#define A2C__%s\tCHANNEL_POSITION_INVALID\n' |
// column -s $'\t' -t | tee /dev/stderr | cut -d ' ' -f2 | cut -d '_' -f3- |
// xargs printf ' X(%s)' | xargs -0 printf '#define A2C_CHANNELS%s\n'
#define A2C__FRONT_LEFT            CHANNEL_POSITION_FRONT_LEFT
#define A2C__FRONT_RIGHT           CHANNEL_POSITION_FRONT_RIGHT
#define A2C__FRONT_CENTER          CHANNEL_POSITION_FRONT_CENTER
#define A2C__LOW_FREQUENCY         CHANNEL_POSITION_LFE
#define A2C__BACK_LEFT             CHANNEL_POSITION_REAR_LEFT
#define A2C__BACK_RIGHT            CHANNEL_POSITION_REAR_RIGHT
#define A2C__FRONT_LEFT_OF_CENTER  CHANNEL_POSITION_FRONT_LEFT_OF_CENTER
#define A2C__FRONT_RIGHT_OF_CENTER CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER
#define A2C__BACK_CENTER           CHANNEL_POSITION_REAR_CENTER
#define A2C__SIDE_LEFT             CHANNEL_POSITION_SIDE_LEFT
#define A2C__SIDE_RIGHT            CHANNEL_POSITION_SIDE_RIGHT
#define A2C__TOP_CENTER            CHANNEL_POSITION_TOP_CENTER
#define A2C__TOP_FRONT_LEFT        CHANNEL_POSITION_TOP_FRONT_LEFT
#define A2C__TOP_FRONT_CENTER      CHANNEL_POSITION_TOP_FRONT_CENTER
#define A2C__TOP_FRONT_RIGHT       CHANNEL_POSITION_TOP_FRONT_RIGHT
#define A2C__TOP_BACK_LEFT         CHANNEL_POSITION_TOP_REAR_LEFT
#define A2C__TOP_BACK_CENTER       CHANNEL_POSITION_TOP_REAR_CENTER
#define A2C__TOP_BACK_RIGHT        CHANNEL_POSITION_TOP_REAR_RIGHT
#define A2C__TOP_SIDE_LEFT         CHANNEL_POSITION_INVALID
#define A2C__TOP_SIDE_RIGHT        CHANNEL_POSITION_INVALID
#define A2C__BOTTOM_FRONT_LEFT     CHANNEL_POSITION_INVALID
#define A2C__BOTTOM_FRONT_CENTER   CHANNEL_POSITION_INVALID
#define A2C__BOTTOM_FRONT_RIGHT    CHANNEL_POSITION_INVALID
#define A2C__LOW_FREQUENCY_2       CHANNEL_POSITION_INVALID
#define A2C__FRONT_WIDE_LEFT       CHANNEL_POSITION_INVALID
#define A2C__FRONT_WIDE_RIGHT      CHANNEL_POSITION_INVALID
#define A2C_CHANNELS X(FRONT_LEFT) X(FRONT_RIGHT) X(FRONT_CENTER) X(LOW_FREQUENCY) X(BACK_LEFT) X(BACK_RIGHT) X(FRONT_LEFT_OF_CENTER) X(FRONT_RIGHT_OF_CENTER) X(BACK_CENTER) X(SIDE_LEFT) X(SIDE_RIGHT) X(TOP_CENTER) X(TOP_FRONT_LEFT) X(TOP_FRONT_CENTER) X(TOP_FRONT_RIGHT) X(TOP_BACK_LEFT) X(TOP_BACK_CENTER) X(TOP_BACK_RIGHT) X(TOP_SIDE_LEFT) X(TOP_SIDE_RIGHT) X(BOTTOM_FRONT_LEFT) X(BOTTOM_FRONT_CENTER) X(BOTTOM_FRONT_RIGHT) X(LOW_FREQUENCY_2) X(FRONT_WIDE_LEFT) X(FRONT_WIDE_RIGHT)

// mapping from AAUDIO_CHANNEL_* masks to cmus channel_position_t lists
//
// cat "$(find ${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk} -wholename '*/aaudio/AAudio.h' | sort -n | tail -n1)" |
// grep AAUDIO_CHANNEL | tr -d ' \n' | tr '|,' ',\n' | grep -Fve '<<' -e '-1' |
// cut -d '_' -f3- | xargs printf '#define A2C__%s\n' | tr '=' '\t' | sed -E 's/AAUDIO_CHANNEL_([A-Z0-9_]+)/A2C__\1/g' | 
// column -s $'\t' -t | tee /dev/stderr | cut -d ' ' -f2 | cut -d '_' -f3- |
// xargs printf ' X(%s)' | xargs -0 printf '#define A2C_LAYOUTS%s\n'
#define A2C__MONO           A2C__FRONT_LEFT
#define A2C__STEREO         A2C__FRONT_LEFT,A2C__FRONT_RIGHT
#define A2C__2POINT1        A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__LOW_FREQUENCY
#define A2C__TRI            A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__FRONT_CENTER
#define A2C__TRI_BACK       A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__BACK_CENTER
#define A2C__3POINT1        A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__FRONT_CENTER,A2C__LOW_FREQUENCY
#define A2C__2POINT0POINT2  A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__TOP_SIDE_LEFT,A2C__TOP_SIDE_RIGHT
#define A2C__2POINT1POINT2  A2C__2POINT0POINT2,A2C__LOW_FREQUENCY
#define A2C__3POINT0POINT2  A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__FRONT_CENTER,A2C__TOP_SIDE_LEFT,A2C__TOP_SIDE_RIGHT
#define A2C__3POINT1POINT2  A2C__3POINT0POINT2,A2C__LOW_FREQUENCY
#define A2C__QUAD           A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__BACK_LEFT,A2C__BACK_RIGHT
#define A2C__QUAD_SIDE      A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__SIDE_LEFT,A2C__SIDE_RIGHT
#define A2C__SURROUND       A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__FRONT_CENTER,A2C__BACK_CENTER
#define A2C__PENTA          A2C__QUAD,A2C__FRONT_CENTER
#define A2C__5POINT1        A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__FRONT_CENTER,A2C__LOW_FREQUENCY,A2C__BACK_LEFT,A2C__BACK_RIGHT
#define A2C__5POINT1_SIDE   A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__FRONT_CENTER,A2C__LOW_FREQUENCY,A2C__SIDE_LEFT,A2C__SIDE_RIGHT
#define A2C__6POINT1        A2C__FRONT_LEFT,A2C__FRONT_RIGHT,A2C__FRONT_CENTER,A2C__LOW_FREQUENCY,A2C__BACK_LEFT,A2C__BACK_RIGHT,A2C__BACK_CENTER
#define A2C__7POINT1        A2C__5POINT1,A2C__SIDE_LEFT,A2C__SIDE_RIGHT
#define A2C__5POINT1POINT2  A2C__5POINT1,A2C__TOP_SIDE_LEFT,A2C__TOP_SIDE_RIGHT
#define A2C__5POINT1POINT4  A2C__5POINT1,A2C__TOP_FRONT_LEFT,A2C__TOP_FRONT_RIGHT,A2C__TOP_BACK_LEFT,A2C__TOP_BACK_RIGHT
#define A2C__7POINT1POINT2  A2C__7POINT1,A2C__TOP_SIDE_LEFT,A2C__TOP_SIDE_RIGHT
#define A2C__7POINT1POINT4  A2C__7POINT1,A2C__TOP_FRONT_LEFT,A2C__TOP_FRONT_RIGHT,A2C__TOP_BACK_LEFT,A2C__TOP_BACK_RIGHT
#define A2C__9POINT1POINT4  A2C__7POINT1POINT4,A2C__FRONT_WIDE_LEFT,A2C__FRONT_WIDE_RIGHT
#define A2C__9POINT1POINT6  A2C__9POINT1POINT4,A2C__TOP_SIDE_LEFT,A2C__TOP_SIDE_RIGHT
#define A2C__FRONT_BACK     A2C__FRONT_CENTER,A2C__BACK_CENTER
#define A2C_LAYOUTS X(MONO) X(STEREO) X(2POINT1) X(TRI) X(TRI_BACK) X(3POINT1) X(2POINT0POINT2) X(2POINT1POINT2) X(3POINT0POINT2) X(3POINT1POINT2) X(QUAD) X(QUAD_SIDE) X(SURROUND) X(PENTA) X(5POINT1) X(5POINT1_SIDE) X(6POINT1) X(7POINT1) X(5POINT1POINT2) X(5POINT1POINT4) X(7POINT1POINT2) X(7POINT1POINT4) X(9POINT1POINT4) X(9POINT1POINT6) X(FRONT_BACK)

// convert a cmus channel map to an equivalent aaudio channel mask (the returned
// value will either be invalid or have the same number of bits set as the
// number of channels)
static aaudio_channel_mask_t cmus_channel_map_to_aaudio_mask(int channels, const channel_position_t *channel_map) {
	aaudio_channel_mask_t mask = 0;

	// we can only convert a valid channel map
	if (channels >= CHANNELS_MAX || !channel_map || !channel_map_valid(channel_map)) {
		return AAUDIO_CHANNEL_INVALID;
	}

	// special case for mono since cmus defines a separate channel position
	// for it
	if (channels == 1 && channel_map[0] == CHANNEL_POSITION_MONO) {
		return AAUDIO_CHANNEL_FRONT_LEFT;
	}

	// fill the mask, returning invalid if it has duplicates or no mapping
	for (int i = 0; i < channels; i++) {
		#define X(aaudio) \
		if (A2C__##aaudio != CHANNEL_POSITION_INVALID && channel_map[i] == A2C__##aaudio) { \
			if (mask & AAUDIO_CHANNEL_##aaudio) \
				return AAUDIO_CHANNEL_INVALID; \
			mask |= AAUDIO_CHANNEL_##aaudio; \
		}
		A2C_CHANNELS
		#undef X
	}

	return mask;
}

// get the expected cmus channel order for the specified aaudio channel mask
static bool channel_map_init_aaudio(aaudio_channel_mask_t mask, channel_position_t *map) {
	switch (mask) {
	#define X(aaudio) \
	case AAUDIO_CHANNEL_##aaudio: channel_map_copy(map, (channel_position_t[CHANNELS_MAX]){ A2C__##aaudio }); return true;
	A2C_LAYOUTS
	#undef X
	}
	return false;
}

// get the name of a known aaudio channel mask
static const char *aaudio_channel_to_string(aaudio_channel_mask_t mask) {
	switch (mask) {
		#define X(aaudio) \
		case AAUDIO_CHANNEL_##aaudio: return #aaudio;
		A2C_CHANNELS
		#undef X
	}
	switch (mask) {
		#define X(aaudio) \
		case AAUDIO_CHANNEL_##aaudio: return #aaudio;
		A2C_LAYOUTS
		#undef X
	}
	return NULL;
}

// fill a map of output frame byte indexes to input frame byte indexes (or
// -1 to zero) to remap channels (map must be sf_get_frame_size elements)
static void make_channel_remap(ssize_t *map, const channel_position_t *channel_map_out, const channel_position_t *channel_map_in, sample_format_t sf) {
	int byte, channel_out, channel_in;

	if (!channel_map_out || !channel_map_valid(channel_map_out) || !channel_map_in || !channel_map_valid(channel_map_in)) {
		for (byte = 0; byte < sf_get_frame_size(sf); byte++) {
			map[byte] = byte;
		}
	} else {
		for (byte = 0; byte < sf_get_frame_size(sf); byte++) {
			map[byte] = -1;
		}
		for (channel_out = 0; channel_out < sf_get_channels(sf); channel_out++) {
			if (channel_map_out[channel_out] != CHANNEL_POSITION_INVALID) {
				for (channel_in = 0; channel_in < sf_get_channels(sf); channel_in++) {
					if (channel_map_in[channel_in] == channel_map_out[channel_out]) {
						for (byte = 0; byte < sf_get_sample_size(sf); byte++) {
							map[sf_get_sample_size(sf) * channel_out + byte] = (ssize_t) sf_get_sample_size(sf) * channel_in + byte;
						}
						break;
					}
				}
			}
		}
	}

	d_print("remap bytes");
	for (byte = 0; byte < sf_get_frame_size(sf); byte++) {
		d_print(" %03zd", map[byte]);
	}
	d_print("\n");
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

// maps an res to a suitable error code
static int OP_ERROR_AAUDIO(aaudio_result_t res) {
	// see https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/private/bionic_errdefs.h
	switch (res) {
	case AAUDIO_OK:                                           return 0;
	case AAUDIO_ERROR_INTERNAL:                               return OP_ERROR_INTERNAL;
	case AAUDIO_ERROR_NO_SERVICE:                             return OP_ERROR_NOT_SUPPORTED;
	case AAUDIO_ERROR_INVALID_FORMAT:                         return OP_ERROR_SAMPLE_FORMAT;
	case AAUDIO_ERROR_INVALID_RATE:                           return OP_ERROR_SAMPLE_FORMAT;
	case AAUDIO_ERROR_UNAVAILABLE:      errno = ECONNREFUSED; return OP_ERROR_ERRNO; // Connection refused
	case AAUDIO_ERROR_DISCONNECTED:     errno = ECONNRESET;   return OP_ERROR_ERRNO; // Connection reset by peer
	case AAUDIO_ERROR_TIMEOUT:          errno = ETIMEDOUT;    return OP_ERROR_ERRNO; // Connection timed out
	case AAUDIO_ERROR_WOULD_BLOCK:      errno = ENOBUFS;      return OP_ERROR_ERRNO; // No buffer space available
	case AAUDIO_ERROR_UNIMPLEMENTED:    errno = ENOSYS;       return OP_ERROR_ERRNO; // Function not implemented
	case AAUDIO_ERROR_NO_FREE_HANDLES:  errno = EMFILE;       return OP_ERROR_ERRNO; // Too many open files
	case AAUDIO_ERROR_NO_MEMORY:        errno = ENOMEM;       return OP_ERROR_ERRNO; // Out of memory
	case AAUDIO_ERROR_NULL:             errno = EFAULT;       return OP_ERROR_ERRNO; // Bad address
	case AAUDIO_ERROR_OUT_OF_RANGE:     errno = EINVAL;       return OP_ERROR_ERRNO; // Invalid argument
	case AAUDIO_ERROR_INVALID_HANDLE:   errno = EBADF;        return OP_ERROR_ERRNO; // Bad file descriptor
	case AAUDIO_ERROR_INVALID_STATE:    errno = EBADFD;       return OP_ERROR_ERRNO; // File descriptor in bad state
	case AAUDIO_ERROR_ILLEGAL_ARGUMENT: errno = EINVAL;       return OP_ERROR_ERRNO; // Invalid argument
	default:                                                  return OP_ERROR_INTERNAL;
	}
}

static AAudioStream *strm;
static int32_t strm_frame_size;
static int32_t strm_last_device;
static aaudio_result_t strm_error;
static bool strm_remap;
static ssize_t *strm_remap_map;
static char *strm_remap_buf;
static size_t strm_remap_buf_sz;
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
		op_aaudio_opt_sharing_mode = AAUDIO_SHARING_MODE_SHARED;
		return OP_ERROR_SUCCESS;
	}
	if (!strcmp(val, "exclusive")) {
		op_aaudio_opt_sharing_mode = AAUDIO_SHARING_MODE_EXCLUSIVE;
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
	strm_error = error;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_open(sample_format_t sf, const channel_position_t *channel_map)
{
	aaudio_result_t rc;
	aaudio_channel_mask_t mask;
	channel_position_t mask_expected_channels[CHANNELS_MAX];
	AAudioStreamBuilder *bld;

	// create the stream builder
	rc = AAudio_createStreamBuilder(&bld);
	if (rc) {
		d_print("create stream builder failed (%d - %s)\n", rc, AAudio_convertResultToText(rc));
		return -OP_ERROR_AAUDIO(rc);
	}

	// apply the options
	AAudioStreamBuilder_setSharingMode(bld, op_aaudio_opt_sharing_mode);
	AAudioStreamBuilder_setPerformanceMode(bld, op_aaudio_opt_performance_mode);
	if (API_AT_LEAST(28)) AAudioStreamBuilder_setContentType(bld, AAUDIO_CONTENT_TYPE_MUSIC);
	if (API_AT_LEAST(28)) AAudioStreamBuilder_setUsage(bld, AAUDIO_USAGE_MEDIA);
	if (API_AT_LEAST(29)) AAudioStreamBuilder_setAllowedCapturePolicy(bld, op_aaudio_opt_allowed_capture);
	if (API_AT_LEAST(31)) AAudioStreamBuilder_setAttributionTag(bld, "cmus");
	if (API_AT_LEAST(32)) AAudioStreamBuilder_setSpatializationBehavior(bld, op_aaudio_opt_disable_spatialization ? AAUDIO_SPATIALIZATION_BEHAVIOR_NEVER : AAUDIO_SPATIALIZATION_BEHAVIOR_AUTO);

	// set the channel count
	//
	// note: if no channel mask is set, aaudio will treat the first two
	// channels as left/right (duplicating mono to stereo if required), and
	// leave the rest up to the device, dropping them if the device doesn't
	// have that many channels
	AAudioStreamBuilder_setChannelCount(bld, sf_get_channels(sf));

	// if we have a channel map, apply it on a best-effort basis
	strm_remap = false;
	if (channel_map && channel_map_valid(channel_map)) {
		if (API_AT_LEAST(32)) {
			mask = cmus_channel_map_to_aaudio_mask(sf_get_channels(sf), channel_map);
			d_print("channel map aaudio mask %d (%s)\n", mask, aaudio_channel_to_string(mask) ? aaudio_channel_to_string(mask) : "(null)");
			if (mask == AAUDIO_CHANNEL_INVALID) {
				d_print("not applying channel map since it contains duplicates or not all channels have an aaudio equivalent\n");
			} else {
				if (!channel_map_init_aaudio(mask, mask_expected_channels)) {
					d_print("not applying channel map since there isn't a valid cmus channel mapping for the aaudio mask\n");
				} else {
					if (!channel_map_equal(channel_map, mask_expected_channels, sf_get_channels(sf))) {
						d_print("will remap channels since the input channel_map order doesn't match the order expected by aaudio\n");
						strm_remap = true;
						strm_remap_map = xnew(ssize_t, (size_t) sf_get_frame_size(sf));
						make_channel_remap(strm_remap_map, mask_expected_channels, channel_map, sf);
					}
					d_print("applying channel mask\n");
					AAudioStreamBuilder_setChannelMask(bld, mask);
				}
			}
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
	strm_error = 0;
	rc = AAudioStreamBuilder_openStream(bld, &strm);
	if (rc) {
		d_print("open stream failed (%d - %s)\n", rc, AAudio_convertResultToText(rc));
		AAudioStreamBuilder_delete(bld);
		return -OP_ERROR_AAUDIO(rc);
	}
	d_print("optimal buffer frames = %d\n", AAudioStream_getFramesPerBurst(strm));
	d_print("buffer capacity frames = %d\n", AAudioStream_getBufferCapacityInFrames(strm));

	if (strm_remap) {
		strm_remap_buf_sz = (size_t) AAudioStream_getBufferCapacityInFrames(strm) * (size_t) sf_get_frame_size(sf);
		d_print("allocating %zu bytes for remap buffer\n", strm_remap_buf_sz);
		strm_remap_buf = xmalloc(strm_remap_buf_sz);
	}

	// cleanup the stream builder
	rc = AAudioStreamBuilder_delete(bld);
	if (rc) {
		d_print("delete stream builder failed (%d - %s)\n", rc, AAudio_convertResultToText(rc));
		AAudioStream_close(strm);
		return -OP_ERROR_AAUDIO(rc);
	}

	// done (we don't actually start the stream until the first write)
	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_close(void)
{
	if (strm_remap_map) {
		free(strm_remap_map);
		strm_remap_map = NULL;
	}
	if (strm_remap_buf) {
		free(strm_remap_buf);
		strm_remap_buf = NULL;
	}
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
				return -OP_ERROR_AAUDIO(rc);
			}
			// the stream will be started again on the first write
		}

		// flush the stream
		rc = aaudio_request_state_change(strm, AAudioStream_requestFlush, AAUDIO_STREAM_STATE_FLUSHED, 0);
		if (rc) {
			return -OP_ERROR_AAUDIO(rc);
		}
	}

	return OP_ERROR_SUCCESS;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_write(const char *buf, int count)
{
	int i, j;
	int32_t device;
	aaudio_result_t rc;
	aaudio_stream_state_t state;

	// if the stream errored, return an error so cmus restarts the output
	// plugin
	//
	// note that this isn't strictly required since AAudioStream_write will
	// return an error on stream disconnection, which will cause cmus to
	// reopen the output plugin
	//
	// https://github.com/google/oboe/wiki/TechNote_Disconnect
	if (strm_error) {
		return -OP_ERROR_AAUDIO(strm_error);
	}

	// note: this is cheap; it's just a field getter internally
	device = AAudioStream_getDeviceId(strm);
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
	state = AAudioStream_getState(strm);
	if (state == AAUDIO_STREAM_STATE_CLOSING || state == AAUDIO_STREAM_STATE_CLOSED) {
		return -OP_ERROR_NOT_OPEN;
	}
	if (state != AAUDIO_STREAM_STATE_STARTING && state != AAUDIO_STREAM_STATE_STARTED) {
		rc = aaudio_request_state_change(strm, AAudioStream_requestStart, AAUDIO_STREAM_STATE_STARTED, AAUDIO_STREAM_STATE_STARTING);
		if (rc) {
			return -OP_ERROR_AAUDIO(rc);
		}
	}

	// remap if necessary
	if (strm_remap) {
		if (count >= strm_remap_buf_sz) {
			// this should never happen since op_aaudio_buffer_space
			// (i.e., AAudioStream_getFramesPerBurst or
			// AAudioStream_getBufferSizeInFrames) should always be
			// less than AAudioStream_getBufferCapacityInFrames
			BUG("cannot remap since trying to write %d >= %zu bytes (n > buffer capacity)\n", count, strm_remap_buf_sz);
			return -OP_ERROR_INTERNAL;
		}
		for (i = 0; i < count; i += strm_frame_size) {
			for (j = 0; j < strm_frame_size; j++) {
				if (strm_remap_map[j] != -1) {
					strm_remap_buf[i+j] = buf[i+strm_remap_map[j]];
				} else {
					strm_remap_buf[i+j] = 0;
				}
			}
		}
		buf = strm_remap_buf;
	}

	// synchronously write the samples to the buffer
	rc = AAudioStream_write(strm, buf, count / strm_frame_size, INT64_MAX);
	if (rc < 0) {
		d_print("write %d = error %d - %s [device=%d] [state=%d]\n", count / strm_frame_size, rc, AAudio_convertResultToText(rc), device, state);
		return -OP_ERROR_AAUDIO(rc);
	}
	d_print("write %d = %d (* %d bytes) [device=%d] [state=%d]\n", count / strm_frame_size, rc, strm_frame_size, device, state);

	// return the number of bytes we write
	return rc * strm_frame_size;
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_pause(void)
{
	// request stream pause, wait until it completes
	return -OP_ERROR_AAUDIO(aaudio_request_state_change(strm, AAudioStream_requestPause, AAUDIO_STREAM_STATE_PAUSED, 0));
}

REQUIRES_API(AAUDIO_MINIMUM_API)
static int op_aaudio_unpause(void)
{
	// request stream start, wait until it starts to start (i.e., will start
	// consuming frames written to it)
	return -OP_ERROR_AAUDIO(aaudio_request_state_change(strm, AAudioStream_requestStart, AAUDIO_STREAM_STATE_STARTED, AAUDIO_STREAM_STATE_STARTING));
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
