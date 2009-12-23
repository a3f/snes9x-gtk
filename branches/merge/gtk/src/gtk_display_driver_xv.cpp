#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "gtk_display.h"
#include "gtk_display_driver_xv.h"

static int
get_inv_shift (uint32 mask, int bpp)
{
    int i;

    /* Find mask */
    for (i = 0; (i < bpp) && !(mask & (1 << i)); i++) {};

    /* Find start of mask */
    for (; (i < bpp) && (mask & (1 << i)); i++) {};

    return (bpp - i);
}

S9xXVDisplayDriver::S9xXVDisplayDriver (Snes9xWindow *window,
                                          Snes9xConfig *config)
{
    this->window = window;
    this->config = config;
    this->drawing_area = GTK_WIDGET (window->drawing_area);
    display =
        gdk_x11_drawable_get_xdisplay (GDK_DRAWABLE (drawing_area->window));

    return;
}

void
S9xXVDisplayDriver::update (int width, int height)
{
    int   c_width, c_height, final_pitch;
    uint8 *final_buffer;
    GdkGC *gc = drawing_area->style->bg_gc[GTK_WIDGET_STATE (drawing_area)];

    c_width = drawing_area->allocation.width;
    c_height = drawing_area->allocation.height;

    if (width == SIZE_FLAG_DIRTY)
    {
        this->clear ();
        return;
    }

    if (width <= 0)
        return;

    if (config->scale_method > 0)
    {
        uint8 *src_buffer = (uint8 *) padded_buffer[0];
        uint8 *dst_buffer = (uint8 *) padded_buffer[1];
        int   src_pitch = image_width * image_bpp;
        int   dst_pitch = scaled_max_width * image_bpp;

        S9xFilter (src_buffer,
                   src_pitch,
                   dst_buffer,
                   dst_pitch,
                   width,
                   height);

        final_buffer = (uint8 *) padded_buffer[1];
        final_pitch = dst_pitch;
    }
    else
    {
        final_buffer = (uint8 *) padded_buffer[0];
        final_pitch = image_width * image_bpp;
    }

    if (!config->scale_to_fit &&
            (width > gdk_buffer_width || height > gdk_buffer_height))
    {
        this->clear ();

        return;
    }

    update_image_size (width, height);

    if (format == FOURCC_YUY2)
    {
        S9xConvertYUV (final_buffer,
                       (uint8 *) xv_image->data,
                       final_pitch,
                       2 * xv_image->width,
                       width + (width < xv_image->width ? (width % 2) + 4 : 0),
                       height + (height < xv_image->height ? 4 : 0));
    }
    else
    {
        S9xConvertMask (final_buffer,
                        (uint8 *) xv_image->data,
                        final_pitch,
                        bytes_per_pixel * xv_image->width,
                        width + (width < xv_image->width ? (width % 2) + 4 : 0),
                        height + (height < xv_image->height ? 4 : 0),
                        rshift,
                        gshift,
                        bshift,
                        bpp);
    }

    if (config->scale_to_fit)
    {
        double screen_aspect = (double) c_width / (double) c_height;
        double snes_aspect = S9xGetAspect ();
        double granularity = 1.0 / (double) MAX (c_width, c_height);

        if (config->maintain_aspect_ratio &&
            !(screen_aspect <= snes_aspect * (1.0 + granularity) &&
              screen_aspect >= snes_aspect * (1.0 - granularity)))
        {
            if (screen_aspect > snes_aspect)
            {
                XvShmPutImage (display,
                               xv_portid,
                               GDK_WINDOW_XWINDOW (drawing_area->window),
                               GDK_GC_XGC (gc),
                               xv_image,
                               0, 0,
                               width, height,
                               (c_width - (int) (c_height * snes_aspect)) / 2,
                               0,
                               (int) (c_height * snes_aspect),
                               c_height,
                               True);

                window->set_mouseable_area ((c_width -
                        (int) (c_height * snes_aspect)) / 2,
                        0,
                        (int) (c_height * snes_aspect),
                        c_height);
            }
            else
            {
                XvShmPutImage (display,
                               xv_portid,
                               GDK_WINDOW_XWINDOW (drawing_area->window),
                               GDK_GC_XGC (gc),
                               xv_image,
                               0, 0,
                               width, height,
                               0,
                               (c_height - c_width / snes_aspect) / 2,
                               c_width, (c_width / snes_aspect),
                               True);

                window->set_mouseable_area (
                    0,
                    (c_height - (int)(c_width / snes_aspect)) / 2,
                    c_width,
                    (int)(c_width / snes_aspect));

            }
        }
        else
        {
            XvShmPutImage (display,
                           xv_portid,
                           GDK_WINDOW_XWINDOW (drawing_area->window),
                           GDK_GC_XGC (gc),
                           xv_image,
                           0, 0,
                           width, height,
                           0, 0,
                           c_width, c_height,
                           True);

            window->set_mouseable_area (0, 0, c_width, c_height);
        }
    }
    else
    {
        XvShmPutImage (display,
                       xv_portid,
                       GDK_WINDOW_XWINDOW (drawing_area->window),
                       GDK_GC_XGC (gc),
                       xv_image,
                       0, 0,
                       width, height,
                       (c_width - width) / 2, (c_height - height) / 2,
                       width, height,
                       True);

        window->set_mouseable_area ((c_width - width) / 2,
                                    (c_height - height) / 2,
                                    width,
                                    height);
    }

    XSync (display, False);

    return;
}

void
S9xXVDisplayDriver::update_image_size (int width, int height)
{
    if (desired_width != width || desired_height != height)
    {
        XShmDetach (display, &shm);
        XSync (display, 0);

        shmctl (shm.shmid, IPC_RMID, 0);
        shmdt (shm.shmaddr);

        xv_image = XvShmCreateImage (display,
                                     xv_portid,
                                     format,
                                     0,
                                     width,
                                     height,
                                     &shm);

        shm.shmid = shmget (IPC_PRIVATE, xv_image->data_size, IPC_CREAT | 0777);
        for (int tries = 0; tries <= 10; tries++)
        {
            shm.shmaddr = (char *) shmat (shm.shmid, 0, 0);

            if (shm.shmaddr == (void *) -1 && tries >= 10)
            {
                /* Can't recover, send exit. */
                fprintf (stderr, "Couldn't reallocate shared memory.\n");
                S9xExit ();
            }
            else if (shm.shmaddr != (void *) -1)
            {
                break;
            }
        }

        shm.readOnly = FALSE;

        xv_image->data = shm.shmaddr;

        XShmAttach (display, &shm);

        desired_width = width;
        desired_height = height;
    }

    return;
}

int
S9xXVDisplayDriver::init (void)
{
    int                 padding;
    int                 num_formats, num_attrs, highest_formats = 0;
    XvImageFormatValues *formats = NULL;
    XvAdaptorInfo       *adaptors;
    XvAttribute         *port_attr;
    unsigned int        num_adaptors;
    GdkScreen           *screen;
    GdkWindow           *root;

    buffer[0] = malloc (image_padded_size);
    buffer[1] = malloc (scaled_padded_size);

    padding = (image_padded_size - image_size) / 2;
    padded_buffer[0] = (void *) (((uint8 *) buffer[0]) + padding);

    padding = (scaled_padded_size - scaled_size) / 2;
    padded_buffer[1] = (void *) (((uint8 *) buffer[1]) + padding);

    gdk_buffer_width = drawing_area->allocation.width;
    gdk_buffer_height = drawing_area->allocation.height;

    memset (buffer[0], 0, image_padded_size);
    memset (buffer[1], 0, scaled_padded_size);

    /* Setup XV */
    gtk_widget_realize (drawing_area);

    display = gdk_x11_drawable_get_xdisplay (GDK_DRAWABLE (drawing_area->window));
    screen = gtk_widget_get_screen (drawing_area);
    root = gdk_screen_get_root_window (screen);

    xv_portid = -1;
    XvQueryAdaptors (display,
                     GDK_WINDOW_XWINDOW (root),
                     &num_adaptors,
                     &adaptors);


    for (int i = 0; i < (int) num_adaptors; i++)
    {
        if (adaptors[i].type & XvInputMask &&
            adaptors[i].type & XvImageMask)
        {
            formats = XvListImageFormats (display,
                                          adaptors[i].base_id,
                                          &num_formats);

            if (num_formats > highest_formats)
            {
                xv_portid = adaptors[i].base_id;
                highest_formats = num_formats;
            }

            free (formats);
        }
    }

    XvFreeAdaptorInfo (adaptors);

    if (xv_portid < 0)
    {
        fprintf (stderr, "Could not open Xv output port.\n");
        return -1;
    }

    /* Set XV_AUTOPAINT_COLORKEY _only_ if available */
    port_attr = XvQueryPortAttributes (display, xv_portid, &num_attrs);

    for (int i = 0; i < num_attrs; i++)
    {
        if (!strcmp (port_attr[i].name, "XV_AUTOPAINT_COLORKEY"))
        {
            Atom colorkey = None;

            colorkey = XInternAtom (display, "XV_AUTOPAINT_COLORKEY", True);
            if (colorkey != None)
                XvSetPortAttribute (display, xv_portid, colorkey, 1);
        }
    }

    /* Try to find an RGB format */
    format = FOURCC_YUY2;
    bpp = 100;

    formats = XvListImageFormats (display,
                                  xv_portid,
                                  &num_formats);

    for (int i = 0; i < num_formats; i++)
    {
        if (formats[i].id == 0x3 || formats[i].type == XvRGB)
        {
            if (formats[i].bits_per_pixel < bpp)
            {
                format = formats[i].id;
                bpp = formats[i].bits_per_pixel;
                bytes_per_pixel = (bpp == 15) ? 2 : bpp >> 3;

                this->rshift = get_inv_shift (formats[i].red_mask, bpp);
                this->gshift = get_inv_shift (formats[i].green_mask, bpp);
                this->bshift = get_inv_shift (formats[i].blue_mask, bpp);

                /* Check for red-blue inversion on SiliconMotion drivers */
                if (formats[i].red_mask  == 0x001f &&
                    formats[i].blue_mask == 0x7c00)
                {
                    int copy = this->rshift;
                    this->rshift = this->bshift;
                    this->bshift = copy;
                }

                /* on big-endian Xv still seems to like LSB order */
                if (config->force_inverted_byte_order)
                    S9xSetEndianess (ENDIAN_MSB);
                else
                    S9xSetEndianess (ENDIAN_LSB);
            }
        }
    }

    if (format == FOURCC_YUY2)
    {
        for (int i = 0; i < num_formats; i++)
        {
            if (formats[i].id == FOURCC_YUY2)
            {
                if (formats[i].byte_order == LSBFirst)
                {
                    if (config->force_inverted_byte_order)
                        S9xSetEndianess (ENDIAN_MSB);
                    else
                        S9xSetEndianess (ENDIAN_LSB);
                }
                else
                {
                    if (config->force_inverted_byte_order)
                        S9xSetEndianess (ENDIAN_LSB);
                    else
                        S9xSetEndianess (ENDIAN_MSB);
                }

                break;
            }
        }
    }

    free (formats);

    xv_image = XvShmCreateImage (display,
                                 xv_portid,
                                 format,
                                 0,
                                 scaled_max_width,
                                 scaled_max_width,
                                 &shm);

    shm.shmid = shmget (IPC_PRIVATE, xv_image->data_size, IPC_CREAT | 0777);
    shm.shmaddr = (char *) shmat (shm.shmid, 0, 0);
    if (shm.shmaddr == (void *) -1)
    {
        fprintf (stderr, "Could not attach shared memory segment.\n");
        return -1;
    }

    shm.readOnly = FALSE;

    xv_image->data = shm.shmaddr;

    XShmAttach (display, &shm);

    desired_width = scaled_max_width;
    desired_height = scaled_max_width;

    /* Build a table for yuv conversion */
    if (format == FOURCC_YUY2)
    {
        for (unsigned int color = 0; color < 65536; color++)
        {
            int r, g, b;
            int y, u, v;

            r = (color & 0x7c00) >> 7;
            g = (color & 0x03e0) >> 2;
            b = (color & 0x001F) << 3;

            y = (int) ((0.257  * ((double) r)) + (0.504  * ((double) g)) + (0.098  * ((double) b)) + 16.0);
            u = (int) ((-0.148 * ((double) r)) + (-0.291 * ((double) g)) + (0.439  * ((double) b)) + 128.0);
            v = (int) ((0.439  * ((double) r)) + (-0.368 * ((double) g)) + (-0.071 * ((double) b)) + 128.0);

            y_table[color] = CLAMP (y, 0, 255);
            u_table[color] = CLAMP (u, 0, 255);
            v_table[color] = CLAMP (v, 0, 255);
        }

        S9xRegisterYUVTables (y_table, u_table, v_table);
    }

    clear_buffers ();

    /* Give Snes9x core a pointer to draw on */
    GFX.Screen = (uint16 *) padded_buffer[0];
    GFX.Pitch = image_width * image_bpp;

    return 0;
}

void
S9xXVDisplayDriver::deinit (void)
{
    XShmDetach (display, &shm);
    XSync (display, 0);

    free (buffer[0]);
    free (buffer[1]);

    shmctl (shm.shmid, IPC_RMID, 0);
    shmdt (shm.shmaddr);

    padded_buffer[0] = NULL;
    padded_buffer[1] = NULL;

    return;
}

void
S9xXVDisplayDriver::clear (void)
{
    int      w, h;
    GdkColor black = { 0, 0, 0, 0 };
    int      c_width = drawing_area->allocation.width;
    int      c_height = drawing_area->allocation.height;
    GdkGC    *gc = NULL;

    gc = drawing_area->style->fg_gc[GTK_WIDGET_STATE (drawing_area)];
    gdk_gc_set_rgb_fg_color (gc, &black);

    if (window->last_width <= 0 || window->last_height <= 0)
    {
        gdk_draw_rectangle (drawing_area->window,
                            gc,
                            TRUE,
                            0, 0,
                            c_width, c_height);
        return;
    }

    /* Get width of modified display */
    w = window->last_width;
    h = window->last_height;
    get_filter_scale (w, h);

    if (config->scale_to_fit)
    {
        double screen_aspect = (double) c_width / (double) c_height;
        double snes_aspect = S9xGetAspect ();
        double granularity = 1.0 / (double) MAX (c_width, c_height);

        if (config->maintain_aspect_ratio &&
            !(screen_aspect <= snes_aspect * (1.0 + granularity) &&
              screen_aspect >= snes_aspect * (1.0 - granularity)))
        {
            int bar_size;

            if (screen_aspect > snes_aspect)
            {
                /* Black bars on left and right */
                w = (int) (c_height * snes_aspect);
                bar_size = (c_width - w) / 2;

                gdk_draw_rectangle (drawing_area->window,
                                    gc,
                                    TRUE,
                                    0, 0,
                                    bar_size, c_height);
                gdk_draw_rectangle (drawing_area->window,
                                    gc,
                                    TRUE,
                                    bar_size + w, 0,
                                    c_width - bar_size - w,
                                    c_height);
            }
            else
            {
                /* Black bars on top and bottom */
                h = (int) (c_width / snes_aspect);
                bar_size = (c_height - h) / 2;
                gdk_draw_rectangle (drawing_area->window,
                                    gc,
                                    TRUE,
                                    0, 0,
                                    c_width, bar_size);
                gdk_draw_rectangle (drawing_area->window,
                                    gc,
                                    TRUE,
                                    0, bar_size + h,
                                    c_width,
                                    c_height - bar_size - h);
            }
        }
        else
            return;
    }
    else
    {
        /* Black bars on top, bottom, left, and right :-) */
        int bar_width, bar_height;
        bar_height = (c_height - h) / 2;
        bar_width = (c_width - w) / 2;

        gdk_draw_rectangle (drawing_area->window,
                            gc,
                            TRUE,
                            0, 0,
                            c_width, bar_height);
        gdk_draw_rectangle (drawing_area->window,
                            gc,
                            TRUE,
                            0,
                            bar_height + h,
                            c_width,
                            c_height - (bar_height + h));
        gdk_draw_rectangle (drawing_area->window,
                            gc,
                            TRUE,
                            0, bar_height,
                            bar_width, h);
        gdk_draw_rectangle (drawing_area->window,
                            gc,
                            TRUE,
                            bar_width + w, bar_height,
                            c_width - (bar_width + w),
                            h);
    }

    return;
}

void
S9xXVDisplayDriver::refresh (int width, int height)
{
    int c_width, c_height;

    c_width = drawing_area->allocation.width;
    c_height = drawing_area->allocation.height;

    if (c_width != gdk_buffer_width || c_height != gdk_buffer_height)
    {
        gdk_buffer_width = c_width;
        gdk_buffer_height = c_height;
    }

    if (!config->rom_loaded)
        return;

    clear ();

    return;
}

uint16 *
S9xXVDisplayDriver::get_next_buffer (void)
{
    return (uint16 *) padded_buffer[0];
}

uint16 *
S9xXVDisplayDriver::get_current_buffer (void)
{
    return get_next_buffer ();
}

void
S9xXVDisplayDriver::push_buffer (uint16 *src)
{
    memmove (GFX.Screen, src, image_size);
    update (window->last_width, window->last_height);

    return;
}

void
S9xXVDisplayDriver::clear_buffers (void)
{
    uint32 black;
    uint8  *color;

    memset (buffer[0], 0, image_padded_size);
    memset (buffer[1], 0, scaled_padded_size);

    /* Construct value byte-order independently */
    if (format == FOURCC_YUY2)
    {
        color = (uint8 *) &black;
        *color++ = y_table[0];
        *color++ = u_table[0];
        *color++ = y_table[0];
        *color++ = v_table[0];

        for (int i = 0; i < xv_image->data_size >> 2; i++)
        {
            *(((uint32 *) xv_image->data) + i) = black;
        }
    }
    else
    {
        memset (xv_image->data, 0, xv_image->data_size);
    }

    return;
}

int
S9xXVDisplayDriver::query_availability (void)
{
    unsigned int p_version,
                 p_release,
                 p_request_base,
                 p_event_base,
                 p_error_base;

    /* Test if XV and SHM are feasible */
    if (!XShmQueryExtension (GDK_DISPLAY ()))
    {
        return 0;
    }

    if (XvQueryExtension (GDK_DISPLAY (),
                          &p_version,
                          &p_release,
                          &p_request_base,
                          &p_event_base,
                          &p_error_base) != Success)
    {
        return 0;
    }

    return 1;
}

void
S9xXVDisplayDriver::reconfigure (int width, int height)
{
    return;
}
