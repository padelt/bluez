/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <glib.h>

#include "src/shared/util.h"
#include "src/log.h"
#include "android/avdtp.h"

struct test_pdu {
	bool valid;
	const uint8_t *data;
	size_t size;
};

struct test_data {
	char *test_name;
	struct test_pdu *pdu_list;
};

#define data(args...) ((const unsigned char[]) { args })

#define raw_pdu(args...) \
	{							\
		.valid = true,					\
		.data = data(args),				\
		.size = sizeof(data(args)),			\
	}

#define define_test(name, function, args...) \
	do {								\
		const struct test_pdu pdus[] = {			\
			args, { }, { }					\
		};							\
		static struct test_data data;				\
		data.test_name = g_strdup(name);			\
		data.pdu_list = g_malloc(sizeof(pdus));			\
		memcpy(data.pdu_list, pdus, sizeof(pdus));		\
		g_test_add_data_func(name, &data, function);		\
	} while (0)

struct context {
	GMainLoop *main_loop;
	struct avdtp *session;
	struct avdtp_local_sep *sep;
	struct avdtp_stream *stream;
	guint source;
	int fd;
	int mtu;
	gboolean pending_open;
	unsigned int pdu_offset;
	const struct test_data *data;
};

static void test_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	g_print("%s%s\n", prefix, str);
}

static void test_free(gconstpointer user_data)
{
	const struct test_data *data = user_data;

	g_free(data->test_name);
	g_free(data->pdu_list);
}

static void context_quit(struct context *context)
{
	g_main_loop_quit(context->main_loop);
}

static gboolean send_pdu(gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	ssize_t len;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	len = write(context->fd, pdu->data, pdu->size);

	if (g_test_verbose())
		util_hexdump('<', pdu->data, len, test_debug, "AVDTP: ");

	g_assert(len == (ssize_t) pdu->size);

	return FALSE;
}

static void context_process(struct context *context)
{
	if (!context->data->pdu_list[context->pdu_offset].valid) {
		context_quit(context);
		return;
	}

	g_idle_add(send_pdu, context);
}

static gboolean transport_open(struct avdtp_stream *stream)
{
	int fd;

	fd = open("/dev/null", O_RDWR, 0);
	if (fd < 0)
		g_assert_not_reached();

	return avdtp_stream_set_transport(stream, fd, 672, 672);
}

static gboolean test_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	unsigned char buf[512];
	ssize_t len;
	int fd;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	len = read(fd, buf, sizeof(buf));

	g_assert(len > 0);

	if (g_test_verbose())
		util_hexdump('>', buf, len, test_debug, "AVDTP: ");

	g_assert((size_t) len == pdu->size);

	g_assert(memcmp(buf, pdu->data, pdu->size) == 0);

	if (context->pending_open) {
		context->pending_open = FALSE;
		g_assert(transport_open(context->stream));
	}

	context_process(context);

	return TRUE;
}

static struct context *create_context(uint16_t version, gconstpointer data)
{
	struct context *context = g_new0(struct context, 1);
	GIOChannel *channel;
	int err, sv[2];

	context->main_loop = g_main_loop_new(NULL, FALSE);
	g_assert(context->main_loop);

	err = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
	g_assert(err == 0);

	context->session = avdtp_new(sv[0], 672, 672, version);
	g_assert(context->session != NULL);

	channel = g_io_channel_unix_new(sv[1]);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	context->source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				test_handler, context);
	g_assert(context->source > 0);

	g_io_channel_unref(channel);

	context->fd = sv[1];
	context->data = data;

	return context;
}

static void execute_context(struct context *context)
{
	g_main_loop_run(context->main_loop);

	g_source_remove(context->source);
	avdtp_unref(context->session);

	g_main_loop_unref(context->main_loop);

	test_free(context->data);
	g_free(context);
}

static gboolean sep_getcap_ind(struct avdtp *session,
					struct avdtp_local_sep *sep,
					gboolean get_all, GSList **caps,
					uint8_t *err, void *user_data)
{
	struct avdtp_service_capability *media_transport, *media_codec;
	struct avdtp_media_codec_capability *codec_caps;
	uint8_t cap[4] = { 0xff, 0xff, 2, 64 };

	*caps = NULL;

	media_transport = avdtp_service_cap_new(AVDTP_MEDIA_TRANSPORT,
						NULL, 0);

	*caps = g_slist_append(*caps, media_transport);

	codec_caps = g_malloc0(sizeof(*codec_caps) + sizeof(cap));
	codec_caps->media_type = AVDTP_MEDIA_TYPE_AUDIO;
	codec_caps->media_codec_type = 0x00;
	memcpy(codec_caps->data, cap, sizeof(cap));

	media_codec = avdtp_service_cap_new(AVDTP_MEDIA_CODEC, codec_caps,
					sizeof(*codec_caps) + sizeof(cap));

	*caps = g_slist_append(*caps, media_codec);
	g_free(codec_caps);

	return TRUE;
}

static gboolean sep_open_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream, uint8_t *err,
				void *user_data)
{
	struct context *context = user_data;

	context->pending_open = TRUE;
	context->stream = stream;

	return TRUE;
}

static struct avdtp_sep_ind sep_ind = {
	.get_capability		= sep_getcap_ind,
	.open			= sep_open_ind,
};

static void sep_setconf_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream,
				struct avdtp_error *err, void *user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	int ret;

	g_assert(err == NULL);

	if (!context)
		return;

	pdu = &context->data->pdu_list[context->pdu_offset];

	if (pdu->size < 2)
		return;

	if (g_str_equal(context->data->test_name, "/TP/SIG/SMG/BV-11-C"))
		ret = avdtp_get_configuration(session, stream);
	if (g_str_equal(context->data->test_name, "/TP/SIG/SMG/BV-23-C"))
		ret = avdtp_abort(session, stream);
	else
		ret = avdtp_open(session, stream);

	g_assert_cmpint(ret, ==, 0);
}

static void sep_open_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	int ret;

	g_assert(err == NULL);

	g_assert(transport_open(stream));

	ret = avdtp_start(session, stream);
	g_assert_cmpint(ret, ==, 0);
}

static void sep_start_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct context *context = user_data;
	int ret;

	g_assert(err == NULL);

	if (g_str_equal(context->data->test_name, "/TP/SIG/SMG/BV-19-C"))
		ret = avdtp_close(session, stream, FALSE);
	else
		ret = avdtp_suspend(session, stream);

	g_assert_cmpint(ret, ==, 0);
}

static struct avdtp_sep_cfm sep_cfm = {
	.set_configuration	= sep_setconf_cfm,
	.open			= sep_open_cfm,
	.start			= sep_start_cfm,
};

static void test_server(gconstpointer data)
{
	struct context *context = create_context(0x0100, data);
	struct avdtp_local_sep *sep;

	sep = avdtp_register_sep(AVDTP_SEP_TYPE_SOURCE, AVDTP_MEDIA_TYPE_AUDIO,
					0x00, TRUE, &sep_ind, NULL, context);

	g_idle_add(send_pdu, context);

	execute_context(context);

	avdtp_unregister_sep(sep);
}

static void discover_cb(struct avdtp *session, GSList *seps,
				struct avdtp_error *err, void *user_data)
{
	struct context *context = user_data;
	struct avdtp_stream *stream;
	struct avdtp_remote_sep *rsep;
	struct avdtp_service_capability *media_transport, *media_codec;
	struct avdtp_media_codec_capability *cap;
	GSList *caps;
	uint8_t data[4] = { 0x21, 0x02, 2, 32 };
	int ret;

	if (g_str_equal(context->data->test_name, "/TP/SIG/SMG/BV-05-C") ||
		g_str_equal(context->data->test_name, "/TP/SIG/SMG/BV-07-C"))
		return;

	g_assert(err == NULL);
	g_assert_cmpint(g_slist_length(seps), !=, 0);

	rsep = avdtp_find_remote_sep(session, context->sep);
	g_assert(rsep != NULL);

	media_transport = avdtp_service_cap_new(AVDTP_MEDIA_TRANSPORT,
						NULL, 0);

	caps = g_slist_append(NULL, media_transport);

	cap = g_malloc0(sizeof(*cap) + sizeof(data));
	cap->media_type = AVDTP_MEDIA_TYPE_AUDIO;
	cap->media_codec_type = 0x00;
	memcpy(cap->data, data, sizeof(data));

	media_codec = avdtp_service_cap_new(AVDTP_MEDIA_CODEC, cap,
						sizeof(*cap) + sizeof(data));

	caps = g_slist_append(caps, media_codec);
	g_free(cap);

	ret = avdtp_set_configuration(session, rsep, context->sep, caps,
								&stream);
	g_assert_cmpint(ret, ==, 0);

	g_slist_free_full(caps, g_free);
}

static void test_client(gconstpointer data)
{
	struct context *context = create_context(0x0100, data);
	struct avdtp_local_sep *sep;

	sep = avdtp_register_sep(AVDTP_SEP_TYPE_SINK, AVDTP_MEDIA_TYPE_AUDIO,
					0x00, FALSE, NULL, &sep_cfm,
					context);
	context->sep = sep;

	avdtp_discover(context->session, discover_cb, context);

	execute_context(context);

	avdtp_unregister_sep(sep);
}

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	if (g_test_verbose())
		__btd_log_init("*", 0);

	/*
	 * Stream Management Service
	 *
	 * To verify that the following procedures are implemented according to
	 * their specification in AVDTP.
	 */
	define_test("/TP/SIG/SMG/BV-05-C", test_client,
			raw_pdu(0x00, 0x01));
	define_test("/TP/SIG/SMG/BV-06-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00));
	define_test("/TP/SIG/SMG/BV-07-C", test_client,
			raw_pdu(0x10, 0x01),
			raw_pdu(0x12, 0x01, 0x04, 0x00),
			raw_pdu(0x20, 0x02, 0x04));
	define_test("/TP/SIG/SMG/BV-08-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40));
	define_test("/TP/SIG/SMG/BV-09-C", test_client,
			raw_pdu(0x30, 0x01),
			raw_pdu(0x32, 0x01, 0x04, 0x00),
			raw_pdu(0x40, 0x02, 0x04),
			raw_pdu(0x42, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x50, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20));
	define_test("/TP/SIG/SMG/BV-10-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x20, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x22, 0x03));
	define_test("/TP/SIG/SMG/BV-11-C", test_client,
			raw_pdu(0x60, 0x01),
			raw_pdu(0x62, 0x01, 0x04, 0x00),
			raw_pdu(0x70, 0x02, 0x04),
			raw_pdu(0x72, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x80, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x82, 0x03),
			raw_pdu(0x90, 0x04, 0x04));
	define_test("/TP/SIG/SMG/BV-12-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x20, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x22, 0x03),
			raw_pdu(0x30, 0x04, 0x04),
			raw_pdu(0x32, 0x04, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0x21, 0x02, 0x02, 0x20));
	define_test("/TP/SIG/SMG/BV-15-C", test_client,
			raw_pdu(0xa0, 0x01),
			raw_pdu(0xa2, 0x01, 0x04, 0x00),
			raw_pdu(0xb0, 0x02, 0x04),
			raw_pdu(0xb2, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0xc0, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0xc2, 0x03),
			raw_pdu(0xd0, 0x06, 0x04));
	define_test("/TP/SIG/SMG/BV-16-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x20, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x22, 0x03),
			raw_pdu(0x30, 0x06, 0x04),
			raw_pdu(0x32, 0x06));
	define_test("/TP/SIG/SMG/BV-17-C", test_client,
			raw_pdu(0xe0, 0x01),
			raw_pdu(0xe2, 0x01, 0x04, 0x00),
			raw_pdu(0xf0, 0x02, 0x04),
			raw_pdu(0xf2, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x00, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x02, 0x03),
			raw_pdu(0x10, 0x06, 0x04),
			raw_pdu(0x12, 0x06),
			raw_pdu(0x20, 0x07, 0x04));
	define_test("/TP/SIG/SMG/BV-18-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x20, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x22, 0x03),
			raw_pdu(0x30, 0x06, 0x04),
			raw_pdu(0x32, 0x06),
			raw_pdu(0x40, 0x07, 0x04),
			raw_pdu(0x42, 0x07));
	define_test("/TP/SIG/SMG/BV-19-C", test_client,
			raw_pdu(0x30, 0x01),
			raw_pdu(0x32, 0x01, 0x04, 0x00),
			raw_pdu(0x40, 0x02, 0x04),
			raw_pdu(0x42, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x50, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x52, 0x03),
			raw_pdu(0x60, 0x06, 0x04),
			raw_pdu(0x62, 0x06),
			raw_pdu(0x70, 0x07, 0x04),
			raw_pdu(0x72, 0x07),
			raw_pdu(0x80, 0x08, 0x04));
	define_test("/TP/SIG/SMG/BV-20-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x20, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x22, 0x03),
			raw_pdu(0x30, 0x06, 0x04),
			raw_pdu(0x32, 0x06),
			raw_pdu(0x40, 0x07, 0x04),
			raw_pdu(0x42, 0x07),
			raw_pdu(0x50, 0x08, 0x04),
			raw_pdu(0x52, 0x08));
	define_test("/TP/SIG/SMG/BV-21-C", test_client,
			raw_pdu(0x90, 0x01),
			raw_pdu(0x92, 0x01, 0x04, 0x00),
			raw_pdu(0xa0, 0x02, 0x04),
			raw_pdu(0xa2, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0xb0, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0xb2, 0x03),
			raw_pdu(0xc0, 0x06, 0x04),
			raw_pdu(0xc2, 0x06),
			raw_pdu(0xd0, 0x07, 0x04),
			raw_pdu(0xd2, 0x07),
			raw_pdu(0xe0, 0x09, 0x04));
	define_test("/TP/SIG/SMG/BV-22-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x20, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x22, 0x03),
			raw_pdu(0x30, 0x06, 0x04),
			raw_pdu(0x32, 0x06),
			raw_pdu(0x40, 0x07, 0x04),
			raw_pdu(0x42, 0x07),
			raw_pdu(0x50, 0x09, 0x04),
			raw_pdu(0x52, 0x09));
	define_test("/TP/SIG/SMG/BV-23-C", test_client,
			raw_pdu(0xf0, 0x01),
			raw_pdu(0xf2, 0x01, 0x04, 0x00),
			raw_pdu(0x00, 0x02, 0x04),
			raw_pdu(0x02, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x10, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x12, 0x03),
			raw_pdu(0x20, 0x0a, 0x04));
	define_test("/TP/SIG/SMG/BV-24-C", test_server,
			raw_pdu(0x00, 0x01),
			raw_pdu(0x02, 0x01, 0x04, 0x00),
			raw_pdu(0x10, 0x02, 0x04),
			raw_pdu(0x12, 0x02, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00,
				0xff, 0xff, 0x02, 0x40),
			raw_pdu(0x20, 0x03, 0x04, 0x04, 0x01, 0x00, 0x07, 0x06,
				0x00, 0x00, 0x21, 0x02, 0x02, 0x20),
			raw_pdu(0x22, 0x03),
			raw_pdu(0x30, 0x0a, 0x04),
			raw_pdu(0x32, 0x0a));

	return g_test_run();
}
