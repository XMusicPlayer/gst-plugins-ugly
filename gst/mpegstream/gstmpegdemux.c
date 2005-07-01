/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstmpegdemux.h"


GST_DEBUG_CATEGORY_STATIC (gstmpegdemux_debug);
#define GST_CAT_DEFAULT (gstmpegdemux_debug)

GST_DEBUG_CATEGORY_EXTERN (GST_CAT_SEEK);

#define PARSE_CLASS(o)  GST_MPEG_PARSE_CLASS (G_OBJECT_GET_CLASS (o))
#define CLASS(o)  GST_MPEG_DEMUX_CLASS (G_OBJECT_GET_CLASS (o))

/* elementfactory information */
static GstElementDetails mpeg_demux_details = {
  "MPEG Demuxer",
  "Codec/Demuxer",
  "Demultiplexes MPEG1 and MPEG2 System Streams",
  "Erik Walthinsen <omega@cse.ogi.edu>\n" "Wim Taymans <wim.taymans@chello.be>"
};

/* MPEG2Demux signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_BIT_RATE,
  ARG_MPEG2
      /* FILL ME */
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpeg, "
        "mpegversion = (int) { 1, 2 }, " "systemstream = (boolean) TRUE")
    );

static GstStaticPadTemplate video_template =
GST_STATIC_PAD_TEMPLATE ("video_%02d",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("video/mpeg, "
        "mpegversion = (int) { 1, 2 }, " "systemstream = (boolean) FALSE")
    );

static GstStaticPadTemplate audio_template =
GST_STATIC_PAD_TEMPLATE ("audio_%02d",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("audio/mpeg, " "mpegversion = (int) 1"
        /* FIXME "layer = (int) { 1, 2 }" */
    )
    );

static GstStaticPadTemplate private_template =
GST_STATIC_PAD_TEMPLATE ("private_%d",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static void gst_mpeg_demux_base_init (GstMPEGDemuxClass * klass);
static void gst_mpeg_demux_class_init (GstMPEGDemuxClass * klass);
static void gst_mpeg_demux_init (GstMPEGDemux * mpeg_demux);

static void gst_mpeg_demux_send_data (GstMPEGParse * mpeg_parse,
    GstData * data, GstClockTime time);
static void gst_mpeg_demux_send_discont (GstMPEGParse * mpeg_parse,
    GstClockTime time);
static void gst_mpeg_demux_handle_discont (GstMPEGParse * mpeg_parse,
    GstEvent * event);
static void gst_mpeg_demux_send_event (GstMPEGParse * mpeg_parse,
    GstEvent * event, GstClockTime time);

static GstPad *gst_mpeg_demux_new_output_pad (GstMPEGDemux * mpeg_demux,
    const gchar * name, GstPadTemplate * temp);
static void gst_mpeg_demux_init_stream (GstMPEGDemux * mpeg_demux,
    gint type,
    GstMPEGStream * str,
    gint number, const gchar * name, GstPadTemplate * temp);
static GstMPEGStream *gst_mpeg_demux_get_video_stream (GstMPEGDemux *
    mpeg_demux, guint8 stream_nr, gint type, const gpointer info);
static GstMPEGStream *gst_mpeg_demux_get_audio_stream (GstMPEGDemux *
    mpeg_demux, guint8 stream_nr, gint type, const gpointer info);
static GstMPEGStream *gst_mpeg_demux_get_private_stream (GstMPEGDemux *
    mpeg_demux, guint8 stream_nr, gint type, const gpointer info);

static gboolean gst_mpeg_demux_parse_packhead (GstMPEGParse * mpeg_parse,
    GstBuffer * buffer);
static gboolean gst_mpeg_demux_parse_syshead (GstMPEGParse * mpeg_parse,
    GstBuffer * buffer);
static gboolean gst_mpeg_demux_parse_packet (GstMPEGParse * mpeg_parse,
    GstBuffer * buffer);
static gboolean gst_mpeg_demux_parse_pes (GstMPEGParse * mpeg_parse,
    GstBuffer * buffer);

static void gst_mpeg_demux_send_subbuffer (GstMPEGDemux * mpeg_demux,
    GstMPEGStream * outstream, GstBuffer * buffer,
    GstClockTime timestamp, guint offset, guint size);
static void gst_mpeg_demux_process_private (GstMPEGDemux * mpeg_demux,
    GstBuffer * buffer,
    guint stream_nr, GstClockTime timestamp, guint headerlen, guint datalen);
static void gst_mpeg_demux_synchronise_pads (GstMPEGDemux * mpeg_demux,
    GstClockTime threshold, GstClockTime new_ts);
static void gst_mpeg_demux_sync_stream_to_time (GstMPEGDemux * mpeg_demux,
    GstMPEGStream * stream, GstClockTime last_ts);

const GstFormat *gst_mpeg_demux_get_src_formats (GstPad * pad);

static gboolean index_seek (GstPad * pad, GstEvent * event, gint64 * offset);
static gboolean normal_seek (GstPad * pad, GstEvent * event, gint64 * offset);

static gboolean gst_mpeg_demux_handle_src_event (GstPad * pad,
    GstEvent * event);
static void gst_mpeg_demux_reset (GstMPEGDemux * mpeg_demux);

static gboolean gst_mpeg_demux_handle_src_query (GstPad * pad,
    GstQueryType type, GstFormat * format, gint64 * value);

static GstElementStateReturn gst_mpeg_demux_change_state (GstElement * element);

static void gst_mpeg_demux_set_index (GstElement * element, GstIndex * index);
static GstIndex *gst_mpeg_demux_get_index (GstElement * element);


static GstMPEGParseClass *parent_class = NULL;

/*static guint gst_mpeg_demux_signals[LAST_SIGNAL] = { 0 };*/

GType
gst_mpeg_demux_get_type (void)
{
  static GType mpeg_demux_type = 0;

  if (!mpeg_demux_type) {
    static const GTypeInfo mpeg_demux_info = {
      sizeof (GstMPEGDemuxClass),
      (GBaseInitFunc) gst_mpeg_demux_base_init,
      NULL,
      (GClassInitFunc) gst_mpeg_demux_class_init,
      NULL,
      NULL,
      sizeof (GstMPEGDemux),
      0,
      (GInstanceInitFunc) gst_mpeg_demux_init,
    };

    mpeg_demux_type =
        g_type_register_static (GST_TYPE_MPEG_PARSE, "GstMPEGDemux",
        &mpeg_demux_info, 0);

    GST_DEBUG_CATEGORY_INIT (gstmpegdemux_debug, "mpegdemux", 0,
        "MPEG demultiplexer element");
  }

  return mpeg_demux_type;
}

static void
gst_mpeg_demux_base_init (GstMPEGDemuxClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  klass->video_template = gst_static_pad_template_get (&video_template);
  klass->audio_template = gst_static_pad_template_get (&audio_template);
  klass->private_template = gst_static_pad_template_get (&private_template);

  gst_element_class_add_pad_template (element_class, klass->video_template);
  gst_element_class_add_pad_template (element_class, klass->audio_template);
  gst_element_class_add_pad_template (element_class, klass->private_template);

  gst_element_class_set_details (element_class, &mpeg_demux_details);
}

static void
gst_mpeg_demux_class_init (GstMPEGDemuxClass * klass)
{
  GstElementClass *gstelement_class;
  GstMPEGParseClass *mpeg_parse_class;

  parent_class = g_type_class_ref (GST_TYPE_MPEG_PARSE);

  gstelement_class = (GstElementClass *) klass;
  mpeg_parse_class = (GstMPEGParseClass *) klass;

  gstelement_class->change_state = gst_mpeg_demux_change_state;
  gstelement_class->set_index = gst_mpeg_demux_set_index;
  gstelement_class->get_index = gst_mpeg_demux_get_index;

  mpeg_parse_class->parse_packhead = gst_mpeg_demux_parse_packhead;
  mpeg_parse_class->parse_syshead = gst_mpeg_demux_parse_syshead;
  mpeg_parse_class->parse_packet = gst_mpeg_demux_parse_packet;
  mpeg_parse_class->parse_pes = gst_mpeg_demux_parse_pes;
  mpeg_parse_class->send_data = gst_mpeg_demux_send_data;
  mpeg_parse_class->send_discont = gst_mpeg_demux_send_discont;
  mpeg_parse_class->handle_discont = gst_mpeg_demux_handle_discont;
  mpeg_parse_class->send_event = gst_mpeg_demux_send_event;

  klass->new_output_pad = gst_mpeg_demux_new_output_pad;
  klass->init_stream = gst_mpeg_demux_init_stream;
  klass->get_video_stream = gst_mpeg_demux_get_video_stream;
  klass->get_audio_stream = gst_mpeg_demux_get_audio_stream;
  klass->get_private_stream = gst_mpeg_demux_get_private_stream;
  klass->send_subbuffer = gst_mpeg_demux_send_subbuffer;
  klass->process_private = gst_mpeg_demux_process_private;
  klass->synchronise_pads = gst_mpeg_demux_synchronise_pads;
  klass->sync_stream_to_time = gst_mpeg_demux_sync_stream_to_time;

  /* we have our own sink pad template, but don't use it in subclasses */
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));
}

static void
gst_mpeg_demux_init (GstMPEGDemux * mpeg_demux)
{
  gint i;

  /* i think everything is already zero'd, but oh well */
  for (i = 0; i < GST_MPEG_DEMUX_NUM_VIDEO_STREAMS; i++) {
    mpeg_demux->video_stream[i] = NULL;
  }
  for (i = 0; i < GST_MPEG_DEMUX_NUM_AUDIO_STREAMS; i++) {
    mpeg_demux->audio_stream[i] = NULL;
  }
  for (i = 0; i < GST_MPEG_DEMUX_NUM_PRIVATE_STREAMS; i++) {
    mpeg_demux->private_stream[i] = NULL;
  }

  mpeg_demux->adjust = 0;
  mpeg_demux->max_gap = GST_CLOCK_TIME_NONE;
  mpeg_demux->max_gap_tolerance = GST_CLOCK_TIME_NONE;
  mpeg_demux->just_flushed = FALSE;

  GST_FLAG_SET (mpeg_demux, GST_ELEMENT_EVENT_AWARE);
}

static void
gst_mpeg_demux_send_data (GstMPEGParse * mpeg_parse, GstData * data,
    GstClockTime time)
{
  if (GST_IS_BUFFER (data)) {
    gst_buffer_unref (GST_BUFFER (data));
  } else {
    GstEvent *event = GST_EVENT (data);

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_FILLER:
      case GST_EVENT_DISCONTINUOUS:
      case GST_EVENT_FLUSH:
        PARSE_CLASS (mpeg_parse)->send_event (mpeg_parse, event,
            GST_CLOCK_TIME_NONE);
        break;
      default:
        /* Propagate the event normally. */
        gst_pad_event_default (mpeg_parse->sinkpad, event);
        break;
    }
  }
}

static void
gst_mpeg_demux_send_discont (GstMPEGParse * mpeg_parse, GstClockTime time)
{
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (mpeg_parse);

  if (GST_CLOCK_TIME_IS_VALID (time))
    time += mpeg_demux->adjust;

  if (!mpeg_demux->just_flushed) {
    GST_DEBUG_OBJECT (mpeg_parse, "Discont without flush, ts = %llu", time);
    /* Add padding to the end to make sure all streams end at the same timestamp */
    CLASS (mpeg_demux)->synchronise_pads (mpeg_demux,
        mpeg_parse->current_ts + mpeg_demux->adjust + (GST_SECOND / 20),
        mpeg_parse->current_ts + mpeg_demux->adjust + (GST_SECOND / 20));
  } else {
    GST_DEBUG_OBJECT (mpeg_parse, "Discont after flush, ts = %llu", time);
  }
  mpeg_demux->just_flushed = FALSE;

  parent_class->send_discont (mpeg_parse, time);
}

static void
gst_mpeg_demux_send_event (GstMPEGParse * mpeg_parse, GstEvent * event,
    GstClockTime time)
{
  /*
   * Distribute the event to all active pads
   */
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (mpeg_parse);
  gint i;

  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH) {
    GST_DEBUG_OBJECT (mpeg_demux, "Sending flush event");
    mpeg_demux->just_flushed = TRUE;
  }

  for (i = 0; i < GST_MPEG_DEMUX_NUM_VIDEO_STREAMS; i++) {
    if (mpeg_demux->video_stream[i]) {
      if (GST_PAD_IS_USABLE (mpeg_demux->video_stream[i]->pad)) {
        gst_event_ref (event);
        gst_pad_push (mpeg_demux->video_stream[i]->pad, GST_DATA (event));
      }
      if (GST_CLOCK_TIME_IS_VALID (time))
        mpeg_demux->video_stream[i]->cur_ts = time;
    }
  }

  for (i = 0; i < GST_MPEG_DEMUX_NUM_AUDIO_STREAMS; i++) {
    if (mpeg_demux->audio_stream[i]) {
      if (GST_PAD_IS_USABLE (mpeg_demux->audio_stream[i]->pad)) {
        gst_event_ref (event);
        gst_pad_push (mpeg_demux->audio_stream[i]->pad, GST_DATA (event));
      }
      if (GST_CLOCK_TIME_IS_VALID (time))
        mpeg_demux->audio_stream[i]->cur_ts = time;
    }
  }

  for (i = 0; i < GST_MPEG_DEMUX_NUM_PRIVATE_STREAMS; i++) {
    if (mpeg_demux->private_stream[i]) {
      if (GST_PAD_IS_USABLE (mpeg_demux->private_stream[i]->pad)) {
        gst_event_ref (event);
        gst_pad_push (mpeg_demux->private_stream[i]->pad, GST_DATA (event));
      }
      if (GST_CLOCK_TIME_IS_VALID (time))
        mpeg_demux->private_stream[i]->cur_ts = time;
    }
  }

  parent_class->send_event (mpeg_parse, event, time);
}

static void
gst_mpeg_demux_handle_discont (GstMPEGParse * mpeg_parse, GstEvent * event)
{
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (mpeg_parse);

  if (GST_EVENT_DISCONT_NEW_MEDIA (event)) {
    gst_mpeg_demux_reset (mpeg_demux);
  }

  if (parent_class->handle_discont != NULL)
    parent_class->handle_discont (mpeg_parse, event);
}

static gint
_demux_get_writer_id (GstIndex * index, GstPad * pad)
{
  gint id;

  if (!gst_index_get_writer_id (index, GST_OBJECT (pad), &id)) {
    GST_CAT_WARNING_OBJECT (GST_CAT_SEEK, index,
        "can't get index id for %s:%s", GST_DEBUG_PAD_NAME (pad));
    return -1;
  } else {
    GST_CAT_LOG_OBJECT (GST_CAT_SEEK, index,
        "got index id %d for %s:%s", id, GST_DEBUG_PAD_NAME (pad));
    return id;
  }
}

static GstPad *
gst_mpeg_demux_new_output_pad (GstMPEGDemux * mpeg_demux,
    const gchar * name, GstPadTemplate * temp)
{
  GstPad *pad;

  pad = gst_pad_new_from_template (temp, name);

  gst_pad_set_formats_function (pad, gst_mpeg_demux_get_src_formats);
  gst_pad_set_convert_function (pad, gst_mpeg_parse_convert_src);
  gst_pad_set_event_mask_function (pad, gst_mpeg_parse_get_src_event_masks);
  gst_pad_set_event_function (pad, gst_mpeg_demux_handle_src_event);
  gst_pad_set_query_type_function (pad, gst_mpeg_parse_get_src_query_types);
  gst_pad_set_query_function (pad, gst_mpeg_demux_handle_src_query);
  gst_pad_use_explicit_caps (pad);

  return pad;
}

static void
gst_mpeg_demux_init_stream (GstMPEGDemux * mpeg_demux,
    gint type,
    GstMPEGStream * str, gint number, const gchar * name, GstPadTemplate * temp)
{
  str->type = type;
  str->number = number;

  str->pad = CLASS (mpeg_demux)->new_output_pad (mpeg_demux, name, temp);
  gst_pad_set_element_private (str->pad, str);

  if (mpeg_demux->index) {
    str->index_id = _demux_get_writer_id (mpeg_demux->index, str->pad);
  }

  str->cur_ts = 0;
  str->scr_offs = 0;
}

static GstMPEGStream *
gst_mpeg_demux_get_video_stream (GstMPEGDemux * mpeg_demux,
    guint8 stream_nr, gint type, const gpointer info)
{
  gint mpeg_version = *((gint *) info);
  GstMPEGStream *str;
  GstMPEGVideoStream *video_str;
  gchar *name;
  GstCaps *caps;
  gboolean set_caps = FALSE;

  g_return_val_if_fail (stream_nr < GST_MPEG_DEMUX_NUM_VIDEO_STREAMS, NULL);
  g_return_val_if_fail (type > GST_MPEG_DEMUX_VIDEO_UNKNOWN &&
      type < GST_MPEG_DEMUX_VIDEO_LAST, NULL);

  str = mpeg_demux->video_stream[stream_nr];

  if (str == NULL) {
    video_str = g_new0 (GstMPEGVideoStream, 1);
    str = (GstMPEGStream *) video_str;

    name = g_strdup_printf ("video_%02d", stream_nr);
    CLASS (mpeg_demux)->init_stream (mpeg_demux, type, str, stream_nr, name,
        CLASS (mpeg_demux)->video_template);
    g_free (name);

    mpeg_demux->video_stream[stream_nr] = str;

    set_caps = TRUE;
  } else {
    /* This stream may have been created by a derived class, reset the
       size. */
    video_str = g_renew (GstMPEGVideoStream, str, 1);
    mpeg_demux->video_stream[stream_nr] = str = (GstMPEGStream *) video_str;
  }

  if (set_caps || video_str->mpeg_version != mpeg_version) {
    gchar *codec;
    GstTagList *list;

    /* We need to set new caps for this pad. */
    caps = gst_caps_new_simple ("video/mpeg",
        "mpegversion", G_TYPE_INT, mpeg_version,
        "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
    if (!gst_pad_set_explicit_caps (str->pad, caps)) {
      GST_ELEMENT_ERROR (GST_ELEMENT (mpeg_demux),
          CORE, NEGOTIATION, (NULL), ("failed to set caps"));
      gst_caps_free (caps);
      gst_element_add_pad (GST_ELEMENT (mpeg_demux), str->pad);
      return str;
    }
    gst_caps_free (caps);
    gst_element_add_pad (GST_ELEMENT (mpeg_demux), str->pad);

    /* Store the current values. */
    video_str->mpeg_version = mpeg_version;

    /* set stream metadata */
    codec = g_strdup_printf ("MPEG-%d video", mpeg_version);
    list = gst_tag_list_new ();
    gst_tag_list_add (list, GST_TAG_MERGE_REPLACE,
        GST_TAG_VIDEO_CODEC, codec, NULL);
    g_free (codec);
    gst_element_found_tags_for_pad (GST_ELEMENT (mpeg_demux),
        str->pad, 0, list);
  }

  return str;
}

static GstMPEGStream *
gst_mpeg_demux_get_audio_stream (GstMPEGDemux * mpeg_demux,
    guint8 stream_nr, gint type, const gpointer info)
{
  GstMPEGStream *str;
  gchar *name;
  GstCaps *caps;
  gboolean set_caps = FALSE;

  g_return_val_if_fail (stream_nr < GST_MPEG_DEMUX_NUM_AUDIO_STREAMS, NULL);
  g_return_val_if_fail (type > GST_MPEG_DEMUX_AUDIO_UNKNOWN &&
      type < GST_MPEG_DEMUX_AUDIO_LAST, NULL);

  str = mpeg_demux->audio_stream[stream_nr];
  if (str && str->type != type) {
    gst_element_remove_pad (GST_ELEMENT (mpeg_demux), str->pad);
    g_free (str);
    str = mpeg_demux->audio_stream[stream_nr] = NULL;
  }

  if (str == NULL) {
    str = g_new0 (GstMPEGStream, 1);

    name = g_strdup_printf ("audio_%02d", stream_nr);
    CLASS (mpeg_demux)->init_stream (mpeg_demux, type, str, stream_nr, name,
        CLASS (mpeg_demux)->audio_template);
    g_free (name);

    mpeg_demux->audio_stream[stream_nr] = str;

    /* new pad, set caps */
    set_caps = TRUE;
  } else {
    /* This stream may have been created by a derived class, reset the
       size. */
    str = g_renew (GstMPEGStream, str, 1);
  }

  if (set_caps) {
    GstTagList *list;

    /* We need to set new caps for this pad. */
    caps = gst_caps_new_simple ("audio/mpeg",
        "mpegversion", G_TYPE_INT, 1, NULL);
    if (!gst_pad_set_explicit_caps (str->pad, caps)) {
      GST_ELEMENT_ERROR (GST_ELEMENT (mpeg_demux),
          CORE, NEGOTIATION, (NULL), ("failed to set caps"));
      gst_caps_free (caps);
      gst_element_add_pad (GST_ELEMENT (mpeg_demux), str->pad);
      return str;
    }
    gst_caps_free (caps);
    gst_element_add_pad (GST_ELEMENT (mpeg_demux), str->pad);

    /* stream metadata */
    list = gst_tag_list_new ();
    gst_tag_list_add (list, GST_TAG_MERGE_REPLACE,
        GST_TAG_AUDIO_CODEC, "MPEG-1 audio", NULL);
    gst_element_found_tags_for_pad (GST_ELEMENT (mpeg_demux),
        str->pad, 0, list);
  }

  return str;
}

static GstMPEGStream *
gst_mpeg_demux_get_private_stream (GstMPEGDemux * mpeg_demux,
    guint8 stream_nr, gint type, const gpointer info)
{
  GstMPEGStream *str;
  gchar *name;

  g_return_val_if_fail (stream_nr < GST_MPEG_DEMUX_NUM_PRIVATE_STREAMS, NULL);

  str = mpeg_demux->private_stream[stream_nr];

  if (str == NULL) {
    name = g_strdup_printf ("private_%d", stream_nr + 1);
    str = g_new0 (GstMPEGStream, 1);
    CLASS (mpeg_demux)->init_stream (mpeg_demux, type, str, stream_nr, name,
        CLASS (mpeg_demux)->private_template);
    g_free (name);
    gst_element_add_pad (GST_ELEMENT (mpeg_demux), str->pad);

    mpeg_demux->private_stream[stream_nr] = str;
  }

  return str;
}

static gboolean
gst_mpeg_demux_parse_packhead (GstMPEGParse * mpeg_parse, GstBuffer * buffer)
{
  guint8 *buf;

  parent_class->parse_packhead (mpeg_parse, buffer);

  buf = GST_BUFFER_DATA (buffer);
  /* do something useful here */

  return TRUE;
}

static gboolean
gst_mpeg_demux_parse_syshead (GstMPEGParse * mpeg_parse, GstBuffer * buffer)
{
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (mpeg_parse);
  guint16 header_length;
  guchar *buf;

  buf = GST_BUFFER_DATA (buffer);
  buf += 4;

  header_length = GST_READ_UINT16_BE (buf);
  GST_DEBUG_OBJECT (mpeg_demux, "header_length %d", header_length);
  buf += 2;

  /* marker:1==1 ! rate_bound:22 | marker:1==1 */
  buf += 3;

  /* audio_bound:6==1 ! fixed:1 | constrained:1 */
  buf += 1;

  /* audio_lock:1 | video_lock:1 | marker:1==1 | video_bound:5 */
  buf += 1;

  /* apacket_rate_restriction:1 | reserved:7==0x7F */
  buf += 1;

  if (!GST_MPEG_PARSE_IS_MPEG2 (mpeg_demux)) {
    gint stream_count = (header_length - 6) / 3;
    gint i, j = 0;

    /* Reset the total_size_bound before counting it up */
    mpeg_demux->total_size_bound = 0;

    GST_DEBUG_OBJECT (mpeg_demux, "number of streams: %d ", stream_count);

    for (i = 0; i < stream_count; i++) {
      guint8 stream_id;
      gboolean STD_buffer_bound_scale;
      guint16 STD_buffer_size_bound;
      guint32 buf_byte_size_bound;
      GstMPEGStream *outstream = NULL;

      stream_id = *buf++;
      if (!(stream_id & 0x80)) {
        GST_DEBUG_OBJECT (mpeg_demux, "error in system header length");
        return FALSE;
      }

      /* check marker bits */
      if ((*buf & 0xC0) != 0xC0) {
        GST_DEBUG_OBJECT (mpeg_demux, "expecting placeholder bit values"
            " '11' after stream id");
        return FALSE;
      }

      STD_buffer_bound_scale = *buf & 0x20;
      STD_buffer_size_bound = ((guint16) (*buf++ & 0x1F)) << 8;
      STD_buffer_size_bound |= *buf++;

      if (STD_buffer_bound_scale == 0) {
        buf_byte_size_bound = STD_buffer_size_bound * 128;
      } else {
        buf_byte_size_bound = STD_buffer_size_bound * 1024;
      }

      if (stream_id == 0xBD) {
        /* Private stream 1. */
        outstream = CLASS (mpeg_demux)->get_private_stream (mpeg_demux,
            0, GST_MPEG_DEMUX_PRIVATE_UNKNOWN, NULL);
      } else if (stream_id == 0xBF) {
        /* Private stream 2. */
        outstream = CLASS (mpeg_demux)->get_private_stream (mpeg_demux,
            1, GST_MPEG_DEMUX_PRIVATE_UNKNOWN, NULL);
      } else if (stream_id >= 0xC0 && stream_id <= 0xDF) {
        /* Audio. */
        outstream = CLASS (mpeg_demux)->get_audio_stream (mpeg_demux,
            stream_id - 0xC0, GST_MPEG_DEMUX_AUDIO_MPEG, NULL);
      } else if (stream_id >= 0xE0 && stream_id <= 0xEF) {
        /* Video. */
        gint mpeg_version = !GST_MPEG_PARSE_IS_MPEG2 (mpeg_demux) ? 1 : 2;

        outstream = CLASS (mpeg_demux)->get_video_stream (mpeg_demux,
            stream_id - 0xE0, GST_MPEG_DEMUX_VIDEO_MPEG, &mpeg_version);
      } else {
        GST_WARNING_OBJECT (mpeg_demux, "unknown stream id 0x%02x", stream_id);
      }

      GST_DEBUG_OBJECT (mpeg_demux, "STD_buffer_bound_scale %d",
          STD_buffer_bound_scale);
      GST_DEBUG_OBJECT (mpeg_demux, "STD_buffer_size_bound %d or %d bytes",
          STD_buffer_size_bound, buf_byte_size_bound);

      if (outstream != NULL) {
        outstream->size_bound = buf_byte_size_bound;
        mpeg_demux->total_size_bound += buf_byte_size_bound;

        if (mpeg_demux->index) {
          outstream->index_id =
              _demux_get_writer_id (mpeg_demux->index, outstream->pad);
        }
      }

      j++;
    }
  }

  return TRUE;
}

static gboolean
gst_mpeg_demux_parse_packet (GstMPEGParse * mpeg_parse, GstBuffer * buffer)
{
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (mpeg_parse);
  guint8 id;
  guint16 headerlen;

  guint16 packet_length;
  gboolean STD_buffer_bound_scale;
  guint16 STD_buffer_size_bound;
  guint64 dts;
  gint64 pts = -1;

  guint16 datalen;

  GstMPEGStream *outstream = NULL;
  guint8 *buf, *basebuf;
  gint64 timestamp;

  basebuf = buf = GST_BUFFER_DATA (buffer);
  id = *(buf + 3);
  buf += 4;

  /* start parsing */
  packet_length = GST_READ_UINT16_BE (buf);

  GST_DEBUG_OBJECT (mpeg_demux, "got packet_length %d", packet_length);
  headerlen = 2;
  buf += 2;

  /* loop through looping for stuffing bits, STD, PTS, DTS, etc */
  do {
    guint8 bits = *buf++;

    /* stuffing bytes */
    switch (bits & 0xC0) {
      case 0xC0:
        if (bits == 0xff) {
          GST_DEBUG_OBJECT (mpeg_demux, "have stuffing byte");
        } else {
          GST_DEBUG_OBJECT (mpeg_demux, "expected stuffing byte");
        }
        headerlen++;
        break;
      case 0x40:
        GST_DEBUG_OBJECT (mpeg_demux, "have STD");

        STD_buffer_bound_scale = bits & 0x20;
        STD_buffer_size_bound = ((guint16) (bits & 0x1F)) << 8;
        STD_buffer_size_bound |= *buf++;

        headerlen += 2;
        break;
      case 0x00:
        switch (bits & 0x30) {
          case 0x20:
            /* pts:3 ! 1 ! pts:15 ! 1 | pts:15 ! 1 */
            pts = ((guint64) (bits & 0x0E)) << 29;
            pts |= ((guint64) (*buf++)) << 22;
            pts |= ((guint64) (*buf++ & 0xFE)) << 14;
            pts |= ((guint64) (*buf++)) << 7;
            pts |= ((guint64) (*buf++ & 0xFE)) >> 1;

            GST_DEBUG_OBJECT (mpeg_demux, "PTS = %" G_GUINT64_FORMAT, pts);
            headerlen += 5;
            goto done;
          case 0x30:
            /* pts:3 ! 1 ! pts:15 ! 1 | pts:15 ! 1 */
            pts = ((guint64) (bits & 0x0E)) << 29;
            pts |= ((guint64) (*buf++)) << 22;
            pts |= ((guint64) (*buf++ & 0xFE)) << 14;
            pts |= ((guint64) (*buf++)) << 7;
            pts |= ((guint64) (*buf++ & 0xFE)) >> 1;

            /* sync:4 ! pts:3 ! 1 ! pts:15 ! 1 | pts:15 ! 1 */
            dts = ((guint64) (*buf++ & 0x0E)) << 29;
            dts |= ((guint64) * buf++) << 22;
            dts |= ((guint64) (*buf++ & 0xFE)) << 14;
            dts |= ((guint64) * buf++) << 7;
            dts |= ((guint64) (*buf++ & 0xFE)) >> 1;

            GST_DEBUG_OBJECT (mpeg_demux, "PTS = %" G_GUINT64_FORMAT
                ", DTS = %" G_GUINT64_FORMAT, pts, dts);
            headerlen += 10;
            goto done;
          case 0x00:
            GST_DEBUG_OBJECT (mpeg_demux, "have no pts/dts");
            GST_DEBUG_OBJECT (mpeg_demux, "got trailer bits %x", (bits & 0x0f));
            if ((bits & 0x0f) != 0xf) {
              GST_DEBUG_OBJECT (mpeg_demux, "not a valid packet time sequence");
              return FALSE;
            }
            headerlen++;
          default:
            goto done;
        }
      default:
        goto done;
    }
  } while (1);
  GST_DEBUG_OBJECT (mpeg_demux, "done with header loop");

done:

  /* calculate the amount of real data in this packet */
  datalen = packet_length - headerlen + 2;

  GST_DEBUG_OBJECT (mpeg_demux, "headerlen is %d, datalen is %d",
      headerlen, datalen);

  if (pts != -1) {
    pts += mpeg_parse->adjust;
    timestamp = MPEGTIME_TO_GSTTIME (pts) + mpeg_demux->adjust;

    /* this apparently happens for some input were headers are
     * rewritten to make time start at zero... */
    if ((gint64) timestamp < 0)
      timestamp = 0;
  } else {
    timestamp = GST_CLOCK_TIME_NONE;
  }

  if (id == 0xBD) {
    /* Private stream 1. */
    GST_DEBUG_OBJECT (mpeg_demux, "we have a private 1 packet");
    CLASS (mpeg_demux)->process_private (mpeg_demux, buffer, 0, timestamp,
        headerlen, datalen);
  } else if (id == 0xBF) {
    /* Private stream 2. */
    GST_DEBUG_OBJECT (mpeg_demux, "we have a private 2 packet");
    CLASS (mpeg_demux)->process_private (mpeg_demux, buffer, 1, timestamp,
        headerlen, datalen);
  } else if (id >= 0xC0 && id <= 0xDF) {
    /* Audio. */
    GST_DEBUG_OBJECT (mpeg_demux, "we have an audio packet");
    outstream = CLASS (mpeg_demux)->get_audio_stream (mpeg_demux,
        id - 0xC0, GST_MPEG_DEMUX_AUDIO_MPEG, NULL);
    CLASS (mpeg_demux)->send_subbuffer (mpeg_demux, outstream, buffer,
        timestamp, headerlen + 4, datalen);
  } else if (id >= 0xE0 && id <= 0xEF) {
    /* Video. */
    gint mpeg_version = !GST_MPEG_PARSE_IS_MPEG2 (mpeg_demux) ? 1 : 2;

    GST_DEBUG_OBJECT (mpeg_demux, "we have a video packet");

    outstream = CLASS (mpeg_demux)->get_video_stream (mpeg_demux,
        id - 0xE0, GST_MPEG_DEMUX_VIDEO_MPEG, &mpeg_version);
    CLASS (mpeg_demux)->send_subbuffer (mpeg_demux, outstream, buffer,
        timestamp, headerlen + 4, datalen);
  } else {
    GST_WARNING_OBJECT (mpeg_demux, "unknown stream id 0x%02x", id);
  }

  return TRUE;
}

static gboolean
gst_mpeg_demux_parse_pes (GstMPEGParse * mpeg_parse, GstBuffer * buffer)
{
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (mpeg_parse);
  guint8 id;

  guint16 packet_length;
  guint8 header_data_length = 0;

  guint16 datalen;
  guint16 headerlen;
  GstClockTime timestamp;

  GstMPEGStream *outstream = NULL;
  guint8 *buf;

  buf = GST_BUFFER_DATA (buffer);
  id = *(buf + 3);
  buf += 4;

  /* start parsing */
  packet_length = GST_READ_UINT16_BE (buf);

  GST_DEBUG_OBJECT (mpeg_demux, "packet_length %d", packet_length);
  buf += 2;

  /* we don't operate on: program_stream_map, padding_stream, */
  /* private_stream_2, ECM, EMM, or program_stream_directory  */
  if ((id != 0xBC) && (id != 0xBE) && (id != 0xBF) && (id != 0xF0) &&
      (id != 0xF1) && (id != 0xFF)) {
    guchar flags1 = *buf++;
    guchar flags2 = *buf++;

    if ((flags1 & 0xC0) != 0x80) {
      return FALSE;
    }

    header_data_length = *buf++;

    GST_DEBUG_OBJECT (mpeg_demux, "header_data_length: %d", header_data_length);

    /* check for PTS */
    if ((flags2 & 0x80)) {
      gint64 pts;

      pts = ((guint64) (*buf++ & 0x0E)) << 29;
      pts |= ((guint64) * buf++) << 22;
      pts |= ((guint64) (*buf++ & 0xFE)) << 14;
      pts |= ((guint64) * buf++) << 7;
      pts |= ((guint64) (*buf++ & 0xFE)) >> 1;

      timestamp =
          MPEGTIME_TO_GSTTIME (pts + mpeg_parse->adjust) + mpeg_demux->adjust;

      GST_DEBUG_OBJECT (mpeg_demux,
          "0x%02x (% " G_GINT64_FORMAT ") PTS = %" G_GUINT64_FORMAT, id, pts,
          MPEGTIME_TO_GSTTIME (pts));
    } else {
      timestamp = GST_CLOCK_TIME_NONE;
    }

    if ((flags2 & 0x40)) {
      GST_DEBUG_OBJECT (mpeg_demux, "%x DTS found", id);
      buf += 5;
    }

    if ((flags2 & 0x20)) {
      GST_DEBUG_OBJECT (mpeg_demux, "%x ESCR found", id);
      buf += 6;
    }

    if ((flags2 & 0x10)) {
      guint32 es_rate;

      es_rate = ((guint32) (*buf++ & 0x07)) << 14;
      es_rate |= ((guint32) (*buf++)) << 7;
      es_rate |= ((guint32) (*buf++ & 0xFE)) >> 1;
      GST_DEBUG_OBJECT (mpeg_demux, "%x ES Rate found", id);
    }
    /* FIXME: lots of PES parsing missing here... */

    /* calculate the amount of real data in this PES packet */
    /* constant is 2 bytes packet_length, 2 bytes of bits, 1 byte header len */
    headerlen = 5 + header_data_length;
    /* constant is 2 bytes of bits, 1 byte header len */
    datalen = packet_length - (3 + header_data_length);
  } else {
    /* Deliver the whole packet. */
    /* constant corresponds to the 2 bytes of the packet length. */
    headerlen = 2;
    datalen = packet_length;

    timestamp = GST_CLOCK_TIME_NONE;
  }

  GST_DEBUG_OBJECT (mpeg_demux, "headerlen is %d, datalen is %d",
      headerlen, datalen);

  if (id == 0xBD) {
    /* Private stream 1. */
    GST_DEBUG_OBJECT (mpeg_demux, "we have a private 1 packet");
    CLASS (mpeg_demux)->process_private (mpeg_demux, buffer, 0, timestamp,
        headerlen, datalen);
  } else if (id == 0xBF) {
    /* Private stream 2. */
    GST_DEBUG_OBJECT (mpeg_demux, "we have a private 2 packet");
    CLASS (mpeg_demux)->process_private (mpeg_demux, buffer, 1, timestamp,
        headerlen, datalen);
  } else if (id >= 0xC0 && id <= 0xDF) {
    /* Audio. */
    GST_DEBUG_OBJECT (mpeg_demux, "we have an audio packet");
    outstream = CLASS (mpeg_demux)->get_audio_stream (mpeg_demux,
        id - 0xC0, GST_MPEG_DEMUX_AUDIO_MPEG, NULL);
    CLASS (mpeg_demux)->send_subbuffer (mpeg_demux, outstream, buffer,
        timestamp, headerlen + 4, datalen);
  } else if (id >= 0xE0 && id <= 0xEF) {
    /* Video. */
    gint mpeg_version = !GST_MPEG_PARSE_IS_MPEG2 (mpeg_demux) ? 1 : 2;

    GST_DEBUG_OBJECT (mpeg_demux, "we have a video packet");

    outstream = CLASS (mpeg_demux)->get_video_stream (mpeg_demux,
        id - 0xE0, GST_MPEG_DEMUX_VIDEO_MPEG, &mpeg_version);
    CLASS (mpeg_demux)->send_subbuffer (mpeg_demux, outstream, buffer,
        timestamp, headerlen + 4, datalen);
  } else {
    GST_WARNING_OBJECT (mpeg_demux, "unknown stream id 0x%02x", id);
  }

  return TRUE;
}

static void
gst_mpeg_demux_send_subbuffer (GstMPEGDemux * mpeg_demux,
    GstMPEGStream * outstream, GstBuffer * buffer,
    GstClockTime timestamp, guint offset, guint size)
{
  GstMPEGParse *mpeg_parse = GST_MPEG_PARSE (mpeg_demux);
  GstBuffer *outbuf;

  mpeg_demux->just_flushed = FALSE;

  if (timestamp != GST_CLOCK_TIME_NONE) {
    outstream->cur_ts = timestamp;
    outstream->scr_offs =
        GST_CLOCK_DIFF (timestamp, mpeg_parse->current_ts + mpeg_demux->adjust);
    if (outstream->scr_offs < 0)
      outstream->scr_offs = 0;

    if (mpeg_demux->index != NULL) {
      /* Register a new index position. */
      gst_index_add_association (mpeg_demux->index,
          outstream->index_id, 0,
          GST_FORMAT_BYTES,
          GST_BUFFER_OFFSET (buffer), GST_FORMAT_TIME, timestamp, 0);
    }
  } else {
    outstream->cur_ts =
        mpeg_parse->current_ts + mpeg_demux->adjust + outstream->scr_offs;
  }

  if (!GST_PAD_IS_USABLE (outstream->pad) || (size == 0)) {
    return;
  }

  GST_DEBUG_OBJECT (mpeg_demux, "Creating subbuffer size %d, time=%"
      GST_TIME_FORMAT, size, GST_TIME_ARGS (timestamp));
  outbuf = gst_buffer_create_sub (buffer, offset, size);

  GST_BUFFER_TIMESTAMP (outbuf) = timestamp;
  GST_BUFFER_OFFSET (outbuf) = GST_BUFFER_OFFSET (buffer) + offset;

  gst_pad_push (outstream->pad, GST_DATA (outbuf));

  if (GST_CLOCK_TIME_IS_VALID (mpeg_demux->max_gap) &&
      GST_CLOCK_TIME_IS_VALID (mpeg_parse->current_ts) &&
      (mpeg_parse->current_ts + mpeg_demux->adjust > mpeg_demux->max_gap)) {
    GstClockTime threshold =
        GST_CLOCK_DIFF (mpeg_parse->current_ts + mpeg_demux->adjust,
        mpeg_demux->max_gap);

    CLASS (mpeg_demux)->synchronise_pads (mpeg_demux, threshold,
        mpeg_parse->current_ts + mpeg_demux->adjust -
        mpeg_demux->max_gap_tolerance);
  }
}

static void
gst_mpeg_demux_process_private (GstMPEGDemux * mpeg_demux,
    GstBuffer * buffer,
    guint stream_nr, GstClockTime timestamp, guint headerlen, guint datalen)
{
  GstMPEGStream *outstream;

  outstream = CLASS (mpeg_demux)->get_private_stream (mpeg_demux,
      stream_nr, GST_MPEG_DEMUX_PRIVATE_UNKNOWN, NULL);
  CLASS (mpeg_demux)->send_subbuffer (mpeg_demux, outstream, buffer,
      timestamp, headerlen + 4, datalen);
}

static void
gst_mpeg_demux_synchronise_pads (GstMPEGDemux * mpeg_demux,
    GstClockTime threshold, GstClockTime new_ts)
{
  /*
   * Send a filler event to any pad with cur_ts < threshold to catch it up
   */
  gint i;

  for (i = 0; i < GST_MPEG_DEMUX_NUM_VIDEO_STREAMS; i++)
    if (mpeg_demux->video_stream[i]
        && mpeg_demux->video_stream[i]->cur_ts < threshold) {
      CLASS (mpeg_demux)->sync_stream_to_time (mpeg_demux,
          mpeg_demux->video_stream[i], new_ts);
      mpeg_demux->video_stream[i]->cur_ts = new_ts;
    }

  for (i = 0; i < GST_MPEG_DEMUX_NUM_AUDIO_STREAMS; i++)
    if (mpeg_demux->audio_stream[i]
        && mpeg_demux->audio_stream[i]->cur_ts < threshold) {
      CLASS (mpeg_demux)->sync_stream_to_time (mpeg_demux,
          mpeg_demux->audio_stream[i], new_ts);
      mpeg_demux->audio_stream[i]->cur_ts = new_ts;
    }

  for (i = 0; i < GST_MPEG_DEMUX_NUM_PRIVATE_STREAMS; i++)
    if (mpeg_demux->private_stream[i]
        && mpeg_demux->private_stream[i]->cur_ts < threshold) {
      CLASS (mpeg_demux)->sync_stream_to_time (mpeg_demux,
          mpeg_demux->private_stream[i], new_ts);
      mpeg_demux->private_stream[i]->cur_ts = new_ts;
    }
}

/* Send a filler event on the indicated pad to catch it up to
 * last_ts. Query the pad for current time, and use that time
 * to set the duration of the filler event, otherwise we use
 * the last timestamp of the stream and rely on the sinks
 * to absorb any overlap with the decoded data.
 */
static void
gst_mpeg_demux_sync_stream_to_time (GstMPEGDemux * mpeg_demux,
    GstMPEGStream * stream, GstClockTime last_ts)
{
  GstClockTime start_ts;
  GstEvent *filler = NULL;
  GstFormat fmt = GST_FORMAT_TIME;

  if (!GST_PAD_PEER (stream->pad)
      || !gst_pad_query (GST_PAD_PEER (stream->pad), GST_QUERY_POSITION, &fmt,
          (gint64 *) & start_ts)) {
    start_ts = stream->cur_ts;
  }

  if (start_ts < last_ts) {
    filler = gst_event_new_filler_stamped (start_ts, GST_CLOCK_DIFF (last_ts,
            start_ts));
  }

  if (filler) {
    if (GST_PAD_IS_USABLE (stream->pad)) {
      GST_LOG ("Advancing %s from %llu by %lld to %llu (diff %lld)",
          gst_pad_get_name (stream->pad), stream->cur_ts,
          gst_event_filler_get_duration (filler), last_ts,
          GST_CLOCK_DIFF (last_ts, stream->cur_ts));

      gst_pad_push (stream->pad, GST_DATA (filler));
    } else
      gst_event_unref (filler);
  }
}

const GstFormat *
gst_mpeg_demux_get_src_formats (GstPad * pad)
{
  static const GstFormat formats[] = {
    GST_FORMAT_TIME,            /* we prefer seeking on time */
    GST_FORMAT_BYTES,
    0
  };

  return formats;
}

static gboolean
index_seek (GstPad * pad, GstEvent * event, gint64 * offset)
{
  GstIndexEntry *entry;
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (gst_pad_get_parent (pad));
  GstMPEGStream *stream = gst_pad_get_element_private (pad);

  entry = gst_index_get_assoc_entry (mpeg_demux->index, stream->index_id,
      GST_INDEX_LOOKUP_BEFORE, 0,
      GST_EVENT_SEEK_FORMAT (event), GST_EVENT_SEEK_OFFSET (event));
  if (!entry) {
    GST_CAT_WARNING (GST_CAT_SEEK, "%s:%s index %s %" G_GINT64_FORMAT
        " -> failed",
        GST_DEBUG_PAD_NAME (pad),
        gst_format_get_details (GST_EVENT_SEEK_FORMAT (event))->nick,
        GST_EVENT_SEEK_OFFSET (event));
    return FALSE;
  }

  if (gst_index_entry_assoc_map (entry, GST_FORMAT_BYTES, offset)) {
    GST_CAT_DEBUG (GST_CAT_SEEK, "%s:%s index %s %" G_GINT64_FORMAT
        " -> %" G_GINT64_FORMAT " bytes",
        GST_DEBUG_PAD_NAME (pad),
        gst_format_get_details (GST_EVENT_SEEK_FORMAT (event))->nick,
        GST_EVENT_SEEK_OFFSET (event), *offset);
    return TRUE;
  }
  return FALSE;
}

static gboolean
normal_seek (GstPad * pad, GstEvent * event, gint64 * offset)
{
  gboolean res = FALSE;
  gint64 adjust;
  GstFormat format;
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (gst_pad_get_parent (pad));

  format = GST_EVENT_SEEK_FORMAT (event);

  res = gst_pad_convert (pad, GST_FORMAT_BYTES, mpeg_demux->total_size_bound,
      &format, &adjust);

  if (res) {
    *offset = MAX (GST_EVENT_SEEK_OFFSET (event) - adjust, 0);

    GST_CAT_DEBUG (GST_CAT_SEEK, "%s:%s guesstimate %" G_GINT64_FORMAT
        " %s -> %" G_GINT64_FORMAT
        " (total_size_bound = %" G_GINT64_FORMAT ")",
        GST_DEBUG_PAD_NAME (pad),
        GST_EVENT_SEEK_OFFSET (event),
        gst_format_get_details (GST_EVENT_SEEK_FORMAT (event))->nick,
        *offset, mpeg_demux->total_size_bound);
  }

  return res;
}

static gboolean
gst_mpeg_demux_handle_src_event (GstPad * pad, GstEvent * event)
{
  gboolean res = FALSE;
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
    {
      gint64 desired_offset;

      if (mpeg_demux->index)
        res = index_seek (pad, event, &desired_offset);
      if (!res)
        res = normal_seek (pad, event, &desired_offset);

      if (res) {
        GstEvent *new_event;

        new_event =
            gst_event_new_seek (GST_EVENT_SEEK_TYPE (event), desired_offset);
        res = gst_mpeg_parse_handle_src_event (pad, new_event);
      }
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_NAVIGATION:
    {
      GstPad *out = GST_PAD_PEER (GST_MPEG_PARSE (mpeg_demux)->sinkpad);

      if (out && GST_PAD_IS_USABLE (out))
        return gst_pad_send_event (out, event);
    }
      /* fall-through */
    default:
      gst_event_unref (event);
      break;
  }
  return res;
}

static gboolean
gst_mpeg_demux_handle_src_query (GstPad * pad, GstQueryType type,
    GstFormat * format, gint64 * value)
{
  gboolean res;

  res = gst_mpeg_parse_handle_src_query (pad, type, format, value);

  if (res && (type == GST_QUERY_POSITION) && (format)
      && (*format == GST_FORMAT_TIME)) {
    GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (gst_pad_get_parent (pad));

    *value += mpeg_demux->adjust;
  }

  return res;
}

static void
gst_mpeg_demux_reset (GstMPEGDemux * mpeg_demux)
{
  int i;

  /* Reset the element */

  GST_INFO ("Resetting the MPEG Demuxer");

  /* free the streams , remove the pads */
  /* filled in init_stream */
  /* check get_audio/video_stream because it can be derivated */
  for (i = 0; i < GST_MPEG_DEMUX_NUM_VIDEO_STREAMS; i++)
    if (mpeg_demux->video_stream[i]) {
      if (GST_PAD_IS_USABLE (mpeg_demux->video_stream[i]->pad)) {
        gst_pad_push (mpeg_demux->video_stream[i]->pad,
            GST_DATA (gst_event_new (GST_EVENT_EOS)));
      }
      gst_element_remove_pad (GST_ELEMENT (mpeg_demux),
          mpeg_demux->video_stream[i]->pad);
      g_free (mpeg_demux->video_stream[i]);
      mpeg_demux->video_stream[i] = NULL;
    }
  for (i = 0; i < GST_MPEG_DEMUX_NUM_AUDIO_STREAMS; i++)
    if (mpeg_demux->audio_stream[i]) {
      if (GST_PAD_IS_USABLE (mpeg_demux->audio_stream[i]->pad)) {
        gst_pad_push (mpeg_demux->audio_stream[i]->pad,
            GST_DATA (gst_event_new (GST_EVENT_EOS)));
      }
      gst_element_remove_pad (GST_ELEMENT (mpeg_demux),
          mpeg_demux->audio_stream[i]->pad);
      g_free (mpeg_demux->audio_stream[i]);
      mpeg_demux->audio_stream[i] = NULL;
    }
  for (i = 0; i < GST_MPEG_DEMUX_NUM_PRIVATE_STREAMS; i++)
    if (mpeg_demux->private_stream[i]) {
      if (GST_PAD_IS_USABLE (mpeg_demux->private_stream[i]->pad)) {
        gst_pad_push (mpeg_demux->private_stream[i]->pad,
            GST_DATA (gst_event_new (GST_EVENT_EOS)));
      }
      gst_element_remove_pad (GST_ELEMENT (mpeg_demux),
          mpeg_demux->private_stream[i]->pad);
      g_free (mpeg_demux->private_stream[i]);
      mpeg_demux->private_stream[i] = NULL;
    }

  mpeg_demux->in_flush = FALSE;
  mpeg_demux->header_length = 0;
  mpeg_demux->rate_bound = 0;
  mpeg_demux->audio_bound = 0;
  mpeg_demux->video_bound = 0;
  mpeg_demux->fixed = FALSE;
  mpeg_demux->constrained = FALSE;
  mpeg_demux->audio_lock = FALSE;
  mpeg_demux->video_lock = FALSE;

  mpeg_demux->packet_rate_restriction = FALSE;
  mpeg_demux->total_size_bound = 0LL;

  mpeg_demux->index = NULL;

  /*
   * Don't adjust things that are only for subclass use
   * - if they changed it, they can reset it.
   *
   * mpeg_demux->adjust = 0;
   * mpeg_demux->max_gap = GST_CLOCK_TIME_NONE;
   * mpeg_demux->max_gap_tolerance = GST_CLOCK_TIME_NONE;
   * mpeg_demux->just_flushed = FALSE;
   */
}

static GstElementStateReturn
gst_mpeg_demux_change_state (GstElement * element)
{
  GstMPEGDemux *mpeg_demux = GST_MPEG_DEMUX (element);

  switch (GST_STATE_TRANSITION (element)) {
    case GST_STATE_PAUSED_TO_READY:
      gst_mpeg_demux_reset (mpeg_demux);
      break;
    default:
      break;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element);
}

static void
gst_mpeg_demux_set_index (GstElement * element, GstIndex * index)
{
  GstMPEGDemux *mpeg_demux;

  GST_ELEMENT_CLASS (parent_class)->set_index (element, index);

  mpeg_demux = GST_MPEG_DEMUX (element);

  mpeg_demux->index = index;
}

static GstIndex *
gst_mpeg_demux_get_index (GstElement * element)
{
  GstMPEGDemux *mpeg_demux;

  mpeg_demux = GST_MPEG_DEMUX (element);

  return mpeg_demux->index;
}


gboolean
gst_mpeg_demux_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "mpegdemux",
      GST_RANK_SECONDARY, GST_TYPE_MPEG_DEMUX);
}
