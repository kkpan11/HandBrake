/* rendersub.c

   Copyright (c) 2003-2024 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/hbffmpeg.h"
#include "handbrake/extradata.h"
#include "libavutil/bswap.h"
#include <ass/ass.h>

#define ABS(a) ((a) > 0 ? (a) : (-(a)))

struct hb_filter_private_s
{
    // Common
    int                 pix_fmt_alpha;
    int                 depth;
    int                 hshift;
    int                 wshift;
    int                 crop[4];
    int                 type;
    struct SwsContext * sws;
    int                 sws_width;
    int                 sws_height;

    // VOBSUB && PGSSUB
    hb_list_t         * sub_list; // List of active subs

    // SSA
    ASS_Library       * ssa;
    ASS_Renderer      * renderer;
    ASS_Track         * ssaTrack;
    uint8_t             script_initialized;
    hb_buffer_t       * last_render;

    // SRT
    int                 line;
    hb_buffer_t       * current_sub;

    void (*blend)(hb_buffer_t *dst, const hb_buffer_t *src,
                  const int left, const int top, const int shift);
    unsigned            chromaCoeffs[2][4];

    hb_filter_init_t    input;
    hb_filter_init_t    output;
};

// VOBSUB
static int vobsub_post_init( hb_filter_object_t * filter, hb_job_t * job );

static int vobsub_work( hb_filter_object_t * filter,
                        hb_buffer_t ** buf_in,
                        hb_buffer_t ** buf_out );

static void vobsub_close( hb_filter_object_t * filter );


// SSA
static int ssa_post_init( hb_filter_object_t * filter, hb_job_t * job );

static int ssa_work( hb_filter_object_t * filter,
                     hb_buffer_t ** buf_in,
                     hb_buffer_t ** buf_out );

static void ssa_close( hb_filter_object_t * filter );


// SRT
static int textsub_post_init( hb_filter_object_t * filter, hb_job_t * job );
static int cc608sub_post_init( hb_filter_object_t * filter, hb_job_t * job );

static int textsub_work( hb_filter_object_t * filter,
                     hb_buffer_t ** buf_in,
                     hb_buffer_t ** buf_out );

static void textsub_close( hb_filter_object_t * filter );


// PGS
static int pgssub_post_init( hb_filter_object_t * filter, hb_job_t * job );

static int pgssub_work ( hb_filter_object_t * filter,
                      hb_buffer_t ** buf_in,
                      hb_buffer_t ** buf_out );

static void pgssub_close( hb_filter_object_t * filter );


// Entry points
static int hb_rendersub_init( hb_filter_object_t * filter,
                                 hb_filter_init_t * init );

static int hb_rendersub_post_init( hb_filter_object_t * filter, hb_job_t *job );

static int hb_rendersub_work( hb_filter_object_t * filter,
                                 hb_buffer_t ** buf_in,
                                 hb_buffer_t ** buf_out );

static void hb_rendersub_close( hb_filter_object_t * filter );

hb_filter_object_t hb_filter_render_sub =
{
    .id            = HB_FILTER_RENDER_SUB,
    .enforce_order = 1,
    .name          = "Subtitle renderer",
    .settings      = NULL,
    .init          = hb_rendersub_init,
    .post_init     = hb_rendersub_post_init,
    .work          = hb_rendersub_work,
    .close         = hb_rendersub_close,
};

// blends src YUVA4**P buffer into dst
static void blend8on8(hb_buffer_t *dst, const hb_buffer_t *src, const int left, const int top, const int shift)
{
    int xx, yy;
    int ww, hh;
    int x0, y0;
    uint8_t *y_in, *y_out;
    uint8_t *u_in, *u_out;
    uint8_t *v_in, *v_out;
    uint8_t *a_in, alpha;

    x0 = y0 = 0;
    if( left < 0 )
    {
        x0 = -left;
    }
    if( top < 0 )
    {
        y0 = -top;
    }

    ww = src->f.width;
    if( src->f.width - x0 > dst->f.width - left )
    {
        ww = dst->f.width - left + x0;
    }
    hh = src->f.height;
    if( src->f.height - y0 > dst->f.height - top )
    {
        hh = dst->f.height - top + y0;
    }
    // Blend luma
    for( yy = y0; yy < hh; yy++ )
    {
        y_in   = src->plane[0].data + yy * src->plane[0].stride;
        y_out   = dst->plane[0].data + ( yy + top ) * dst->plane[0].stride;
        a_in = src->plane[3].data + yy * src->plane[3].stride;
        for( xx = x0; xx < ww; xx++ )
        {
            alpha = a_in[xx];
            /*
             * Merge the luminance and alpha with the picture
             */
            y_out[left + xx] =
                ( (uint16_t)y_out[left + xx] * ( 255 - alpha ) +
                     (uint16_t)y_in[xx] * alpha ) / 255;
        }
    }

    // Blend U & V
    // Assumes source and dest are the same PIX_FMT
    int hshift = 0;
    int wshift = 0;
    if( dst->plane[1].height < dst->plane[0].height )
        hshift = 1;
    if( dst->plane[1].width < dst->plane[0].width )
        wshift = 1;

    for( yy = y0 >> hshift; yy < hh >> hshift; yy++ )
    {
        u_in = src->plane[1].data + yy * src->plane[1].stride;
        u_out = dst->plane[1].data + ( yy + ( top >> hshift ) ) * dst->plane[1].stride;
        v_in = src->plane[2].data + yy * src->plane[2].stride;
        v_out = dst->plane[2].data + ( yy + ( top >> hshift ) ) * dst->plane[2].stride;
        a_in = src->plane[3].data + ( yy << hshift ) * src->plane[3].stride;

        for( xx = x0 >> wshift; xx < ww >> wshift; xx++ )
        {
            alpha = a_in[xx << wshift];

            // Blend U and alpha
            u_out[(left >> wshift) + xx] =
                ( (uint16_t)u_out[(left >> wshift) + xx] * ( 255 - alpha ) +
                  (uint16_t)u_in[xx] * alpha ) / 255;

            // Blend V and alpha
            v_out[(left >> wshift) + xx] =
                ( (uint16_t)v_out[(left >> wshift) + xx] * ( 255 - alpha ) +
                  (uint16_t)v_in[xx] * alpha ) / 255;
        }
    }
}

static void blend8on1x(hb_buffer_t *dst, const hb_buffer_t *src, const int left, const int top, const int shift)
{
    int xx, yy;
    int ww, hh;
    int x0, y0;
    int max;

    uint8_t *y_in;
    uint8_t *u_in;
    uint8_t *v_in;
    uint8_t *a_in;

    uint16_t *y_out;
    uint16_t *u_out;
    uint16_t *v_out;
    uint16_t alpha;

    x0 = y0 = 0;
    if( left < 0 )
    {
        x0 = -left;
    }
    if( top < 0 )
    {
        y0 = -top;
    }

    ww = src->f.width;
    if( src->f.width - x0 > dst->f.width - left )
    {
        ww = dst->f.width - left + x0;
    }
    hh = src->f.height;
    if( src->f.height - y0 > dst->f.height - top )
    {
        hh = dst->f.height - top + y0;
    }

    max = (256 << shift) -1;

    // Blend luma
    for( yy = y0; yy < hh; yy++ )
    {
        y_in   = src->plane[0].data + yy * src->plane[0].stride;
        y_out   = (uint16_t*)(dst->plane[0].data + ( yy + top ) * dst->plane[0].stride);
        a_in = src->plane[3].data + yy * src->plane[3].stride;
        for( xx = x0; xx < ww; xx++ )
        {
            alpha = a_in[xx] << shift;
            /*
             * Merge the luminance and alpha with the picture
             */
            y_out[left + xx] =
                ( (uint32_t)y_out[left + xx] * ( max - alpha ) +
                     ((uint32_t)y_in[xx] << shift) * alpha ) / max;
        }
    }

    // Blend U & V
    int hshift = 0;
    int wshift = 0;
    if( dst->plane[1].height < dst->plane[0].height )
        hshift = 1;
    if( dst->plane[1].width < dst->plane[0].width )
        wshift = 1;

    for( yy = y0 >> hshift; yy < hh >> hshift; yy++ )
    {
        u_in = src->plane[1].data + yy * src->plane[1].stride;
        u_out = (uint16_t*)(dst->plane[1].data + ( yy + ( top >> hshift ) ) * dst->plane[1].stride);
        v_in = src->plane[2].data + yy * src->plane[2].stride;
        v_out = (uint16_t*)(dst->plane[2].data + ( yy + ( top >> hshift ) ) * dst->plane[2].stride);
        a_in = src->plane[3].data + ( yy << hshift ) * src->plane[3].stride;

        for( xx = x0 >> wshift; xx < ww >> wshift; xx++ )
        {
            alpha = a_in[xx << wshift] << shift;

            // Blend U and alpha
            u_out[(left >> wshift) + xx] =
                ( (uint32_t)u_out[(left >> wshift) + xx] * ( max - alpha ) +
                  ((uint32_t)u_in[xx] << shift) * alpha ) / max;

            // Blend V and alpha
            v_out[(left >> wshift) + xx] =
                ( (uint32_t)v_out[(left >> wshift) + xx] * ( max - alpha ) +
                  ((uint32_t)v_in[xx] << shift) * alpha ) / max;
        }
    }
}

static void blend8onbi8(hb_buffer_t *dst, const hb_buffer_t *src, const int left, const int top, const int shift)
{
    int xx, yy;
    int ww, hh;
    int x0, y0;
    uint8_t *y_in, *y_out;
    uint8_t *u_in, *u_out;
    uint8_t *v_in, *v_out;
    uint8_t *a_in, alpha;

    x0 = y0 = 0;
    if (left < 0)
    {
        x0 = -left;
    }
    if (top < 0)
    {
        y0 = -top;
    }

    ww = src->f.width;
    if (src->f.width - x0 > dst->f.width - left)
    {
        ww = dst->f.width - left + x0;
    }
    hh = src->f.height;
    if (src->f.height - y0 > dst->f.height - top)
    {
        hh = dst->f.height - top + y0;
    }
    // Blend luma
    for (yy = y0; yy < hh; yy++)
    {
        y_in   = src->plane[0].data + yy * src->plane[0].stride;
        y_out   = dst->plane[0].data + ( yy + top ) * dst->plane[0].stride;
        a_in = src->plane[3].data + yy * src->plane[3].stride;
        for (xx = x0; xx < ww; xx++)
        {
            alpha = a_in[xx];
            /*
             * Merge the luminance and alpha with the picture
             */
            y_out[left + xx] =
                ( (uint16_t)y_out[left + xx] * ( 255 - alpha ) +
                     (uint16_t)y_in[xx] * alpha ) / 255;
        }
    }

    // Blend U & V
    // Assumes source and dest are the same PIX_FMT
    int hshift = 0;
    int wshift = 0;
    if (dst->plane[1].height < dst->plane[0].height)
        hshift = 1;
    if (dst->plane[1].width < dst->plane[0].width)
        wshift = 1;

    for (yy = y0 >> hshift; yy < hh >> hshift; yy++)
    {
        u_in = src->plane[1].data + yy * src->plane[1].stride;
        u_out = dst->plane[1].data + (yy + (top >> hshift)) * dst->plane[1].stride;
        v_in = src->plane[2].data + yy * src->plane[2].stride;
        v_out = dst->plane[1].data + (yy + (top >> hshift)) * dst->plane[1].stride;
        a_in = src->plane[3].data + (yy << hshift) * src->plane[3].stride;

        for (xx = x0 >> wshift; xx < ww >> wshift; xx++)
        {
            alpha = a_in[xx << wshift];

            // Blend U and alpha
            u_out[((left >> wshift) + xx) * 2] =
                ( (uint16_t)u_out[((left >> wshift) + xx) * 2] * (255 - alpha) +
                  (uint16_t)u_in[xx] * alpha) / 255;

            // Blend V and alpha
            v_out[((left >> wshift) + xx) * 2 +1] =
                ( (uint16_t)v_out[((left >> wshift) + xx) * 2 + 1] * (255 - alpha) +
                  (uint16_t)v_in[xx] * alpha) / 255;
        }
    }
}


static void blend8onbi1x(hb_buffer_t *dst, const hb_buffer_t *src, const int left, const int top, const int shift)
{
    int xx, yy;
    int ww, hh;
    int x0, y0;
    int max;

    uint8_t *y_in;
    uint8_t *u_in;
    uint8_t *v_in;
    uint8_t *a_in;

    uint16_t *y_out;
    uint16_t *u_out;
    uint16_t *v_out;
    uint16_t alpha;

    x0 = y0 = 0;
    if (left < 0)
    {
        x0 = -left;
    }
    if (top < 0)
    {
        y0 = -top;
    }

    ww = src->f.width;
    if (src->f.width - x0 > dst->f.width - left)
    {
        ww = dst->f.width - left + x0;
    }
    hh = src->f.height;
    if (src->f.height - y0 > dst->f.height - top)
    {
        hh = dst->f.height - top + y0;
    }

    max = (256 << shift) -1;

    // Blend luma
    for (yy = y0; yy < hh; yy++)
    {
        y_in   = src->plane[0].data + yy * src->plane[0].stride;
        y_out   = (uint16_t*)(dst->plane[0].data + ( yy + top ) * dst->plane[0].stride);
        a_in = src->plane[3].data + yy * src->plane[3].stride;
        for( xx = x0; xx < ww; xx++ )
        {
            alpha = a_in[xx] << shift;
            /*
             * Merge the luminance and alpha with the picture
             */
            y_out[left + xx] =
                ( (uint32_t)y_out[left + xx] * ( max - alpha ) +
                     ((uint32_t)av_bswap16(y_in[xx])) * alpha ) / max;
        }
    }

    // Blend U & V
    int hshift = 0;
    int wshift = 0;
    if (dst->plane[1].height < dst->plane[0].height)
        hshift = 1;
    if (dst->plane[1].width < dst->plane[0].width)
        wshift = 1;

    for (yy = y0 >> hshift; yy < hh >> hshift; yy++)
    {
        u_in = src->plane[1].data + yy * src->plane[1].stride;
        u_out = (uint16_t *)(dst->plane[1].data + ( yy + ( top >> hshift ) ) * dst->plane[1].stride);
        v_in = src->plane[2].data + yy * src->plane[2].stride;
        v_out = (uint16_t *)(dst->plane[1].data + ( yy + ( top >> hshift ) ) * dst->plane[1].stride);
        a_in = src->plane[3].data + ( yy << hshift ) * src->plane[3].stride;

        for (xx = x0 >> wshift; xx < ww >> wshift; xx++)
        {
            alpha = a_in[xx << wshift] << shift;

            // Blend averge U and alpha
            u_out[((left >> wshift) + xx) * 2] =
                ( (uint32_t)u_out[((left >> wshift) + xx) * 2] * ( max - alpha ) +
                  ((uint32_t)av_bswap16(u_in[xx])) * alpha ) / max;

            // Blend V and alpha
            v_out[((left >> wshift) + xx) * 2 + 1] =
                ( (uint32_t)v_out[((left >> wshift) + xx) * 2 + 1] * ( max - alpha ) +
                  ((uint32_t)av_bswap16(v_in[xx])) * alpha ) / max;
        }
    }
}

// Assumes that the input destination buffer has the same dimensions
// as the original title dimensions
static void ApplySub(const hb_filter_private_t *pv, hb_buffer_t *buf, const hb_buffer_t *sub)
{
    pv->blend(buf, sub, sub->f.x, sub->f.y, pv->depth - 8);
}

static hb_buffer_t * ScaleSubtitle(hb_filter_private_t *pv,
                                   hb_buffer_t *sub, hb_buffer_t *buf)
{
    hb_buffer_t * scaled;
    double xfactor = 1., yfactor = 1.;

    // Do we need to rescale subtitles?
    if (sub->f.window_width > 0 && sub->f.window_height > 0)
    {

        // TODO: Factor aspect ratio
        // For now, assume subtitle and video PAR is the same.
        xfactor     = (double)buf->f.width  / sub->f.window_width;
        yfactor     = (double)buf->f.height / sub->f.window_height;
        // The video may have been cropped.  This will make xfactor != yfactor
        // even though video and subtitles are the same PAR.  So use the
        // larger of as the scale factor.
        if (xfactor > yfactor)
        {
            yfactor = xfactor;
        }
        else
        {
            xfactor = yfactor;
        }
    }
    if (ABS(xfactor - 1) > 0.01 || ABS(yfactor - 1) > 0.01)
    {
        uint8_t * in_data[4], * out_data[4];
        int       in_stride[4], out_stride[4];
        int       width, height;

        width       = sub->f.width  * xfactor;
        height      = sub->f.height * yfactor;
        // Note that subtitle frame buffer is YUVA420P, not YUV420P, it has alpha
        scaled      = hb_frame_buffer_init(AV_PIX_FMT_YUVA420P, width, height);
        if (scaled == NULL)
            return NULL;

        scaled->f.x = sub->f.x * xfactor;
        scaled->f.y = sub->f.y * yfactor;

        hb_picture_fill(in_data,  in_stride,  sub);
        hb_picture_fill(out_data, out_stride, scaled);

        if (pv->sws        == NULL   ||
            pv->sws_width  != width  ||
            pv->sws_height != height)
        {
            if (pv->sws!= NULL)
                sws_freeContext(pv->sws);
            pv->sws = hb_sws_get_context(
                                sub->f.width, sub->f.height, sub->f.fmt, AVCOL_RANGE_MPEG,
                                scaled->f.width, scaled->f.height, sub->f.fmt, AVCOL_RANGE_MPEG,
                                SWS_LANCZOS|SWS_ACCURATE_RND, SWS_CS_DEFAULT);
            pv->sws_width   = width;
            pv->sws_height  = height;
        }
        sws_scale(pv->sws, (const uint8_t* const *)in_data, in_stride,
                  0, sub->f.height, out_data, out_stride);
    }
    else
    {
        scaled = hb_buffer_dup(sub);
    }

    int top, left, margin_top, margin_percent;

    /*
     * Percent of height of picture that form a margin that subtitles
     * should not be displayed within.
     */
    margin_percent = 2;

    /*
     * If necessary, move the subtitle so it is not in a cropped zone.
     * When it won't fit, we center it so we lose as much on both ends.
     * Otherwise we try to leave a 20px or 2% margin around it.
     */
    margin_top = ( ( buf->f.height - pv->crop[0] - pv->crop[1] ) *
                   margin_percent ) / 100;

    if( margin_top > 20 )
    {
        /*
         * A maximum margin of 20px regardless of height of the picture.
         */
        margin_top = 20;
    }

    if( scaled->f.height > buf->f.height - pv->crop[0] - pv->crop[1] -
        ( margin_top * 2 ) )
    {
        /*
         * The subtitle won't fit in the cropped zone, so center
         * it vertically so we fit in as much as we can.
         */
        top = pv->crop[0] + ( buf->f.height - pv->crop[0] -
                                      pv->crop[1] - scaled->f.height ) / 2;
    }
    else if( scaled->f.y < pv->crop[0] + margin_top )
    {
        /*
         * The subtitle fits in the cropped zone, but is currently positioned
         * within our top margin, so move it outside of our margin.
         */
        top = pv->crop[0] + margin_top;
    }
    else if( scaled->f.y > buf->f.height - pv->crop[1] - margin_top - scaled->f.height )
    {
        /*
         * The subtitle fits in the cropped zone, and is not within the top
         * margin but is within the bottom margin, so move it to be above
         * the margin.
         */
        top = buf->f.height - pv->crop[1] - margin_top - scaled->f.height;
    }
    else
    {
        /*
         * The subtitle is fine where it is.
         */
        top = scaled->f.y;
    }

    if( scaled->f.width > buf->f.width - pv->crop[2] - pv->crop[3] - 40 )
        left = pv->crop[2] + ( buf->f.width - pv->crop[2] -
                pv->crop[3] - scaled->f.width ) / 2;
    else if( scaled->f.x < pv->crop[2] + 20 )
        left = pv->crop[2] + 20;
    else if( scaled->f.x > buf->f.width - pv->crop[3] - 20 - scaled->f.width )
        left = buf->f.width - pv->crop[3] - 20 - scaled->f.width;
    else
        left = scaled->f.x;

    scaled->f.x = left;
    scaled->f.y = top;

    return scaled;
}

// Assumes that the input buffer has the same dimensions
// as the original title dimensions
static void ApplyVOBSubs( hb_filter_private_t * pv, hb_buffer_t * buf )
{
    int ii;
    hb_buffer_t *sub, *next;

    // Note that VOBSUBs can overlap in time.
    // I.e. more than one may be rendered to the screen at once.
    for( ii = 0; ii < hb_list_count(pv->sub_list); )
    {
        sub = hb_list_item( pv->sub_list, ii );
        if (ii + 1 < hb_list_count(pv->sub_list))
            next = hb_list_item( pv->sub_list, ii + 1 );
        else
            next = NULL;

        if ((sub->s.stop != AV_NOPTS_VALUE && sub->s.stop <= buf->s.start) ||
            (next != NULL && sub->s.stop == AV_NOPTS_VALUE && next->s.start <= buf->s.start))
        {
            // Subtitle stop is in the past, delete it
            hb_list_rem( pv->sub_list, sub );
            hb_buffer_close( &sub );
        }
        else if( sub->s.start <= buf->s.start )
        {
            // The subtitle has started before this frame and ends
            // after it.  Render the subtitle into the frame.
            while ( sub )
            {
                hb_buffer_t *scaled = ScaleSubtitle(pv, sub, buf);
                ApplySub( pv, buf, scaled );
                hb_buffer_close(&scaled);
                sub = sub->next;
            }
            ii++;
        }
        else
        {
            // The subtitle starts in the future.  No need to continue.
            break;
        }
    }
}

static int vobsub_post_init( hb_filter_object_t * filter, hb_job_t * job )
{
    hb_filter_private_t * pv = filter->private_data;

    pv->sub_list = hb_list_init();

    return 0;
}

static void vobsub_close( hb_filter_object_t * filter )
{
    hb_filter_private_t * pv = filter->private_data;

    if( !pv )
    {
        return;
    }

    if( pv->sub_list )
        hb_list_empty( &pv->sub_list );

    free( pv );
    filter->private_data = NULL;
}

static int vobsub_work( hb_filter_object_t * filter,
                        hb_buffer_t ** buf_in,
                        hb_buffer_t ** buf_out )
{
    hb_filter_private_t * pv = filter->private_data;
    hb_buffer_t * in = *buf_in;
    hb_buffer_t * sub;

    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        *buf_in = NULL;
        *buf_out = in;
        return HB_FILTER_DONE;
    }

    // Get any pending subtitles and add them to the active
    // subtitle list
    while( ( sub = hb_fifo_get( filter->subtitle->fifo_out ) ) )
    {
        if (sub->s.flags & HB_BUF_FLAG_EOF)
        {
            hb_buffer_close(&sub);
            break;
        }
        hb_list_add( pv->sub_list, sub );
    }

    ApplyVOBSubs( pv, in );
    *buf_in = NULL;
    *buf_out = in;

    return HB_FILTER_OK;
}

#define ALPHA_BLEND(srcA, srcRGB, dstAc, dstRGB, outA) \
    (((srcA * srcRGB + dstRGB * dstAc + (outA >> 1)) / outA))
#define div255(x) (((x + ((x + 128) >> 8)) + 128) >> 8)

static uint8_t inline ssaAlpha(const ASS_Image *frame, const int x, const int y)
{
    const unsigned frameA = (frame->color) & 0xff;
    const unsigned gliphA = frame->bitmap[y*frame->stride + x];

    // Alpha for this pixel is the frame opacity (255 - frameA)
    // multiplied by the gliph alpha (gliphA) for this pixel
    return (uint8_t)div255((255 - frameA) * gliphA);
}

static hb_buffer_t* ComposeSubsampleASS( hb_filter_private_t *pv, const ASS_Image *frameList,
                                         const unsigned width, const unsigned height,
                                         const unsigned x, const unsigned y )
{
    const ASS_Image *frame = frameList;
    const unsigned flatStride = width << 2;
    uint8_t *compo = calloc( flatStride*height, sizeof(uint8_t) );

    if ( !compo ) {
        return NULL;
    }

    while ( frame ) {
        if ( frame->w && frame->h ) {
            const int yuv = hb_rgb2yuv_bt709( frame->color >> 8 );

            const unsigned frameY = (yuv >> 16) & 0xff;
            const unsigned frameV = (yuv >> 8 ) & 0xff;
            const unsigned frameU = (yuv >> 0 ) & 0xff;
            unsigned frameA, resAlpha, alphaCompo, alphaInScaled;

            const unsigned iniFx = (frame->dst_x - x)*4 + (frame->dst_y - y)*flatStride;

            for ( int yy = 0, fx = iniFx; yy < frame->h; yy++, fx = iniFx + yy*flatStride )
            {
                for ( int xx = 0; xx < frame->w; xx++, fx += 4 )
                {
                    frameA = ssaAlpha( frame, xx, yy );

                    //skip if transparent
                    if ( frameA ) {
                        if ( compo[fx+3] ) {
                            alphaInScaled = frameA * 255;
                            alphaCompo = compo[fx+3] * (255 - frameA);
                            resAlpha = (alphaInScaled + alphaCompo);

                            compo[fx  ] = ALPHA_BLEND( alphaInScaled, frameY, alphaCompo, compo[fx  ], resAlpha );
                            compo[fx+1] = ALPHA_BLEND( alphaInScaled, frameU, alphaCompo, compo[fx+1], resAlpha );
                            compo[fx+2] = ALPHA_BLEND( alphaInScaled, frameV, alphaCompo, compo[fx+2], resAlpha );
                            compo[fx+3] = div255( resAlpha );
                        } else {
                            compo[fx  ] = frameY;
                            compo[fx+1] = frameU;
                            compo[fx+2] = frameV;
                            compo[fx+3] = frameA;
                        }
                    }
                }
            }
        }
        frame = frame->next;
    }

    hb_buffer_t *sub = hb_frame_buffer_init( pv->pix_fmt_alpha, width, height );
    if ( !sub ) {
        free(compo);
        return NULL;
    }

    sub->f.x = x;
    sub->f.y = y;

    uint8_t *y_out, *u_out, *v_out, *a_out;
    y_out = sub->plane[0].data;
    u_out = sub->plane[1].data;
    v_out = sub->plane[2].data;
    a_out = sub->plane[3].data;

    unsigned int accuA, accuB, accuC, coeff, isChromaLine;

    for ( int yy = 0, ys = 0, fx = 0; yy < height; yy++, ys = yy >> pv->hshift, fx = yy*flatStride )
    {
        isChromaLine = yy == ys << pv->hshift;
        for ( int xx = 0, xs = 0; xx < width; xx++, xs = xx >> pv->wshift, fx += 4 )
        {
            y_out[xx] = compo[ fx ];
            a_out[xx] = compo[fx+3];

            //Are we on a chroma sample?
            if ( isChromaLine && xx == xs << pv->wshift ) {
                accuA = accuB = accuC = 0;
                for ( int yz = 0; yz < (1 << pv->hshift) && (yz + yy < height); yz++ ) {
                    for ( int xz = 0; xz < (1 << pv->wshift) && (xz + xx < width); xz++ ) {
                        //Access compo to avoid cache misses with a_out.
                        coeff = pv->chromaCoeffs[0][xz]*pv->chromaCoeffs[1][yz]*compo[fx + yz*flatStride + 3];
                        accuA += coeff*compo[fx + yz*flatStride + 1];
                        accuB += coeff*compo[fx + yz*flatStride + 2];
                        accuC += coeff;
                    }
                }
                if (accuC) {
                    u_out[xs] = (accuA + (accuC - 1))/accuC;
                    v_out[xs] = (accuB + (accuC - 1))/accuC;
                }
            }
        }

        if (isChromaLine) {
            u_out += sub->plane[1].stride;
            v_out += sub->plane[2].stride;
        }
        y_out += sub->plane[0].stride;
        a_out += sub->plane[3].stride;
    }

    free(compo);
    return sub;
}

// To enable and use with bitmap subtitles
#if 0
// Output a YUVA4**P frame from a YUVA444P8 frame
static hb_buffer_t * SubsampleOverlay(const hb_filter_private_t *pv, const hb_buffer_t *overlay)
{
    hb_buffer_t *sub = hb_frame_buffer_init( pv->pix_fmt_alpha, overlay->f.width, overlay->f.height );
    if ( !sub )
    {
        return NULL;
    }
    sub->f.x = overlay->f.x;
    sub->f.y = overlay->f.y;

    uint8_t *y_out, *u_out, *v_out, *a_out, *pA_in, *pB_in;
    y_out = sub->plane[0].data;
    u_out = sub->plane[1].data;
    v_out = sub->plane[2].data;
    a_out = sub->plane[3].data;

    //process YA
    pA_in = overlay->plane[0].data;
    pB_in = overlay->plane[3].data;
    for (int yy = 0; yy < overlay->f.height; yy++)
    {
        memcpy(y_out, pA_in, overlay->f.width);
        memcpy(a_out, pB_in, overlay->f.width);
        y_out += sub->plane[0].stride;
        a_out += sub->plane[3].stride;
        pA_in += overlay->plane[0].stride;
        pB_in += overlay->plane[3].stride;
    }

    unsigned int accuA, accuB, accuC;
    unsigned int coeff;

    //process UV
    pA_in = overlay->plane[1].data;
    pB_in = overlay->plane[2].data;
    a_out = sub->plane[3].data;
    for (int yy = 0; yy < overlay->f.height >> pv->hshift; yy++)
    {
        for (int xx = 0, xo = 0; xx < overlay->f.width >> pv->wshift; xx++, xo = xx << pv->wshift)
        {
            accuA = accuB = accuC = 0;
            for (int yz = 0; yz < (1 << pv->hshift); yz++) {
                for (int xz = 0; xz < (1 << pv->wshift); xz++) {
                    coeff = pv->chromaCoeffs[0][xz]*pv->chromaCoeffs[1][yz]*(a_out + yz*sub->plane[3].stride)[xo + xz];
                    accuA += coeff*(pA_in + yz*overlay->plane[1].stride)[xo + xz];
                    accuB += coeff*(pB_in + yz*overlay->plane[2].stride)[xo + xz];
                    accuC += coeff;
                }
            }
            if (accuC) {
                u_out[xx] = (accuA + (accuC - 1))/accuC;
                v_out[xx] = (accuB + (accuC - 1))/accuC;
            }
        }
        u_out += sub->plane[1].stride;
        v_out += sub->plane[2].stride;
        a_out += sub->plane[3].stride << pv->hshift;
        pA_in += overlay->plane[1].stride << pv->hshift;
        pB_in += overlay->plane[2].stride << pv->hshift;
    }
    return sub;
}
#endif

static hb_buffer_t * render_ssa_subs(hb_filter_private_t *pv, int64_t start)
{
    ASS_Image *frameList;
    int changed;

    frameList = ass_render_frame( pv->renderer, pv->ssaTrack,
                                  start / 90, &changed );
    if ( !frameList )
    {
        return NULL;
    }

    // re-use cached overlay, whenever possible
    if ( changed ) {
        if ( pv->last_render )
            hb_buffer_close( &pv->last_render );

        unsigned int x1, y1, x2, y2;
        x1 = y1 = (unsigned)(-1);
        x2 = y2 = 0;

        //Find overlay size and pos (faster than composing at the video dimensions)
        for ( ASS_Image *frame = frameList; frame; frame = frame->next ) {
            if ( frame->w && frame->h ) {
                x2 = FFMAX( x2, frame->dst_x + frame->w );
                y2 = FFMAX( y2, frame->dst_y + frame->h );
                x1 = FFMIN( x1, frame->dst_x );
                y1 = FFMIN( y1, frame->dst_y );
            }
        }

        //don't process empty framelist
        if (x2 > 0) {
            //overlay must be aligned to the chroma plane, pad as needed.
            x1 -= (x1 + pv->crop[2]) & ((1 << pv->wshift) - 1);
            y1 -= (y1 + pv->crop[0]) & ((1 << pv->hshift) - 1);

            pv->last_render = ComposeSubsampleASS( pv, frameList, x2-x1, y2-y1, x1, y1 );

            if ( pv->last_render ) {
                pv->last_render->f.x += pv->crop[2];
                pv->last_render->f.y += pv->crop[0];
            }
        }
    }

    if ( pv->last_render )
    {
        return pv->last_render;
    }
    else
    {
        return NULL;
    }
}

static void ssa_log(int level, const char *fmt, va_list args, void *data)
{
    if ( level < 5 )      // same as default verbosity when no callback is set
    {
        hb_valog( 1, "[ass]", fmt, args );
    }
}

static int ssa_post_init( hb_filter_object_t * filter, hb_job_t * job )
{
    hb_filter_private_t * pv = filter->private_data;

    pv->ssa = ass_library_init();
    if ( !pv->ssa ) {
        hb_error( "decssasub: libass initialization failed\n" );
        return 1;
    }

    // Redirect libass output to hb_log
    ass_set_message_cb( pv->ssa, ssa_log, NULL );

    // Load embedded fonts
    hb_list_t * list_attachment = job->list_attachment;
    int i;
    for ( i = 0; i < hb_list_count(list_attachment); i++ )
    {
        hb_attachment_t * attachment = hb_list_item( list_attachment, i );

        if ( attachment->type == FONT_TTF_ATTACH ||
             attachment->type == FONT_OTF_ATTACH )
        {
            ass_add_font(
                pv->ssa,
                attachment->name,
                attachment->data,
                attachment->size );
        }
    }

    ass_set_extract_fonts( pv->ssa, 1 );
    ass_set_style_overrides( pv->ssa, NULL );

    pv->renderer = ass_renderer_init( pv->ssa );
    if ( !pv->renderer ) {
        hb_log( "decssasub: renderer initialization failed\n" );
        return 1;
    }

    ass_set_use_margins( pv->renderer, 0 );
    ass_set_hinting( pv->renderer, ASS_HINTING_NONE );
    ass_set_font_scale( pv->renderer, 1.0 );
    ass_set_line_spacing( pv->renderer, 1.0 );

    // Setup default font family
    //
    // SSA v4.00 requires that "Arial" be the default font
    const char *font = NULL;
    const char *family = "Arial";
    // NOTE: This can sometimes block for several *seconds*.
    //       It seems that process_fontdata() for some embedded fonts is slow.
    ass_set_fonts( pv->renderer, font, family, /*haveFontConfig=*/1, NULL, 1 );

    // Setup track state
    pv->ssaTrack = ass_new_track( pv->ssa );
    if ( !pv->ssaTrack ) {
        hb_log( "decssasub: ssa track initialization failed\n" );
        return 1;
    }

    // Do not use Read Order to eliminate duplicates
    // we never send the same subtitles sample twice,
    // and some MKVs have duplicated Read Orders
    // and won't render properly when this is enabled.
    ass_set_check_readorder(pv->ssaTrack, 0);

    int height = job->title->geometry.height - job->crop[0] - job->crop[1];
    int width = job->title->geometry.width - job->crop[2] - job->crop[3];
    ass_set_frame_size(pv->renderer, width, height);
    ass_set_storage_size(pv->renderer, width, height);

    if ( pv->last_render ) {
        hb_buffer_close(&pv->last_render);
        pv->last_render = NULL;
    }

    return 0;
}

static void ssa_close( hb_filter_object_t * filter )
{
    hb_filter_private_t * pv = filter->private_data;

    if( !pv )
    {
        return;
    }

    if ( pv->ssaTrack )
        ass_free_track( pv->ssaTrack );
    if ( pv->renderer )
        ass_renderer_done( pv->renderer );
    if ( pv->ssa )
        ass_library_done( pv->ssa );
    if ( pv->last_render )
        hb_buffer_close( &pv->last_render );

    free( pv );
    filter->private_data = NULL;
}

static int ssa_work( hb_filter_object_t * filter,
                     hb_buffer_t ** buf_in,
                     hb_buffer_t ** buf_out )
{
    hb_filter_private_t * pv = filter->private_data;
    hb_buffer_t * in = *buf_in;
    hb_buffer_t * out = in;
    hb_buffer_t * sub;
    hb_buffer_t * rendered_subs;

    if (!pv->script_initialized)
    {
        // NOTE: The codec extradata is expected to be in MKV format
        // I would like to initialize this in ssa_post_init, but when we are
        // transcoding text subtitles to SSA, the extradata does not
        // get initialized until the decoder is initialized.  Since
        // decoder initialization happens after filter initialization,
        // we need to postpone this.
        ass_process_codec_private(pv->ssaTrack,
                                  (char *)filter->subtitle->extradata->bytes,
                                  filter->subtitle->extradata->size);
        pv->script_initialized = 1;
    }
    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        *buf_in = NULL;
        *buf_out = in;
        return HB_FILTER_DONE;
    }

    // Get any pending subtitles and add them to the active
    // subtitle list
    while( ( sub = hb_fifo_get( filter->subtitle->fifo_out ) ) )
    {
        if (sub->s.flags & HB_BUF_FLAG_EOF)
        {
            hb_buffer_close(&sub);
            break;
        }
        // Parse MKV-SSA packet
        // SSA subtitles always have an explicit stop time, so we
        // do not need to do special processing for stop == AV_NOPTS_VALUE
        ass_process_chunk( pv->ssaTrack, (char*)sub->data, sub->size,
                           sub->s.start / 90,
                           (sub->s.stop - sub->s.start) / 90 );
        hb_buffer_close(&sub);
    }

    rendered_subs = render_ssa_subs(pv, in->s.start);

    if (rendered_subs && hb_buffer_is_writable(in) == 0)
    {
        out = hb_buffer_dup(in);
    }
    else
    {
        *buf_in = NULL;
    }

    if (rendered_subs)
    {
        ApplySub(pv, out, rendered_subs);
    }

    *buf_out = out;

    return HB_FILTER_OK;
}

static int cc608sub_post_init( hb_filter_object_t * filter, hb_job_t * job )
{
    // Text subtitles for which we create a dummy ASS header need
    // to have the header rewritten with the correct dimensions.
    int height = job->title->geometry.height - job->crop[0] - job->crop[1];
    int width = job->title->geometry.width - job->crop[2] - job->crop[3];
    int safe_height = 0.8 * job->title->geometry.height;
    // Use fixed width font for CC
    hb_set_ssa_extradata(&filter->subtitle->extradata, HB_FONT_MONO,
                         .08 * safe_height, width, height);
    return ssa_post_init(filter, job);
}

static int textsub_post_init( hb_filter_object_t * filter, hb_job_t * job )
{
    // Text subtitles for which we create a dummy ASS header need
    // to have the header rewritten with the correct dimensions.
    int height = job->title->geometry.height - job->crop[0] - job->crop[1];
    int width = job->title->geometry.width - job->crop[2] - job->crop[3];
    hb_set_ssa_extradata(&filter->subtitle->extradata, HB_FONT_SANS,
                         .066 * job->title->geometry.height,
                         width, height);
    return ssa_post_init(filter, job);
}

static void textsub_close( hb_filter_object_t * filter )
{
    return ssa_close(filter);
}

static void process_sub(hb_filter_private_t *pv, hb_buffer_t *sub)
{
    ass_process_chunk(pv->ssaTrack, (char *)sub->data, sub->size,
                      sub->s.start, sub->s.stop - sub->s.start);
}

static int textsub_work(hb_filter_object_t * filter,
                    hb_buffer_t ** buf_in,
                    hb_buffer_t ** buf_out)
{
    hb_filter_private_t * pv = filter->private_data;
    hb_buffer_t * in = *buf_in;
    hb_buffer_t * out = in;
    hb_buffer_t * sub;
    hb_buffer_t * rendered_subs;

    if (!pv->script_initialized)
    {
        ass_process_codec_private(pv->ssaTrack,
                                  (char*)filter->subtitle->extradata->bytes,
                                  filter->subtitle->extradata->size);
        pv->script_initialized = 1;
    }

    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        *buf_in = NULL;
        *buf_out = in;
        return HB_FILTER_DONE;
    }

    int in_start_ms = in->s.start / 90;

    // Get any pending subtitles and add them to the active
    // subtitle list
    while ((sub = hb_fifo_get(filter->subtitle->fifo_out)))
    {
        if (sub->s.flags & HB_BUF_FLAG_EOF)
        {
            hb_buffer_close(&sub);
            if (pv->current_sub != NULL)
            {
                // Make us some duration for final sub
                pv->current_sub->s.stop = pv->current_sub->s.start +
                                          90000LL * 10;
                process_sub(pv, pv->current_sub);
                hb_buffer_close(&pv->current_sub);
            }
            break;
        }

        // libass expects times in ms.  So to make the math easy,
        // convert to ms immediately.
        sub->s.start /= 90;
        if (sub->s.stop != AV_NOPTS_VALUE)
        {
            sub->s.stop /= 90;
        }

        // Subtitle formats such as CC can have stop times
        // that are not known until an "erase display" command
        // is encountered in the stream.  For these formats
        // current_sub is the currently active subtitle for which
        // we do not yet know the stop time.  We do not currently
        // support overlapping subtitles of this type.
        if (pv->current_sub != NULL)
        {
            // Next sub start time tells us the stop time of the
            // current sub when it is not known in advance.
            pv->current_sub->s.stop = sub->s.start;
            process_sub(pv, pv->current_sub);
            hb_buffer_close(&pv->current_sub);
        }
        if (sub->s.flags & HB_BUF_FLAG_EOS)
        {
            // marker used to "clear" previous sub that had
            // an unknown duration
            hb_buffer_close(&sub);
        }
        else if (sub->s.stop == AV_NOPTS_VALUE)
        {
            // We don't know the duration of this sub.  So we will
            // apply it to every video frame until we see a "clear" sub.
            pv->current_sub = sub;
            pv->current_sub->s.stop = pv->current_sub->s.start;
        }
        else
        {
            // Duration of this subtitle is known, so we can just
            // process it normally.
            process_sub(pv, sub);
            hb_buffer_close(&sub);
        }
    }
    if (pv->current_sub != NULL && pv->current_sub->s.start <= in_start_ms)
    {
        // We don't know the duration of this subtitle, but we know
        // that it started before the current video frame and that
        // it is still active.  So render it on this video frame.
        pv->current_sub->s.start = pv->current_sub->s.stop;
        pv->current_sub->s.stop = in_start_ms + 1;
        process_sub(pv, pv->current_sub);
    }

    rendered_subs = render_ssa_subs(pv, in->s.start);

    if (rendered_subs && hb_buffer_is_writable(in) == 0)
    {
        out = hb_buffer_dup(in);
    }
    else
    {
        *buf_in = NULL;
    }

    if (rendered_subs)
    {
        ApplySub(pv, out, rendered_subs);
    }

    *buf_out = out;

    return HB_FILTER_OK;
}

static void ApplyPGSSubs( hb_filter_private_t * pv, hb_buffer_t * buf )
{
    int index;
    hb_buffer_t * old_sub;
    hb_buffer_t * sub;

    // Each PGS subtitle supersedes anything that preceded it.
    // Find the active subtitle (if there is one), and delete
    // everything before it.
    for( index = hb_list_count( pv->sub_list ) - 1; index > 0; index-- )
    {
        sub = hb_list_item( pv->sub_list, index);
        if ( sub->s.start <= buf->s.start )
        {
            while ( index > 0 )
            {
                old_sub = hb_list_item( pv->sub_list, index - 1);
                hb_list_rem( pv->sub_list, old_sub );
                hb_buffer_close( &old_sub );
                index--;
            }
        }
    }

    // Some PGS subtitles have no content and only serve to clear
    // the screen. If any of these are at the front of our list,
    // we can now get rid of them.
    while ( hb_list_count( pv->sub_list ) > 0 )
    {
        sub = hb_list_item( pv->sub_list, 0 );
        if (sub->f.width != 0 && sub->f.height != 0)
            break;

        hb_list_rem( pv->sub_list, sub );
        hb_buffer_close( &sub );
    }

    // Check to see if there's an active subtitle, and apply it.
    if ( hb_list_count( pv->sub_list ) > 0)
    {
        sub = hb_list_item( pv->sub_list, 0 );
        if ( sub->s.start <= buf->s.start )
        {
            hb_buffer_t *scaled = ScaleSubtitle(pv, sub, buf);
            ApplySub( pv, buf, scaled );
            hb_buffer_close(&scaled);
        }
    }
}

static int pgssub_post_init( hb_filter_object_t * filter, hb_job_t * job )
{
    hb_filter_private_t * pv = filter->private_data;

    pv->sub_list = hb_list_init();

    return 0;
}

static void pgssub_close( hb_filter_object_t * filter )
{
    hb_filter_private_t * pv = filter->private_data;

    if ( !pv )
    {
        return;
    }

    if ( pv->sub_list )
        hb_list_empty( &pv->sub_list );

    free( pv );
    filter->private_data = NULL;
}

static int pgssub_work( hb_filter_object_t * filter,
                        hb_buffer_t ** buf_in,
                        hb_buffer_t ** buf_out)
{
    hb_filter_private_t * pv = filter->private_data;
    hb_buffer_t * in = *buf_in;
    hb_buffer_t * sub;

    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        *buf_in = NULL;
        *buf_out = in;
        return HB_FILTER_DONE;
    }

    // Get any pending subtitles and add them to the active
    // subtitle list
    while ( ( sub = hb_fifo_get( filter->subtitle->fifo_out ) ) )
    {
        if (sub->s.flags & HB_BUF_FLAG_EOF)
        {
            hb_buffer_close(&sub);
            break;
        }
        hb_list_add( pv->sub_list, sub );
    }

    ApplyPGSSubs( pv, in );
    *buf_in = NULL;
    *buf_out = in;

    return HB_FILTER_OK;
}

static int hb_rendersub_init( hb_filter_object_t * filter,
                              hb_filter_init_t * init )
{
    filter->private_data = calloc( 1, sizeof(struct hb_filter_private_s) );
    if (filter->private_data == NULL)
    {
        hb_error("rendersub: calloc failed");
        return -1;
    }
    hb_filter_private_t * pv = filter->private_data;
    hb_subtitle_t *subtitle;
    int ii;

    pv->input = *init;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(init->pix_fmt);
    pv->depth      = desc->comp[0].depth;
    pv->wshift     = desc->log2_chroma_w;
    pv->hshift     = desc->log2_chroma_h;

    //Compute chroma smoothing coefficients wrt video chroma location
    int wX, wY;
    wX = 4 - (1 << desc->log2_chroma_w);
    wY = 4 - (1 << desc->log2_chroma_h);

    switch (init->chroma_location)
    {
    case AVCHROMA_LOC_TOPLEFT:
        wX += (1 << desc->log2_chroma_w) - 1;
    case AVCHROMA_LOC_TOP:
        wY += (1 << desc->log2_chroma_h) - 1;
        break;
    case AVCHROMA_LOC_LEFT:
        wX += (1 << desc->log2_chroma_w) - 1;
        break;
    case AVCHROMA_LOC_BOTTOMLEFT:
        wX += (1 << desc->log2_chroma_w) - 1;
    case AVCHROMA_LOC_BOTTOM:
        wY += (1 << desc->log2_chroma_h) - 1;
    case AVCHROMA_LOC_CENTER:
    default: //center chroma value for unknown/unsupported
        break;
    }

    const unsigned baseCoefficients[] = {1, 3, 9, 27, 9, 3, 1};
    //If wZ is even, an intermediate value is interpolated for symmetry.
    for (int x = 0; x < 4; x++) {
        pv->chromaCoeffs[0][x] = (baseCoefficients[x + wX] + baseCoefficients[x + wX + !(wX & 0x1)]) >> 1;
        pv->chromaCoeffs[1][x] = (baseCoefficients[x + wY] + baseCoefficients[x + wY + !(wY & 0x1)]) >> 1;
    }

    switch (init->pix_fmt)
    {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_P010:
        case AV_PIX_FMT_P012:
        case AV_PIX_FMT_P016:
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUV420P16:
            pv->pix_fmt_alpha = AV_PIX_FMT_YUVA420P;
            break;
        case AV_PIX_FMT_NV16:
        case AV_PIX_FMT_P210:
        case AV_PIX_FMT_P212:
        case AV_PIX_FMT_P216:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUV422P16:
            pv->pix_fmt_alpha = AV_PIX_FMT_YUVA422P;
            break;
        case AV_PIX_FMT_NV24:
        case AV_PIX_FMT_P410:
        case AV_PIX_FMT_P412:
        case AV_PIX_FMT_P416:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_YUV444P12:
        case AV_PIX_FMT_YUV444P16:
        default:
            pv->pix_fmt_alpha = AV_PIX_FMT_YUVA444P;
            break;
    }

    const int planes_count = av_pix_fmt_count_planes(init->pix_fmt);

    switch (pv->depth)
    {
        case 8:
            switch (planes_count)
            {
                case 2:
                    pv->blend = blend8onbi8;
                    break;
                default:
                    pv->blend = blend8on8;
                    break;
            }
            break;
        default:
            switch (planes_count)
            {
                case 2:
                    pv->blend = blend8onbi1x;
                    break;
                default:
                    pv->blend = blend8on1x;
                    break;
            }
    }

    // Find the subtitle we need
    for( ii = 0; ii < hb_list_count(init->job->list_subtitle); ii++ )
    {
        subtitle = hb_list_item( init->job->list_subtitle, ii );
        if( subtitle && subtitle->config.dest == RENDERSUB )
        {
            // Found it
            filter->subtitle = subtitle;
            pv->type = subtitle->source;
            break;
        }
    }
    if( filter->subtitle == NULL )
    {
        hb_log("rendersub: no subtitle marked for burn");
        return 1;
    }
    pv->output = *init;

    return 0;
}

static int hb_rendersub_post_init( hb_filter_object_t * filter, hb_job_t *job )
{
    hb_filter_private_t * pv = filter->private_data;

    pv->crop[0] = job->crop[0];
    pv->crop[1] = job->crop[1];
    pv->crop[2] = job->crop[2];
    pv->crop[3] = job->crop[3];

    switch( pv->type )
    {
        case VOBSUB:
        {
            return vobsub_post_init( filter, job );
        }

        case SSASUB:
        {
            return ssa_post_init( filter, job );
        }

        case IMPORTSRT:
        case IMPORTSSA:
        case UTF8SUB:
        case TX3GSUB:
        {
            return textsub_post_init( filter, job );
        }

        case CC608SUB:
        {
            return cc608sub_post_init( filter, job );
        }

        case DVBSUB:
        case PGSSUB:
        {
            return pgssub_post_init( filter, job );
        }

        default:
        {
            hb_log("rendersub: unsupported subtitle format %d", pv->type );
            return 1;
        }
    }
}

static int hb_rendersub_work( hb_filter_object_t * filter,
                                 hb_buffer_t ** buf_in,
                                 hb_buffer_t ** buf_out )
{
    hb_filter_private_t * pv = filter->private_data;
    switch( pv->type )
    {
        case VOBSUB:
        {
            return vobsub_work( filter, buf_in, buf_out );
        }

        case SSASUB:
        {
            return ssa_work( filter, buf_in, buf_out );
        }

        case IMPORTSRT:
        case IMPORTSSA:
        case CC608SUB:
        case UTF8SUB:
        case TX3GSUB:
        {
            return textsub_work( filter, buf_in, buf_out );
        }

        case DVBSUB:
        case PGSSUB:
        {
            return pgssub_work( filter, buf_in, buf_out );
        }

        default:
        {
            hb_error("rendersub: unsupported subtitle format %d", pv->type );
            return 1;
        }
    }
}

static void hb_rendersub_close( hb_filter_object_t * filter )
{
    hb_filter_private_t * pv = filter->private_data;

    if (pv->sws != NULL)
    {
        sws_freeContext(pv->sws);
    }
    switch( pv->type )
    {
        case VOBSUB:
        {
            vobsub_close( filter );
        } break;

        case SSASUB:
        {
            ssa_close( filter );
        } break;

        case IMPORTSRT:
        case IMPORTSSA:
        case CC608SUB:
        case UTF8SUB:
        case TX3GSUB:
        {
            textsub_close( filter );
        } break;

        case DVBSUB:
        case PGSSUB:
        {
            pgssub_close( filter );
        } break;

        default:
        {
            hb_error("rendersub: unsupported subtitle format %d", pv->type );
        } break;
    }
}

