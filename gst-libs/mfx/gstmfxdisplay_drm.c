#include "sysdeps.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libudev.h>
#include <xf86drm.h>
#include <va/va_drm.h>
#include "gstmfxtypes.h"
#include "gstmfxdisplay_priv.h"
#include "gstmfxdisplay_drm.h"
#include "gstmfxdisplay_drm_priv.h"
#include "gstmfxwindow_drm.h"


static const guint g_display_types = 1U << GST_MFX_DISPLAY_TYPE_DRM;

typedef enum
{
  DRM_DEVICE_LEGACY = 1,
  DRM_DEVICE_RENDERNODES,
} DRMDeviceType;

static DRMDeviceType g_drm_device_type;
static GMutex g_drm_device_type_lock;

/* Get default device path. Actually, the first match in the DRM subsystem */
static const gchar *
get_default_device_path (GstMfxDisplay * display)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);
  const gchar *syspath, *devpath;
  struct udev *udev = NULL;
  struct udev_device *device, *parent;
  struct udev_enumerate *e = NULL;
  struct udev_list_entry *l;
  int fd;

  if (!priv->device_path_default) {
    udev = udev_new ();
    if (!udev)
      goto end;

    e = udev_enumerate_new (udev);
    if (!e)
      goto end;

    udev_enumerate_add_match_subsystem (e, "drm");
    switch (g_drm_device_type) {
      case DRM_DEVICE_LEGACY:
        udev_enumerate_add_match_sysname (e, "card[0-9]*");
        break;
      case DRM_DEVICE_RENDERNODES:
        udev_enumerate_add_match_sysname (e, "renderD[0-9]*");
        break;
      default:
        GST_ERROR ("unknown drm device type (%d)", g_drm_device_type);
        goto end;
    }
    udev_enumerate_scan_devices (e);
    udev_list_entry_foreach (l, udev_enumerate_get_list_entry (e)) {
      syspath = udev_list_entry_get_name (l);
      device = udev_device_new_from_syspath (udev, syspath);
      parent = udev_device_get_parent (device);
      if (strcmp (udev_device_get_subsystem (parent), "pci") != 0) {
        udev_device_unref (device);
        continue;
      }

      devpath = udev_device_get_devnode (device);
      fd = open (devpath, O_RDWR | O_CLOEXEC);
      if (fd < 0) {
        udev_device_unref (device);
        continue;
      }

      priv->device_path_default = g_strdup (devpath);
      close (fd);
      udev_device_unref (device);
      break;
    }

  end:
    if (e)
      udev_enumerate_unref (e);
    if (udev)
      udev_unref (udev);
  }
  return priv->device_path_default;
}

/* Reconstruct a device path without our prefix */
static const gchar *
get_device_path (GstMfxDisplay * display)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);
  const gchar *device_path = priv->device_path;

  if (!device_path || *device_path == '\0')
    return NULL;
  return device_path;
}

/* Mangle device path with our prefix */
static gboolean
set_device_path (GstMfxDisplay * display, const gchar * device_path)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);

  g_free (priv->device_path);
  priv->device_path = NULL;

  if (!device_path) {
    device_path = get_default_device_path (display);
    if (!device_path)
      return FALSE;
  }
  priv->device_path = g_strdup (device_path);
  return priv->device_path != NULL;
}

/* Set device path from file descriptor */
static gboolean
set_device_path_from_fd (GstMfxDisplay * display, gint drm_device)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);
  const gchar *busid, *path, *str;
  gsize busid_length, path_length;
  struct udev *udev = NULL;
  struct udev_device *device;
  struct udev_enumerate *e = NULL;
  struct udev_list_entry *l;
  gboolean success = FALSE;

  g_free (priv->device_path);
  priv->device_path = NULL;

  if (drm_device < 0)
    goto end;

  busid = drmGetBusid (drm_device);
  if (!busid)
    goto end;
  if (strncmp (busid, "pci:", 4) != 0)
    goto end;
  busid += 4;
  busid_length = strlen (busid);

  udev = udev_new ();
  if (!udev)
    goto end;

  e = udev_enumerate_new (udev);
  if (!e)
    goto end;

  udev_enumerate_add_match_subsystem (e, "drm");
  udev_enumerate_scan_devices (e);
  udev_list_entry_foreach (l, udev_enumerate_get_list_entry (e)) {
    path = udev_list_entry_get_name (l);
    str = strstr (path, busid);
    if (!str || str <= path || str[-1] != '/')
      continue;

    path_length = strlen (path);
    if (str + busid_length >= path + path_length)
      continue;
    if (strncmp (&str[busid_length], "/drm/card", 9) != 0 &&
        strncmp (&str[busid_length], "/drm/renderD", 12) != 0)
      continue;

    device = udev_device_new_from_syspath (udev, path);
    if (!device)
      continue;

    path = udev_device_get_devnode (device);
    priv->device_path = g_strdup (path);
    udev_device_unref (device);
    break;
  }
  success = TRUE;

end:
  if (e)
    udev_enumerate_unref (e);
  if (udev)
    udev_unref (udev);
  return success;
}

static gboolean
gst_mfx_display_drm_bind_display (GstMfxDisplay * display,
    gpointer native_display)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);

  priv->drm_device = GPOINTER_TO_INT (native_display);
  priv->use_foreign_display = TRUE;

  if (!set_device_path_from_fd (display, priv->drm_device))
    return FALSE;
  return TRUE;
}

static gboolean
gst_mfx_display_drm_open_display (GstMfxDisplay * display,
    const gchar * name)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);
  GstMfxDisplayCache *const cache = GST_MFX_DISPLAY_CACHE (display);
  const GstMfxDisplayInfo *info;

  if (!set_device_path (display, name))
    return FALSE;

  info = gst_mfx_display_cache_lookup_by_name (cache, priv->device_path,
      g_display_types);
  if (info) {
    priv->drm_device = GPOINTER_TO_INT (info->native_display);
    priv->use_foreign_display = TRUE;
  } else {
    priv->drm_device = open (get_device_path (display), O_RDWR | O_CLOEXEC);
    if (priv->drm_device < 0)
      return FALSE;
    priv->use_foreign_display = FALSE;
  }
  return TRUE;
}

static void
gst_mfx_display_drm_close_display (GstMfxDisplay * display)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);

  if (priv->drm_device >= 0) {
    if (!priv->use_foreign_display)
      close (priv->drm_device);
    priv->drm_device = -1;
  }

  if (priv->device_path) {
    g_free (priv->device_path);
    priv->device_path = NULL;
  }

  if (priv->device_path_default) {
    g_free (priv->device_path_default);
    priv->device_path_default = NULL;
  }
}

static gboolean
gst_mfx_display_drm_get_display_info (GstMfxDisplay * display,
    GstMfxDisplayInfo * info)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);
  GstMfxDisplayCache *const cache = GST_MFX_DISPLAY_CACHE (display);
  const GstMfxDisplayInfo *cached_info;

  /* Return any cached info even if child has its own VA display */
  cached_info = gst_mfx_display_cache_lookup_by_native_display (cache,
      GINT_TO_POINTER (priv->drm_device), g_display_types);
  if (cached_info) {
    *info = *cached_info;
    return TRUE;
  }

  /* Otherwise, create VA display if there is none already */
  info->native_display = GINT_TO_POINTER (priv->drm_device);
  info->display_name = priv->device_path;
  if (!info->va_display) {
    info->va_display = vaGetDisplayDRM (priv->drm_device);
    if (!info->va_display)
      return FALSE;
    info->display_type = GST_MFX_DISPLAY_TYPE_DRM;
  }
  return TRUE;
}

static GstMfxWindow *
gst_mfx_display_drm_create_window (GstMfxDisplay * display, GstMfxID id,
    guint width, guint height)
{
  return id != GST_MFX_ID_INVALID ?
      NULL : gst_mfx_window_drm_new (display, width, height);
}

static void
gst_mfx_display_drm_init (GstMfxDisplay * display)
{
  GstMfxDisplayDRMPrivate *const priv =
      GST_MFX_DISPLAY_DRM_PRIVATE (display);

  priv->drm_device = -1;
}

static void
gst_mfx_display_drm_class_init (GstMfxDisplayDRMClass * klass)
{
  GstMfxMiniObjectClass *const object_class =
      GST_MFX_MINI_OBJECT_CLASS (klass);
  GstMfxDisplayClass *const dpy_class = GST_MFX_DISPLAY_CLASS (klass);

  gst_mfx_display_class_init (&klass->parent_class);

  object_class->size = sizeof (GstMfxDisplayDRM);
  dpy_class->display_type = GST_MFX_DISPLAY_TYPE_DRM;
  dpy_class->init = gst_mfx_display_drm_init;
  dpy_class->bind_display = gst_mfx_display_drm_bind_display;
  dpy_class->open_display = gst_mfx_display_drm_open_display;
  dpy_class->close_display = gst_mfx_display_drm_close_display;
  dpy_class->get_display = gst_mfx_display_drm_get_display_info;
  dpy_class->create_window = gst_mfx_display_drm_create_window;
}

static inline const GstMfxDisplayClass *
gst_mfx_display_drm_class (void)
{
  static GstMfxDisplayDRMClass g_class;
  static gsize g_class_init = FALSE;

  if (g_once_init_enter (&g_class_init)) {
    gst_mfx_display_drm_class_init (&g_class);
    g_once_init_leave (&g_class_init, TRUE);
  }
  return GST_MFX_DISPLAY_CLASS (&g_class);
}

/**
 * gst_mfx_display_drm_new:
 * @device_path: the DRM device path
 *
 * Opens an DRM file descriptor using @device_path and returns a newly
 * allocated #GstMfxDisplay object. The DRM display will be cloed
 * when the reference count of the object reaches zero.
 *
 * If @device_path is NULL, the DRM device path will be automatically
 * determined as the first positive match in the list of available DRM
 * devices.
 *
 * Return value: a newly allocated #GstMfxDisplay object
 */
GstMfxDisplay *
gst_mfx_display_drm_new (const gchar * device_path)
{
  GstMfxDisplay *display;
  guint types[2], i, num_types = 0;

  g_mutex_lock (&g_drm_device_type_lock);
  if (device_path)
    types[num_types++] = 0;
  else if (g_drm_device_type)
    types[num_types++] = g_drm_device_type;
  else {
    types[num_types++] = DRM_DEVICE_RENDERNODES;
    types[num_types++] = DRM_DEVICE_LEGACY;
  }

  for (i = 0; i < num_types; i++) {
    g_drm_device_type = types[i];
    display = gst_mfx_display_new (gst_mfx_display_drm_class (),
        GST_MFX_DISPLAY_INIT_FROM_DISPLAY_NAME, (gpointer) device_path);
    if (display || device_path)
      break;
  }
  g_mutex_unlock (&g_drm_device_type_lock);
  return display;
}

/**
 * gst_mfx_display_drm_new_with_device:
 * @device: an open DRM device (file descriptor)
 *
 * Creates a #GstMfxDisplay based on the open DRM @device. The
 * caller still owns the device file descriptor and must call close()
 * when all #GstMfxDisplay references are released. Doing so too
 * early can yield undefined behaviour.
 *
 * Return value: a newly allocated #GstMfxDisplay object
 */
GstMfxDisplay *
gst_mfx_display_drm_new_with_device (gint device)
{
  g_return_val_if_fail (device >= 0, NULL);

  return gst_mfx_display_new (gst_mfx_display_drm_class (),
      GST_MFX_DISPLAY_INIT_FROM_NATIVE_DISPLAY, GINT_TO_POINTER (device));
}

/**
 * gst_mfx_display_drm_get_device:
 * @display: a #GstMfxDisplayDRM
 *
 * Returns the underlying DRM device file descriptor that was created
 * by gst_mfx_display_drm_new() or that was bound from
 * gst_mfx_display_drm_new_with_device().
 *
 * Return value: the DRM file descriptor attached to @display
 */
gint
gst_mfx_display_drm_get_device (GstMfxDisplayDRM * display)
{
  g_return_val_if_fail (GST_MFX_IS_DISPLAY_DRM (display), -1);

  return GST_MFX_DISPLAY_DRM_DEVICE (display);
}

/**
 * gst_mfx_display_drm_get_device_path:
 * @display: a #GstMfxDisplayDRM
 *
 * Returns the underlying DRM device path name was created by
 * gst_mfx_display_drm_new() or that was bound from
 * gst_mfx_display_drm_new_with_device().
 *
 * Note: the #GstMfxDisplayDRM object owns the resulting string, so
 * it shall not be deallocated.
 *
 * Return value: the DRM device path name attached to @display
 */
const gchar *
gst_mfx_display_drm_get_device_path (GstMfxDisplayDRM * display)
{
  g_return_val_if_fail (GST_MFX_IS_DISPLAY_DRM (display), NULL);

  return get_device_path (GST_MFX_DISPLAY_CAST (display));
}