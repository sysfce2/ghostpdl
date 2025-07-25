/* Copyright (C) 2001-2025 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  39 Mesa Street, Suite 108A, San Francisco,
   CA 94129, USA, for further information.
*/


/* Support for rinkj (resplendent inkjet) drivers. */

#include "math_.h"
#include "gdevprn.h"
#include "gsparam.h"
#include "gscrd.h"
#include "gscrdp.h"
#include "gxlum.h"
#include "gdevdcrd.h"
#include "gstypes.h"
#include "gxdcconv.h"
#include "gsicc_cache.h"
#include "gsicc_manage.h"
#include "gsicc_cms.h"
#include "gdevdevn.h"

#include "rinkj/rinkj-device.h"
#include "rinkj/rinkj-byte-stream.h"
#include "rinkj/rinkj-screen-eb.h"
#include "rinkj/rinkj-epson870.h"

#ifndef MAX_CHAN
#   define MAX_CHAN 15
#endif

/* Define the device parameters. */
#ifndef X_DPI
#  define X_DPI 720
#endif
#ifndef Y_DPI
#  define Y_DPI 720
#endif

/* The device descriptor */
static dev_proc_get_params(rinkj_get_params);
static dev_proc_close_device(rinkj_close_device);
static dev_proc_put_params(rinkj_put_params);
static dev_proc_print_page(rinkj_print_page);
static dev_proc_map_color_rgb(rinkj_map_color_rgb);
static dev_proc_get_color_mapping_procs(get_rinkj_color_mapping_procs);
static dev_proc_get_color_comp_index(rinkj_get_color_comp_index);
static dev_proc_encode_color(rinkj_encode_color);
static dev_proc_decode_color(rinkj_decode_color);

/*
 * Structure for holding SeparationNames and SeparationOrder elements.
 */
typedef struct gs_separation_names_s {
    int num_names;
    const gs_param_string * names[GX_DEVICE_COLOR_MAX_COMPONENTS];
} gs_separation_names;

/* This is redundant with color_info.cm_name. We may eliminate this
   typedef and use the latter string for everything. */
typedef enum {
    RINKJ_DEVICE_GRAY,
    RINKJ_DEVICE_RGB,
    RINKJ_DEVICE_CMYK,
    RINKJ_DEVICE_N
} rinkj_color_model;

/*
 * A structure definition for a DeviceN type device
 */
typedef struct rinkj_device_s {
    gx_device_common;
    gx_prn_device_common;

    /*        ... device-specific parameters ... */

    rinkj_color_model color_model;

    /*
     * Bits per component (device colorant).  Currently only 1 and 8 are
     * supported.
     */
    int bitspercomponent;
    int n_planes_out; /* actual number of channels in device */

    /*
     * Pointer to the colorant names for the color model.  This will be
     * null if we have DeviceN type device.  The actual possible colorant
     * names are those in this list plus those in the separation_names
     * list (below).
     */
    fixed_colorant_names_list std_colorant_names;
    int num_std_colorant_names;	/* Number of names in list */

    /*
    * Separation names (if any).
    */
    gs_separation_names separation_names;

    /*
     * Separation Order (if specified).
     */
    gs_separation_names separation_order;

    /* This device can use a device link ICC profile to map
       the colors to the appropriate color space.  Not
       as flexible as having source and destination profiles
       and creating the link on the fly, but I am doing
       the minimal changes on this device to make it work
       with the new ICC architecture.  No optimizations yet. */
    gcmmhlink_t icc_link;
    cmm_profile_t *link_profile;

    /* ICC color profile objects, for color conversion. */
    char profile_out_fn[256];
    char setup_fn[256];
} rinkj_device;

static void
spot_cmyk_initialize_device_procs(gx_device *dev)
{
    set_dev_proc(dev, open_device, gdev_prn_open);
    set_dev_proc(dev, output_page, gdev_prn_bg_output_page);
    set_dev_proc(dev, close_device, rinkj_close_device);
    set_dev_proc(dev, map_color_rgb, rinkj_map_color_rgb);
    set_dev_proc(dev, get_params, rinkj_get_params);
    set_dev_proc(dev, put_params, rinkj_put_params);
    set_dev_proc(dev, get_page_device, gx_page_device_get_page_device);
    set_dev_proc(dev, get_color_mapping_procs, get_rinkj_color_mapping_procs);
    set_dev_proc(dev, get_color_comp_index, rinkj_get_color_comp_index);
    set_dev_proc(dev, encode_color, rinkj_encode_color);
    set_dev_proc(dev, decode_color, rinkj_decode_color);
}

const rinkj_device gs_rinkj_device =
{
    prn_device_body_extended(rinkj_device,
         spot_cmyk_initialize_device_procs, "rinkj",
         DEFAULT_WIDTH_10THS, DEFAULT_HEIGHT_10THS,
         X_DPI, Y_DPI,		/* X and Y hardware resolution */
         0, 0, 0, 0,		/* margins */
         GX_DEVICE_COLOR_MAX_COMPONENTS, 4,	/* MaxComponents, NumComp */
         GX_CINFO_POLARITY_SUBTRACTIVE,		/* Polarity */
         32, 0,			/* Depth, Gray_index, */
         255, 255, 1, 1,	/* MaxGray, MaxColor, DitherGray, DitherColor */
         GX_CINFO_SEP_LIN,      /* Linear & Separable */
         "DeviceN",		/* Process color model name */
         rinkj_print_page),	/* Printer page print routine */
    /* DeviceN device specific parameters */
    RINKJ_DEVICE_CMYK,		/* Color model */
    8,				/* Bits per color - must match ncomp, depth, etc. above */
    4,				/* Number of output color planes, overwritten below. */
    DeviceCMYKComponents,	/* Names of color model colorants */
    4,				/* Number colorants for CMYK */
    {0},			/* SeparationNames */
    {0},			/* SeparationOrder names */
     0,              /* icc_link (link handle) */
     0               /* link_profile (device link profile) */
};

/*
 * The following procedures are used to map the standard color spaces into
 * the color components for the spotrgb device.
 */
static void
gray_cs_to_spotrgb_cm(const gx_device * dev, frac gray, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    int i = ((rinkj_device *)dev)->separation_names.num_names;

    out[0] = out[1] = out[2] = gray;
    for(; i>0; i--)			/* Clear spot colors */
        out[2 + i] = 0;
}

static void
rgb_cs_to_spotrgb_cm(const gx_device * dev, const gs_gstate *pgs,
                                  frac r, frac g, frac b, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    int i = ((rinkj_device *)dev)->separation_names.num_names;

    out[0] = r;
    out[1] = g;
    out[2] = b;
    for(; i>0; i--)			/* Clear spot colors */
        out[2 + i] = 0;
}

static void
cmyk_cs_to_spotrgb_cm(const gx_device * dev, frac c, frac m, frac y, frac k, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    int i = ((rinkj_device *)dev)->separation_names.num_names;

    color_cmyk_to_rgb(c, m, y, k, NULL, out, dev->memory);
    for(; i>0; i--)			/* Clear spot colors */
        out[2 + i] = 0;
}

static void
gray_cs_to_spotcmyk_cm(const gx_device * dev, frac gray, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    int i = ((rinkj_device *)dev)->separation_names.num_names;

    out[0] = out[1] = out[2] = 0;
    out[3] = frac_1 - gray;
    for(; i>0; i--)			/* Clear spot colors */
        out[3 + i] = 0;
}

static void
rgb_cs_to_spotcmyk_cm(const gx_device * dev, const gs_gstate *pgs,
                                   frac r, frac g, frac b, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    rinkj_device *rdev = (rinkj_device *)dev;
    int n = rdev->separation_names.num_names;
    int i;

    color_rgb_to_cmyk(r, g, b, pgs, out, dev->memory);
    for(i = 0; i < n; i++)			/* Clear spot colors */
        out[4 + i] = 0;
}

static void
cmyk_cs_to_spotcmyk_cm(const gx_device * dev, frac c, frac m, frac y, frac k, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    rinkj_device *rdev = (rinkj_device *)dev;
    int n = rdev->separation_names.num_names;
    int i;

    out[0] = c;
    out[1] = m;
    out[2] = y;
    out[3] = k;
    for(i = 0; i < n; i++)			/* Clear spot colors */
        out[4 + i] = 0;
};

static void
cmyk_cs_to_spotn_cm(const gx_device * dev, frac c, frac m, frac y, frac k, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    rinkj_device *rdev = (rinkj_device *)dev;
    int n = rdev->separation_names.num_names;
    int i;

    /* If no profile given, assume CMYK */
    out[0] = c;
    out[1] = m;
    out[2] = y;
    out[3] = k;
    for(i = 0; i < n; i++)			/* Clear spot colors */
        out[4 + i] = 0;
};

static void
gray_cs_to_spotn_cm(const gx_device * dev, frac gray, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */

    cmyk_cs_to_spotn_cm(dev, 0, 0, 0, frac_1 - gray, out);
}

static void
rgb_cs_to_spotn_cm(const gx_device * dev, const gs_gstate *pgs,
                                   frac r, frac g, frac b, frac out[])
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    frac cmyk[4];

    color_rgb_to_cmyk(r, g, b, pgs, cmyk, dev->memory);
    cmyk_cs_to_spotn_cm(dev, cmyk[0], cmyk[1], cmyk[2], cmyk[3],
                        out);
}

static const gx_cm_color_map_procs spotRGB_procs = {
    gray_cs_to_spotrgb_cm, rgb_cs_to_spotrgb_cm, cmyk_cs_to_spotrgb_cm
};

static const gx_cm_color_map_procs spotCMYK_procs = {
    gray_cs_to_spotcmyk_cm, rgb_cs_to_spotcmyk_cm, cmyk_cs_to_spotcmyk_cm
};

static const gx_cm_color_map_procs spotN_procs = {
    gray_cs_to_spotn_cm, rgb_cs_to_spotn_cm, cmyk_cs_to_spotn_cm
};

/*
 * These are the handlers for returning the list of color space
 * to color model conversion routines.
 */

static const gx_cm_color_map_procs *
get_rinkj_color_mapping_procs(const gx_device * dev, const gx_device **tdev)
{
    const rinkj_device *rdev = (const rinkj_device *)dev;

    *tdev = dev;
    if (rdev->color_model == RINKJ_DEVICE_RGB)
        return &spotRGB_procs;
    else if (rdev->color_model == RINKJ_DEVICE_CMYK)
        return &spotCMYK_procs;
    else if (rdev->color_model == RINKJ_DEVICE_N)
        return &spotN_procs;
    else
        return NULL;
}

/*
 * Encode a list of colorant values into a gx_color_index_value.
 */
static gx_color_index
rinkj_encode_color(gx_device *dev, const gx_color_value colors[])
{
    int bpc = ((rinkj_device *)dev)->bitspercomponent;
    gx_color_index color = 0;
    int i = 0;
    int ncomp = dev->color_info.num_components;
    COLROUND_VARS;

    COLROUND_SETUP(bpc);
    for (; i<ncomp; i++) {
        color <<= bpc;
        color |= COLROUND_ROUND(colors[i]);
    }
    return (color == gx_no_color_index ? color ^ 1 : color);
}

/*
 * Decode a gx_color_index value back to a list of colorant values.
 */
static int
rinkj_decode_color(gx_device * dev, gx_color_index color, gx_color_value * out)
{
    int bpc = ((rinkj_device *)dev)->bitspercomponent;
    int drop = sizeof(gx_color_value) * 8 - bpc;
    int mask = (1 << bpc) - 1;
    int i = 0;
    int ncomp = dev->color_info.num_components;

    for (; i<ncomp; i++) {
        out[ncomp - i - 1] = (color & mask) << drop;
        color >>= bpc;
    }
    return 0;
}

/*
 * Convert a gx_color_index to RGB.
 */
static int
rinkj_map_color_rgb(gx_device *dev, gx_color_index color, gx_color_value rgb[3])
{
    rinkj_device *rdev = (rinkj_device *)dev;

    if (rdev->color_model == RINKJ_DEVICE_RGB)
        return rinkj_decode_color(dev, color, rgb);
    /* TODO: return reasonable values. */
    rgb[0] = 0;
    rgb[1] = 0;
    rgb[2] = 0;
    return 0;
}

static int
rinkj_open_profile(rinkj_device *rdev)
{
    gsicc_rendering_param_t rendering_params;

    if (rdev->link_profile == NULL && rdev->profile_out_fn[0]) {

        rdev->link_profile = gsicc_get_profile_handle_file(rdev->profile_out_fn,
                    strlen(rdev->profile_out_fn), rdev->memory);

        if (rdev->link_profile == NULL)
            return gs_throw(-1, "Could not create output profile for rinkj device");

        /* Set up the rendering parameters */

        rendering_params.black_point_comp = gsBPNOTSPECIFIED;
        rendering_params.graphics_type_tag = GS_UNKNOWN_TAG;  /* Already rendered */
        rendering_params.rendering_intent = gsPERCEPTUAL;

        /* Call with a NULL destination profile since we are using a device link profile here */
        rdev->icc_link = gscms_get_link(rdev->link_profile,
                                        NULL, &rendering_params, 0, rdev->memory);

        if (rdev->icc_link == NULL)
            return gs_throw(-1, "Could not create link handle for rinkj device");
    }
    return(0);
}

#define set_param_array(a, d, s)\
  (a.data = d, a.size = s, a.persistent = false);

/* Get parameters.  We provide a default CRD. */
static int
rinkj_get_params(gx_device * pdev, gs_param_list * plist)
{
    rinkj_device *rdev = (rinkj_device *)pdev;
    int code;
    bool seprs = false;
    gs_param_string_array scna;
    gs_param_string pos;
    gs_param_string sfs;

    set_param_array(scna, NULL, 0);

    if ( (code = gdev_prn_get_params(pdev, plist)) < 0 ||
         (code = sample_device_crd_get_params(pdev, plist, "CRDDefault")) < 0 ||
         (code = param_write_name_array(plist, "SeparationColorNames", &scna)) < 0 ||
         (code = param_write_bool(plist, "Separations", &seprs)) < 0)
        return code;

    pos.data = (const byte *)rdev->profile_out_fn,
        pos.size = strlen(rdev->profile_out_fn),
        pos.persistent = false;
    code = param_write_string(plist, "ProfileOut", &pos);
    if (code < 0)
        return code;

    sfs.data = (const byte *)rdev->setup_fn,
        sfs.size = strlen(rdev->setup_fn),
        sfs.persistent = false;
    code = param_write_string(plist, "SetupFile", &sfs);

    return code;
}
#undef set_param_array

#define compare_color_names(name, name_size, str, str_size) \
    (name_size == str_size && \
        (strncmp((const char *)name, (const char *)str, name_size) == 0))

/*
 * This routine will check if a name matches any item in a list of process model
 * color component names.
 */
static bool
check_process_color_names(fixed_colorant_names_list plist,
                          const gs_param_string * pstring)
{
    if (plist) {
        uint size = pstring->size;

        while( *plist) {
            if (compare_color_names(*plist, strlen(*plist), pstring->data, size)) {
                return true;
            }
            plist++;
        }
    }
    return false;
}

#define BEGIN_ARRAY_PARAM(pread, pname, pa, psize, e)\
    BEGIN\
    switch (code = pread(plist, (param_name = pname), &(pa))) {\
      case 0:\
        if ((pa).size != psize) {\
          ecode = gs_note_error(gs_error_rangecheck);\
          (pa).data = 0;	/* mark as not filled */\
        } else
#define END_ARRAY_PARAM(pa, e)\
        goto e;\
      default:\
        ecode = code;\
e:	param_signal_error(plist, param_name, ecode);\
      case 1:\
        (pa).data = 0;		/* mark as not filled */\
    }\
    END

static int
rinkj_param_read_fn(gs_param_list *plist, const char *name,
                  gs_param_string *pstr, int max_len)
{
    int code = param_read_string(plist, name, pstr);

    if (code == 0) {
        if (pstr->size >= max_len)
            param_signal_error(plist, name, code = gs_error_rangecheck);
    } else {
        pstr->data = 0;
    }
    return code;
}

/* Compare a C string and a gs_param_string. */
static bool
param_string_eq(const gs_param_string *pcs, const char *str)
{
    return (strlen(str) == pcs->size &&
            !strncmp(str, (const char *)pcs->data, pcs->size));
}

static int
rinkj_set_color_model(rinkj_device *rdev, rinkj_color_model color_model)
{
    int bpc = 8;

    rdev->color_model = color_model;
    if (color_model == RINKJ_DEVICE_GRAY) {
        rdev->std_colorant_names = DeviceGrayComponents;
        rdev->num_std_colorant_names = 1;
        rdev->color_info.cm_name = "DeviceGray";
        rdev->color_info.polarity = GX_CINFO_POLARITY_ADDITIVE;
    } else if (color_model == RINKJ_DEVICE_RGB) {
        rdev->std_colorant_names = DeviceRGBComponents;
        rdev->num_std_colorant_names = 3;
        rdev->color_info.cm_name = "DeviceRGB";
        rdev->color_info.polarity = GX_CINFO_POLARITY_ADDITIVE;
    } else if (color_model == RINKJ_DEVICE_CMYK) {
        rdev->std_colorant_names = DeviceCMYKComponents;
        rdev->num_std_colorant_names = 4;
        rdev->color_info.cm_name = "DeviceCMYK";
        rdev->color_info.polarity = GX_CINFO_POLARITY_SUBTRACTIVE;
    } else if (color_model == RINKJ_DEVICE_N) {
        rdev->std_colorant_names = DeviceCMYKComponents;
        rdev->num_std_colorant_names = 4;
        rdev->color_info.cm_name = "DeviceN";
        rdev->color_info.polarity = GX_CINFO_POLARITY_SUBTRACTIVE;
    } else {
        return -1;
    }

    rdev->color_info.max_components = rdev->num_std_colorant_names;
    rdev->color_info.num_components = rdev->num_std_colorant_names;
    rdev->color_info.depth = bpc * rdev->num_std_colorant_names;
    return 0;
}

/* Set parameters.  We allow setting the number of bits per component. */
static int
rinkj_put_params(gx_device * pdev, gs_param_list * plist)
{
    rinkj_device * const pdevn = (rinkj_device *) pdev;
    gx_device_color_info save_info;
    gs_param_name param_name;
    int npcmcolors;
    int num_spot = pdevn->separation_names.num_names;
    int ecode = 0;
    int code;
    gs_param_string_array scna;
    gs_param_string po;
    gs_param_string sf;
    gs_param_string pcm;
    rinkj_color_model color_model = pdevn->color_model;

    BEGIN_ARRAY_PARAM(param_read_name_array, "SeparationColorNames", scna, scna.size, scne) {
        break;
    } END_ARRAY_PARAM(scna, scne);

    if (code >= 0)
        code = rinkj_param_read_fn(plist, "ProfileOut", &po,
                                 sizeof(pdevn->profile_out_fn));

    if (code >= 0)
        code = rinkj_param_read_fn(plist, "SetupFile", &sf,
                                 sizeof(pdevn->setup_fn));

    if (code >= 0)
        code = param_read_name(plist, "ProcessColorModel", &pcm);
    if (code == 0) {
        if (param_string_eq (&pcm, "DeviceGray"))
            color_model = RINKJ_DEVICE_GRAY;
        else if (param_string_eq (&pcm, "DeviceRGB"))
            color_model = RINKJ_DEVICE_RGB;
        else if (param_string_eq (&pcm, "DeviceCMYK"))
            color_model = RINKJ_DEVICE_CMYK;
        else if (param_string_eq (&pcm, "DeviceN"))
            color_model = RINKJ_DEVICE_N;
        else {
            param_signal_error(plist, "ProcessColorModel",
                               code = gs_error_rangecheck);
        }
    }
    if (code < 0)
        return code;

    /*
     * Save the color_info in case gdev_prn_put_params fails, and for
     * comparison.
     */
    save_info = pdevn->color_info;
    ecode = rinkj_set_color_model(pdevn, color_model);
    if (ecode == 0)
        ecode = gdev_prn_put_params(pdev, plist);
    if (ecode < 0) {
        pdevn->color_info = save_info;
        return ecode;
    }

    /* Separations are only valid with a subtractive color model */
    if (pdev->color_info.polarity == GX_CINFO_POLARITY_SUBTRACTIVE) {
        /*
         * Process the SeparationColorNames.  Remove any names that already
         * match the process color model colorant names for the device.
         */
        if (scna.data != 0) {
            int num_names = scna.size, i = 0;
            fixed_colorant_names_list pcomp_names = pdevn->std_colorant_names;

            if (num_spot + num_names > pdev->color_info.max_components) {
                param_signal_error(plist, "SeparationColorNames", gs_error_rangecheck);
                return_error(gs_error_rangecheck);
            }
            for (i = 0; i < num_names; i++) {
                /* Verify that the name is not one of our process colorants */
                if (!check_process_color_names(pcomp_names, &scna.data[i])) {
                    byte * sep_name;
                    int name_size = scna.data[i].size;
                    gs_param_string *new_string;

                    new_string = (gs_param_string *)gs_alloc_bytes(pdev->memory->non_gc_memory, sizeof(gs_param_string), "devicen_put_params_no_sep_order");
                    if (new_string == NULL) {
                        param_signal_error(plist, "SeparationColorNames", gs_error_VMerror);
                        return_error(gs_error_VMerror);
                    }
                    /* We have a new separation */
                    sep_name = (byte *)gs_alloc_bytes(pdev->memory->non_gc_memory,
                        name_size, "devicen_put_params_no_sep_order");
                    if (sep_name == NULL) {
                        gs_free_object(pdev->memory, new_string, "devicen_put_params_no_sep_order");
                        param_signal_error(plist, "SeparationColorNames", gs_error_VMerror);
                        return_error(gs_error_VMerror);
                    }
                    memcpy(sep_name, scna.data[i].data, name_size);
                    new_string->size = name_size;
                    new_string->data = sep_name;
                    new_string->persistent = true;
                    if (pdevn->separation_names.names[num_spot] != NULL) {
                        gs_free_object(pdev->memory->non_gc_memory, (void *)pdevn->separation_names.names[num_spot]->data, "devicen_put_params_no_sep_order");
                        gs_free_object(pdev->memory->non_gc_memory, (void *)pdevn->separation_names.names[num_spot], "devicen_put_params_no_sep_order");
                    }
                    pdevn->separation_names.names[num_spot] = new_string;

                    num_spot++;
                }
            }
            pdevn->separation_names.num_names = num_spot;
        }
    }
    npcmcolors = pdevn->num_std_colorant_names;
    pdevn->color_info.num_components = npcmcolors + num_spot;
    if (pdevn->color_info.num_components > pdevn->color_info.max_components)
        pdevn->color_info.num_components = pdevn->color_info.max_components;

    /*
     * The DeviceN device can have zero components if nothing has been
     * specified.  This causes some problems so force at least one
     * component until something is specified.
     */
    if (!pdevn->color_info.num_components)
        pdevn->color_info.num_components = 1;
    pdevn->color_info.depth = bpc_to_depth(pdevn->color_info.num_components,
                                           pdevn->bitspercomponent);
    if (pdevn->color_info.depth != save_info.depth) {
        gs_closedevice(pdev);
    }

    if (po.data != 0) {
        memcpy(pdevn->profile_out_fn, po.data, po.size);
        pdevn->profile_out_fn[po.size] = 0;
    }
    if (sf.data != 0) {
        memcpy(pdevn->setup_fn, sf.data, sf.size);
        pdevn->setup_fn[sf.size] = 0;
    }
    code = rinkj_open_profile(pdevn);

    return code;
}

/*
 * Close device and clean up ICC structures.
 */

static int
rinkj_close_device(gx_device *dev)
{
    rinkj_device * const rdev = (rinkj_device *) dev;
    int i;

    /* ICC link profile only used (and set) if specified on command line */
    if (rdev->icc_link != NULL)
        gscms_release_link(rdev->icc_link);
    rc_decrement(rdev->link_profile, "rinkj_close_device");

    /* Free all the colour separation names */
    for (i = 0; i < rdev->separation_names.num_names; i++) {
        if (rdev->separation_names.names[i] != NULL) {
            gs_free_object(rdev->memory->non_gc_memory, (void *)rdev->separation_names.names[i]->data, "devicen_put_params_no_sep_order");
            gs_free_object(rdev->memory->non_gc_memory, (void *)rdev->separation_names.names[i], "devicen_put_params_no_sep_order");
        }
        rdev->separation_names.names[i] = NULL;
    }
    rdev->separation_names.num_names = 0;

    return gdev_prn_close(dev);
}

/*
 * This routine will check to see if the color component name  match those
 * that are available amoung the current device's color components.
 *
 * Parameters:
 *   dev - pointer to device data structure.
 *   pname - pointer to name (zero termination not required)
 *   nlength - length of the name
 *
 * This routine returns a positive value (0 to n) which is the device colorant
 * number if the name is found.  It returns a negative value if not found.
 */
static int
rinkj_get_color_comp_index(gx_device * dev, const char * pname, int name_size,
                                int src_index)
{
/* TO_DO_DEVICEN  This routine needs to include the effects of the SeparationOrder array */
    fixed_colorant_name * pcolor = ((const rinkj_device *)dev)->std_colorant_names;
    int color_component_number = 0;
    int i;

    /* Check if the component is in the implied list. */
    if (pcolor) {
        while( *pcolor) {
            if (compare_color_names(pname, name_size, *pcolor, strlen(*pcolor)))
                return color_component_number;
            pcolor++;
            color_component_number++;
        }
    }

    /* Check if the component is in the separation names list. */
    {
        const gs_separation_names * separations = &((const rinkj_device *)dev)->separation_names;
        int num_spot = separations->num_names;

        for (i=0; i<num_spot; i++) {
            if (compare_color_names((const char *)separations->names[i]->data,
                  separations->names[i]->size, pname, name_size)) {
                return color_component_number;
            }
            color_component_number++;
        }
    }

    return -1;
}

/* simple linear interpolation */
static double
rinkj_graph_lookup (const double *graph_x, const double *graph_y, int n_graph, double x)
{
  int i;

  for (i = 0; i < n_graph - 1; i++)
    {
      if (graph_x[i + 1] > x)
        break;
    }
  return graph_y[i] + (x - graph_x[i]) * (graph_y[i + 1] - graph_y[i]) /
    (graph_x[i + 1] - graph_x[i]);
}

typedef struct rinkj_lutset_s rinkj_lutset;
typedef struct rinkj_lutchain_s rinkj_lutchain;

struct rinkj_lutset_s {
    const char *plane_names;
    rinkj_lutchain *lut[MAX_CHAN];
};

struct rinkj_lutchain_s {
    rinkj_lutchain *next;
    int n_graph;
    double *graph_x;
    double *graph_y;
};

static int
rinkj_add_lut(rinkj_device *rdev, rinkj_lutset *lutset, char plane, gp_file *f)
{
    char linebuf[256];
    rinkj_lutchain *chain;
    int n_graph;
    int plane_ix;
    int i;
    rinkj_lutchain **pp;

    for (plane_ix = 0; lutset->plane_names[plane_ix]; plane_ix++)
        if (lutset->plane_names[plane_ix] == plane)
            break;
    if (lutset->plane_names[plane_ix] != plane)
        return -1;
    pp = &lutset->lut[plane_ix];

    if (gp_fgets(linebuf, sizeof(linebuf), f) == NULL)
        return -1;
    if (sscanf(linebuf, "%d", &n_graph) != 1)
        return -1;
    if (n_graph < 0 || n_graph > 256)
        return -1;
    chain = (rinkj_lutchain *)gs_alloc_bytes(rdev->memory, sizeof(rinkj_lutchain), "rinkj_add_lut");
    if (chain == NULL) {
        return -1;
    }
    chain->next = NULL;
    chain->n_graph = n_graph;
    chain->graph_x = (double *)gs_alloc_bytes(rdev->memory, (size_t)sizeof(double) * n_graph, "rinkj_add_lut");
    if (chain->graph_x == NULL) {
        gs_free_object(rdev->memory, chain, "rinkj_add_lut");
        return -1;
    }
    chain->graph_y = (double *)gs_alloc_bytes(rdev->memory, (size_t)sizeof(double) * n_graph, "rinkj_add_lut");
    if (chain->graph_y == NULL) {
        gs_free_object(rdev->memory, chain->graph_x, "rinkj_add_lut");
        gs_free_object(rdev->memory, chain, "rinkj_add_lut");
        return -1;
    }
    for (i = 0; i < n_graph; i++) {
        double x, y;

        if (gp_fgets(linebuf, sizeof(linebuf), f) == NULL)
            return -1;
        if (sscanf(linebuf, "%lf %lf", &y, &x) != 2)
            return -1;
        chain->graph_x[i] = x / 1.0;
        chain->graph_y[i] = y / 1.0;
    }
    /* add at end of chain */
    while (*pp) {
        pp = &((*pp)->next);
    }
    *pp = chain;
    return 0;
}

static int
rinkj_apply_luts(rinkj_device *rdev, RinkjDevice *cmyk_dev, const rinkj_lutset *lutset)
{
    int plane_ix;
    double lut[256];

    for (plane_ix = 0; plane_ix < 7; plane_ix++) {
        int i;
        for (i = 0; i < 256; i++) {
            double g = i / 255.0;
            rinkj_lutchain *chain;

            for (chain = lutset->lut[plane_ix]; chain; chain = chain->next) {
                g = rinkj_graph_lookup(chain->graph_x, chain->graph_y,
                                       chain->n_graph, g);
            }
            lut[i] = g;
        }
        rinkj_screen_eb_set_lut(cmyk_dev, plane_ix, lut);
    }
    return 0;
}

static int
rinkj_set_luts(rinkj_device *rdev,
               RinkjDevice *printer_dev, RinkjDevice *cmyk_dev,
               const char *config_fn, const RinkjDeviceParams *params)
{
    gp_file *f = gp_fopen(rdev->memory, config_fn, "r");
    char linebuf[256];
    char key[256];
    char *val;
    rinkj_lutset lutset;
    int i;

    if (f == NULL)
        return_error(gs_error_ioerror);

    lutset.plane_names = "KkCMcmY";
    for (i = 0; i < MAX_CHAN; i++) {
        lutset.lut[i] = NULL;
    }
    for (;;) {
        if (gp_fgets(linebuf, sizeof(linebuf), f) == NULL)
            break;
        for (i = 0; linebuf[i]; i++)
            if (linebuf[i] == ':') break;
        if (linebuf[i] != ':') {
            continue;
        }
        memcpy(key, linebuf, i);
        key[i] = 0;
        for (i++; linebuf[i] == ' '; i++);
        val = linebuf + i;

        if (!strcmp(key, "AddLut")) {
            if_debug1m('r', rdev->memory, "[r]%s", linebuf);
            rinkj_add_lut(rdev, &lutset, val[0], f);
        } else if (!strcmp(key, "Dither") || !strcmp(key, "Aspect")) {
            rinkj_device_set_param_string(cmyk_dev, key, val);
        } else {
            rinkj_device_set_param_string(printer_dev, key, val);
        }
    }

    gp_fclose(f);

    rinkj_apply_luts(rdev, cmyk_dev, &lutset);
    /* todo: free lutset contents */

    return 0;
}

static RinkjDevice *
rinkj_init(rinkj_device *rdev, gp_file *file)
{
    RinkjByteStream *bs;
    RinkjDevice *epson_dev;
    RinkjDevice *cmyk_dev;
    RinkjDeviceParams params;

    bs = rinkj_byte_stream_file_new(file);
    if (bs == NULL)
        return NULL;
    epson_dev = rinkj_epson870_new(bs);
    if (epson_dev == NULL) {
        free(bs);
        return NULL;
    }
    cmyk_dev = rinkj_screen_eb_new(epson_dev);
    if (cmyk_dev == NULL) {
        free(epson_dev);
        free(bs);
        return NULL;
    }

    params.width = rdev->width;
    params.height = rdev->height;
    params.n_planes = 7;
    params.plane_names = "CMYKcmk";
    rdev->n_planes_out = params.n_planes;

    if (rinkj_set_luts(rdev, epson_dev, cmyk_dev, rdev->setup_fn, &params) < 0) {
        free(cmyk_dev);
        free(epson_dev);
        free(bs);
        return NULL;
    }

    rinkj_device_init (cmyk_dev, &params);

    return cmyk_dev;
}

typedef struct rinkj_color_cache_entry_s rinkj_color_cache_entry;

struct rinkj_color_cache_entry_s {
    bits32 key;
    bits32 value;
};

#define RINKJ_CCACHE_LOGSIZE 16
#define RINKJ_CCACHE_SIZE (1 << RINKJ_CCACHE_LOGSIZE)

static inline bits32
rinkj_color_hash(bits32 color)
{
    /* This is somewhat arbitrary */
    return (color ^ (color >> 10) ^ (color >> 20)) & (RINKJ_CCACHE_SIZE - 1);
}

static int
rinkj_write_image_data(gx_device_printer *pdev, RinkjDevice *cmyk_dev)
{
    rinkj_device *rdev = (rinkj_device *)pdev;
    size_t raster = gdev_prn_raster(rdev);
    byte *line;
    byte *plane_data[MAX_CHAN];
    const byte *split_plane_data[MAX_CHAN];
    int xsb;
    int n_planes;
    int n_planes_in = pdev->color_info.num_components;
    int n_planes_out = 4;
    int i;
    int y;
    int code = 0;
    rinkj_color_cache_entry *cache = NULL;

    n_planes = n_planes_in + rdev->separation_names.num_names;
    if_debug1m('r', rdev->memory, "[r]n_planes = %d\n", n_planes);
    xsb = pdev->width;
    for (i = 0; i < n_planes_out; i++) {
        plane_data[i] = gs_alloc_bytes(pdev->memory, xsb, "rinkj_write_image_data");
        if (plane_data[i] == NULL) {
            while (--i >= 0)
                gs_free_object(pdev->memory, plane_data[i], "rinkj_write_image_data");
            return_error(gs_error_VMerror);
        }
    }
    if (rdev->icc_link != NULL) {

        cache = (rinkj_color_cache_entry *)gs_alloc_bytes(pdev->memory, RINKJ_CCACHE_SIZE * (size_t)sizeof(rinkj_color_cache_entry), "rinkj_write_image_data");
        if (cache == NULL) {
            /* i == n_planes_out from above */
            while (--i >= 0)
                gs_free_object(pdev->memory, plane_data[i], "rinkj_write_image_data");
            return_error(gs_error_VMerror);
        }

        /* Set up cache so that none of the keys will hit. */

        cache[0].key = 1;
        for (i = 1; i < RINKJ_CCACHE_SIZE; i++)
            cache[i].key = 0;

    }

    /* do CMYK -> CMYKcmk ink split by plane replication */
    split_plane_data[0] = plane_data[0];
    split_plane_data[1] = plane_data[1];
    split_plane_data[2] = plane_data[2];
    split_plane_data[3] = plane_data[3];
    split_plane_data[4] = plane_data[0];
    split_plane_data[5] = plane_data[1];
    split_plane_data[6] = plane_data[3];

    line = gs_alloc_bytes(pdev->memory, raster, "rinkj_write_image_data");
    if (line == NULL)
        goto xit;
    for (y = 0; y < pdev->height; y++) {
        byte *row;
        int x;

        code = gdev_prn_get_bits(pdev, y, line, &row);
        if (code < 0)
            goto xit;

        if (rdev->icc_link == NULL) {
            int rowix = 0;
            for (x = 0; x < pdev->width; x++) {
                for (i = 0; i < n_planes_in; i++)
                    plane_data[i][x] = row[rowix + i];
                rowix += n_planes;
            }
        } else if (n_planes == 3) {
            int rowix = 0;
            for (x = 0; x < pdev->width; x++) {
                byte cbuf[4] = {0, 0, 0, 0};
                bits32 color;
                bits32 hash;
                byte vbuf[4];

                memcpy(cbuf, row + rowix, 3);
                color = ((bits32 *)cbuf)[0];
                hash = rinkj_color_hash(color);

                if (cache[hash].key != color) {

                    /* 3 channel to CMYK */
                    gscms_transform_color((gx_device *)rdev, rdev->icc_link,
                                          &cbuf, &(vbuf), 1);
                    cache[hash].key = color;
                    cache[hash].value = ((bits32 *)vbuf)[0];

                } else {
                    ((bits32 *)vbuf)[0] = cache[hash].value;
                }
                plane_data[0][x] = vbuf[0];
                plane_data[1][x] = vbuf[1];
                plane_data[2][x] = vbuf[2];
                plane_data[3][x] = vbuf[3];
                rowix += n_planes;
            }
        } else if (n_planes == 4) {
            for (x = 0; x < pdev->width; x++) {
                bits32 color = ((bits32 *)row)[x];
                bits32 hash = rinkj_color_hash(color);
                byte vbuf[4];

                if (cache[hash].key != color) {
                    byte cbuf[4];

                    ((bits32 *)cbuf)[0] = color;

                    /* 4 channel to CMYK */
                    gscms_transform_color((gx_device *)rdev, rdev->icc_link,
                                           &cbuf, &(vbuf), 1);
                    cache[hash].key = color;
                    cache[hash].value = ((bits32 *)vbuf)[0];
                } else {
                    ((bits32 *)vbuf)[0] = cache[hash].value;
                }
                plane_data[0][x] = vbuf[0];
                plane_data[1][x] = vbuf[1];
                plane_data[2][x] = vbuf[2];
                plane_data[3][x] = vbuf[3];
            }
        } else if (n_planes == 5) {
            int rowix = 0;
            for (x = 0; x < pdev->width; x++) {
                byte cbuf[4];
                bits32 color;
                bits32 hash;
                byte vbuf[4];
                byte spot;
                int scolor[4] = { 0x08, 0xc0, 0x80, 0 };

                memcpy(cbuf, row + rowix, 4);
                color = ((bits32 *)cbuf)[0];
                hash = rinkj_color_hash(color);

                if (cache[hash].key != color) {

                    /* Not sure what is going on here.  Old
                       code was still working with 4 to 4
                       conversion.  Replacing with new ICC AMP call */

                    gscms_transform_color((gx_device *) rdev, rdev->icc_link,
                                          &cbuf, &(vbuf), 1);
                    cache[hash].key = color;
                    cache[hash].value = ((bits32 *)vbuf)[0];
                } else {
                    ((bits32 *)vbuf)[0] = cache[hash].value;
                }
                spot = row[rowix + 4];
                if (spot != 0) {
                    for (i = 0; i < 4; i++) {
                        int cmyk = vbuf[i], sp_i = spot;
                        int tmp = (cmyk << 8) - cmyk;
                        tmp += (sp_i * scolor[i] * (255 - cmyk)) >> 8;
                        tmp += 0x80;
                        plane_data[i][x] = (tmp + (tmp >> 8)) >> 8;
                    }
                } else {
                    plane_data[0][x] = vbuf[0];
                    plane_data[1][x] = vbuf[1];
                    plane_data[2][x] = vbuf[2];
                    plane_data[3][x] = vbuf[3];
                }
                rowix += n_planes;
            }
        }

        code = rinkj_device_write(cmyk_dev, (const char **)split_plane_data);
    }

    rinkj_device_write(cmyk_dev, NULL);
xit:
    for (i = 0; i < n_planes_in; i++)
        gs_free_object(pdev->memory, plane_data[i], "rinkj_write_image_data");
    gs_free_object(pdev->memory, line, "rinkj_write_image_data");
    gs_free_object(pdev->memory, cache, "rinkj_write_image_data");

    return code;
}

static int
rinkj_print_page(gx_device_printer *pdev, gp_file *file)
{
    rinkj_device *rdev = (rinkj_device *)pdev;
    int code = 0;
    RinkjDevice *cmyk_dev;

    if (rdev->setup_fn[0] == 0) {
        emprintf(rdev->memory, "Error, SetupFile not defined, output aborted\n");
        return 0;
    }
    cmyk_dev = rinkj_init(rdev, file);
    if (cmyk_dev == 0)
        return gs_note_error(gs_error_ioerror);

    code = rinkj_write_image_data(pdev, cmyk_dev);
    return code;
}
