/**
 * @file gntmedia.c GNT Media API
 * @ingroup finch
 */

/* finch
 *
 * Finch is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include "finch.h"

#include "gntconv.h"
#include "gntmedia.h"

#include "gnt.h"
#include "gntbutton.h"
#include "gntbox.h"
#include "gntlabel.h"

#include "cmds.h"
#include "conversation.h"
#include "debug.h"
#include "media-gst.h"
#include "mediamanager.h"

/* An incredibly large part of the following is from gtkmedia.c */
#ifdef USE_VV

#undef hangup

struct _FinchMediaPrivate
{
	PurpleMedia *media;

	GntWidget *accept;
	GntWidget *reject;
	GntWidget *hangup;
	GntWidget *calling;

	PurpleConversation *conv;
};

#define FINCH_MEDIA_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), FINCH_TYPE_MEDIA, FinchMediaPrivate))

static void finch_media_class_init (FinchMediaClass *klass);
static void finch_media_init (FinchMedia *media);
static void finch_media_finalize (GObject *object);
static void finch_media_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void finch_media_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);

static GntBoxClass *parent_class = NULL;

enum {
	MESSAGE,
	LAST_SIGNAL
};
static guint finch_media_signals[LAST_SIGNAL] = {0};

enum {
	PROP_0,
	PROP_MEDIA,
};

GType
finch_media_get_type(void)
{
	static GType type = 0;

	if (type == 0) {
		static const GTypeInfo info = {
			sizeof(FinchMediaClass),
			NULL,
			NULL,
			(GClassInitFunc) finch_media_class_init,
			NULL,
			NULL,
			sizeof(FinchMedia),
			0,
			(GInstanceInitFunc) finch_media_init,
			NULL
		};
		type = g_type_register_static(GNT_TYPE_BOX, "FinchMedia", &info, 0);
	}
	return type;
}


static void
finch_media_class_init (FinchMediaClass *klass)
{
	GObjectClass *gobject_class = (GObjectClass*)klass;
	parent_class = g_type_class_peek_parent(klass);

	gobject_class->finalize = finch_media_finalize;
	gobject_class->set_property = finch_media_set_property;
	gobject_class->get_property = finch_media_get_property;

	g_object_class_install_property(gobject_class, PROP_MEDIA,
			g_param_spec_object("media",
			"PurpleMedia",
			"The PurpleMedia associated with this media.",
			PURPLE_TYPE_MEDIA,
			G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE));

	finch_media_signals[MESSAGE] = g_signal_new("message", G_TYPE_FROM_CLASS(klass),
					G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					g_cclosure_marshal_VOID__STRING,
					G_TYPE_NONE, 1, G_TYPE_STRING);

	g_type_class_add_private(klass, sizeof(FinchMediaPrivate));
}


static void
finch_media_init (FinchMedia *media)
{
	media->priv = FINCH_MEDIA_GET_PRIVATE(media);

	media->priv->calling = gnt_label_new(_("Calling ... "));
	media->priv->hangup = gnt_button_new(_("Hangup"));
	media->priv->accept = gnt_button_new(_("Accept"));
	media->priv->reject = gnt_button_new(_("Reject"));

	gnt_box_set_alignment(GNT_BOX(media), GNT_ALIGN_MID);

	gnt_box_add_widget(GNT_BOX(media), media->priv->accept);
	gnt_box_add_widget(GNT_BOX(media), media->priv->reject);
}

static void
finch_media_finalize (GObject *media)
{
	FinchMedia *gntmedia = FINCH_MEDIA(media);
	purple_debug_info("gntmedia", "finch_media_finalize\n");
	if (gntmedia->priv->media)
		g_object_unref(gntmedia->priv->media);
}

static void
finch_media_emit_message(FinchMedia *gntmedia, const char *msg)
{
	g_signal_emit(gntmedia, finch_media_signals[MESSAGE], 0, msg);
}

static void
finch_media_accept_cb(PurpleMedia *media, FinchMedia *gntmedia)
{
	GntWidget *parent;

	finch_media_emit_message(gntmedia, _("Call in progress."));

	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->accept);
	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->reject);
	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->hangup);
	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->calling);

	gnt_box_add_widget(GNT_BOX(gntmedia), gntmedia->priv->hangup);

	gnt_widget_destroy(gntmedia->priv->accept);
	gnt_widget_destroy(gntmedia->priv->reject);
	gnt_widget_destroy(gntmedia->priv->calling);
	gntmedia->priv->accept = NULL;
	gntmedia->priv->reject = NULL;
	gntmedia->priv->calling = NULL;

	parent = GNT_WIDGET(gntmedia);
	while (parent->parent)
		parent = parent->parent;
	gnt_box_readjust(GNT_BOX(parent));
	gnt_widget_draw(parent);
}

static void
finch_media_wait_cb(PurpleMedia *media, FinchMedia *gntmedia)
{
	GntWidget *parent;

	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->accept);
	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->reject);
	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->hangup);
	gnt_box_remove(GNT_BOX(gntmedia), gntmedia->priv->calling);

	gnt_box_add_widget(GNT_BOX(gntmedia), gntmedia->priv->calling);
	gnt_box_add_widget(GNT_BOX(gntmedia), gntmedia->priv->hangup);

	parent = GNT_WIDGET(gntmedia);
	while (parent->parent)
		parent = parent->parent;
	gnt_box_readjust(GNT_BOX(parent));
	gnt_widget_draw(parent);
}

static void
finch_media_state_changed_cb(PurpleMedia *media, PurpleMediaState state,
		gchar *sid, gchar *name, FinchMedia *gntmedia)
{
	purple_debug_info("gntmedia", "state: %d sid: %s name: %s\n",
			state, sid, name);
	if (sid == NULL && name == NULL) {
		if (state == PURPLE_MEDIA_STATE_END) {
			finch_media_emit_message(gntmedia,
					_("The call has been terminated."));
			finch_conversation_set_info_widget(
					gntmedia->priv->conv, NULL);
			gnt_widget_destroy(GNT_WIDGET(gntmedia));
			/*
			 * XXX: This shouldn't have to be here
			 * to free the FinchMedia widget.
			 */
			g_object_unref(gntmedia);
		}
	} else if (state == PURPLE_MEDIA_STATE_CONNECTED) {
		finch_media_accept_cb(media, gntmedia);
	}
}

static void
finch_media_stream_info_cb(PurpleMedia *media, PurpleMediaInfoType type,
		gchar *sid, gchar *name, FinchMedia *gntmedia)
{
	if (type == PURPLE_MEDIA_INFO_REJECT) {
		finch_media_emit_message(gntmedia,
				_("You have rejected the call."));
	}
}

static void
finch_media_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	FinchMedia *media;
	g_return_if_fail(FINCH_IS_MEDIA(object));

	media = FINCH_MEDIA(object);
	switch (prop_id) {
		case PROP_MEDIA:
		{
			gboolean is_initiator;
			if (media->priv->media)
				g_object_unref(media->priv->media);
			media->priv->media = g_value_get_object(value);
			g_object_ref(media->priv->media);
			g_signal_connect_swapped(G_OBJECT(media->priv->accept), "activate",
				 G_CALLBACK(purple_media_accept), media->priv->media);
			g_signal_connect_swapped(G_OBJECT(media->priv->reject), "activate",
				 G_CALLBACK(purple_media_reject), media->priv->media);
			g_signal_connect_swapped(G_OBJECT(media->priv->hangup), "activate",
				 G_CALLBACK(purple_media_hangup), media->priv->media);

			g_object_get(G_OBJECT(media->priv->media), "initiator",
					&is_initiator, NULL);
			if (is_initiator == TRUE) {
				finch_media_wait_cb(media->priv->media, media);
			}
			g_signal_connect(G_OBJECT(media->priv->media), "state-changed",
				G_CALLBACK(finch_media_state_changed_cb), media);
			g_signal_connect(G_OBJECT(media->priv->media), "stream-info",
				G_CALLBACK(finch_media_stream_info_cb), media);
			break;
		}
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
finch_media_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	FinchMedia *media;
	g_return_if_fail(FINCH_IS_MEDIA(object));

	media = FINCH_MEDIA(object);

	switch (prop_id) {
		case PROP_MEDIA:
			g_value_set_object(value, media->priv->media);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);	
			break;
	}
}

GntWidget *
finch_media_new(PurpleMedia *media)
{
	return GNT_WIDGET(g_object_new(finch_media_get_type(),
				"media", media,
				"vertical", FALSE,
				"homogeneous", FALSE,
				NULL));
}

static void
gntmedia_message_cb(FinchMedia *gntmedia, const char *msg, PurpleConversation *conv)
{
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM) {
		purple_conv_im_write(PURPLE_CONV_IM(conv), NULL, msg, PURPLE_MESSAGE_SYSTEM, time(NULL));
	}
}

static gboolean
finch_new_media(PurpleMediaManager *manager, PurpleMedia *media,
		PurpleConnection *gc, gchar *name, gpointer null)
{
	GntWidget *gntmedia;
	PurpleConversation *conv;

	conv = purple_conversation_new(PURPLE_CONV_TYPE_IM,
			purple_connection_get_account(gc), name);

	gntmedia = finch_media_new(media);
	g_signal_connect(G_OBJECT(gntmedia), "message", G_CALLBACK(gntmedia_message_cb), conv);
	FINCH_MEDIA(gntmedia)->priv->conv = conv;
	finch_conversation_set_info_widget(conv, gntmedia);
	return TRUE;
}

static PurpleCmdRet
call_cmd_cb(PurpleConversation *conv, const char *cmd, char **args,
		char **eror, gpointer data)
{
	PurpleAccount *account = purple_conversation_get_account(conv);

	if (!purple_prpl_initiate_media(account,
			purple_conversation_get_name(conv),
			PURPLE_MEDIA_AUDIO))
		return PURPLE_CMD_STATUS_FAILED;

	return PURPLE_CMD_STATUS_OK;
}

static GstElement *
create_default_audio_src(void)
{
	GstElement *bin, *src, *volume;
	GstPad *pad, *ghost;
	const gchar *audio_device = purple_prefs_get_string(
			"/purple/media/audio/device");
	double input_volume = purple_prefs_get_int(
			"/purple/media/audio/volume/input")/10.0;

	bin = gst_bin_new("purplesendaudiobin");
	src = gst_element_factory_make("alsasrc", "asrc");
	volume = gst_element_factory_make("volume", "purpleaudioinputvolume");
	g_object_set(volume, "volume", input_volume, NULL);
	gst_bin_add_many(GST_BIN(bin), src, volume, NULL);
	gst_element_link(src, volume);
	pad = gst_element_get_pad(volume, "src");
	ghost = gst_ghost_pad_new("ghostsrc", pad);
	gst_element_add_pad(bin, ghost);

	if (audio_device != NULL && strcmp(audio_device, ""))
		g_object_set(G_OBJECT(src), "device", audio_device, NULL);

	return bin;
}

static GstElement *
create_default_audio_sink(void)
{
	GstElement *bin, *sink, *volume, *queue;
	GstPad *pad, *ghost;
	double output_volume = purple_prefs_get_int(
			"/purple/media/audio/volume/output")/10.0;

	bin = gst_bin_new("pidginrecvaudiobin");
	sink = gst_element_factory_make("alsasink", "asink");
	volume = gst_element_factory_make("volume", "purpleaudiooutputvolume");
	g_object_set(volume, "volume", output_volume, NULL);
	queue = gst_element_factory_make("queue", NULL);
	gst_bin_add_many(GST_BIN(bin), sink, volume, queue, NULL);
	gst_element_link(volume, sink);
	gst_element_link(queue, volume);
	pad = gst_element_get_pad(queue, "sink");
	ghost = gst_ghost_pad_new("ghostsink", pad);
	gst_element_add_pad(bin, ghost);

	return bin;
}

static PurpleMediaElementInfo default_audio_src =
{
	"finchdefaultaudiosrc",		/* id */
	PURPLE_MEDIA_ELEMENT_AUDIO	/* type */
			| PURPLE_MEDIA_ELEMENT_SRC
			| PURPLE_MEDIA_ELEMENT_ONE_SRC
			| PURPLE_MEDIA_ELEMENT_UNIQUE,
	create_default_audio_src,	/* create */
};

static PurpleMediaElementInfo default_audio_sink =
{
	"finchdefaultaudiosink",	/* id */
	PURPLE_MEDIA_ELEMENT_AUDIO	/* type */
			| PURPLE_MEDIA_ELEMENT_SINK
			| PURPLE_MEDIA_ELEMENT_ONE_SINK,
	create_default_audio_sink,	/* create */
};

void finch_media_manager_init(void)
{
	PurpleMediaManager *manager = purple_media_manager_get();
	g_signal_connect(G_OBJECT(manager), "init-media", G_CALLBACK(finch_new_media), NULL);
	purple_cmd_register("call", "", PURPLE_CMD_P_DEFAULT,
			PURPLE_CMD_FLAG_IM, NULL,
			call_cmd_cb, _("call: Make an audio call."), NULL);

	purple_debug_info("gntmedia", "Registering media element types\n");
	purple_media_manager_set_active_element(manager, &default_audio_src);
	purple_media_manager_set_active_element(manager, &default_audio_sink);
}

void finch_media_manager_uninit(void)
{
	PurpleMediaManager *manager = purple_media_manager_get();
	g_signal_handlers_disconnect_by_func(G_OBJECT(manager),
			G_CALLBACK(finch_new_media), NULL);
}

#endif  /* USE_VV */
