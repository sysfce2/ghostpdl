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


/* Common code for ImageType 1 and 4 initialization */
#include "gx.h"
#include "math_.h"
#include "memory_.h"
#include "gpcheck.h"
#include "gscdefs.h"            /* for image class table */
#include "gserrors.h"
#include "gsstruct.h"
#include "gsutil.h"
#include "gxfixed.h"
#include "gxfrac.h"
#include "gxarith.h"
#include "gxmatrix.h"
#include "gsccolor.h"
#include "gspaint.h"
#include "gzstate.h"
#include "gxdevice.h"
#include "gzpath.h"
#include "gzcpath.h"
#include "gxdevmem.h"
#include "gximage.h"
#include "gxiparam.h"
#include "gdevmrop.h"
#include "gscspace.h"
#include "gscindex.h"
#include "gsicc_cache.h"
#include "gsicc_cms.h"
#include "gsicc_manage.h"
#include "gxdevsop.h"

/* Structure descriptors */
private_st_gx_image_enum();

/* Image class procedures */
extern_gx_image_class_table();

/* Enumerator procedures */
static const gx_image_enum_procs_t image1_enum_procs = {
    gx_image1_plane_data, gx_image1_end_image, gx_image1_flush
};

/* GC procedures */
gs_private_st_ptrs2(st_color_cache, gx_image_color_cache_t, "gx_image_color_cache",
                    color_cache_enum_ptrs, color_cache_reloc_ptrs,
                    is_transparent, device_contone);
static
ENUM_PTRS_WITH(image_enum_enum_ptrs, gx_image_enum *eptr)
{
    int bps;
    gs_ptr_type_t ret;

    /* Enumerate the used members of clues.dev_color. */
    index -= gx_image_enum_num_ptrs;
    bps = eptr->unpack_bps;
    if (eptr->spp != 1)
        bps = 8;
    else if (bps > 8 || eptr->unpack == sample_unpack_copy)
        bps = 1;
    if (index >= (1 << bps) * st_device_color_max_ptrs)         /* done */
        return 0;
    /* the clues may have been cleared by gx_image_free_enum, but not freed in that */
    /* function due to being at a different save level. Only trace if dev_color.type != 0. */
    if (eptr->spp == 1) {
        if (eptr->clues != NULL) {
            if (eptr->clues[(index/st_device_color_max_ptrs) *
                (255 / ((1 << bps) - 1))].dev_color.type != 0) {
                ret = ENUM_USING(st_device_color,
                                 &eptr->clues[(index / st_device_color_max_ptrs) *
                                 (255 / ((1 << bps) - 1))].dev_color,
                                 sizeof(eptr->clues[0].dev_color),
                                 index % st_device_color_max_ptrs);
            } else {
                ret = 0;
            }
        } else {
            ret = 0;
        }
    } else {
        ret = 0;
    }
    if (ret == 0)               /* don't stop early */
        ENUM_RETURN(0);
    return ret;
}

#define e1(i,elt) ENUM_PTR(i,gx_image_enum,elt);
gx_image_enum_do_ptrs(e1)
#undef e1
ENUM_PTRS_END

static RELOC_PTRS_WITH(image_enum_reloc_ptrs, gx_image_enum *eptr)
{
    int i;

#define r1(i,elt) RELOC_PTR(gx_image_enum,elt);
    gx_image_enum_do_ptrs(r1)
#undef r1
    {
        int bps = eptr->unpack_bps;

        if (eptr->spp != 1)
            bps = 8;
        else if (bps > 8 || eptr->unpack == sample_unpack_copy)
            bps = 1;
        if (eptr->spp == 1) {
        for (i = 0; i <= 255; i += 255 / ((1 << bps) - 1))
            RELOC_USING(st_device_color,
                        &eptr->clues[i].dev_color, sizeof(gx_device_color));
    }
}
}
RELOC_PTRS_END

/* Forward declarations */
static int color_draws_b_w(gx_device * dev,
                            const gx_drawing_color * pdcolor);
static int image_init_colors(gx_image_enum * penum, int bps, int spp,
                               gs_image_format_t format,
                               const float *decode,
                               const gs_gstate * pgs, gx_device * dev,
                               const gs_color_space * pcs, bool * pdcb);

/* Procedures for unpacking the input data into bytes or fracs. */
/*extern SAMPLE_UNPACK_PROC(sample_unpack_copy); *//* declared above */

/*
 * Do common initialization for processing an ImageType 1 or 4 image.
 * Allocate the enumerator and fill in the following members:
 *      rect
 */
int
gx_image_enum_alloc(const gs_image_common_t * pic,
                    const gs_int_rect * prect, gs_memory_t * mem,
                    gx_image_enum **ppenum)
{
    const gs_pixel_image_t *pim = (const gs_pixel_image_t *)pic;
    int width = pim->Width, height = pim->Height;
    int bpc = pim->BitsPerComponent;
    gx_image_enum *penum;

    if (width < 0 || height < 0)
        return_error(gs_error_rangecheck);
    switch (pim->format) {
    case gs_image_format_chunky:
    case gs_image_format_component_planar:
        switch (bpc) {
        case 1: case 2: case 4: case 8: case 12: case 16: break;
        default: return_error(gs_error_rangecheck);
        }
        break;
    case gs_image_format_bit_planar:
        if (bpc < 1 || bpc > 8)
            return_error(gs_error_rangecheck);
    }
    if (prect) {
        if (prect->p.x < 0 || prect->p.y < 0 ||
            prect->q.x < prect->p.x || prect->q.y < prect->p.y ||
            prect->q.x > width || prect->q.y > height
            )
            return_error(gs_error_rangecheck);
    }
    *ppenum = NULL;		/* in case alloc fails and caller doesn't check code */
    penum = gs_alloc_struct(mem, gx_image_enum, &st_gx_image_enum,
                            "gx_default_begin_image");
    if (penum == 0)
        return_error(gs_error_VMerror);
    memset(penum, 0, sizeof(gx_image_enum));	/* in case of failure, no dangling pointers */
    if (prect) {
        penum->rect.x = prect->p.x;
        penum->rect.y = prect->p.y;
        penum->rect.w = prect->q.x - prect->p.x;
        penum->rect.h = prect->q.y - prect->p.y;
    } else {
        penum->rect.x = 0, penum->rect.y = 0;
        penum->rect.w = width, penum->rect.h = height;
    }
    penum->rrect.x = penum->rect.x;
    penum->rrect.y = penum->rect.y;
    penum->rrect.w = penum->rect.w;
    penum->rrect.h = penum->rect.h;
    penum->drect.x = penum->rect.x;
    penum->drect.y = penum->rect.y;
    penum->drect.w = penum->rect.w;
    penum->drect.h = penum->rect.h;
#ifdef DEBUG
    if (gs_debug_c('b')) {
        dmlprintf2(mem, "[b]Image: w=%d h=%d", width, height);
        if (prect)
            dmprintf4(mem, " ((%d,%d),(%d,%d))",
                     prect->p.x, prect->p.y, prect->q.x, prect->q.y);
    }
#endif
    *ppenum = penum;
    return 0;
}

/* Convert and restrict to a valid range. */
static inline fixed float2fixed_rounded_boxed(double src) {
    float v = floor(src*fixed_scale + 0.5);

    if (v <= min_fixed)
        return min_fixed;
    else if (v >= max_fixed)
        return max_fixed;
    else
        return 	(fixed)v;
}

/* Compute the image matrix combining the ImageMatrix with either the pmat or the pgs ctm */
int
gx_image_compute_mat(const gs_gstate *pgs, const gs_matrix *pmat, const gs_matrix *ImageMatrix,
                     gs_matrix_double *rmat)
{
    int code = 0;

    if (pmat == 0)
        pmat = &ctm_only(pgs);
    if (ImageMatrix->xx == pmat->xx && ImageMatrix->xy == pmat->xy &&
        ImageMatrix->yx == pmat->yx && ImageMatrix->yy == pmat->yy) {
        /* Process common special case separately to accept singular matrix. */
        rmat->xx = rmat->yy = 1.;
        rmat->xy = rmat->yx = 0.;
        rmat->tx = pmat->tx - ImageMatrix->tx;
        rmat->ty = pmat->ty - ImageMatrix->ty;
    } else {
        if ((code = gs_matrix_invert_to_double(ImageMatrix, rmat)) < 0 ||
            (code = gs_matrix_multiply_double(rmat, pmat, rmat)) < 0
            ) {
            return code;
        }
    }
    return code;
}

/*
 * Finish initialization for processing an ImageType 1 or 4 image.
 * Assumes the following members of *penum are set in addition to those
 * set by gx_image_enum_alloc:
 *      alpha, use_mask_color, mask_color (if use_mask_color is true),
 *      masked, adjust
 */
int
gx_image_enum_begin(gx_device * dev, const gs_gstate * pgs,
                    const gs_matrix *pmat, const gs_image_common_t * pic,
                const gx_drawing_color * pdcolor, const gx_clip_path * pcpath,
                gs_memory_t * mem, gx_image_enum *penum)
{
    const gs_pixel_image_t *pim = (const gs_pixel_image_t *)pic;
    gs_image_format_t format = pim->format;
    const int width = pim->Width;
    const int height = pim->Height;
    const int bps = pim->BitsPerComponent;
    bool masked = penum->masked;
    const float *decode = pim->Decode;
    gs_matrix_double mat;
    int index_bps;
    gs_color_space *pcs = pim->ColorSpace;
    gs_logical_operation_t lop = (pgs ? pgs->log_op : lop_default);
    int code;
    int log2_xbytes = (bps <= 8 ? 0 : arch_log2_sizeof_frac);
    int spp, nplanes, spread;
    uint bsize;
    byte *buffer = NULL;
    fixed mtx, mty;
    gs_fixed_point row_extent, col_extent, x_extent, y_extent;
    bool device_color = true;
    gs_fixed_rect obox, cbox;
    bool gridfitimages = 0;
    bool in_pattern_accumulator;
    bool in_smask;
    int orthogonal;
    int force_interpolation = 0;

    penum->pcs = NULL;
    penum->clues = NULL;
    penum->icc_setup.has_transfer = false;
    penum->icc_setup.is_lab = false;
    penum->icc_setup.must_halftone = false;
    penum->icc_setup.need_decode = false;
    penum->Width = width;
    penum->Height = height;

    if ((code = gx_image_compute_mat(pgs, pmat, &(pim->ImageMatrix), &mat)) < 0) {
        return code;
    }
    lop = lop_sanitize(lop);
    /* Grid fit: A common construction in postscript/PDF files is for images
     * to be constructed as a series of 'stacked' 1 pixel high images.
     * Furthermore, many of these are implemented as an imagemask plotted on
     * top of thin rectangles. The different fill rules for images and line
     * art produces problems; line art fills a pixel if any part of it is
     * touched - images only fill a pixel if the centre of the pixel is
     * covered. Bug 692666 is such a problem.
     *
     * As a workaround for this problem, the code below was introduced. The
     * concept is that orthogonal images can be 'grid fitted' (or 'stretch')
     * to entirely cover pixels that they touch. Initially I had this working
     * for all images regardless of type, but as testing has proceeded, this
     * showed more and more regressions, so I've cut the cases back in which
     * this code is used until it now only triggers on imagemasks that are
     * either 1 pixel high, or wide, and then not if we are rendering a
     * glyph (such as from a type3 font).
     */

    /* Ask the device if we are in a pattern accumulator */
    in_pattern_accumulator = (dev_proc(dev, dev_spec_op)(dev, gxdso_in_pattern_accumulator, NULL, 0));
    if (in_pattern_accumulator < 0)
        in_pattern_accumulator = 0;

    /* Figure out if we are orthogonal */
    if (mat.xy == 0 && mat.yx == 0)
        orthogonal = 1;
    else if (mat.xx == 0 && mat.yy == 0)
        orthogonal = 2;
    else
        orthogonal = 0;

    /* If we are in a pattern accumulator, we choose to always grid fit
     * orthogonal images. We do this by asking the device whether we
     * should grid fit. This allows us to avoid nasty blank lines around
     * the edges of cells. Similarly, for smasks.
     */
    in_smask = (pim->override_in_smask ||
                (dev_proc(dev, dev_spec_op)(dev, gxdso_in_smask, NULL, 0)) > 0);
    gridfitimages = (in_smask || in_pattern_accumulator) && orthogonal;

    if (pgs != NULL && pgs->show_gstate != NULL) {
        /* If we're a graphics state, and we're in a text object, then we
         * must be in a type3 font. Don't fiddle with it. */
    } else if (!gridfitimages &&
               (!penum->masked || penum->image_parent_type != 0)) {
        /* Other than for images we are specifically looking to grid fit (such as
         * ones in a pattern device), we only grid fit imagemasks */
    } else if (gridfitimages && (penum->masked && penum->image_parent_type == 0)) {
        /* We don't gridfit imagemasks in a pattern accumulator */
    } else if (pgs != NULL && pgs->fill_adjust.x == 0 && pgs->fill_adjust.y == 0) {
        /* If fill adjust is disabled, so is grid fitting */
    } else if (orthogonal == 1) {
        if (width == 1 || gridfitimages) {
            if (mat.xx > 0) {
                fixed ix0 = int2fixed(fixed2int(float2fixed(mat.tx)));
                double x1 = mat.tx + mat.xx * width;
                fixed ix1 = int2fixed(fixed2int_ceiling(float2fixed(x1)));
                mat.tx = (double)fixed2float(ix0);
                mat.xx = (double)(fixed2float(ix1 - ix0)/width);
            } else if (mat.xx < 0) {
                fixed ix0 = int2fixed(fixed2int_ceiling(float2fixed(mat.tx)));
                double x1 = mat.tx + mat.xx * width;
                fixed ix1 = int2fixed(fixed2int(float2fixed(x1)));
                mat.tx = (double)fixed2float(ix0);
                mat.xx = (double)(fixed2float(ix1 - ix0)/width);
            }
        }
        if (height == 1 || gridfitimages) {
            if (mat.yy > 0) {
                fixed iy0 = int2fixed(fixed2int(float2fixed(mat.ty)));
                double y1 = mat.ty + mat.yy * height;
                fixed iy1 = int2fixed(fixed2int_ceiling(float2fixed(y1)));
                mat.ty = (double)fixed2float(iy0);
                mat.yy = (double)(fixed2float(iy1 - iy0)/height);
            } else if (mat.yy < 0) {
                fixed iy0 = int2fixed(fixed2int_ceiling(float2fixed(mat.ty)));
                double y1 = mat.ty + mat.yy * height;
                fixed iy1 = int2fixed(fixed2int(float2fixed(y1)));
                mat.ty = (double)fixed2float(iy0);
                mat.yy = ((double)fixed2float(iy1 - iy0)/height);
            }
        }
    } else if (orthogonal == 2) {
        if (height == 1 || gridfitimages) {
            if (mat.yx > 0) {
                fixed ix0 = int2fixed(fixed2int(float2fixed(mat.tx)));
                double x1 = mat.tx + mat.yx * height;
                fixed ix1 = int2fixed(fixed2int_ceiling(float2fixed(x1)));
                mat.tx = (double)fixed2float(ix0);
                mat.yx = (double)(fixed2float(ix1 - ix0)/height);
            } else if (mat.yx < 0) {
                fixed ix0 = int2fixed(fixed2int_ceiling(float2fixed(mat.tx)));
                double x1 = mat.tx + mat.yx * height;
                fixed ix1 = int2fixed(fixed2int(float2fixed(x1)));
                mat.tx = (double)fixed2float(ix0);
                mat.yx = (double)(fixed2float(ix1 - ix0)/height);
            }
        }
        if (width == 1 || gridfitimages) {
            if (mat.xy > 0) {
                fixed iy0 = int2fixed(fixed2int(float2fixed(mat.ty)));
                double y1 = mat.ty + mat.xy * width;
                fixed iy1 = int2fixed(fixed2int_ceiling(float2fixed(y1)));
                mat.ty = (double)fixed2float(iy0);
                mat.xy = (double)(fixed2float(iy1 - iy0)/width);
            } else if (mat.xy < 0) {
                fixed iy0 = int2fixed(fixed2int_ceiling(float2fixed(mat.ty)));
                double y1 = mat.ty + mat.xy * width;
                fixed iy1 = int2fixed(fixed2int(float2fixed(y1)));
                mat.ty = (double)fixed2float(iy0);
                mat.xy = ((double)fixed2float(iy1 - iy0)/width);
            }
        }
    }

    /* When rendering to a pattern accumulator, if we are downscaling
     * then enable interpolation, as otherwise dropouts can cause
     * serious problems. */
    if (in_pattern_accumulator) {
        double ome = ((double)(fixed_1 - fixed_epsilon)) / (double)fixed_1; /* One Minus Epsilon */

        if (orthogonal == 1) {
            if ((mat.xx > -ome && mat.xx < ome) || (mat.yy > -ome && mat.yy < ome)) {
                force_interpolation = true;
            }
        } else if (orthogonal == 2) {
            if ((mat.xy > -ome && mat.xy < ome) || (mat.yx > -ome && mat.yx < ome)) {
                force_interpolation = true;
            }
        }
    }

    /* Can we restrict the amount of image we need? */
    while (!pim->imagematrices_are_untrustworthy) /* So we can break out of it */
    {
        gs_rect rect, rect_src;
        gs_matrix mi;
        const gs_matrix *m = pgs != NULL ? &ctm_only(pgs) : NULL;
        gs_int_rect irect;
        if (m == NULL || (code = gs_matrix_invert(m, &mi)) < 0 ||
            (code = gs_matrix_multiply(&mi, &pic->ImageMatrix, &mi)) < 0) {
            /* Give up trying to shrink the render box, but continue processing */
            break;
        }
        if (pcpath)
        {
            gs_fixed_rect obox;
            gx_cpath_outer_box(pcpath, &obox);
            rect.p.x = fixed2float(obox.p.x);
            rect.p.y = fixed2float(obox.p.y);
            rect.q.x = fixed2float(obox.q.x);
            rect.q.y = fixed2float(obox.q.y);
        }
        else
        {
            rect.p.x = 0;
            rect.p.y = 0;
            rect.q.x = dev->width;
            rect.q.y = dev->height;
        }
        /* rect is in destination space. Calculate rect_src, in source space. */
        code = gs_bbox_transform(&rect, &mi, &rect_src);
        if (code < 0) {
            /* Give up trying to shrink the render/decode boxes, but continue processing */
            break;
        }
        /* Need to expand the region to allow for the fact that the mitchell
         * scaler reads multiple pixels in. */
        /* If mi.{xx,yy} > 1 then we are downscaling. During downscaling,
         * the support increases to ensure that we don't lose pixels contributions
         * entirely. */
        if (pim->Interpolate)
        {
            float support = any_abs(mi.xx);
            int isupport;
            if (any_abs(mi.yy) > support)
                support = any_abs(mi.yy);
            if (any_abs(mi.xy) > support)
                support = any_abs(mi.xy);
            if (any_abs(mi.yx) > support)
                support = any_abs(mi.yx);
            /* If upscaling (support < 1) then we need 2 extra lines on each side of the source region
             * (2 being the maximum support for mitchell scaling).
             * If downscaling, then the number of lines is increased to avoid individual
             * contributions dropping out. */
            isupport = 2; /* Mitchell support. */
            if (support > 1)
                isupport = (int)ceil(isupport * support);
            rect_src.p.x -= isupport;
            rect_src.p.y -= isupport;
            rect_src.q.x += isupport;
            rect_src.q.y += isupport+1; /* +1 is a fudge! */
        }
        irect.p.x = (int)floor(rect_src.p.x);
        irect.p.y = (int)floor(rect_src.p.y);
        irect.q.x = (int)ceil(rect_src.q.x);
        irect.q.y = (int)ceil(rect_src.q.y);
        /* We therefore only need to render within irect. Restrict rrect to this. */
        if (penum->rrect.x < irect.p.x) {
            penum->rrect.w -= irect.p.x - penum->rrect.x;
            if (penum->rrect.w < 0)
               penum->rrect.w = 0;
            penum->rrect.x = irect.p.x;
        }
        if (penum->rrect.x + penum->rrect.w > irect.q.x) {
            penum->rrect.w = irect.q.x - penum->rrect.x;
            if (penum->rrect.w < 0)
                penum->rrect.w = 0;
        }
        if (penum->rrect.y < irect.p.y) {
            penum->rrect.h -= irect.p.y - penum->rrect.y;
            if (penum->rrect.h < 0)
                penum->rrect.h = 0;
            penum->rrect.y = irect.p.y;
        }
        if (penum->rrect.y + penum->rrect.h > irect.q.y) {
            penum->rrect.h = irect.q.y - penum->rrect.y;
            if (penum->rrect.h < 0)
                penum->rrect.h = 0;
        }
        if (penum->drect.x < irect.p.x) {
            penum->drect.w -= irect.p.x - penum->drect.x;
            if (penum->drect.w < 0)
               penum->drect.w = 0;
            penum->drect.x = irect.p.x;
        }
        if (penum->drect.x + penum->drect.w > irect.q.x) {
            penum->drect.w = irect.q.x - penum->drect.x;
            if (penum->drect.w < 0)
                penum->drect.w = 0;
        }
        if (penum->drect.y < irect.p.y) {
            penum->drect.h -= irect.p.y - penum->drect.y;
            if (penum->drect.h < 0)
                penum->drect.h = 0;
            penum->drect.y = irect.p.y;
        }
        if (penum->drect.y + penum->drect.h > irect.q.y) {
            penum->drect.h = irect.q.y - penum->drect.y;
            if (penum->drect.h < 0)
                penum->drect.h = 0;
        }
        break; /* Out of the while */
    }
    /* Check for the intersection being null */
    if (penum->drect.x + penum->drect.w <= penum->rect.x  ||
        penum->rect.x  + penum->rect.w  <= penum->drect.x ||
        penum->drect.y + penum->drect.h <= penum->rect.y  ||
        penum->rect.y  + penum->rect.h  <= penum->drect.y)
    {
          /* Something may have gone wrong with the floating point above.
           * set the region to something sane. */
        penum->drect.x = penum->rect.x;
        penum->drect.y = penum->rect.y;
        penum->drect.w = 0;
        penum->drect.h = 0;
    }
    if (penum->rrect.x + penum->rrect.w <= penum->drect.x  ||
        penum->drect.x + penum->drect.w  <= penum->rrect.x ||
        penum->rrect.y + penum->rrect.h <= penum->drect.y  ||
        penum->drect.y + penum->drect.h  <= penum->rrect.y)
    {
          /* Something may have gone wrong with the floating point above.
           * set the region to something sane. */
        penum->rrect.x = penum->drect.x;
        penum->rrect.y = penum->drect.y;
        penum->rrect.w = 0;
        penum->rrect.h = 0;
    }

    /*penum->matrix = mat;*/
    penum->matrix.xx = mat.xx;
    penum->matrix.xy = mat.xy;
    penum->matrix.yx = mat.yx;
    penum->matrix.yy = mat.yy;
    penum->matrix.tx = mat.tx;
    penum->matrix.ty = mat.ty;
    if_debug6m('b', mem, " [%g %g %g %g %g %g]\n",
              mat.xx, mat.xy, mat.yx, mat.yy, mat.tx, mat.ty);
    /* following works for 1, 2, 4, 8, 12, 16 */
    index_bps = (bps < 8 ? bps >> 1 : (bps >> 2) + 1);
    /*
     * Compute extents with distance transformation.
     */
    if (mat.tx > 0)
        mtx = float2fixed(mat.tx);
    else { /* Use positive values to ensure round down. */
        int f = (int)-mat.tx + 1;

        mtx = float2fixed(mat.tx + f) - int2fixed(f);
    }
    if (mat.ty > 0)
        mty = float2fixed(mat.ty);
    else {  /* Use positive values to ensure round down. */
        int f = (int)-mat.ty + 1;

        mty = float2fixed(mat.ty + f) - int2fixed(f);
    }

    row_extent.x = float2fixed_rounded_boxed(width * mat.xx);
    row_extent.y =
        (is_fzero(mat.xy) ? fixed_0 :
         float2fixed_rounded_boxed(width * mat.xy));
    col_extent.x =
        (is_fzero(mat.yx) ? fixed_0 :
         float2fixed_rounded_boxed(height * mat.yx));
    col_extent.y = float2fixed_rounded_boxed(height * mat.yy);
    gx_image_enum_common_init((gx_image_enum_common_t *)penum,
                              (const gs_data_image_t *)pim,
                              &image1_enum_procs, dev,
                              (masked ? 1 : (penum->alpha ? cs_num_components(pcs)+1 : cs_num_components(pcs))),
                              format);
    if (penum->rect.w == width && penum->rect.h == height) {
        x_extent = row_extent;
        y_extent = col_extent;
    } else {
        int rw = penum->rect.w, rh = penum->rect.h;

        x_extent.x = float2fixed_rounded_boxed(rw * mat.xx);
        x_extent.y =
            (is_fzero(mat.xy) ? fixed_0 :
             float2fixed_rounded_boxed(rw * mat.xy));
        y_extent.x =
            (is_fzero(mat.yx) ? fixed_0 :
             float2fixed_rounded_boxed(rh * mat.yx));
        y_extent.y = float2fixed_rounded_boxed(rh * mat.yy);
    }
    /* Set icolor0 and icolor1 to point to image clues locations if we have
       1spp or an imagemask, otherwise image clues is not used and
       we have these values point to other member variables */
    if (masked || cs_num_components(pcs) == 1) {
        /* Go ahead and allocate now if not already done.  For a mask
           we really should only do 2 values. For now, the goal is to
           eliminate the 256 bytes for the >8bpp image enumerator */
        penum->clues = (gx_image_clue*) gs_alloc_bytes(mem, sizeof(gx_image_clue)*256,
                             "gx_image_enum_begin");
        if (penum->clues == NULL) {
            code = gs_error_VMerror;
            goto fail;
        }
        penum->icolor0 = &(penum->clues[0].dev_color);
        penum->icolor1 = &(penum->clues[255].dev_color);
    } else {
        penum->icolor0 = &(penum->icolor0_val);
        penum->icolor1 = &(penum->icolor1_val);
    }
    penum->icolor0->tag = penum->icolor1->tag = device_current_tag(dev);

    if (masked) {       /* This is imagemask. */
        if (bps != 1 || pcs != NULL || penum->alpha || decode[0] == decode[1]) {
            code = gs_error_rangecheck;
            goto fail;
        }
        /* Initialize color entries 0 and 255. */
        set_nonclient_dev_color(penum->icolor0, gx_no_color_index);
        set_nonclient_dev_color(penum->icolor1, gx_no_color_index);
        *(penum->icolor1) = *pdcolor;
        memcpy(&penum->map[0].table.lookup4x1to32[0],
               (decode[0] < decode[1] ? lookup4x1to32_inverted :
                lookup4x1to32_identity),
               16 * 4);
        penum->map[0].decoding = sd_none;
        spp = 1;
        lop = rop3_know_S_0(lop);
    } else {                    /* This is image, not imagemask. */
        const gs_color_space_type *pcst = pcs->type;
        int b_w_color;

        spp = cs_num_components(pcs);
        if (spp < 0) {          /* Pattern not allowed */
            code = gs_error_rangecheck;
            goto fail;
        }
        if (penum->alpha)
            ++spp;
        /* Use a less expensive format if possible. */
        switch (format) {
        case gs_image_format_bit_planar:
            if (bps > 1)
                break;
            format = gs_image_format_component_planar;
        case gs_image_format_component_planar:
            if (spp == 1)
                format = gs_image_format_chunky;
        default:                /* chunky */
            break;
        }

        if (pcs->cmm_icc_profile_data != NULL) {
            device_color = false;
        } else {
            device_color = (*pcst->concrete_space) (pcs, pgs) == pcs;
        }

        code = image_init_colors(penum, bps, spp, format, decode, pgs, dev,
                          pcs, &device_color);
        if (code < 0) {
            gs_free_object(mem, penum->clues, "gx_image_enum_begin");
            gs_free_object(mem, penum, "gx_default_begin_image");
            return gs_throw(code, "Image colors initialization failed");
        }
        /* If we have a CIE based color space and the icc equivalent profile
           is not yet set, go ahead and handle that now.  It may already
           be done due to the above init_colors which may go through remap. */
        if (gs_color_space_is_PSCIE(pcs) && pcs->icc_equivalent == NULL) {
            code = gs_colorspace_set_icc_equivalent((gs_color_space *)pcs, &(penum->icc_setup.is_lab),
                                                pgs->memory);
            if (code < 0)
                goto fail;
            if (penum->icc_setup.is_lab) {
                /* Free what ever profile was created and use the icc manager's
                   cielab profile */
                gs_color_space *curr_pcs = (gs_color_space *)pcs;
                rc_decrement(curr_pcs->icc_equivalent,"gx_image_enum_begin");
                gsicc_adjust_profile_rc(curr_pcs->cmm_icc_profile_data, -1,"gx_image_enum_begin");
                curr_pcs->cmm_icc_profile_data = pgs->icc_manager->lab_profile;
                gsicc_adjust_profile_rc(curr_pcs->cmm_icc_profile_data, 1,"gx_image_enum_begin");
            }
        }
        /* Try to transform non-default RasterOps to something */
        /* that we implement less expensively. */
        if (!pim->CombineWithColor)
            lop = rop3_know_T_0(lop);
        else if ((rop3_uses_T(lop) && color_draws_b_w(dev, pdcolor) == 0))
            lop = rop3_know_T_0(lop);

        if (lop != rop3_S &&    /* if best case, no more work needed */
            !rop3_uses_T(lop) && bps == 1 && spp == 1 &&
            (b_w_color =
             color_draws_b_w(dev, penum->icolor0)) >= 0 &&
            color_draws_b_w(dev, penum->icolor1) == (b_w_color ^ 1)
            ) {
            if (b_w_color) {    /* Swap the colors and invert the RasterOp source. */
                gx_device_color dcolor;

                dcolor = *(penum->icolor0);
                *(penum->icolor0) = *(penum->icolor1);
                *(penum->icolor1) = dcolor;
                lop = rop3_invert_S(lop);
            }
            /*
             * At this point, we know that the source pixels
             * correspond directly to the S input for the raster op,
             * i.e., icolor0 is black and icolor1 is white.
             */
            switch (lop) {
                case rop3_D & rop3_S:
                    /* Implement this as an inverted mask writing 0s. */
                    *(penum->icolor1) = *(penum->icolor0);
                    /* (falls through) */
                case rop3_D | rop3_not(rop3_S):
                    /* Implement this as an inverted mask writing 1s. */
                    memcpy(&penum->map[0].table.lookup4x1to32[0],
                           lookup4x1to32_inverted, 16 * 4);
                  rmask:        /* Fill in the remaining parameters for a mask. */
                    penum->masked = masked = true;
                    set_nonclient_dev_color(penum->icolor0, gx_no_color_index);
                    penum->map[0].decoding = sd_none;
                    lop = rop3_T;
                    break;
                case rop3_D & rop3_not(rop3_S):
                    /* Implement this as a mask writing 0s. */
                    *(penum->icolor1) = *(penum->icolor0);
                    /* (falls through) */
                case rop3_D | rop3_S:
                    /* Implement this as a mask writing 1s. */
                    memcpy(&penum->map[0].table.lookup4x1to32[0],
                           lookup4x1to32_identity, 16 * 4);
                    goto rmask;
                default:
                    ;
            }
        }
    }
    penum->device_color = device_color;
    /*
     * Adjust width upward for unpacking up to 7 trailing bits in
     * the row, plus 1 byte for end-of-run, plus up to 7 leading
     * bits for data_x offset within a packed byte.
     */
    bsize = ((bps > 8 ? width * 2 : width) + 15) * spp;
    buffer = gs_alloc_bytes(mem, bsize, "image buffer");
    if (buffer == 0) {
        code = gs_error_VMerror;
        goto fail;
    }
    penum->bps = bps;
    penum->unpack_bps = bps;
    penum->log2_xbytes = log2_xbytes;
    penum->spp = spp;
    switch (format) {
    case gs_image_format_chunky:
        nplanes = 1;
        spread = 1 << log2_xbytes;
        break;
    case gs_image_format_component_planar:
        nplanes = spp;
        spread = spp << log2_xbytes;
        break;
    case gs_image_format_bit_planar:
        nplanes = spp * bps;
        spread = spp << log2_xbytes;
        break;
    default:
        /* No other cases are possible (checked by gx_image_enum_alloc). */
        return_error(gs_error_Fatal);
    }
    penum->num_planes = nplanes;
    penum->spread = spread;
    /*
     * If we're asked to interpolate in a partial image, we have to
     * assume that the client either really only is interested in
     * the given sub-image, or else is constructing output out of
     * overlapping pieces.
     */
    penum->interpolate = force_interpolation ? interp_force : pim->Interpolate ? interp_on : interp_off;
    penum->x_extent = x_extent;
    penum->y_extent = y_extent;
    penum->posture =
        ((x_extent.y | y_extent.x) == 0 ? image_portrait :
         (x_extent.x | y_extent.y) == 0 ? image_landscape :
         image_skewed);
    penum->pgs = pgs;
    if (pgs != NULL)
        penum->pgs_level = pgs->level;
    penum->pcs = pcs;
    rc_increment_cs(pcs); /* Grab a ref (will decrement in gx_image1_end_image() */
    penum->memory = mem;
    penum->buffer = buffer;
    penum->buffer_size = bsize;
    penum->line = NULL;
    penum->icc_link = NULL;
    penum->color_cache = NULL;
    penum->ht_buffer = NULL;
    penum->thresh_buffer = NULL;
    penum->use_cie_range = false;
    penum->line_size = 0;
    penum->use_rop = lop != (masked ? rop3_T : rop3_S);
#ifdef DEBUG
    if (gs_debug_c('*')) {
        if (penum->use_rop)
            dmprintf1(mem, "[%03x]", lop);
        dmprintf5(mem, "%c%d%c%dx%d ",
                 (masked ? (color_is_pure(pdcolor) ? 'm' : 'h') : 'i'),
                 bps,
                 (penum->posture == image_portrait ? ' ' :
                  penum->posture == image_landscape ? 'L' : 'T'),
                 width, height);
    }
#endif
    penum->slow_loop = 0;
    if (pcpath == 0) {
        (*dev_proc(dev, get_clipping_box)) (dev, &obox);
        cbox = obox;
        penum->clip_image = 0;
    } else
        penum->clip_image =
            (gx_cpath_outer_box(pcpath, &obox) |        /* not || */
             gx_cpath_inner_box(pcpath, &cbox) ?
             0 : image_clip_region);
    penum->clip_outer = obox;
    penum->clip_inner = cbox;
    penum->log_op = rop3_T;     /* rop device takes care of this */
    penum->clip_dev = 0;        /* in case we bail out */
    penum->rop_dev = 0;         /* ditto */
    penum->scaler = 0;          /* ditto */
    /*
     * If all four extrema of the image fall within the clipping
     * rectangle, clipping is never required.  When making this check,
     * we must carefully take into account the fact that we only care
     * about pixel centers.
     */
    {
        fixed
            epx = min(row_extent.x, 0) + min(col_extent.x, 0),
            eqx = max(row_extent.x, 0) + max(col_extent.x, 0),
            epy = min(row_extent.y, 0) + min(col_extent.y, 0),
            eqy = max(row_extent.y, 0) + max(col_extent.y, 0);

        {
            int hwx, hwy;

            switch (penum->posture) {
                case image_portrait:
                    hwx = width, hwy = height;
                    break;
                case image_landscape:
                    hwx = height, hwy = width;
                    break;
                default:
                    hwx = hwy = 0;
            }
            /*
             * If the image is only 1 sample wide or high,
             * and is less than 1 device pixel wide or high,
             * move it slightly so that it covers pixel centers.
             * This is a hack to work around a bug in some old
             * versions of TeX/dvips, which use 1-bit-high images
             * to draw horizontal and vertical lines without
             * positioning them properly.
             */
            if (hwx == 1 && eqx - epx < fixed_1) {
                fixed diff =
                arith_rshift_1(row_extent.x + col_extent.x);

                mtx = (((mtx + diff) | fixed_half) & -fixed_half) - diff;
            }
            if (hwy == 1 && eqy - epy < fixed_1) {
                fixed diff =
                arith_rshift_1(row_extent.y + col_extent.y);

                mty = (((mty + diff) | fixed_half) & -fixed_half) - diff;
            }
        }
        if_debug5m('b', mem, "[b]Image: %sspp=%d, bps=%d, mt=(%g,%g)\n",
                   (masked? "masked, " : ""), spp, bps,
                   fixed2float(mtx), fixed2float(mty));
        if_debug9m('b', mem,
                   "[b]   cbox=(%g,%g),(%g,%g), obox=(%g,%g),(%g,%g), clip_image=0x%x\n",
                   fixed2float(cbox.p.x), fixed2float(cbox.p.y),
                   fixed2float(cbox.q.x), fixed2float(cbox.q.y),
                   fixed2float(obox.p.x), fixed2float(obox.p.y),
                   fixed2float(obox.q.x), fixed2float(obox.q.y),
                   penum->clip_image);
        /* These DDAs enumerate the starting position of each source pixel
         * row in device space. */
        dda_init(penum->dda.row.x, mtx, col_extent.x, height);
        dda_init(penum->dda.row.y, mty, col_extent.y, height);
        if (dda_will_overflow(penum->dda.row.x) ||
            dda_will_overflow(penum->dda.row.y))
        {
            code = gs_error_rangecheck;
            goto fail;
        }
        if (penum->posture == image_portrait) {
            penum->dst_width = row_extent.x;
            penum->dst_height = col_extent.y;
        } else {
            penum->dst_width = col_extent.x;
            penum->dst_height = row_extent.y;
        }
        /* For gs_image_class_0_interpolate. */
        penum->yi0 = fixed2int_pixround_perfect(dda_current(penum->dda.row.y)); /* For gs_image_class_0_interpolate. */
        if (penum->rect.y) {
            int y = penum->rect.y;

            while (y--) {
                dda_next(penum->dda.row.x);
                dda_next(penum->dda.row.y);
            }
        }
        penum->cur.x = penum->prev.x = dda_current(penum->dda.row.x);
        penum->cur.y = penum->prev.y = dda_current(penum->dda.row.y);
        /* These DDAs enumerate the starting positions of each row of our
         * source pixel data, in the subrectangle ('strip') that we are
         * actually rendering. */
        dda_init(penum->dda.strip.x, penum->cur.x, row_extent.x, width);
        dda_init(penum->dda.strip.y, penum->cur.y, row_extent.y, width);
        if (dda_will_overflow(penum->dda.strip.x) ||
            dda_will_overflow(penum->dda.strip.y))
        {
            code = gs_error_rangecheck;
            goto fail;
        }
        if (penum->rect.x) {
            dda_advance(penum->dda.strip.x, penum->rect.x);
            dda_advance(penum->dda.strip.y, penum->rect.x);
        }
        {
            fixed ox = dda_current(penum->dda.strip.x);
            fixed oy = dda_current(penum->dda.strip.y);

            if (!penum->clip_image)     /* i.e., not clip region */
                penum->clip_image =
                    (fixed_pixround(ox + epx) < fixed_pixround(cbox.p.x) ?
                     image_clip_xmin : 0) +
                    (fixed_pixround(ox + eqx) >= fixed_pixround(cbox.q.x) ?
                     image_clip_xmax : 0) +
                    (fixed_pixround(oy + epy) < fixed_pixround(cbox.p.y) ?
                     image_clip_ymin : 0) +
                    (fixed_pixround(oy + eqy) >= fixed_pixround(cbox.q.y) ?
                     image_clip_ymax : 0);
        }
    }
    penum->y = 0;
    penum->used.x = 0;
    penum->used.y = 0;
    if (penum->clip_image && pcpath) {  /* Set up the clipping device. */
        gx_device_clip *cdev =
            gs_alloc_struct(mem, gx_device_clip,
                            &st_device_clip, "image clipper");

        if (cdev == NULL) {
            code = gs_error_VMerror;
            goto fail;
        }
        gx_make_clip_device_in_heap(cdev, pcpath, dev, mem);
        penum->clip_dev = cdev;
        penum->dev = (gx_device *)cdev; /* Will restore this in a mo. Hacky! */
    }
    if (penum->use_rop) {       /* Set up the RasterOp source device. */
        gx_device_rop_texture *rtdev;

        code = gx_alloc_rop_texture_device(&rtdev, mem,
                                           "image RasterOp");
        if (code < 0)
            goto fail;
        /* The 'target' must not be NULL for gx_make_rop_texture_device */
        if (!penum->clip_dev && !dev)
            return_error(gs_error_undefined);

        gx_make_rop_texture_device(rtdev,
                                   (penum->clip_dev != 0 ?
                                    (gx_device *) penum->clip_dev :
                                    dev), lop, pdcolor);
        gx_device_retain((gx_device *)rtdev, true);
        penum->rop_dev = rtdev;
        penum->dev = (gx_device *)rtdev; /* Will restore this in a mo. Hacky! */
    }
    {
        static sample_unpack_proc_t procs[2][6] = {
        {   sample_unpack_1, sample_unpack_2,
            sample_unpack_4, sample_unpack_8,
            sample_unpack_12, sample_unpack_16
        },
        {   sample_unpack_1_interleaved, sample_unpack_2_interleaved,
            sample_unpack_4_interleaved, sample_unpack_8_interleaved,
            sample_unpack_12, sample_unpack_16
        }};
        int num_planes = penum->num_planes;
        bool interleaved = (num_planes == 1 && penum->plane_depths[0] != penum->bps);
        irender_proc_t render_fn = NULL;
        int i;

        if (interleaved) {
            int num_components = penum->plane_depths[0] / penum->bps;

            for (i = 1; i < num_components; i++) {
                if (decode[0] != decode[i * 2 + 0] ||
                    decode[1] != decode[i * 2 + 1])
                    break;
            }
            if (i == num_components)
                interleaved = false; /* Use single table. */
        }
        penum->unpack = procs[interleaved][index_bps];

        if_debug1m('b', mem, "[b]unpack=%d\n", bps);
        /* Set up pixel0 for image class procedures. */
        penum->dda.pixel0 = penum->dda.strip;
        penum->skip_next_line = NULL;
        for (i = 0; i < gx_image_class_table_count; ++i) {
            code = gx_image_class_table[i](penum, &render_fn);
            if (code < 0)
                goto fail;

            if (render_fn != NULL) {
                penum->render = render_fn;
                break;
            }
        }
        penum->dev = dev; /* Restore this (in case it was changed to cdev or rtdev) */
        if (i == gx_image_class_table_count) {
            /* No available class can handle this image. */
            return_error(gs_error_rangecheck);
        }
    }
    return 0;

fail:
    gs_free_object(mem, buffer, "image buffer");
    gs_free_object(mem, penum->clues, "gx_image_enum_begin");
    if (penum->clip_dev != NULL) {
        rc_decrement(penum->clip_dev, "error in gx_begin_image1");
        penum->clip_dev = NULL;
    }
    gs_free_object(mem, penum->clip_dev, "image clipper");
    rc_decrement_cs(penum->pcs, "error in gx_begin_image1");
    penum->pcs = NULL;
    gs_free_object(mem, penum, "gx_begin_image1");
    return code;
}

/* If a drawing color is black or white, return 0 or 1 respectively, */
/* otherwise return -1. */
static int
color_draws_b_w(gx_device * dev, const gx_drawing_color * pdcolor)
{
    if (color_is_pure(pdcolor)) {
        gx_color_value rgb[3];

        (*dev_proc(dev, map_color_rgb)) (dev, gx_dc_pure_color(pdcolor),
                                         rgb);
        if (!(rgb[0] | rgb[1] | rgb[2]))
            return 0;
        if ((rgb[0] & rgb[1] & rgb[2]) == gx_max_color_value)
            return 1;
    }
    return -1;
}


static void
image_cache_decode(gx_image_enum *penum, byte input, byte *output, bool scale)
{
    float temp;

    switch ( penum->map[0].decoding ) {
        case sd_none:
            *output = input;
            break;
        case sd_lookup:
            temp = penum->map[0].decode_lookup[input >> 4]*255.0f;
            if (temp > 255) temp = 255;
            if (temp < 0 ) temp = 0;
            *output = (unsigned char) temp;
            break;
        case sd_compute:
            temp = penum->map[0].decode_base +
                (float) input * penum->map[0].decode_factor;
            if (scale) {
                temp = temp * 255.0;
            }
            if (temp > 255) temp = 255;
            if (temp < 0 ) temp = 0;
            *output = (unsigned char) temp;
            break;
        default:
            *output = 0;
            break;
    }
}

static bool
decode_range_needed(gx_image_enum *penum)
{
    bool scale = true;

    if (penum->map[0].decoding == sd_compute) {
        if (!(gs_color_space_is_ICC(penum->pcs) ||
            gs_color_space_is_PSCIE(penum->pcs))) {
            scale = false;
        }
    }
    return scale;
}

/* A special case where we go ahead and initialize the whole index cache with
   contone.  Device colors.  If we are halftoning we will then go ahead and
   apply the thresholds to the device contone values.  Only used for gray,
   rgb or cmyk source colors (No DeviceN for now) */
/* TO DO  Add in PSCIE decoder */
int
image_init_color_cache(gx_image_enum * penum, int bps, int spp)
{
    int num_des_comp = penum->dev->color_info.num_components;
    int num_src_comp;
    int num_entries = 1 << bps;
    bool need_decode = penum->icc_setup.need_decode;
    bool has_transfer = penum->icc_setup.has_transfer;
    byte value;
    bool decode_scale = true;
    int k, kk;
    byte psrc[4];
    byte *temp_buffer;
    byte *byte_ptr;
    bool is_indexed = (gs_color_space_get_index(penum->pcs) ==
                                            gs_color_space_index_Indexed);
    bool free_temp_buffer = true;
    gsicc_bufferdesc_t input_buff_desc;
    gsicc_bufferdesc_t output_buff_desc;
    gx_color_value conc[GX_DEVICE_COLOR_MAX_COMPONENTS];
    int code;

    if (penum->icc_link == NULL) {
        return gs_rethrow(-1, "ICC Link not created during image render color");
    }
    if (is_indexed) {
        num_src_comp = gs_color_space_num_components(penum->pcs->base_space);
    } else {
        /* Detect case where cache is not needed.  Colors are already in the
           device space.  Need to fast track this one and halftone row directly.
           Detected in gximono.c by looking if penum->color_cache is NULL */
        if (penum->icc_link->is_identity && !need_decode && !has_transfer) {
            return 0;
        }
        num_src_comp = 1;
    }
    /* Allocate cache of device contone values */
    penum->color_cache = gs_alloc_struct(penum->memory, gx_image_color_cache_t,
                                         &st_color_cache,
                                         "image_init_color_cache");
    if (penum->color_cache == NULL)
        return_error(gs_error_VMerror);

    penum->color_cache->device_contone = (byte*) gs_alloc_bytes(penum->memory,
                   (size_t)num_des_comp * num_entries * sizeof(byte), "image_init_color_cache");
    penum->color_cache->is_transparent = (bool*) gs_alloc_bytes(penum->memory,
             (size_t)num_entries * sizeof(bool), "image_init_color_cache");
    if (penum->color_cache->device_contone == NULL || penum->color_cache->is_transparent == NULL) {
        gs_free_object(penum->memory, penum->color_cache->device_contone, "image_init_color_cache");
        gs_free_object(penum->memory, penum->color_cache->is_transparent, "image_init_color_cache");
        gs_free_object(penum->memory, penum->color_cache, "image_init_color_cache");
        penum->color_cache = NULL;
        return_error(gs_error_VMerror);
    }
    /* Initialize */
    memset(penum->color_cache->is_transparent,0,num_entries * sizeof(bool));
    /* Depending upon if we need decode and ICC CM, fill the cache a couple
       different ways. If the link is the identity, then we don't need to do any
       color conversions except for potentially a decode.  This is written in
       the manner shown below so that the common case of no decode and indexed
       image with a look-up-table uses the table data directly or does as many
       operations with memcpy as we can */
    /* Need to check the decode output range so we know how we need to scale.
       We want 8 bit output */
    if (need_decode) {
        decode_scale = decode_range_needed(penum);
    }
    if (penum->icc_link->is_identity) {
        /* No CM needed.  */
        if (need_decode || has_transfer) {
            /* Slower case.  This could be sped up later to avoid the tests
               within the loop by use of specialized loops.  */
            for (k = 0; k < num_entries; k++) {
                /* Data is in k */
                if (need_decode) {
                    image_cache_decode(penum, k, &value, decode_scale);
                } else {
                    value = k;
                }
                /* Data is in value */
                if (is_indexed) {
                    gs_cspace_indexed_lookup_bytes(penum->pcs, value, psrc);
                } else {
                    psrc[0] = value;
                }
                /* Data is in psrc */
                /* These silly transforms need to go away. ToDo. */
                if (has_transfer) {
                    for (kk = 0; kk < num_des_comp; kk++) {
                        conc[kk] = gx_color_value_from_byte(psrc[kk]);
                    }
                    cmap_transfer(&(conc[0]), penum->pgs, penum->dev);
                    for (kk = 0; kk < num_des_comp; kk++) {
                        psrc[kk] = gx_color_value_to_byte(conc[kk]);
                    }
                }
                memcpy(&(penum->color_cache->device_contone[k * num_des_comp]),
                               psrc, num_des_comp);
            }
        } else {
            /* Indexing only.  No CM, decode or transfer functions. */
            for (k = 0; k < num_entries; k++) {
                gs_cspace_indexed_lookup_bytes(penum->pcs, (float)k, psrc);
                memcpy(&(penum->color_cache->device_contone[k * num_des_comp]),
                           psrc, num_des_comp);
            }
        }
    } else {
        /* Need CM */
        /* We need to worry about if the source is indexed and if we need
           to decode first.  Then we can apply CM. Create a temp buffer in
           the source space and then transform it with one call */
        temp_buffer = (byte*) gs_alloc_bytes(penum->memory,
                                             (size_t)num_entries * num_src_comp,
                                             "image_init_color_cache");
        if (temp_buffer == NULL)
            return_error(gs_error_VMerror);

        if (need_decode) {
            if (is_indexed) {
                /* Decode and lookup in index */
                for (k = 0; k < num_entries; k++) {
                    image_cache_decode(penum, k, &value, decode_scale);
                    gs_cspace_indexed_lookup_bytes(penum->pcs, value, psrc);
                    memcpy(&(temp_buffer[k * num_src_comp]), psrc, num_src_comp);
                }
            } else {
                /* Decode only */
                for (k = 0; k < num_entries; k++) {
                    image_cache_decode(penum, k, &(temp_buffer[k]), decode_scale);
                }
            }
        } else {
            /* No Decode */
            if (is_indexed) {
                /* If index uses a num_entries sized table then just use its pointer */
                if (penum->pcs->params.indexed.use_proc ||
                    penum->pcs->params.indexed.hival < (num_entries - 1)) {
                    /* Have to do the slow way */
                    for (k = 0; k <= penum->pcs->params.indexed.hival; k++) {
                        gs_cspace_indexed_lookup_bytes(penum->pcs, (float)k, psrc);
                        memcpy(&(temp_buffer[k * num_src_comp]), psrc, num_src_comp);
                    }
                    /* just use psrc results from converting 'hival' to fill the remaining slots */
                    for (; k < num_entries; k++) {
                        memcpy(&(temp_buffer[k * num_src_comp]), psrc, num_src_comp);
                    }
                } else {
                    /* Use the index table directly. */
                    gs_free_object(penum->memory, temp_buffer, "image_init_color_cache");
                    free_temp_buffer = false;
                    temp_buffer = (byte *)(penum->pcs->params.indexed.lookup.table.data);
                }
            } else {
                /* CM only */
                for (k = 0; k < num_entries; k++) {
                    temp_buffer[k] = k;
                }
            }
        }
        /* Set up the buffer descriptors. */
        gsicc_init_buffer(&input_buff_desc, num_src_comp, 1, false, false, false,
                          0, num_entries * num_src_comp, 1, num_entries);
        gsicc_init_buffer(&output_buff_desc, num_des_comp, 1, false, false, false,
                          0, num_entries * num_des_comp,
                      1, num_entries);
        code = (penum->icc_link->procs.map_buffer)(penum->dev, penum->icc_link,
                                            &input_buff_desc, &output_buff_desc,
                                            (void*) temp_buffer,
                                            (void*) penum->color_cache->device_contone);
        if (code < 0)
            return gs_rethrow(code, "Failure to map color buffer");

        /* Check if we need to apply any transfer functions.  If so then do it now */
        if (has_transfer) {
            for (k = 0; k < num_entries; k++) {
                byte_ptr =
                    &(penum->color_cache->device_contone[k * num_des_comp]);
                for (kk = 0; kk < num_des_comp; kk++) {
                    conc[kk] = gx_color_value_from_byte(byte_ptr[kk]);
                }
                cmap_transfer(&(conc[0]), penum->pgs, penum->dev);
                for (kk = 0; kk < num_des_comp; kk++) {
                    byte_ptr[kk] = gx_color_value_to_byte(conc[kk]);
                }
            }
        }
        if (free_temp_buffer)
            gs_free_object(penum->memory, temp_buffer, "image_init_color_cache");
    }
    return 0;
}

/* Export this for use by image_render_ functions */
void
image_init_clues(gx_image_enum * penum, int bps, int spp)
{
    /* Initialize the color table */
#define ictype(i)\
  penum->clues[i].dev_color.type

    switch ((spp == 1 ? bps : 8)) {
        case 8:         /* includes all color images */
            {
                register gx_image_clue *pcht = &penum->clues[0];
                register int n = 64;    /* 8 bits means 256 clues, do   */
                                        /* 4 at a time for efficiency   */
                do {
                    pcht[0].dev_color.type =
                        pcht[1].dev_color.type =
                        pcht[2].dev_color.type =
                        pcht[3].dev_color.type =
                        gx_dc_type_none;
                    pcht[0].key = pcht[1].key =
                        pcht[2].key = pcht[3].key = 0;
                    pcht += 4;
                }
                while (--n > 0);
                penum->clues[0].key = 1;        /* guarantee no hit */
                break;
            }
        case 4:
            ictype(17) = ictype(2 * 17) = ictype(3 * 17) =
                ictype(4 * 17) = ictype(6 * 17) = ictype(7 * 17) =
                ictype(8 * 17) = ictype(9 * 17) = ictype(11 * 17) =
                ictype(12 * 17) = ictype(13 * 17) = ictype(14 * 17) =
                gx_dc_type_none;
            /* falls through */
        case 2:
            ictype(5 * 17) = ictype(10 * 17) = gx_dc_type_none;
#undef ictype
    }
}

/* Initialize the color mapping tables for a non-mask image. */
static int
image_init_colors(gx_image_enum * penum, int bps, int spp,
                  gs_image_format_t format, const float *decode /*[spp*2] */ ,
                  const gs_gstate * pgs, gx_device * dev,
                  const gs_color_space * pcs, bool * pdcb)
{
    int ci, decode_type, code;
    static const float default_decode[] = {
        0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0
    };

    /* Clues are only used with image_mono_render */
    if (spp == 1) {
        image_init_clues(penum, bps, spp);
    }
    decode_type = 3; /* 0=custom, 1=identity, 2=inverted, 3=impossible */
    for (ci = 0; ci < spp; ci +=2 ) {
        decode_type &= (decode[ci] == 0. && decode[ci + 1] == 1.) |
                       (decode[ci] == 1. && decode[ci + 1] == 0.) << 1;
    }

    /* Initialize the maps from samples to intensities. */
    for (ci = 0; ci < spp; ci++) {
        sample_map *pmap = &penum->map[ci];

        /* If the decoding is [0 1] or [1 0], we can fold it */
        /* into the expansion of the sample values; */
        /* otherwise, we have to use the floating point method. */

        const float *this_decode = &decode[ci * 2];
        const float *map_decode;        /* decoding used to */
                                        /* construct the expansion map */
        const float *real_decode;       /* decoding for expanded samples */

        map_decode = real_decode = this_decode;
        if (!(decode_type & 1)) {
            if ((decode_type & 2) && bps <= 8) {
                real_decode = default_decode;
            } else {
                *pdcb = false;
                map_decode = default_decode;
            }
        }
        if (bps > 2 || format != gs_image_format_chunky) {
            if (bps <= 8)
                image_init_map(&pmap->table.lookup8[0], 1 << bps,
                               map_decode);
        } else {                /* The map index encompasses more than one pixel. */
            byte map[4];
            register int i;

            image_init_map(&map[0], 1 << bps, map_decode);
            switch (bps) {
                case 1:
                    {
                        register bits32 *p = &pmap->table.lookup4x1to32[0];

                        if (map[0] == 0 && map[1] == 0xff)
                            memcpy((byte *) p, lookup4x1to32_identity, 16 * 4);
                        else if (map[0] == 0xff && map[1] == 0)
                            memcpy((byte *) p, lookup4x1to32_inverted, 16 * 4);
                        else
                            for (i = 0; i < 16; i++, p++)
                                ((byte *) p)[0] = map[i >> 3],
                                    ((byte *) p)[1] = map[(i >> 2) & 1],
                                    ((byte *) p)[2] = map[(i >> 1) & 1],
                                    ((byte *) p)[3] = map[i & 1];
                    }
                    break;
                case 2:
                    {
                        register bits16 *p = &pmap->table.lookup2x2to16[0];

                        for (i = 0; i < 16; i++, p++)
                            ((byte *) p)[0] = map[i >> 2],
                                ((byte *) p)[1] = map[i & 3];
                    }
                    break;
            }
        }
        pmap->decode_base /* = decode_lookup[0] */  = real_decode[0];
        pmap->decode_factor =
            (real_decode[1] - real_decode[0]) /
            (bps <= 8 ? 255.0 : (float)frac_1);
        pmap->decode_max /* = decode_lookup[15] */  = real_decode[1];
        if (decode_type) {
            pmap->decoding = sd_none;
            pmap->inverted = map_decode[0] != 0;
        } else if (bps <= 4) {
            int step = 15 / ((1 << bps) - 1);
            int i;

            pmap->decoding = sd_lookup;
            for (i = 15 - step; i > 0; i -= step)
                pmap->decode_lookup[i] = pmap->decode_base +
                    i * (255.0 / 15) * pmap->decode_factor;
            pmap->inverted = 0;
        } else {
            pmap->decoding = sd_compute;
            pmap->inverted = 0;
        }
        if (spp == 1) {         /* and ci == 0 *//* Pre-map entries 0 and 255. */
            gs_client_color cc;

            /* Image clues are used in this case */
            cc.paint.values[0] = real_decode[0];
            code = (*pcs->type->remap_color) (&cc, pcs, penum->icolor0,
                                       pgs, dev, gs_color_select_source);
            if (code < 0)
                return code;
            cc.paint.values[0] = real_decode[1];
            code = (*pcs->type->remap_color) (&cc, pcs, penum->icolor1,
                                       pgs, dev, gs_color_select_source);
            if (code < 0)
                return code;
        }
    }
    return 0;
}
/* Construct a mapping table for sample values. */
/* map_size is 2, 4, 16, or 256.  Note that 255 % (map_size - 1) == 0, */
/* so the division 0xffffL / (map_size - 1) is always exact. */
void
image_init_map(byte * map, int map_size, const float *decode)
{
    float min_v = decode[0];
    float diff_v = decode[1] - min_v;

    if (diff_v == 1 || diff_v == -1) {  /* We can do the stepping with integers, without overflow. */
        byte *limit = map + map_size;
        uint value = (uint)(min_v * 0xffffL);
        int diff = (int)(diff_v * (0xffffL / (map_size - 1)));

        for (; map != limit; map++, value += diff)
            *map = value >> 8;
    } else {                    /* Step in floating point, with clamping. */
        int i;

        for (i = 0; i < map_size; ++i) {
            int value = (int)((min_v + diff_v * i / (map_size - 1)) * 255);

            map[i] = (value < 0 ? 0 : value > 255 ? 255 : value);
        }
    }
}

/*
 * Scale a pair of mask_color values to match the scaling of each sample to
 * a full byte, and complement and swap them if the map incorporates
 * a Decode = [1 0] inversion.
 */
void
gx_image_scale_mask_colors(gx_image_enum *penum, int component_index)
{
    uint scale = 255 / ((1 << penum->bps) - 1);
    uint *values = &penum->mask_color.values[component_index * 2];
    uint v0 = values[0] *= scale;
    uint v1 = values[1] *= scale;

    if (penum->map[component_index].decoding == sd_none &&
        penum->map[component_index].inverted
        ) {
        values[0] = 255 - v1;
        values[1] = 255 - v0;
    }
}

/* Used to indicate for ICC procesing if we have decoding to do */
bool
gx_has_transfer(const gs_gstate *pgs, int num_comps)
{
    int k;

    for (k = 0; k < num_comps; k++) {
        if (pgs->effective_transfer[k]->proc != gs_identity_transfer) {
            return(true);
        }
    }
    return(false);
}
