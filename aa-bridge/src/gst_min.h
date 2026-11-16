// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal, era-safe GStreamer 1.x / GLib C bindings.
//
// WHY HAND-ROLLED, NOT <gst/gst.h>:
// The target unit (TI DRA7 / Jacinto-6) ships GStreamer 1.2.3 and GLib 2.38 as .so
// ONLY — there are no gstreamer/glib dev headers in the device sysroot. Overlaying
// modern (bookworm 1.22 / glib 2.74) headers would risk emitting references to inline
// helpers/symbols newer than the 1.2.3 .so. Instead we declare exactly the handful of
// entry points we call. Every symbol below has existed since GStreamer 1.0 / GLib 2.x,
// so it resolves against the device's 1.2.3 .so AND against any newer desktop GStreamer
// used for the off-hardware smoke test — one source, both targets. The link step
// against the real 1.2.3 .so is the backstop: any accidentally-too-new symbol would
// fail as an undefined reference.
//
// All GStreamer objects are treated as OPAQUE pointers — we never dereference them, so
// struct-layout drift between versions is irrelevant. The only struct we read is
// GError, whose 3-field layout is public, stable GLib ABI. (The pad probes only count
// buffers: their GstPadProbeInfo stays opaque too. A bus message's type is told from its
// structure, which an EOS does not have, rather than from the GstMessage layout.)
#ifndef AA_BRIDGE_GST_MIN_H
#define AA_BRIDGE_GST_MIN_H

#include <cstddef>
#include <cstdint>

#include <dlfcn.h>

extern "C" {

typedef char gchar;
typedef int gint;
typedef int gboolean;
typedef unsigned int guint;
typedef unsigned int guint32; // GQuark is a guint32
typedef void *gpointer;
typedef const void *gconstpointer;
typedef std::size_t gsize;
typedef std::uint64_t guint64;
typedef unsigned long gulong;
typedef guint64 GstClockTime;
typedef void (*GDestroyNotify)(gpointer data);

// Opaque handle types.
typedef struct _GstElement GstElement;
typedef struct _GstBuffer GstBuffer;
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GstAppSrc GstAppSrc;
typedef struct _GstStructure GstStructure;
typedef struct _GstAllocator GstAllocator;
typedef struct _GstAllocationParams GstAllocationParams;
typedef struct _GstPad GstPad;
typedef struct _GstPadProbeInfo GstPadProbeInfo;

// Public, stable GLib ABI (glib/gerror.h).
typedef struct {
    guint32 domain; // GQuark
    gint code;
    gchar *message;
} GError;

typedef enum {
    GST_STATE_VOID_PENDING = 0,
    GST_STATE_NULL = 1,
    GST_STATE_READY = 2,
    GST_STATE_PAUSED = 3,
    GST_STATE_PLAYING = 4
} GstState;

typedef enum {
    GST_STATE_CHANGE_FAILURE = 0,
    GST_STATE_CHANGE_SUCCESS = 1,
    GST_STATE_CHANGE_ASYNC = 2,
    GST_STATE_CHANGE_NO_PREROLL = 3
} GstStateChangeReturn;

typedef enum { GST_FLOW_OK = 0 } GstFlowReturn;

// GstMessageType is a flags enum; we filter only these two (and both at once).
typedef enum {
    GST_MESSAGE_EOS = (1 << 0),
    GST_MESSAGE_ERROR = (1 << 1),
    GST_MESSAGE_EOS_OR_ERROR = (1 << 0) | (1 << 1)
} GstMessageType;

// Values unchanged since 1.0 (gstpad.h).
typedef enum { GST_PAD_PROBE_TYPE_BUFFER = (1 << 4) } GstPadProbeType;
typedef enum { GST_PAD_PROBE_OK = 1 } GstPadProbeReturn;
typedef GstPadProbeReturn (*GstPadProbeCallback)(GstPad *pad, GstPadProbeInfo *info,
                                                 gpointer user_data);

// --- libgstreamer-1.0 ---
void gst_init(int *argc, char **argv[]);
GstElement *gst_parse_launch(const gchar *description, GError **error);
GstElement *gst_bin_get_by_name(GstElement *bin, const gchar *name);
GstStateChangeReturn gst_element_set_state(GstElement *element, GstState state);
GstBus *gst_element_get_bus(GstElement *element);
GstMessage *gst_bus_timed_pop_filtered(GstBus *bus, GstClockTime timeout, GstMessageType types);
void gst_message_parse_error(GstMessage *message, GError **gerror, gchar **debug);
const GstStructure *gst_message_get_structure(GstMessage *message); // NULL for an EOS
GstBuffer *gst_buffer_new_allocate(GstAllocator *allocator, gsize size,
                                   GstAllocationParams *params);
gsize gst_buffer_fill(GstBuffer *buffer, gsize offset, gconstpointer src, gsize size);
GstPad *gst_element_get_static_pad(GstElement *element, const gchar *name);
gulong gst_pad_add_probe(GstPad *pad, GstPadProbeType mask, GstPadProbeCallback callback,
                         gpointer user_data, GDestroyNotify destroy_data);
void gst_object_unref(gpointer object);
void gst_mini_object_unref(gpointer mini_object); // gst_message_unref() is a macro over this

// --- libgstapp-1.0 ---
GstFlowReturn gst_app_src_push_buffer(GstAppSrc *appsrc, GstBuffer *buffer);
GstFlowReturn gst_app_src_end_of_stream(GstAppSrc *appsrc);

} // extern "C"

// Frees what gst_parse_launch and gst_message_parse_error hand back (g_error_free, g_free).
// They live in libglib-2.0, which GStreamer has loaded: looked up at run time, so the bridge
// links no GLib of its own.
inline void aa_free_gerror(GError *err, gchar *debug = nullptr) {
    typedef void (*ErrorFree)(GError *);
    typedef void (*Free)(gpointer);
    static const ErrorFree error_free = reinterpret_cast<ErrorFree>(dlsym(RTLD_DEFAULT, "g_error_free"));
    static const Free free_fn = reinterpret_cast<Free>(dlsym(RTLD_DEFAULT, "g_free"));
    if (err && error_free) error_free(err);
    if (debug && free_fn) free_fn(debug);
}

#endif // AA_BRIDGE_GST_MIN_H
