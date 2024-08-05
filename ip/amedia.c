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

#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// for development, can cross-compile with $ANDROID_HOME/ndk/26.0.10792818/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang -shared -o amedia.so -fPIC -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ -Werror=unguarded-availability -Wall -std=gnu11 ip/amedia.c -lmediandk
// also see https://github.com/android/ndk-samples/blob/master/native-codec/app/src/main/cpp/native-codec-jni.cpp
// also see https://github.com/google/oboe/blob/main/samples/RhythmGame/src/main/cpp/audio/NDKExtractor.cpp

#ifndef __ANDROID__
// make ide autocomplete work without using a full ndk toolchain
#define __INTRODUCED_IN(api_level)
#endif

// https://developer.android.com/ndk/guides/using-newer-apis
#define REQUIRES_API(x) __attribute__((__availability__(android,introduced=x)))
#define API_AT_LEAST(x) __builtin_available(android x, *)
#define AAUDIO_MINIMUM_API 26

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include "../ip.h"
#include "../xmalloc.h"
#include "../debug.h"
#include "../comment.h"

static bool starts_with(const char *str, const char *pfx) {
    size_t len = strlen(pfx);
    return strlen(str) >= len && !strncmp(str, pfx, len);
}

struct amedia_private {
	AMediaExtractor *extractor;
	AMediaFormat *format;
	char *mimetype;
	AMediaCodec *codec;
	int codec_init_result;
	bool codec_eos_input;
	bool codec_eos_output;
	uint8_t *codec_output_buffer;
	size_t codec_output_buffer_sz;
	ssize_t codec_output_buffer_idx;
	size_t codec_output_buffer_off;
};

static int amedia_close(struct input_plugin_data *ip_data);

static int amedia_open(struct input_plugin_data *ip_data)
{
	struct amedia_private *priv;
	media_status_t rc;

	ip_data->private = priv = xnew(struct amedia_private, 1);
	priv->extractor = AMediaExtractor_new();
	priv->format = NULL;
	priv->mimetype = NULL;
	priv->codec = NULL;
	priv->codec_init_result = 0;
	priv->codec_eos_input = false;
	priv->codec_eos_output = false;
	priv->codec_output_buffer = NULL;
	priv->codec_output_buffer_sz = 0;
	priv->codec_output_buffer_idx = 0;
	priv->codec_output_buffer_off = 0;

	// setDataSourceFd needs a real fd (https://issuetracker.google.com/issues/203690996)
	if (ip_data->remote) {
		rc = AMediaExtractor_setDataSource(priv->extractor, ip_data->filename);
	} else {
		struct stat statbuf;
		if (fstat(ip_data->fd, &statbuf)) {
			d_print("failed to stat media fd: %d\n", errno);
			amedia_close(ip_data);
			return -IP_ERROR_ERRNO;
		}
		rc = AMediaExtractor_setDataSourceFd(priv->extractor, ip_data->fd, 0, statbuf.st_size);
	}
	if (rc != AMEDIA_OK) {
		d_print("failed to set extractor data source: %d\n", rc);
			amedia_close(ip_data);
		return -IP_ERROR_INTERNAL;
	}

	size_t tracks = AMediaExtractor_getTrackCount(priv->extractor);
	for (size_t idx = 0; idx < tracks; idx++) {
		AMediaFormat *format = AMediaExtractor_getTrackFormat(priv->extractor, idx);

		const char *mimetype;
		if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mimetype)) {
			if (starts_with(mimetype, "audio/")) {
				priv->format = format;
				priv->mimetype = xstrdup(mimetype);

				rc = AMediaExtractor_selectTrack(priv->extractor, idx);
				if (rc != AMEDIA_OK) {
					d_print("failed to select track %zu of type %s: %d\n", idx, mimetype, rc);
					amedia_close(ip_data);
					return -IP_ERROR_FILE_FORMAT;
				}
				d_print("selected track %zu of type %s\n", idx, mimetype);

				break;
			}
		}

		AMediaFormat_delete(format);
	}
	if (!priv->format) {
		d_print("could not find audio track\n");
		amedia_close(ip_data);
		return -IP_ERROR_FILE_FORMAT;
	}

	int32_t rate;
	if (!AMediaFormat_getInt32(priv->format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate)) {
		d_print("failed to get sample rate\n");
		amedia_close(ip_data);
		return -IP_ERROR_SAMPLE_FORMAT;
	}

	int32_t channels;
	if (!AMediaFormat_getInt32(priv->format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels)) {
		d_print("failed to get channel count\n");
		amedia_close(ip_data);
		return -IP_ERROR_SAMPLE_FORMAT;
	}

	ip_data->sf = sf_rate(rate)
		| sf_channels(channels)
		| sf_bits(16)
		| sf_signed(1)
		| sf_host_endian(); // TODO

	return IP_ERROR_SUCCESS;
}

static int amedia_close(struct input_plugin_data *ip_data)
{
	struct amedia_private *priv = ip_data->private;

	if (priv) {
		if (priv->codec_output_buffer) {
			AMediaCodec_releaseOutputBuffer(priv->codec, priv->codec_output_buffer_idx, false);
			priv->codec_output_buffer = NULL;
			priv->codec_output_buffer_idx = 0;
			priv->codec_output_buffer_sz = 0;
			priv->codec_output_buffer_off = 0;
		}
		if (priv->codec) {
			AMediaCodec_delete(priv->codec);
			priv->codec = NULL;
		}
		if (priv->mimetype) {
			free(priv->mimetype);
			priv->mimetype = NULL;
		}
		if (priv->format) {
			AMediaFormat_delete(priv->format);
			priv->format = NULL;
		}
		if (priv->extractor) {
			AMediaExtractor_delete(priv->extractor);
			priv->extractor = NULL;
		}
		free(priv);
		ip_data->private = NULL;
	}

	return IP_ERROR_SUCCESS;
}

static int amedia_init_codec(struct input_plugin_data *ip_data) {
	struct amedia_private *priv = ip_data->private;
	media_status_t rc;

	if (!priv->codec) {
		priv->codec = AMediaCodec_createDecoderByType(priv->mimetype);
		if (!priv->codec) {
			d_print("failed to create codec %s\n", priv->mimetype);
			priv->codec_init_result = -IP_ERROR_INTERNAL;
			goto ret;
		}

		rc = AMediaCodec_configure(priv->codec, priv->format, NULL, NULL, 0);
		if (rc != AMEDIA_OK) {
			d_print("failed to configure codec: %d\n", rc);
			priv->codec_init_result = -IP_ERROR_FILE_FORMAT;
			goto ret;
		}

		rc = AMediaCodec_start(priv->codec);
		if (rc != AMEDIA_OK) {
			d_print("failed to start codec: %d\n", rc);
			priv->codec_init_result = -IP_ERROR_FILE_FORMAT;
			goto ret;
		}

		priv->codec_init_result = IP_ERROR_SUCCESS;
	}

ret:
	return priv->codec_init_result;
}

static int amedia_read(struct input_plugin_data *ip_data, char *buffer, int count)
{
	struct amedia_private *priv = ip_data->private;
	int actual = 0;

	// initialize the decoder if we haven't done so already
	int init_res = amedia_init_codec(ip_data);
	if (init_res) {
		return init_res;
	}

	// while we have more data in the codec stream
	while (!priv->codec_eos_input || !priv->codec_eos_output) {
		//printf("read %d %d\r\n", priv->codec_eos_input, priv->codec_eos_output);
		//printf("read fill %d %d\r\n", actual, count);

		// fill from our current buffer
		if (actual < count && priv->codec_output_buffer && priv->codec_output_buffer_off < priv->codec_output_buffer_sz) {
			size_t remaining = priv->codec_output_buffer_sz - priv->codec_output_buffer_off;
			size_t n = count - actual;
			if (n > remaining) {
				n = remaining;
			}
			//printf("buf %zd off %zu n %zu sz %zd\r\n", priv->codec_output_buffer_idx, priv->codec_output_buffer_off, n, priv->codec_output_buffer_sz);
			memcpy(buffer + actual, priv->codec_output_buffer + priv->codec_output_buffer_off, n);
			priv->codec_output_buffer_off += n;
			actual += n;
		}

		// return if we have enough
		if (actual == count) {
			break;
		}

		// feed the codec another buffer of input
		if (!priv->codec_eos_input) {
			ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(priv->codec, 2000);
			if (buf_idx >= 0) {
				size_t buf_sz;
				uint8_t *buf = AMediaCodec_getInputBuffer(priv->codec, buf_idx, &buf_sz);

				ssize_t sample_sz = AMediaExtractor_readSampleData(priv->extractor, buf, buf_sz);
				if (sample_sz < 0) {
					sample_sz = 0;
					priv->codec_eos_input = true;
					d_print("codec input eos\n");
				}

				int64_t presentation_time_us = AMediaExtractor_getSampleTime(priv->extractor);
				AMediaCodec_queueInputBuffer(priv->codec, buf_idx, 0, sample_sz, presentation_time_us, priv->codec_eos_input ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
				AMediaExtractor_advance(priv->extractor);
				//printf("pres %ld\r\n", presentation_time_us);
			} else {
				switch (buf_idx) {
				case AMEDIACODEC_INFO_TRY_AGAIN_LATER:
					break; // ignore
				default:
					d_print("codec input error %zd\n", buf_idx);
					return -IP_ERROR_INTERNAL;
				}
			}
		}

		// get the next codec output buffer
		if (!priv->codec_eos_output) {
			if (priv->codec_output_buffer) {
				AMediaCodec_releaseOutputBuffer(priv->codec, priv->codec_output_buffer_idx, false);
				priv->codec_output_buffer = NULL;
				priv->codec_output_buffer_idx = 0;
				priv->codec_output_buffer_sz = 0;
				priv->codec_output_buffer_off = 0;
			}

			AMediaCodecBufferInfo info;
			ssize_t buf_idx = AMediaCodec_dequeueOutputBuffer(priv->codec, &info, 0);
			if (buf_idx >= 0) {
				if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
					priv->codec_eos_output = true;
					d_print("codec output eos\n");
				}

				size_t buf_sz;
				uint8_t *buf = AMediaCodec_getOutputBuffer(priv->codec, buf_idx, &buf_sz);

				priv->codec_output_buffer = buf;
				priv->codec_output_buffer_idx = buf_idx;
				priv->codec_output_buffer_sz = info.size;
				priv->codec_output_buffer_off = 0;
			} else {
				switch (buf_idx) {
				case AMEDIACODEC_INFO_TRY_AGAIN_LATER:
					break; // ignore
				case AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED:
					d_print("codec output buffers changed\n");
					break;
				case AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED: {
					AMediaFormat *format = AMediaCodec_getOutputFormat(priv->codec);
					d_print("codec output format changed to %s\n", AMediaFormat_toString(format));
					AMediaFormat_delete(format);
					break;
				}
				default:
					d_print("output input error %zd\n", buf_idx);
					return -IP_ERROR_INTERNAL;
				}
			}
		}
	}

	// TODO: why is cpu usage so high, especially compared to using libraries directly?

	// if actual is 0 at this point, we're at end-of-stream
	return actual;
}

// https://issuetracker.google.com/issues/37036678
// seek accuracy may vary across codecs

static int amedia_seek(struct input_plugin_data *ip_data, double offset)
{
	struct amedia_private *priv = ip_data->private;
	media_status_t rc;

	// initialize the decoder if we haven't done so already
	int init_res = amedia_init_codec(ip_data);
	if (init_res) {
		return init_res;
	}

	if (priv->codec_output_buffer) {
		AMediaCodec_releaseOutputBuffer(priv->codec, priv->codec_output_buffer_idx, false);
		priv->codec_output_buffer = NULL;
		priv->codec_output_buffer_idx = 0;
		priv->codec_output_buffer_sz = 0;
		priv->codec_output_buffer_off = 0;
	}

	rc = AMediaCodec_flush(priv->codec);
	if (rc != AMEDIA_OK) {
		if (rc == AMEDIA_ERROR_UNSUPPORTED) {
			return -IP_ERROR_FUNCTION_NOT_SUPPORTED;
		}
		d_print("codec buffer flush failed: %d\n", rc);
		return -IP_ERROR_INTERNAL;
	}

	// TODO: maybe make a seek index like in nomad.c for accurate seeks (since inaccurate seeks will make a mess out of the current position since cmus expects offset to be exact)?
	rc = AMediaExtractor_seekTo(priv->extractor, (int64_t)(offset * 1000 * 1000), AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
	if (rc != AMEDIA_OK) {
		if (rc == AMEDIA_ERROR_UNSUPPORTED) {
			return -IP_ERROR_FUNCTION_NOT_SUPPORTED;
		}
		d_print("codec seek failed: %d\n", rc);
		return -IP_ERROR_INTERNAL;
	}

	// TODO: this is incredibly slow, and it isn't helped by the small output buffer sizes

	return 0;
}

static int amedia_read_comments(struct input_plugin_data *ip_data, struct keyval **comments)
{
	struct amedia_private *priv = ip_data->private;

	GROWING_KEYVALS(ckv);

	const char *out;
	// TODO: add key-value pairs from mediaformat according to the interesting array in ip

	keyvals_terminate(&ckv);
	*comments = ckv.keyvals;
	return 0;
}

static int amedia_duration(struct input_plugin_data *ip_data)
{
	struct amedia_private *priv = ip_data->private;
	int64_t duration;
	if (!AMediaFormat_getInt64(priv->format, AMEDIAFORMAT_KEY_DURATION, &duration)) {
		return -1; // unknown duration
	}
	return (int)(duration / 1000L / 1000L);
}

static long amedia_bitrate(struct input_plugin_data *ip_data)
{
	struct amedia_private *priv = ip_data->private;

	int32_t bitrate;
	if (!AMediaFormat_getInt32(priv->format, AMEDIAFORMAT_KEY_BIT_RATE, &bitrate)) {
		return -IP_ERROR_FUNCTION_NOT_SUPPORTED;
	}
	return bitrate;
}

static long amedia_bitrate_current(struct input_plugin_data *ip_data)
{
	return -IP_ERROR_FUNCTION_NOT_SUPPORTED;
}

static char *amedia_codec(struct input_plugin_data *ip_data)
{
	struct amedia_private *priv = ip_data->private;
	return xstrdup(priv->mimetype ? priv->mimetype + strlen("audio/") : "amedia");
}

static char *amedia_codec_profile(struct input_plugin_data *ip_data)
{
	struct amedia_private *priv = ip_data->private;

	const char *profile;
	if (AMediaFormat_getString(priv->format, AMEDIAFORMAT_KEY_AAC_PROFILE, &profile)) {
		return xstrdup(profile);
	}

	return NULL;
}

const struct input_plugin_ops ip_ops = {
	.open = amedia_open,
	.close = amedia_close,
	.read = amedia_read,
	.seek = amedia_seek,
	.read_comments = amedia_read_comments,
	.duration = amedia_duration,
	.bitrate = amedia_bitrate,
	.bitrate_current = amedia_bitrate_current,
	.codec = amedia_codec,
	.codec_profile = amedia_codec_profile
};

// containers and codecs on recent android devices
// - https://developer.android.com/media/platform/supported-formats
// - adb shell dumpsys media.extractor | grep supports:
// - adb shell grep "'<MediaCodec name'" /apex/com.android.media.swcodec/etc/media_codecs.xml
// - https://source.android.com/docs/core/media/updatable-media

const char *const ip_extensions[] = {
	"*",
	// TODO: populate in library constructor based on android version?
	NULL,
};

const char *const ip_mime_types[] = {
	NULL,
};

const struct input_plugin_opt ip_options[] = {
	{ NULL },
};

// higher than ffmpeg, lower than others (user can manually override it if they
// want), mostly because it makes sense to use libraries from termux where
// possible (they are usually the same libraries as what amedia uses
// internally), and because seek accuracy with amedia varies
const int ip_priority = 31;
const unsigned ip_abi_version = IP_ABI_VERSION;
