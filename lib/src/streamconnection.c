/*
 * This file is part of Chiaki.
 *
 * Chiaki is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chiaki is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Chiaki.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <chiaki/streamconnection.h>
#include <chiaki/session.h>
#include <chiaki/launchspec.h>
#include <chiaki/base64.h>
#include <chiaki/audio.h>
#include <chiaki/video.h>

#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <takion.pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <pb.h>

#include "utils.h"
#include "pb_utils.h"


#define STREAM_CONNECTION_PORT 9296

#define EXPECT_TIMEOUT_MS 5000

#define HEARTBEAT_INTERVAL_MS 1000


typedef enum {
	STATE_IDLE,
	STATE_EXPECT_BANG,
	STATE_EXPECT_STREAMINFO
} StreamConnectionState;


static void stream_connection_takion_data(ChiakiTakionMessageDataType data_type, uint8_t *buf, size_t buf_size, void *user);
static ChiakiErrorCode stream_connection_send_big(ChiakiStreamConnection *stream_connection);
static ChiakiErrorCode stream_connection_send_disconnect(ChiakiStreamConnection *stream_connection);
static void stream_connection_takion_data_idle(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_expect_bang(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_expect_streaminfo(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static ChiakiErrorCode stream_connection_send_streaminfo_ack(ChiakiStreamConnection *stream_connection);
static void stream_connection_takion_av(ChiakiTakionAVPacket *packet, void *user);
static ChiakiErrorCode stream_connection_send_heartbeat(ChiakiStreamConnection *stream_connection);


CHIAKI_EXPORT ChiakiErrorCode chiaki_stream_connection_init(ChiakiStreamConnection *stream_connection, ChiakiSession *session)
{
	stream_connection->session = session;
	stream_connection->log = &session->log;

	stream_connection->ecdh_secret = NULL;
	stream_connection->gkcrypt_remote = NULL;
	stream_connection->gkcrypt_local = NULL;

	ChiakiErrorCode err = chiaki_mutex_init(&stream_connection->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;

	err = chiaki_cond_init(&stream_connection->state_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_state_mutex;

	stream_connection->state = STATE_IDLE;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	stream_connection->should_stop = false;

	return CHIAKI_ERR_SUCCESS;

error_state_mutex:
	chiaki_mutex_fini(&stream_connection->state_mutex);
error:
	return err;
}

CHIAKI_EXPORT void chiaki_stream_connection_fini(ChiakiStreamConnection *stream_connection)
{
	chiaki_gkcrypt_free(stream_connection->gkcrypt_remote);
	chiaki_gkcrypt_free(stream_connection->gkcrypt_local);

	free(stream_connection->ecdh_secret);

	chiaki_cond_fini(&stream_connection->state_cond);
	chiaki_mutex_fini(&stream_connection->state_mutex);
}


bool state_finished_cond_check(void *user)
{
	ChiakiStreamConnection *stream_connection = user;
	return stream_connection->state_finished || stream_connection->should_stop;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stream_connection_run(ChiakiStreamConnection *stream_connection)
{
	ChiakiSession *session = stream_connection->session;
	ChiakiErrorCode err;

	ChiakiTakionConnectInfo takion_info;
	takion_info.log = stream_connection->log;
	takion_info.sa_len = session->connect_info.host_addrinfo_selected->ai_addrlen;
	takion_info.sa = malloc(takion_info.sa_len);
	if(!takion_info.sa)
		return CHIAKI_ERR_MEMORY;
	memcpy(takion_info.sa, session->connect_info.host_addrinfo_selected->ai_addr, takion_info.sa_len);
	err = set_port(takion_info.sa, htons(STREAM_CONNECTION_PORT));
	assert(err == CHIAKI_ERR_SUCCESS);

	takion_info.data_cb = stream_connection_takion_data;
	takion_info.data_cb_user = stream_connection;
	takion_info.av_cb = stream_connection_takion_av;
	takion_info.av_cb_user = stream_connection;

	// TODO: make this call stoppable
	err = chiaki_takion_connect(&stream_connection->takion, &takion_info);
	free(takion_info.sa);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&session->log, "StreamConnection connect failed\n");
		return err;
	}

	err = chiaki_mutex_lock(&stream_connection->state_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);

	CHIAKI_LOGI(&session->log, "StreamConnection sending big\n");

	stream_connection->state = STATE_EXPECT_BANG;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	err = stream_connection_send_big(stream_connection);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&session->log, "StreamConnection failed to send big\n");
		goto disconnect;
	}

	err = chiaki_cond_timedwait_pred(&stream_connection->state_cond, &stream_connection->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, stream_connection);
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);

	if(!stream_connection->state_finished)
	{
		if(err == CHIAKI_ERR_TIMEOUT)
			CHIAKI_LOGE(&session->log, "StreamConnection bang receive timeout\n");

		CHIAKI_LOGE(&session->log, "StreamConnection didn't receive bang\n");
		err = CHIAKI_ERR_UNKNOWN;
		goto disconnect;
	}

	CHIAKI_LOGI(&session->log, "StreamConnection successfully received bang\n");


	stream_connection->gkcrypt_local = chiaki_gkcrypt_new(&session->log, 0 /* TODO */, 2, session->handshake_key, stream_connection->ecdh_secret);
	if(!stream_connection->gkcrypt_local)
	{
		CHIAKI_LOGE(&session->log, "StreamConnection failed to initialize GKCrypt with index 2\n");
		goto disconnect;
	}
	stream_connection->gkcrypt_remote = chiaki_gkcrypt_new(&session->log, 0 /* TODO */, 3, session->handshake_key, stream_connection->ecdh_secret);
	if(!stream_connection->gkcrypt_remote)
	{
		CHIAKI_LOGE(&session->log, "StreamConnection failed to initialize GKCrypt with index 3\n");
		goto disconnect;
	}

	// TODO: IMPORTANT!!!
	// This all needs to be synchronized correctly!!!
	// After receiving the bang, we MUST wait for the gkcrypts to be set before handling any new takion packets!
	// Otherwise we might ignore some macs.
	// Also, access to the gkcrypts must be synchronized for key_pos and everything.
	chiaki_takion_set_crypt(&stream_connection->takion, stream_connection->gkcrypt_local, stream_connection->gkcrypt_remote);

	stream_connection->state = STATE_EXPECT_STREAMINFO;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	err = chiaki_cond_timedwait_pred(&stream_connection->state_cond, &stream_connection->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, stream_connection);
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);

	if(!stream_connection->state_finished)
	{
		if(err == CHIAKI_ERR_TIMEOUT)
			CHIAKI_LOGE(&session->log, "StreamConnection streaminfo receive timeout\n");

		CHIAKI_LOGE(&session->log, "StreamConnection didn't receive streaminfo\n");
		err = CHIAKI_ERR_UNKNOWN;
		goto disconnect;
	}

	CHIAKI_LOGI(&session->log, "StreamConnection successfully received streaminfo\n");

	stream_connection->state = STATE_IDLE;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	while(true)
	{
		err = chiaki_cond_timedwait_pred(&stream_connection->state_cond, &stream_connection->state_mutex, HEARTBEAT_INTERVAL_MS, state_finished_cond_check, stream_connection);
		if(err != CHIAKI_ERR_TIMEOUT)
			break;

		err = stream_connection_send_heartbeat(stream_connection);
		if(err != CHIAKI_ERR_SUCCESS)
			CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to send heartbeat\n");
		else
			CHIAKI_LOGI(stream_connection->log, "StreamConnection sent heartbeat\n");
	}

	err = CHIAKI_ERR_SUCCESS;

disconnect:
	CHIAKI_LOGI(&session->log, "StreamConnection is disconnecting\n");
	stream_connection_send_disconnect(stream_connection);

	if(stream_connection->should_stop)
		CHIAKI_LOGI(stream_connection->log, "StreamConnection was requested to stop\n");

	chiaki_mutex_unlock(&stream_connection->state_mutex);

	chiaki_takion_close(&stream_connection->takion);
	CHIAKI_LOGI(&session->log, "StreamConnection closed takion\n");

	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stream_connection_stop(ChiakiStreamConnection *stream_connection)
{
	ChiakiErrorCode err = chiaki_mutex_lock(&stream_connection->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	stream_connection->should_stop = true;
	ChiakiErrorCode unlock_err = chiaki_mutex_unlock(&stream_connection->state_mutex);
	err = chiaki_cond_signal(&stream_connection->state_cond);
	return err == CHIAKI_ERR_SUCCESS ? unlock_err : err;
}


static void stream_connection_takion_data(ChiakiTakionMessageDataType data_type, uint8_t *buf, size_t buf_size, void *user)
{
	if(data_type != CHIAKI_TAKION_MESSAGE_DATA_TYPE_PROTOBUF)
		return;

	ChiakiStreamConnection *stream_connection = user;

	chiaki_mutex_lock(&stream_connection->state_mutex);
	switch(stream_connection->state)
	{
		case STATE_EXPECT_BANG:
			stream_connection_takion_data_expect_bang(stream_connection, buf, buf_size);
			break;
		case STATE_EXPECT_STREAMINFO:
			stream_connection_takion_data_expect_streaminfo(stream_connection, buf, buf_size);
			break;
		default: // STATE_IDLE
			stream_connection_takion_data_idle(stream_connection, buf, buf_size);
			break;
	}
	chiaki_mutex_unlock(&stream_connection->state_mutex);
}


static void stream_connection_takion_data_idle(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!r)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to decode data protobuf\n");
		chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_ERROR, buf, buf_size);
		return;
	}

	//CHIAKI_LOGD(stream_connection->log, "StreamConnection received data\n");
	//chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_DEBUG, buf, buf_size);
}

static void stream_connection_takion_data_expect_bang(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	char ecdh_pub_key[128];
	ChiakiPBDecodeBuf ecdh_pub_key_buf = { sizeof(ecdh_pub_key), 0, (uint8_t *)ecdh_pub_key };
	char ecdh_sig[32];
	ChiakiPBDecodeBuf ecdh_sig_buf = { sizeof(ecdh_sig), 0, (uint8_t *)ecdh_sig };

	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.bang_payload.ecdh_pub_key.arg = &ecdh_pub_key_buf;
	msg.bang_payload.ecdh_pub_key.funcs.decode = chiaki_pb_decode_buf;
	msg.bang_payload.ecdh_sig.arg = &ecdh_sig_buf;
	msg.bang_payload.ecdh_sig.funcs.decode = chiaki_pb_decode_buf;

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!r)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to decode data protobuf\n");
		return;
	}

	if(msg.type != tkproto_TakionMessage_PayloadType_BANG || !msg.has_bang_payload)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection expected bang payload but received something else\n");
		return;
	}

	CHIAKI_LOGD(stream_connection->log, "Got a bang\n");

	if(!msg.bang_payload.version_accepted)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection bang remote didn't accept version\n");
		goto error;
	}

	if(!msg.bang_payload.encrypted_key_accepted)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection bang remote didn't accept encrypted key\n");
		goto error;
	}

	if(!ecdh_pub_key_buf.size)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection didn't get remote ECDH pub key from bang\n");
		goto error;
	}

	if(!ecdh_sig_buf.size)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection didn't get remote ECDH sig from bang\n");
		goto error;
	}

	assert(!stream_connection->ecdh_secret);
	stream_connection->ecdh_secret = malloc(CHIAKI_ECDH_SECRET_SIZE);
	if(!stream_connection->ecdh_secret)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to alloc ECDH secret memory\n");
		goto error;
	}

	ChiakiErrorCode err = chiaki_ecdh_derive_secret(&stream_connection->session->ecdh,
			stream_connection->ecdh_secret,
			ecdh_pub_key_buf.buf, ecdh_pub_key_buf.size,
			stream_connection->session->handshake_key,
			ecdh_sig_buf.buf, ecdh_sig_buf.size);

	if(err != CHIAKI_ERR_SUCCESS)
	{
		free(stream_connection->ecdh_secret);
		stream_connection->ecdh_secret = NULL;
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to derive secret from bang\n");
		goto error;
	}

	// stream_connection->state_mutex is expected to be locked by the caller of this function
	stream_connection->state_finished = true;
	chiaki_cond_signal(&stream_connection->state_cond);
	return;
error:
	stream_connection->state_failed = true;
	chiaki_cond_signal(&stream_connection->state_cond);
}

typedef struct decode_resolutions_context_t
{
	ChiakiStreamConnection *stream_connection;
	ChiakiVideoProfile video_profiles[CHIAKI_VIDEO_PROFILES_MAX];
	size_t video_profiles_count;
} DecodeResolutionsContext;

static bool pb_decode_resolution(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
	DecodeResolutionsContext *ctx = *arg;

	tkproto_ResolutionPayload resolution = { 0 };
	ChiakiPBDecodeBufAlloc header_buf = { 0 };
	resolution.video_header.arg = &header_buf;
	resolution.video_header.funcs.decode = chiaki_pb_decode_buf_alloc;
	if(!pb_decode(stream, tkproto_ResolutionPayload_fields, &resolution))
		return false;

	if(!header_buf.buf)
	{
		CHIAKI_LOGE(&ctx->stream_connection->session->log, "Failed to decode video header\n");
		return true;
	}

	if(ctx->video_profiles_count >= CHIAKI_VIDEO_PROFILES_MAX)
	{
		CHIAKI_LOGE(&ctx->stream_connection->session->log, "Received more resolutions than the maximum\n");
		return true;
	}

	ChiakiVideoProfile *profile = &ctx->video_profiles[ctx->video_profiles_count++];
	profile->width = resolution.width;
	profile->height = resolution.height;
	profile->header_sz = header_buf.size;
	profile->header = header_buf.buf;
	return true;
}


static void stream_connection_takion_data_expect_streaminfo(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	uint8_t audio_header[CHIAKI_AUDIO_HEADER_SIZE];
	ChiakiPBDecodeBuf audio_header_buf = { sizeof(audio_header), 0, (uint8_t *)audio_header };
	msg.stream_info_payload.audio_header.arg = &audio_header_buf;
	msg.stream_info_payload.audio_header.funcs.decode = chiaki_pb_decode_buf;

	DecodeResolutionsContext decode_resolutions_context;
	decode_resolutions_context.stream_connection = stream_connection;
	memset(decode_resolutions_context.video_profiles, 0, sizeof(decode_resolutions_context.video_profiles));
	decode_resolutions_context.video_profiles_count = 0;
	msg.stream_info_payload.resolution.arg = &decode_resolutions_context;
	msg.stream_info_payload.resolution.funcs.decode = pb_decode_resolution;

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!r)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to decode data protobuf\n");
		return;
	}

	if(msg.type != tkproto_TakionMessage_PayloadType_STREAMINFO || !msg.has_stream_info_payload)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection expected streaminfo payload but received something else\n");
		return;
	}

	if(audio_header_buf.size != CHIAKI_AUDIO_HEADER_SIZE)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection received invalid audio header in streaminfo\n");
		goto error;
	}

	ChiakiAudioHeader audio_header_s;
	chiaki_audio_header_load(&audio_header_s, audio_header);
	chiaki_audio_receiver_stream_info(stream_connection->session->audio_receiver, &audio_header_s);

	chiaki_video_receiver_stream_info(stream_connection->session->video_receiver,
			decode_resolutions_context.video_profiles,
			decode_resolutions_context.video_profiles_count);

	// TODO: do some checks?

	stream_connection_send_streaminfo_ack(stream_connection);

	// stream_connection->state_mutex is expected to be locked by the caller of this function
	stream_connection->state_finished = true;
	chiaki_cond_signal(&stream_connection->state_cond);
	return;
error:
	stream_connection->state_failed = true;
	chiaki_cond_signal(&stream_connection->state_cond);
}



static bool chiaki_pb_encode_zero_encrypted_key(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
	if (!pb_encode_tag_for_field(stream, field))
		return false;
	uint8_t data[] = { 0, 0, 0, 0 };
	return pb_encode_string(stream, data, sizeof(data));
}

#define LAUNCH_SPEC_JSON_BUF_SIZE 1024

static ChiakiErrorCode stream_connection_send_big(ChiakiStreamConnection *stream_connection)
{
	ChiakiSession *session = stream_connection->session;

	ChiakiLaunchSpec launch_spec;
	launch_spec.mtu = session->mtu;
	launch_spec.rtt = session->rtt;
	launch_spec.handshake_key = session->handshake_key;

	union
	{
		char json[LAUNCH_SPEC_JSON_BUF_SIZE];
		char b64[LAUNCH_SPEC_JSON_BUF_SIZE * 2];
	} launch_spec_buf;
	ssize_t launch_spec_json_size = chiaki_launchspec_format(launch_spec_buf.json, sizeof(launch_spec_buf.json), &launch_spec);
	if(launch_spec_json_size < 0)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to format LaunchSpec json\n");
		return CHIAKI_ERR_UNKNOWN;
	}
	launch_spec_json_size += 1; // we also want the trailing 0

	CHIAKI_LOGD(stream_connection->log, "LaunchSpec: %s\n", launch_spec_buf.json);

	uint8_t launch_spec_json_enc[LAUNCH_SPEC_JSON_BUF_SIZE];
	memset(launch_spec_json_enc, 0, (size_t)launch_spec_json_size);
	ChiakiErrorCode err = chiaki_rpcrypt_encrypt(&session->rpcrypt, 0, launch_spec_json_enc, launch_spec_json_enc,
			(size_t)launch_spec_json_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to encrypt LaunchSpec\n");
		return err;
	}

	xor_bytes(launch_spec_json_enc, (uint8_t *)launch_spec_buf.json, (size_t)launch_spec_json_size);
	err = chiaki_base64_encode(launch_spec_json_enc, (size_t)launch_spec_json_size, launch_spec_buf.b64, sizeof(launch_spec_buf.b64));
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to encode LaunchSpec as base64\n");
		return err;
	}

	char ecdh_pub_key[128];
	ChiakiPBBuf ecdh_pub_key_buf = { sizeof(ecdh_pub_key), ecdh_pub_key };
	char ecdh_sig[32];
	ChiakiPBBuf ecdh_sig_buf = { sizeof(ecdh_sig), ecdh_sig };
	err = chiaki_ecdh_get_local_pub_key(&session->ecdh,
			ecdh_pub_key, &ecdh_pub_key_buf.size,
			session->handshake_key,
			ecdh_sig, &ecdh_sig_buf.size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to get ECDH key and sig\n");
		return err;
	}

	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_BIG;
	msg.has_big_payload = true;
	msg.big_payload.client_version = 9;
	msg.big_payload.session_key.arg = session->session_id;
	msg.big_payload.session_key.funcs.encode = chiaki_pb_encode_string;
	msg.big_payload.launch_spec.arg = launch_spec_buf.b64;
	msg.big_payload.launch_spec.funcs.encode = chiaki_pb_encode_string;
	msg.big_payload.encrypted_key.funcs.encode = chiaki_pb_encode_zero_encrypted_key;
	msg.big_payload.ecdh_pub_key.arg = &ecdh_pub_key_buf;
	msg.big_payload.ecdh_pub_key.funcs.encode = chiaki_pb_encode_buf;
	msg.big_payload.ecdh_sig.arg = &ecdh_sig_buf;
	msg.big_payload.ecdh_sig.funcs.encode = chiaki_pb_encode_buf;

	uint8_t buf[2048];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection big protobuf encoding failed\n");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	err = chiaki_takion_send_message_data(&stream_connection->takion, 1, 1, buf, buf_size);

	return err;
}

static ChiakiErrorCode stream_connection_send_streaminfo_ack(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));
	msg.type = tkproto_TakionMessage_PayloadType_STREAMINFOACK;

	uint8_t buf[3];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection streaminfo ack protobuf encoding failed\n");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 9, buf, buf_size);
}

static ChiakiErrorCode stream_connection_send_disconnect(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_DISCONNECT;
	msg.has_disconnect_payload = true;
	msg.disconnect_payload.reason.arg = "Client Disconnecting";
	msg.disconnect_payload.reason.funcs.encode = chiaki_pb_encode_string;

	uint8_t buf[26];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection disconnect protobuf encoding failed\n");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	ChiakiErrorCode err = chiaki_takion_send_message_data(&stream_connection->takion, 1, 1, buf, buf_size);

	return err;
}


static void stream_connection_takion_av(ChiakiTakionAVPacket *packet, void *user)
{
	ChiakiStreamConnection *stream_connection = user;

	chiaki_gkcrypt_decrypt(stream_connection->gkcrypt_remote, packet->key_pos + CHIAKI_GKCRYPT_BLOCK_SIZE, packet->data, packet->data_size);

	/*CHIAKI_LOGD(stream_connection->log, "AV: index: %u,%u; b@0x1a: %d; is_video: %d; 0xa: %u; 0xc: %u; 0xe: %u; codec: %u; 0x18: %u; adaptive_stream: %u, 0x2c: %u\n",
				header->packet_index, header->frame_index, header->byte_at_0x1a, header->is_video ? 1 : 0, header->word_at_0xa, header->units_in_frame_total, header->units_in_frame_additional, header->codec,
				header->word_at_0x18, header->adaptive_stream_index, header->byte_at_0x2c);
	chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_DEBUG, buf, buf_size);*/

	if(packet->is_video)
	{
		chiaki_video_receiver_av_packet(stream_connection->session->video_receiver, packet);
	}
	else
	{
		if(packet->codec == 5/*buf[0] == 0xf4 && buf_size >= 0x50*/)
		{
			//CHIAKI_LOGD(stream_connection->log, "audio!\n");
			chiaki_audio_receiver_frame_packet(stream_connection->session->audio_receiver, packet->data, 0x50); // TODO: why 0x50? this is dangerous!!!
		}
		else
		{
			//CHIAKI_LOGD(stream_connection->log, "NON-audio\n");
		}
	}

	/*else if(base_type == 2 && buf[0] != 0xf4)
	{
		CHIAKI_LOGD(stream_connection->log, "av frame 2, which is not audio\n");
	}*/

	//CHIAKI_LOGD(stream_connection->log, "StreamConnection AV %lu\n", buf_size);
	//chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_DEBUG, buf, buf_size);
}


static ChiakiErrorCode stream_connection_send_heartbeat(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg = { 0 };
	msg.type = tkproto_TakionMessage_PayloadType_HEARTBEAT;

	uint8_t buf[8];

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection heartbeat protobuf encoding failed\n");
		return CHIAKI_ERR_UNKNOWN;
	}

	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 1, buf, stream.bytes_written);
}