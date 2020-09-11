/*
 * Copyright (C) 2016 Rockchip Electronics Co.Ltd
 * Authors:
 *  Zhiqin Wei <wzq@rock-chips.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */
#include "NormalRga.h"
#include "NormalRgaContext.h"

#ifdef ANDROID
#include "GrallocOps.h"
#include <cutils/properties.h>

#define RGA_VERSION "1.03"

#elif LINUX
#include <sys/ioctl.h>
#endif

#define RGA_SRCOVER_EN 1

volatile int32_t refCount = 0;
struct rgaContext *rgaCtx = NULL;

void NormalRgaSetLogOnceFlag(int log) {
    struct rgaContext *ctx = NULL;

    ctx->mLogOnce = log;
    return;
}

void NormalRgaSetAlwaysLogFlag(int log) {
    struct rgaContext *ctx = NULL;

    ctx->mLogAlways = log;
    return;
}

#ifdef ANDROID
void is_debug_log(void) {
    struct rgaContext *ctx = rgaCtx;
    ctx->Is_debug = hwc_get_int_property("vendor.rga.log","0");
}

int is_out_log( void ) {
    struct rgaContext *ctx = rgaCtx;
    return ctx->Is_debug;
}

//return property value of pcProperty
int hwc_get_int_property(const char* pcProperty, const char* default_value) {
    char value[PROPERTY_VALUE_MAX];
    int new_value = 0;

    if (pcProperty == NULL || default_value == NULL) {
        ALOGE("hwc_get_int_property: invalid param");
        return -1;
    }

    property_get(pcProperty, value, default_value);
    new_value = atoi(value);

    return new_value;
}
#endif

int NormalRgaOpen(void **context) {
    struct rgaContext *ctx = NULL;
    char buf[30];
    int fd = -1;
    int ret = 0;

    if (!context) {
        ret = -EINVAL;
        goto mallocErr;
    }

    if (!rgaCtx) {
        ctx = (struct rgaContext *)malloc(sizeof(struct rgaContext));
        if(!ctx) {
            ret = -ENOMEM;
            ALOGE("malloc fail:%s.",strerror(errno));
            goto mallocErr;
        }
    } else {
        ctx = rgaCtx;
        ALOGE("Had init the rga dev ctx = %p",ctx);
        goto init;
    }

    fd = open("/dev/rga", O_RDWR, 0);
    if (fd < 0) {
        ret = -ENODEV;
        ALOGE("failed to open DRM:%s.",strerror(errno));
        goto drmOpenErr;
    }
    ctx->rgaFd = fd;

    ret = ioctl(fd, RGA_GET_VERSION, buf);
    ctx->mVersion = atof(buf);
    fprintf(stderr, "librga:RGA_GET_VERSION:%s,%f\n", buf, ctx->mVersion);

    NormalRgaInitTables();

    fprintf(stderr, "ctx=%p,ctx->rgaFd=%d\n",ctx, ctx->rgaFd );
    rgaCtx = ctx;

init:
#ifdef ANDROID
    android_atomic_inc(&refCount);
#endif
    *context = (void *)ctx;
    return ret;

drmOpenErr:
    free(ctx);
mallocErr:
    return ret;
}

int NormalRgaClose(void *context) {
    struct rgaContext *ctx = rgaCtx;

    if (!ctx) {
        ALOGE("Try to exit uninit rgaCtx=%p", ctx);
        return -ENODEV;
    }

    if (!context) {
        ALOGE("Try to uninit rgaCtx=%p", context);
        return -ENODEV;
    }

    if (context != ctx) {
        ALOGE("Try to exit wrong ctx=%p",ctx);
        return -ENODEV;
    }

    if (refCount <= 0) {
        ALOGE("This can not be happened");
        return 0;
    }

#ifdef ANDROID
    if (refCount > 0 && android_atomic_dec(&refCount) != 1)
        return 0;
#endif

    rgaCtx = NULL;

    close(ctx->rgaFd);

    free(ctx);

    return 0;
}

int RgaInit(void **ctx) {
    int ret = 0;
    ret = NormalRgaOpen(ctx);
#ifdef ANDROID
    property_set("vendor.rga.version", RGA_VERSION);
#endif
    return ret;
}

int RgaDeInit(void *ctx) {
    int ret = 0;
    ret = NormalRgaClose(ctx);
    return ret;
}

#ifdef ANDROID
int NormalRgaPaletteTable(buffer_handle_t dst,
                          unsigned int v, drm_rga_t *rects) {
    //check rects
    //check buffer_handle_t with rects
    struct rgaContext *ctx = rgaCtx;
    int srcVirW,srcVirH,srcActW,srcActH,srcXPos,srcYPos;
    int dstVirW,dstVirH,dstActW,dstActH,dstXPos,dstYPos;
    int scaleMode,rotateMode,orientation,ditherEn;
    int srcType,dstType,srcMmuFlag,dstMmuFlag;
    int planeAlpha;
    int dstFd = -1;
    int srcFd = -1;
    int ret = 0;
    drm_rga_t tmpRects,relRects;
    struct rga_req rgaReg;
    bool perpixelAlpha;
    void *srcBuf = NULL;
    void *dstBuf = NULL;
    RECT clip;

    if (!ctx) {
        ALOGE("Try to use uninit rgaCtx=%p",ctx);
        return -ENODEV;
    }

    if (rects && (ctx->mLogAlways || ctx->mLogOnce)) {
        ALOGD("Src:[%d,%d,%d,%d][%d,%d,%d]=>Dst:[%d,%d,%d,%d][%d,%d,%d]",
              rects->src.xoffset,rects->src.yoffset,
              rects->src.width, rects->src.height,
              rects->src.wstride,rects->src.format, rects->src.size,
              rects->dst.xoffset,rects->dst.yoffset,
              rects->dst.width, rects->dst.height,
              rects->dst.wstride,rects->dst.format, rects->dst.size);
    }

    memset(&rgaReg, 0, sizeof(struct rga_req));

    srcType = dstType = srcMmuFlag = dstMmuFlag = 0;

    ret = NormalRgaGetRects(NULL, dst, &srcType, &dstType, &tmpRects);
    if (ret && !rects) {
        ALOGE("%d:Has not rects for render", __LINE__);
        return ret;
    }

    if (rects) {
        if (rects->src.wstride > 0 && rects->dst.wstride > 0)
            memcpy(&relRects, rects, sizeof(drm_rga_t));
        else if (rects->src.wstride > 0) {
            memcpy(&(relRects.src), &(rects->src), sizeof(rga_rect_t));
            memcpy(&(relRects.dst), &(tmpRects.dst), sizeof(rga_rect_t));
        } else if (rects->dst.wstride > 0) {
            memcpy(&(relRects.src), &(tmpRects.src), sizeof(rga_rect_t));
            memcpy(&(relRects.dst), &(rects->dst), sizeof(rga_rect_t));
        }
    } else
        memcpy(&relRects, &tmpRects, sizeof(drm_rga_t));

    if (ctx->mLogAlways || ctx->mLogOnce) {
        ALOGD("Src:[%d,%d,%d,%d][%d,%d,%d]=>Dst:[%d,%d,%d,%d][%d,%d,%d]",
              tmpRects.src.xoffset,tmpRects.src.yoffset,
              tmpRects.src.width, tmpRects.src.height,
              tmpRects.src.wstride,tmpRects.src.format, tmpRects.src.size,
              tmpRects.dst.xoffset,tmpRects.dst.yoffset,
              tmpRects.dst.width, tmpRects.dst.height,
              tmpRects.dst.wstride,tmpRects.dst.format, tmpRects.dst.size);
        ALOGD("Src:[%d,%d,%d,%d][%d,%d,%d]=>Dst:[%d,%d,%d,%d][%d,%d,%d]",
              relRects.src.xoffset,relRects.src.yoffset,
              relRects.src.width, relRects.src.height,
              relRects.src.wstride,relRects.src.format, relRects.src.size,
              relRects.dst.xoffset,relRects.dst.yoffset,
              relRects.dst.width, relRects.dst.height,
              relRects.dst.wstride,relRects.dst.format, relRects.dst.size);
    }

    RkRgaGetHandleMapAddress(dst, &dstBuf);
    RkRgaGetHandleFd(dst, &dstFd);
    if (dstFd == -1 && !dstBuf) {
        ALOGE("%d:dst has not fd and address for render", __LINE__);
        return ret;
    }

    if (dstFd == 0 && !dstBuf) {
        ALOGE("dstFd is zero, now driver not support");
        return -EINVAL;
    } else
        dstFd = -1;

    orientation = 0;
    rotateMode = 0;
    srcVirW = relRects.src.wstride;
    srcVirH = relRects.src.height;
    srcXPos = relRects.src.xoffset;
    srcYPos = relRects.src.yoffset;
    srcActW = relRects.src.width;
    srcActH = relRects.src.height;

    dstVirW = relRects.dst.wstride;
    dstVirH = relRects.dst.height;
    dstXPos = relRects.dst.xoffset;
    dstYPos = relRects.dst.yoffset;
    dstActW = relRects.dst.width;
    dstActH = relRects.dst.height;

    NormalRgaSetSrcActiveInfo(&rgaReg, srcActW, srcActH, srcXPos, srcYPos);
    NormalRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);
    NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                               (unsigned long)srcBuf + srcVirW * srcVirH,
                               (unsigned long)srcBuf + srcVirW * srcVirH * 5/4,
                               srcVirW, srcVirH,
                               RkRgaGetRgaFormat(relRects.src.format),0);
    /*dst*/
    NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                               (unsigned long)dstBuf + dstVirW * dstVirH,
                               (unsigned long)dstBuf + dstVirW * dstVirH * 5/4,
                               dstVirW, dstVirH, &clip,
                               RkRgaGetRgaFormat(relRects.dst.format),0);
    NormalRgaSetPatInfo(&rgaReg, dstVirW, dstVirH,
                        dstXPos, dstYPos, relRects.dst.format);
    NormalRgaSetFadingEnInfo(&rgaReg, v & 0xFF000000, v & 0xFF0000, v & 0xFF00);

    /*mode*/
    NormalRgaUpdatePaletteTableMode(&rgaReg, 0, v & 0xFF);

    if (srcMmuFlag || dstMmuFlag) {
        NormalRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        NormalRgaMmuFlag(&rgaReg, srcMmuFlag, dstMmuFlag);
    }

    if (ctx->mLogAlways || ctx->mLogOnce)
        NormalRgaLogOutRgaReq(rgaReg);

    if(ioctl(ctx->rgaFd, RGA_BLIT_SYNC, &rgaReg)) {
        printf(" %s(%d) RGA_BLIT fail: %s",__FUNCTION__, __LINE__,strerror(errno));
        ALOGE(" %s(%d) RGA_BLIT fail: %s",__FUNCTION__, __LINE__,strerror(errno));
    }

    if (ctx->mLogOnce)
        ctx->mLogOnce = 0;

    return 0;
}
#endif

int RgaBlit(rga_info *src, rga_info *dst, rga_info *src1) {
    //check rects
    //check buffer_handle_t with rects
    struct rgaContext *ctx = rgaCtx;
    int srcVirW,srcVirH,srcActW,srcActH,srcXPos,srcYPos;
    int dstVirW,dstVirH,dstActW,dstActH,dstXPos,dstYPos;
    int src1VirW,src1VirH,src1ActW,src1ActH,src1XPos,src1YPos;
    int scaleMode,rotateMode,orientation,ditherEn;
    int srcType,dstType,src1Type,srcMmuFlag,dstMmuFlag,src1MmuFlag;
    int planeAlpha;
    int dstFd = -1;
    int srcFd = -1;
    int src1Fd = -1;
    int rotation;
    int stretch = 0;
    float hScale = 1;
    float vScale = 1;
    int ret = 0;
    rga_rect_t relSrcRect,tmpSrcRect,relDstRect,tmpDstRect;
    rga_rect_t relSrc1Rect,tmpSrc1Rect;
    struct rga_req rgaReg,tmprgaReg;
    unsigned int blend;
    unsigned int yuvToRgbMode;
    bool perpixelAlpha = 0;
    void *srcBuf = NULL;
    void *dstBuf = NULL;
    void *src1Buf = NULL;
#ifdef ANDROID
    RECT clip;
#elif LINUX
    RECT_t clip;
#endif
    int sync_mode = RGA_BLIT_SYNC;

    //init context
    if (!ctx) {
        ALOGE("Try to use uninit rgaCtx=%p",ctx);
        return -ENODEV;
    }

    //init
    memset(&rgaReg, 0, sizeof(struct rga_req));

    srcType = dstType = srcMmuFlag = dstMmuFlag = 0;
    src1Type = src1MmuFlag = 0;
    rotation = 0;
    blend = 0;
    yuvToRgbMode = 0;

#ifdef ANDROID
    /* print debug log by setting property vendor.rga.log as 1 */
    is_debug_log();
    if(is_out_log())
        ALOGD("<<<<-------- print rgaLog -------->>>>");
#endif

    if (!src && !dst && !src1) {
        ALOGE("src = %p, dst = %p, src1 = %p", src, dst, src1);
        return -EINVAL;
    }

    if (!src && !dst) {
        ALOGE("src = %p, dst = %p", src, dst);
        return -EINVAL;
    }

    /*
     * 1.if src exist, get some parameter from src, such as rotatiom.
     * 2.if need to blend, need blend variable from src to decide how to blend.
     * 3.get effective area from src, if the area is empty, choose to get parameter from handle.
     * */
    if (src) {
        rotation = src->rotation;
        blend = src->blend;
        memcpy(&relSrcRect, &src->rect, sizeof(rga_rect_t));
    }

    /* get effective area from dst and src1, if the area is empty, choose to get parameter from handle. */
    if (dst)
        memcpy(&relDstRect, &dst->rect, sizeof(rga_rect_t));
    if (src1)
        memcpy(&relSrc1Rect, &src1->rect, sizeof(rga_rect_t));

    srcFd = dstFd = src1Fd = -1;

#ifdef ANDROID
	if (is_out_log()) {
		ALOGD("src->hnd = %p , dst->hnd = %p \n",src->hnd,dst->hnd);
		ALOGD("src: Fd = %.2d , phyAddr = %p , virAddr = %p\n",src->fd,src->phyAddr,src->virAddr);
		ALOGD("dst: Fd = %.2d , phyAddr = %p , virAddr = %p\n",dst->fd,dst->phyAddr,dst->virAddr);
	}
#endif
	/*********** get src addr *************/
	if (src && src->phyAddr) {
		srcBuf = src->phyAddr;
	} else if (src && src->fd > 0) {
		srcFd = src->fd;
		src->mmuFlag = 1;
	} else if (src && src->virAddr) {
		srcBuf = src->virAddr;
		src->mmuFlag = 1;
	}
	/*
	 * After getting the fd or virtual address through the handle,
	 * set 'srcType' to 1, and at the end, and then judge
	 * the 'srcType' at the end whether to enable mmu.
	 */
#ifdef ANDROID
	else if (src && src->hnd) {
#ifndef RK3188
		/* RK3188 is special, cannot configure rga through fd. */
		RkRgaGetHandleFd(src->hnd, &srcFd);
#endif
#ifndef ANDROID_8
		if (srcFd < 0 || srcFd == 0) {
			RkRgaGetHandleMapAddress(src->hnd, &srcBuf);
		}
#endif
		if ((srcFd < 0 || srcFd == 0) && srcBuf == NULL) {
			ALOGE("src handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &src->hnd);
			printf("src handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &src->hnd);
			return ret;
		}
		else {
			srcType = 1;
		}
	}

	if (!isRectValid(relSrcRect)) {
		ret = NormalRgaGetRect(src->hnd, &tmpSrcRect);
		if (ret) {
			ALOGE("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &src->hnd);
			printf("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &src->hnd);
			return ret;
		}
		memcpy(&relSrcRect, &tmpSrcRect, sizeof(rga_rect_t));
	}
#endif
	if (srcFd == -1 && !srcBuf) {
		ALOGE("%d:src has not fd and address for render", __LINE__);
		return ret;
	}
	if (srcFd == 0 && !srcBuf) {
		ALOGE("srcFd is zero, now driver not support");
		return -EINVAL;
	}
	/* Old rga driver cannot support fd as zero. */
	if (srcFd == 0)
		srcFd = -1;

	/*********** get dst addr *************/
	if (dst && dst->phyAddr) {
		dstBuf = dst->phyAddr;
	} else if (dst && dst->fd > 0) {
		dstFd = dst->fd;
		dst->mmuFlag = 1;
	} else if (dst && dst->virAddr) {
		dstBuf = dst->virAddr;
		dst->mmuFlag = 1;
	}
	/*
	 * After getting the fd or virtual address through the handle,
	 * set 'dstType' to 1, and at the end, and then judge
	 * the 'dstType' at the end whether to enable mmu.
	 */
#ifdef ANDROID
	else if (dst && dst->hnd) {
#ifndef RK3188
		/* RK3188 is special, cannot configure rga through fd. */
		RkRgaGetHandleFd(dst->hnd, &dstFd);
#endif
#ifndef ANDROID_8
		if (dstFd < 0 || dstFd == 0) {
			RkRgaGetHandleMapAddress(dst->hnd, &dstBuf);
		}
#endif
		if ((dstFd < 0 || dstFd == 0) && dstBuf == NULL) {
			ALOGE("dst handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &dst->hnd);
			printf("dst handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &dst->hnd);
			return ret;
		}
		else {
			dstType = 1;
		}
	}

	if (!isRectValid(relDstRect)) {
		ret = NormalRgaGetRect(dst->hnd, &tmpDstRect);
		if (ret) {
			ALOGE("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &dst->hnd);
			printf("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &dst->hnd);
			return ret;
		}
		memcpy(&relDstRect, &tmpDstRect, sizeof(rga_rect_t));
	}
#endif

	if (dstFd == -1 && !dstBuf) {
		ALOGE("%d:dst has not fd and address for render", __LINE__);
		return ret;
	}
	if (dstFd == 0 && !dstBuf) {
		ALOGE("dstFd is zero, now driver not support");
		return -EINVAL;
	}
	/* Old rga driver cannot support fd as zero. */
	if (dstFd == 0)
		dstFd = -1;

#ifdef ANDROID
	if(is_out_log()) {
		ALOGD("src: Fd = %.2d , buf = %p, mmuFlag = %d, mmuType = %d\n", srcFd, srcBuf, src->mmuFlag, srcType);
		ALOGD("dst: Fd = %.2d , buf = %p, mmuFlag = %d, mmuType = %d\n", dstFd, dstBuf, dst->mmuFlag, dstType);
	}
#endif

#ifdef RK3126C
    if ( (relSrcRect.width == relDstRect.width) && (relSrcRect.height == relDstRect.height ) &&
         (relSrcRect.width + 2*relSrcRect.xoffset == relSrcRect.wstride) &&
         (relSrcRect.height + 2*relSrcRect.yoffset == relSrcRect.hstride) &&
         (relSrcRect.format == HAL_PIXEL_FORMAT_YCrCb_NV12) && (relSrcRect.xoffset > 0 && relSrcRect.yoffset > 0)
       ) {
        relSrcRect.width += 4;
        //relSrcRect.height += 4;
        relSrcRect.xoffset = (relSrcRect.wstride - relSrcRect.width) / 2;
    }
#endif

    /* blend bit[16:23] is to set global alpha. */
    planeAlpha = (blend & 0xFF0000) >> 16;

    /* determined by format, need pixel alpha or not. */
#ifdef ANDROID
    perpixelAlpha = relSrcRect.format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                    relSrcRect.format == HAL_PIXEL_FORMAT_BGRA_8888;
    if(is_out_log())
        ALOGE("blend = %x , perpixelAlpha = %d",blend,perpixelAlpha);
#elif LINUX
    perpixelAlpha = relSrcRect.format == RK_FORMAT_RGBA_8888 ||
                    relSrcRect.format == RK_FORMAT_BGRA_8888;
#endif

    /* blend bit[0:15] is to set which way to blend,such as whether need glabal alpha,and so on. */
    switch ((blend & 0xFFFF)) {
        case 0x0105:
            if (perpixelAlpha && planeAlpha < 255) {
                NormalRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha, 1, 9, 0);
            } else if (perpixelAlpha)
                NormalRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 1, 3, 0);
            else
                NormalRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha, 0, 0, 0);
            break;

        case 0x0405:
            if (perpixelAlpha && planeAlpha < 255)
                NormalRgaSetAlphaEnInfo(&rgaReg, 1, 2, planeAlpha, 0, 0, 0);
            else if (perpixelAlpha)
                NormalRgaSetAlphaEnInfo(&rgaReg, 1, 1, 0, 0, 0, 0);
            else
                NormalRgaSetAlphaEnInfo(&rgaReg, 1, 0, planeAlpha, 0, 0, 0);
            break;

        case 0x0100:
        default:
            /* Tips: BLENDING_NONE is non-zero value, handle zero value as
             * BLENDING_NONE. */
            /* C = Cs
             * A = As */
            break;
    }

    /* discripe a picture need high stride.If high stride not to be set, need use height as high stride. */
    if (relSrcRect.hstride == 0)
        relSrcRect.hstride = relSrcRect.height;

    if (relDstRect.hstride == 0)
        relDstRect.hstride = relDstRect.height;

    /* do some check, check the area of src and dst whether is effective. */
    if (src) {
        ret = checkRectForRga(relSrcRect);
        if (ret) {
            printf("Error srcRect\n");
            ALOGE("[%s,%d]Error srcRect \n", __func__, __LINE__);
            return ret;
        }
    }

    if (dst) {
        ret = checkRectForRga(relDstRect);
        if (ret) {
            printf("Error srcRect\n");
            ALOGE("[%s,%d]Error dstRect \n", __func__, __LINE__);
            return ret;
        }
    }

    /* check the scale magnification. */
    if (src && dst) {
        hScale = (float)relSrcRect.width / relDstRect.width;
        vScale = (float)relSrcRect.height / relDstRect.height;
        if (rotation == HAL_TRANSFORM_ROT_90 || rotation == HAL_TRANSFORM_ROT_270) {
            hScale = (float)relSrcRect.width / relDstRect.height;
            vScale = (float)relSrcRect.height / relDstRect.width;
        }
        if (hScale < 1/16 || hScale > 16 || vScale < 1/16 || vScale > 16) {
            ALOGE("Error scale[%f,%f] line %d", hScale, vScale, __LINE__);
            return -EINVAL;
        }
        if (ctx->mVersion <= 2.0 && (hScale < 1/8 ||
                                     hScale > 8 || vScale < 1/8 || vScale > 8)) {
            ALOGE("Error scale[%f,%f] line %d", hScale, vScale, __LINE__);
            return -EINVAL;
        }
        if (ctx->mVersion <= 1.003 && (hScale < 1/2 || vScale < 1/2)) {
            ALOGE("e scale[%f,%f] ver[%f]", hScale, vScale, ctx->mVersion);
            return -EINVAL;
        }
    }

    /* reselect the scale mode. */
    scaleMode = 0;
    stretch = (hScale != 1.0f) || (vScale != 1.0f);
    /* scale up use bicubic */
    if (hScale < 1 || vScale < 1) {
        scaleMode = 2;
#ifdef ANDROID
        if((src->format == HAL_PIXEL_FORMAT_RGBA_8888  ||src->format == HAL_PIXEL_FORMAT_BGRA_8888))
#elif LINUX
        if((relSrcRect.format == RK_FORMAT_RGBA_8888  || relSrcRect.format == RK_FORMAT_BGRA_8888))
#endif
            scaleMode = 0;     //  force change scale_mode to 0 ,for rga not support
    }

#ifdef ANDROID
    if(is_out_log())
        ALOGD("scaleMode = %d , stretch = %d;",scaleMode,stretch);
#endif

    /*
     * according to the rotation to set corresponding parameter.It's diffrient from the opengl.
     * Following's config which use frequently
     * */
    switch (rotation) {
        case HAL_TRANSFORM_FLIP_H:
            orientation = 0;
            rotateMode = 2;
            srcVirW = relSrcRect.wstride;
            srcVirH = relSrcRect.hstride;
            srcXPos = relSrcRect.xoffset;
            srcYPos = relSrcRect.yoffset;
            srcActW = relSrcRect.width;
            srcActH = relSrcRect.height;

            dstVirW = relDstRect.wstride;
            dstVirH = relDstRect.hstride;
            dstXPos = relDstRect.xoffset;
            dstYPos = relDstRect.yoffset;
            dstActW = relDstRect.width;
            dstActH = relDstRect.height;
            break;
        case HAL_TRANSFORM_FLIP_V:
            orientation = 0;
            rotateMode = 3;
            srcVirW = relSrcRect.wstride;
            srcVirH = relSrcRect.hstride;
            srcXPos = relSrcRect.xoffset;
            srcYPos = relSrcRect.yoffset;
            srcActW = relSrcRect.width;
            srcActH = relSrcRect.height;

            dstVirW = relDstRect.wstride;
            dstVirH = relDstRect.hstride;
            dstXPos = relDstRect.xoffset;
            dstYPos = relDstRect.yoffset;
            dstActW = relDstRect.width;
            dstActH = relDstRect.height;
            break;
        case HAL_TRANSFORM_ROT_90:
            orientation = 90;
            rotateMode = 1;
            srcVirW = relSrcRect.wstride;
            srcVirH = relSrcRect.hstride;
            srcXPos = relSrcRect.xoffset;
            srcYPos = relSrcRect.yoffset;
            srcActW = relSrcRect.width;
            srcActH = relSrcRect.height;

            dstVirW = relDstRect.wstride;
            dstVirH = relDstRect.hstride;
            dstXPos = relDstRect.xoffset + relDstRect.width - 1;
            dstYPos = relDstRect.yoffset;
            dstActW = relDstRect.height;
            dstActH = relDstRect.width;
            break;
        case HAL_TRANSFORM_ROT_180:
            orientation = 180;
            rotateMode = 1;
            srcVirW = relSrcRect.wstride;
            srcVirH = relSrcRect.hstride;
            srcXPos = relSrcRect.xoffset;
            srcYPos = relSrcRect.yoffset;
            srcActW = relSrcRect.width;
            srcActH = relSrcRect.height;

            dstVirW = relDstRect.wstride;
            dstVirH = relDstRect.hstride;
            dstXPos = relDstRect.xoffset + relDstRect.width - 1;
            dstYPos = relDstRect.yoffset + relDstRect.height - 1;
            dstActW = relDstRect.width;
            dstActH = relDstRect.height;
            break;
        case HAL_TRANSFORM_ROT_270:
            orientation = 270;
            rotateMode = 1;
            srcVirW = relSrcRect.wstride;
            srcVirH = relSrcRect.hstride;
            srcXPos = relSrcRect.xoffset;
            srcYPos = relSrcRect.yoffset;
            srcActW = relSrcRect.width;
            srcActH = relSrcRect.height;

            dstVirW = relDstRect.wstride;
            dstVirH = relDstRect.hstride;
            dstXPos = relDstRect.xoffset;
            dstYPos = relDstRect.yoffset + relDstRect.height - 1;
            dstActW = relDstRect.height;
            dstActH = relDstRect.width;
            break;
        default:
            orientation = 0;
            rotateMode = stretch;
            srcVirW = relSrcRect.wstride;
            srcVirH = relSrcRect.hstride;
            srcXPos = relSrcRect.xoffset;
            srcYPos = relSrcRect.yoffset;
            srcActW = relSrcRect.width;
            srcActH = relSrcRect.height;

            dstVirW = relDstRect.wstride;
            dstVirH = relDstRect.hstride;
            dstXPos = relDstRect.xoffset;
            dstYPos = relDstRect.yoffset;
            dstActW = relDstRect.width;
            dstActH = relDstRect.height;
            break;
    }

    /* if pictual out of range should be cliped. */
    clip.xmin = 0;
    clip.xmax = dstVirW - 1;
    clip.ymin = 0;
    clip.ymax = dstVirH - 1;

#ifdef ANDROID
    ditherEn = (android::bytesPerPixel(relSrcRect.format)
                != android::bytesPerPixel(relSrcRect.format) ? 1 : 0);

    if(is_out_log())
        ALOGE("rgaVersion = %lf  , ditherEn =%d ",ctx->mVersion,ditherEn);
#elif LINUX
    ditherEn = (bytesPerPixel(relSrcRect.format)
                != bytesPerPixel(relSrcRect.format) ? 1 : 0);
#endif

    /* only to configure the parameter by driver version, because rga driver has too many version. */
    if (ctx->mVersion <= (float)1.003) {
        srcMmuFlag = dstMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
        NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                   (unsigned long)srcBuf + srcVirW * srcVirH,
                                   (unsigned long)srcBuf + srcVirW * srcVirH * 5/4,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                   (unsigned long)dstBuf + dstVirW * dstVirH,
                                   (unsigned long)dstBuf + dstVirW * dstVirH * 5/4,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
#else
        NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                   (unsigned int)srcBuf + srcVirW * srcVirH,
                                   (unsigned int)srcBuf + srcVirW * srcVirH * 5/4,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                   (unsigned int)dstBuf + dstVirW * dstVirH,
                                   (unsigned int)dstBuf + dstVirW * dstVirH * 5/4,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);

#endif
        /* the version 1.005 is different to assign fd from version 2.0 and above */
    } else if (ctx->mVersion < (float)1.6) {
        /*Src*/
        if (srcFd != -1) {
            srcMmuFlag = srcType ? 1 : 0;
            if (src && srcFd == src->fd)
                srcMmuFlag = src->mmuFlag ? 1 : 0;
            NormalRgaSetSrcVirtualInfo(&rgaReg, 0, 0, 0, srcVirW, srcVirH,
                                       RkRgaGetRgaFormat(relSrcRect.format),0);
            NormalRgaSetFdsOffsets(&rgaReg, srcFd, 0, 0, 0);
        } else {
            if (src && src->hnd)
                srcMmuFlag = srcType ? 1 : 0;
            if (src && srcBuf == src->virAddr)
                srcMmuFlag = 1;
            if (src && srcBuf == src->phyAddr)
                srcMmuFlag = 0;
#if defined(__arm64__) || defined(__aarch64__)
            NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                       (unsigned long)srcBuf + srcVirW * srcVirH,
                                       (unsigned long)srcBuf + srcVirW * srcVirH * 5/4,
                                       srcVirW, srcVirH,
                                       RkRgaGetRgaFormat(relSrcRect.format),0);
#else
            NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                       (unsigned int)srcBuf + srcVirW * srcVirH,
                                       (unsigned int)srcBuf + srcVirW * srcVirH * 5/4,
                                       srcVirW, srcVirH,
                                       RkRgaGetRgaFormat(relSrcRect.format),0);
#endif
        }
        /*dst*/
        if (dstFd != -1) {
            dstMmuFlag = dstType ? 1 : 0;
            if (dst && dstFd == dst->fd)
                dstMmuFlag = dst->mmuFlag ? 1 : 0;
            NormalRgaSetDstVirtualInfo(&rgaReg, 0, 0, 0, dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
            /*src dst fd*/
            NormalRgaSetFdsOffsets(&rgaReg, 0, dstFd, 0, 0);
        } else {
            if (dst && dst->hnd)
                dstMmuFlag = dstType ? 1 : 0;
            if (dst && dstBuf == dst->virAddr)
                dstMmuFlag = 1;
            if (dst && dstBuf == dst->phyAddr)
                dstMmuFlag = 0;
#if defined(__arm64__) || defined(__aarch64__)
            NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                       (unsigned long)dstBuf + dstVirW * dstVirH,
                                       (unsigned long)dstBuf + dstVirW * dstVirH * 5/4,
                                       dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
#else
            NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                       (unsigned int)dstBuf + dstVirW * dstVirH,
                                       (unsigned int)dstBuf + dstVirW * dstVirH * 5/4,
                                       dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
#endif
        }
    } else {
        if (src && src->hnd)
            srcMmuFlag = srcType ? 1 : 0;
        if (src && srcBuf == src->virAddr)
            srcMmuFlag = 1;
        if (src && srcBuf == src->phyAddr)
            srcMmuFlag = 0;
        if (srcFd != -1)
            srcMmuFlag = srcType ? 1 : 0;
        if (src && srcFd == src->fd)
            srcMmuFlag = src->mmuFlag ? 1 : 0;

        if (dst && dst->hnd)
            dstMmuFlag = dstType ? 1 : 0;
        if (dst && dstBuf == dst->virAddr)
            dstMmuFlag = 1;
        if (dst && dstBuf == dst->phyAddr)
            dstMmuFlag = 0;
        if (dstFd != -1)
            dstMmuFlag = dstType ? 1 : 0;
        if (dst && dstFd == dst->fd)
            dstMmuFlag = dst->mmuFlag ? 1 : 0;

#if defined(__arm64__) || defined(__aarch64__)
        NormalRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                   (unsigned long)srcBuf,
                                   (unsigned long)srcBuf + srcVirW * srcVirH,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                   (unsigned long)dstBuf,
                                   (unsigned long)dstBuf + dstVirW * dstVirH,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);

#else
        NormalRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                   (unsigned int)srcBuf,
                                   (unsigned int)srcBuf + srcVirW * srcVirH,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                   (unsigned int)dstBuf,
                                   (unsigned int)dstBuf + dstVirW * dstVirH,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);

#endif
    }

    /* set effective area of src and dst. */
    NormalRgaSetSrcActiveInfo(&rgaReg, srcActW, srcActH, srcXPos, srcYPos);
    NormalRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);
    //NormalRgaSetPatActiveInfo(&rgaReg, src1ActW, src1ActH, src1XPos, src1YPos);

    /* special config for yuv to rgb */
    if (NormalRgaIsYuvFormat(RkRgaGetRgaFormat(relSrcRect.format)) &&
        NormalRgaIsRgbFormat(RkRgaGetRgaFormat(relDstRect.format)))
        yuvToRgbMode |= 0x1 << 0;

    /* special config for rgb to yuv */
    if (NormalRgaIsRgbFormat(RkRgaGetRgaFormat(relSrcRect.format)) &&
        NormalRgaIsYuvFormat(RkRgaGetRgaFormat(relDstRect.format)))
        yuvToRgbMode |= 0x2 << 4;

    if(dst->color_space_mode > 0)
        yuvToRgbMode = dst->color_space_mode;

    /* mode
     * scaleMode:set different algorithm to scale.
     * rotateMode:rotation mode
     * Orientation:rotation orientation
     * ditherEn:enable or not.
     * yuvToRgbMode:yuv to rgb, rgb to yuv , or others
     * */
    NormalRgaSetBitbltMode(&rgaReg, scaleMode, rotateMode, orientation,
                           ditherEn, 0, yuvToRgbMode);

    NormalRgaNNQuantizeMode(&rgaReg, dst);

    if (srcMmuFlag || dstMmuFlag) {
        NormalRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        NormalRgaMmuFlag(&rgaReg, srcMmuFlag, dstMmuFlag);
    }

    /*color key*/
    /* if need this funtion, maybe should patch the rga driver. */
    if(src->colorkey_en == 1) {
        NormalRgaSetSrcTransModeInfo(&rgaReg, 0, 1, 1, 1, 1, src->colorkey_min, src->colorkey_max, 1);
    }

#ifdef ANDROID
    if(is_out_log()) {
        ALOGD("srcMmuFlag = %d , dstMmuFlag = %d , rotateMode = %d \n", srcMmuFlag, dstMmuFlag,rotateMode);
        ALOGD("<<<<-------- rgaReg -------->>>>\n");
        NormalRgaLogOutRgaReq(rgaReg);
    }
#elif LINUX
#ifdef __DEBUG
    NormalRgaLogOutRgaReq(rgaReg);
#endif
#endif

#ifdef ANDROID
#ifndef RK3368
#ifdef  ANDROID_7_DRM
    /* if Android 7.0 and above using drm should configure this parameter. */
    rgaReg.render_mode |= RGA_BUF_GEM_TYPE_DMA;
#endif
#endif
#endif
    if(dst->sync_mode == RGA_BLIT_ASYNC) {
        sync_mode = dst->sync_mode;
    }
    /* using sync to pass config to rga driver. */
    if(ioctl(ctx->rgaFd, sync_mode, &rgaReg)) {
        printf(" %s(%d) RGA_BLIT fail: %s",__FUNCTION__, __LINE__,strerror(errno));
        ALOGE(" %s(%d) RGA_BLIT fail: %s",__FUNCTION__, __LINE__,strerror(errno));
        return -errno;
    }
    return 0;
}

int RgaSrcOver(rga_info *src, rga_info *dst, rga_info *src1) {
#if RGA_SRCOVER_EN
    int ret = 0;
    rga_info temp;
    void *temp_buf = NULL;

	if (!(0x0205 == (src->blend & 0xFFFF) &&
		NormalRgaIsRgbFormat(RkRgaGetRgaFormat(src->rect.format)) &&
		NormalRgaIsYuvFormat(RkRgaGetRgaFormat(dst->rect.format)))) {
		printf("Not src over mode\n");
		return -1;
	}

    temp_buf= (char*)malloc(src->rect.wstride*src->rect.hstride*(src->rect.format*4));
    if (temp_buf == NULL) 
    	goto ERR_FREE_BUF;
    memset(&temp,0x00,sizeof(temp));
    
    temp.fd = -1;
    temp.virAddr = temp_buf;
    temp.mmuFlag = 1;
    temp.rect.width = src->rect.width;
    temp.rect.height = src->rect.height;
    temp.rect.wstride = src->rect.wstride;
    temp.rect.hstride = src->rect.hstride;
    temp.rect.xoffset = src->rect.xoffset;
    temp.rect.yoffset = src->rect.yoffset;
    temp.rect.format = src->rect.format;

    /*dst_YUV(crop & cvtcolor)->L_RGBA*/
    {
        ret = RgaBlit(dst, &temp, src1);
        if (ret) {
            printf("rgaBlit error : %s\n", strerror(errno));
            goto ERR_FREE_BUF;
        }
    }

    /*src_RGBA + temp_RGBA(blend) -> temp_RGBA*/
    {
        src->blend = 0xff0105;

        ret = RgaBlit(src, &temp, src1);
        if (ret) {
            printf("rgaBlit error : %s\n", strerror(errno));
            goto ERR_FREE_BUF;
        }
    }

    /*temp_RGBA(cvtcolor & translate)->dst_YUV*/
    {
        ret = RgaBlit(&temp, dst, src1);
        if (ret) {
            printf("rgaBlit error : %s\n", strerror(errno));
            goto ERR_FREE_BUF;
        }
    }

	free(temp_buf);
	return 0;
	
ERR_FREE_BUF:
	free(temp_buf);
	return -1;
#else

	return -1;
#endif
}

int RgaFlush() {
    struct rgaContext *ctx = rgaCtx;

    //init context
    if (!ctx) {
        ALOGE("Try to use uninit rgaCtx=%p",ctx);
        return -ENODEV;
    }

    if(ioctl(ctx->rgaFd, RGA_FLUSH, NULL)) {
        printf(" %s(%d) RGA_FLUSH fail: %s",__FUNCTION__, __LINE__,strerror(errno));
        ALOGE(" %s(%d) RGA_FLUSH fail: %s",__FUNCTION__, __LINE__,strerror(errno));
        return -errno;
    }
    return 0;
}

int RgaCollorFill(rga_info *dst) {
    //check rects
    //check buffer_handle_t with rects
    struct rgaContext *ctx = rgaCtx;
    int dstVirW,dstVirH,dstActW,dstActH,dstXPos,dstYPos;
    int scaleMode,ditherEn;
    int dstType,dstMmuFlag;
    int dstFd = -1;
    int ret = 0;
    unsigned int color = 0x00000000;
    rga_rect_t relDstRect,tmpDstRect;
    struct rga_req rgaReg;
    COLOR_FILL fillColor ;
    void *dstBuf = NULL;
#ifdef ANDROID
    RECT clip;
#elif LINUX
    RECT_t  clip;
#endif
    int sync_mode = RGA_BLIT_SYNC;

    if (!ctx) {
        ALOGE("Try to use uninit rgaCtx=%p",ctx);
        return -ENODEV;
    }

    memset(&rgaReg, 0, sizeof(struct rga_req));

    dstType = dstMmuFlag = 0;

    if (!dst) {
        ALOGE("src = %p, dst = %p", dst, dst);
        return -EINVAL;
    }

    if (dst) {
        color = dst->color;
        memcpy(&relDstRect, &dst->rect, sizeof(rga_rect_t));
    }

    dstFd = -1;

    if (relDstRect.hstride == 0)
        relDstRect.hstride = relDstRect.height;
#ifdef ANDROID
    if (dst && dst->hnd) {
        ret = RkRgaGetHandleFd(dst->hnd, &dstFd);
        if (ret) {
            ALOGE("dst handle get fd fail ret = %d,hnd=%p", ret, &dst->hnd);
            printf("-dst handle get fd fail ret = %d,hnd=%p", ret, &dst->hnd);
            return ret;
        }
        if (!isRectValid(relDstRect)) {
            ret = NormalRgaGetRect(dst->hnd, &tmpDstRect);
            if (ret)
                return ret;
            memcpy(&relDstRect, &tmpDstRect, sizeof(rga_rect_t));
        }
        NormalRgaGetMmuType(dst->hnd, &dstType);
    }
#endif

    if (dst && dstFd < 0)
        dstFd = dst->fd;

    if (dst && dst->phyAddr)
        dstBuf = dst->phyAddr;
    else if (dst && dst->virAddr)
        dstBuf = dst->virAddr;
#ifdef ANDROID
    else if (dst && dst->hnd)
        ret = RkRgaGetHandleMapAddress(dst->hnd, &dstBuf);
#endif

    if (dst && dstFd == -1 && !dstBuf) {
        ALOGE("%d:dst has not fd and address for render", __LINE__);
        return ret;
    }

    if (dst && dstFd == 0 && !dstBuf) {
        ALOGE("dstFd is zero, now driver not support");
        return -EINVAL;
    }

    if (dstFd == 0)
        dstFd = -1;

    if (relDstRect.hstride == 0)
        relDstRect.hstride = relDstRect.height;

    dstVirW = relDstRect.wstride;
    dstVirH = relDstRect.hstride;
    dstXPos = relDstRect.xoffset;
    dstYPos = relDstRect.yoffset;
    dstActW = relDstRect.width;
    dstActH = relDstRect.height;

    clip.xmin = 0;
    clip.xmax = dstActW - 1;
    clip.ymin = 0;
    clip.ymax = dstActH - 1;

    if (ctx->mVersion <= 1.003) {
#if defined(__arm64__) || defined(__aarch64__)
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                   (unsigned long)dstBuf + dstVirW * dstVirH,
                                   (unsigned long)dstBuf + dstVirW * dstVirH * 5/4,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
#else
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                   (unsigned int)dstBuf + dstVirW * dstVirH,
                                   (unsigned int)dstBuf + dstVirW * dstVirH * 5/4,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
#endif
    } else if (ctx->mVersion < 1.6 ) {
        /*dst*/
        if (dstFd != -1) {
            dstMmuFlag = dstType ? 1 : 0;
            if (dst && dstFd == dst->fd)
                dstMmuFlag = dst->mmuFlag ? 1 : 0;
            NormalRgaSetDstVirtualInfo(&rgaReg, 0, 0, 0, dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
            /*src dst fd*/
            NormalRgaSetFdsOffsets(&rgaReg, 0, dstFd, 0, 0);
        } else {
            if (dst && dst->hnd)
                dstMmuFlag = dstType ? 1 : 0;
            if (dst && dstBuf == dst->virAddr)
                dstMmuFlag = 1;
            if (dst && dstBuf == dst->phyAddr)
                dstMmuFlag = 0;
#if defined(__arm64__) || defined(__aarch64__)
            NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                       (unsigned long)dstBuf + dstVirW * dstVirH,
                                       (unsigned long)dstBuf + dstVirW * dstVirH * 5/4,
                                       dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
#else
            NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                       (unsigned int)dstBuf + dstVirW * dstVirH,
                                       (unsigned int)dstBuf + dstVirW * dstVirH * 5/4,
                                       dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
#endif
        }
    } else {
        if (dst && dst->hnd)
            dstMmuFlag = dstType ? 1 : 0;
        if (dst && dstBuf == dst->virAddr)
            dstMmuFlag = 1;
        if (dst && dstBuf == dst->phyAddr)
            dstMmuFlag = 0;
        if (dstFd != -1)
            dstMmuFlag = dstType ? 1 : 0;
        if (dst && dstFd == dst->fd)
            dstMmuFlag = dst->mmuFlag ? 1 : 0;
#if defined(__arm64__) || defined(__aarch64__)
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                   (unsigned long)dstBuf,
                                   (unsigned long)dstBuf + dstVirW * dstVirH,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
#else
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                   (unsigned int)dstBuf,
                                   (unsigned int)dstBuf + dstVirW * dstVirH,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
#endif
    }

    NormalRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);

    memset(&fillColor, 0x0, sizeof(COLOR_FILL));

    /*mode*/
    NormalRgaSetColorFillMode(&rgaReg, &fillColor, 0, 0, color, 0, 0, 0, 0, 0);

    if (dstMmuFlag) {
        NormalRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        NormalRgaMmuFlag(&rgaReg, dstMmuFlag, dstMmuFlag);
    }

#ifdef LINUX
#ifdef __DEBUG
    NormalRgaLogOutRgaReq(rgaReg);
#endif
#endif

#ifndef RK3368
#ifdef  ANDROID_7_DRM
    rgaReg.render_mode |= RGA_BUF_GEM_TYPE_DMA;
#endif
#endif

    if(dst->sync_mode == RGA_BLIT_ASYNC) {
        sync_mode = dst->sync_mode;
    }

    if(ioctl(ctx->rgaFd, sync_mode, &rgaReg)) {
        printf(" %s(%d) RGA_BLIT fail: %s",__FUNCTION__, __LINE__,strerror(errno));
        ALOGE(" %s(%d) RGA_BLIT fail: %s",__FUNCTION__, __LINE__,strerror(errno));
    }

    return 0;
}

int RgaCollorPalette(rga_info *src, rga_info *dst, rga_info *lut) {

    struct rgaContext *ctx = rgaCtx;
    struct rga_req  Rga_Request;
    struct rga_req  Rga_Request2;
    int srcVirW ,srcVirH ,srcActW ,srcActH ,srcXPos ,srcYPos;
    int dstVirW ,dstVirH ,dstActW ,dstActH ,dstXPos ,dstYPos;
    int lutVirW ,lutVirH ,lutActW ,lutActH ,lutXPos ,lutYPos;
    int srcType ,dstType ,lutType ,srcMmuFlag ,dstMmuFlag, lutMmuFlag;
    int dstFd = -1;
    int srcFd = -1;
    int lutFd = -1;
    int ret = 0;
    rga_rect_t relSrcRect,tmpSrcRect,relDstRect,tmpDstRect, relLutRect, tmpLutRect;
    struct rga_req rgaReg,tmprgaReg;
    void *srcBuf = NULL;
    void *dstBuf = NULL;
    void *lutBuf = NULL;
#ifdef ANDROID
    RECT clip;
#elif LINUX
    RECT_t clip;
#endif

    //init context
    if (!ctx) {
        ALOGE("Try to use uninit rgaCtx=%p",ctx);
        return -ENODEV;
    }

    //init
    memset(&rgaReg, 0, sizeof(struct rga_req));

    srcType = dstType = lutType = srcMmuFlag = dstMmuFlag = lutMmuFlag = 0;

#ifdef ANDROID
    /* print debug log by setting property vendor.rga.log as 1 */
    is_debug_log();
    if(is_out_log())
    ALOGD("<<<<-------- print rgaLog -------->>>>");
#endif

    if (!src && !dst) {
        ALOGE("src = %p, dst = %p, lut = %p", src, dst, lut);
        return -EINVAL;
    }

     /* get effective area from src、dst and lut, if the area is empty, choose to get parameter from handle. */
    if (src)
        memcpy(&relSrcRect, &src->rect, sizeof(rga_rect_t));
    if (dst)
        memcpy(&relDstRect, &dst->rect, sizeof(rga_rect_t));
    if (lut)
        memcpy(&relLutRect, &lut->rect, sizeof(rga_rect_t));

    srcFd = dstFd = lutFd = -1;

#ifdef ANDROID
    if(is_out_log()) {
        ALOGD("src->hnd = %p , dst->hnd = %p, lut->hnd = %p \n",src->hnd,dst->hnd, lut->hnd);
        ALOGD("src: Fd = %.2d , phyAddr = %p , virAddr = %p\n",src->fd,src->phyAddr,src->virAddr);
        ALOGD("dst: Fd = %.2d , phyAddr = %p , virAddr = %p\n",dst->fd,dst->phyAddr,dst->virAddr);
        ALOGD("lut: Fd = %.2d , phyAddr = %p , virAddr = %p\n",lut->fd,lut->phyAddr,lut->virAddr);
    }
#endif

    /*********** get src addr *************/
    if (src && src->phyAddr) {
        srcBuf = src->phyAddr;
    } else if (src && src->fd > 0) {
        srcFd = src->fd;
        src->mmuFlag = 1;
    } else if (src && src->virAddr) {
        srcBuf = src->virAddr;
        src->mmuFlag = 1;
    }
#ifdef ANDROID
    else if (src && src->hnd) {
#ifndef RK3188
        /* RK3188 is special, cannot configure rga through fd. */
        RkRgaGetHandleFd(src->hnd, &srcFd);
#endif
#ifndef ANDROID_8
        if (srcFd < 0 || srcFd == 0) {
            RkRgaGetHandleMapAddress(src->hnd, &srcBuf);
        }
#endif
        if ((srcFd < 0 || srcFd == 0) && srcBuf == NULL) {
            ALOGE("src handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &src->hnd);
            printf("src handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &src->hnd);
            return ret;
        }
        else {
            srcType = 1;
        }
    }

    if (!isRectValid(relSrcRect)) {
        ret = NormalRgaGetRect(src->hnd, &tmpSrcRect);
        if (ret) {
            ALOGE("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &src->hnd);
            printf("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &src->hnd);
            return ret;
        }
        memcpy(&relSrcRect, &tmpSrcRect, sizeof(rga_rect_t));
    }
#endif

    if (srcFd == -1 && !srcBuf) {
        ALOGE("%d:src has not fd and address for render", __LINE__);
        return ret;
    }
    if (srcFd == 0 && !srcBuf) {
        ALOGE("srcFd is zero, now driver not support");
        return -EINVAL;
    }
    /* Old rga driver cannot support fd as zero. */
    if (srcFd == 0)
        srcFd = -1;

    /*********** get dst addr *************/
    if (dst && dst->phyAddr) {
        dstBuf = dst->phyAddr;
    } else if (dst && dst->fd > 0) {
        dstFd = dst->fd;
        dst->mmuFlag = 1;
    } else if (dst && dst->virAddr) {
        dstBuf = dst->virAddr;
        dst->mmuFlag = 1;
    }
#ifdef ANDROID
    else if (dst && dst->hnd) {
#ifndef RK3188
        /* RK3188 is special, cannot configure rga through fd. */
        RkRgaGetHandleFd(dst->hnd, &dstFd);
#endif
#ifndef ANDROID_8
        if (dstFd < 0 || dstFd == 0) {
            RkRgaGetHandleMapAddress(dst->hnd, &dstBuf);
        }
#endif
        if ((dstFd < 0 || dstFd == 0) && dstBuf == NULL) {
            ALOGE("dst handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &dst->hnd);
            printf("dst handle get fd and vir_addr fail ret = %d,hnd=%p", ret, &dst->hnd);
            return ret;
        }
        else {
            dstType = 1;
        }
    }

    if (!isRectValid(relDstRect)) {
        ret = NormalRgaGetRect(dst->hnd, &tmpDstRect);
        if (ret) {
            ALOGE("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &dst->hnd);
            printf("dst handleGetRect fail ,ret = %d,hnd=%p", ret, &dst->hnd);
            return ret;
        }
        memcpy(&relDstRect, &tmpDstRect, sizeof(rga_rect_t));
    }
#endif

    if (dstFd == -1 && !dstBuf) {
        ALOGE("%d:dst has not fd and address for render", __LINE__);
        return ret;
    }
    if (dstFd == 0 && !dstBuf) {
        ALOGE("dstFd is zero, now driver not support");
        return -EINVAL;
    }
    /* Old rga driver cannot support fd as zero. */
    if (dstFd == 0)
        dstFd = -1;

    /*********** get lut addr *************/
    if (lut && lut->phyAddr) {
        lutBuf = lut->phyAddr;
    } else if (lut && lut->fd > 0) {
        lutFd = lut->fd;
        lut->mmuFlag = 1;
    } else if (lut && lut->virAddr) {
        lutBuf = lut->virAddr;
        lut->mmuFlag = 1;
    }
#ifdef ANDROID
    else if (lut && lut->hnd) {
#ifndef RK3188
        /* RK3188 is special, cannot configure rga through fd. */
        RkRgaGetHandleFd(lut->hnd, &lutFd);
#endif
#ifndef ANDROID_8
        if (lutFd < 0 || lutFd == 0) {
            RkRgaGetHandleMapAddress(lut->hnd, &lutBuf);
        }
#endif
        if ((lutFd < 0 || lutFd == 0) && lutBuf == NULL) {
            ALOGE("No lut address,not using update palette table mode.\n");
            printf("No lut address,not using update palette table mode.\n");
        }
        else {
            lutType = 1;
        }
    }
    ALOGD("lut->mmuFlag = %d", lut->mmuFlag);
    if (!isRectValid(relLutRect)) {
        ret = NormalRgaGetRect(lut->hnd, &tmpLutRect);
        if (ret) {
            ALOGE("lut handleGetRect fail ,ret = %d,hnd=%p", ret, &lut->hnd);
            printf("lut handleGetRect fail ,ret = %d,hnd=%p", ret, &lut->hnd);
        }
        memcpy(&relLutRect, &tmpLutRect, sizeof(rga_rect_t));
    }
#endif

    /* Old rga driver cannot support fd as zero. */
    if (lutFd == 0)
        lutFd = -1;

#ifdef ANDROID
    if(is_out_log()) {
        ALOGD("src: Fd = %.2d , buf = %p, mmuFlag = %d, mmuType = %d\n", srcFd, srcBuf, src->mmuFlag, srcType);
        ALOGD("dst: Fd = %.2d , buf = %p, mmuFlag = %d, mmuType = %d\n", dstFd, dstBuf, dst->mmuFlag, dstType);
        ALOGD("lut: Fd = %.2d , buf = %p, mmuFlag = %d, mmuType = %d\n", lutFd, lutBuf, lut->mmuFlag, lutType);
    }
#endif

#ifdef RK3126C
    if ( (relSrcRect.width == relDstRect.width) && (relSrcRect.height == relDstRect.height ) &&
         (relSrcRect.width + 2*relSrcRect.xoffset == relSrcRect.wstride) &&
         (relSrcRect.height + 2*relSrcRect.yoffset == relSrcRect.hstride) &&
         (relSrcRect.format == HAL_PIXEL_FORMAT_YCrCb_NV12) && (relSrcRect.xoffset > 0 && relSrcRect.yoffset > 0)
       ) {
        relSrcRect.width += 4;
        //relSrcRect.height += 4;
        relSrcRect.xoffset = (relSrcRect.wstride - relSrcRect.width) / 2;
    }
#endif
    /* discripe a picture need high stride.If high stride not to be set, need use height as high stride. */
    if (relSrcRect.hstride == 0)
        relSrcRect.hstride = relSrcRect.height;

    if (relDstRect.hstride == 0)
        relDstRect.hstride = relDstRect.height;

    /* do some check, check the area of src and dst whether is effective. */
    if (src) {
        ret = checkRectForRga(relSrcRect);
        if (ret) {
            printf("Error srcRect\n");
            ALOGE("[%s,%d]Error srcRect \n", __func__, __LINE__);
            return ret;
        }
    }

    if (dst) {
        ret = checkRectForRga(relDstRect);
        if (ret) {
            printf("Error dstRect\n");
            ALOGE("[%s,%d]Error dstRect \n", __func__, __LINE__);
            return ret;
        }
    }

    srcVirW = relSrcRect.wstride;
    srcVirH = relSrcRect.hstride;
    srcXPos = relSrcRect.xoffset;
    srcYPos = relSrcRect.yoffset;
    srcActW = relSrcRect.width;
    srcActH = relSrcRect.height;

    dstVirW = relDstRect.wstride;
    dstVirH = relDstRect.hstride;
    dstXPos = relDstRect.xoffset;
    dstYPos = relDstRect.yoffset;
    dstActW = relDstRect.width;
    dstActH = relDstRect.height;

    lutVirW = relLutRect.wstride;
    lutVirH = relLutRect.hstride;
    lutXPos = relLutRect.xoffset;
    lutYPos = relLutRect.yoffset;
    lutActW = relLutRect.width;
    lutActH = relLutRect.height;

    /* if pictual out of range should be cliped. */
    clip.xmin = 0;
    clip.xmax = dstVirW - 1;
    clip.ymin = 0;
    clip.ymax = dstVirH - 1;

    /* only to configure the parameter by driver version, because rga driver has too many version. */
    if (ctx->mVersion <= (float)1.003) {
        srcMmuFlag = dstMmuFlag = lutMmuFlag = 1;

#if defined(__arm64__) || defined(__aarch64__)
        NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                   (unsigned long)srcBuf + srcVirW * srcVirH,
                                   (unsigned long)srcBuf + srcVirW * srcVirH * 5/4,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                   (unsigned long)dstBuf + dstVirW * dstVirH,
                                   (unsigned long)dstBuf + dstVirW * dstVirH * 5/4,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
        /*lut*/
        NormalRgaSetPatVirtualInfo(&rgaReg, (unsigned long)lutBuf,
                                   (unsigned long)lutBuf + lutVirW * lutVirH,
                                   (unsigned long)lutBuf + lutVirW * lutVirH * 5/4,
                                   lutVirW, lutVirH, &clip,
                                   RkRgaGetRgaFormat(relLutRect.format),0);
#else
        NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                   (unsigned int)srcBuf + srcVirW * srcVirH,
                                   (unsigned int)srcBuf + srcVirW * srcVirH * 5/4,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                   (unsigned int)dstBuf + dstVirW * dstVirH,
                                   (unsigned int)dstBuf + dstVirW * dstVirH * 5/4,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
        /*lut*/
        NormalRgaSetPatVirtualInfo(&rgaReg, (unsigned long)lutBuf,
                                   (unsigned int)lutBuf + lutVirW * lutVirH,
                                   (unsigned int)lutBuf + lutVirW * lutVirH * 5/4,
                                   lutVirW, lutVirH, &clip,
                                   RkRgaGetRgaFormat(relLutRect.format),0);

#endif
        /* the version 1.005 is different to assign fd from version 2.0 and above */
    } else if (ctx->mVersion < (float)1.6) {
        /*Src*/
        if (srcFd != -1) {
            srcMmuFlag = srcType ? 1 : 0;
            if (src && srcFd == src->fd)
                srcMmuFlag = src->mmuFlag ? 1 : 0;
            NormalRgaSetSrcVirtualInfo(&rgaReg, 0, 0, 0, srcVirW, srcVirH,
                                       RkRgaGetRgaFormat(relSrcRect.format),0);
            NormalRgaSetFdsOffsets(&rgaReg, srcFd, 0, 0, 0);
        } else {
            if (src && src->hnd)
                srcMmuFlag = srcType ? 1 : 0;
            if (src && srcBuf == src->virAddr)
                srcMmuFlag = 1;
            if (src && srcBuf == src->phyAddr)
                srcMmuFlag = 0;
#if defined(__arm64__) || defined(__aarch64__)
            NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned long)srcBuf,
                                       (unsigned long)srcBuf + srcVirW * srcVirH,
                                       (unsigned long)srcBuf + srcVirW * srcVirH * 5/4,
                                       srcVirW, srcVirH,
                                       RkRgaGetRgaFormat(relSrcRect.format),0);
#else
            NormalRgaSetSrcVirtualInfo(&rgaReg, (unsigned int)srcBuf,
                                       (unsigned int)srcBuf + srcVirW * srcVirH,
                                       (unsigned int)srcBuf + srcVirW * srcVirH * 5/4,
                                       srcVirW, srcVirH,
                                       RkRgaGetRgaFormat(relSrcRect.format),0);
#endif
        }
        /*dst*/
        if (dstFd != -1) {
            dstMmuFlag = dstType ? 1 : 0;
            if (dst && dstFd == dst->fd)
                dstMmuFlag = dst->mmuFlag ? 1 : 0;
            NormalRgaSetDstVirtualInfo(&rgaReg, 0, 0, 0, dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
            /*src dst fd*/
            NormalRgaSetFdsOffsets(&rgaReg, 0, dstFd, 0, 0);
        } else {
            if (dst && dst->hnd)
                dstMmuFlag = dstType ? 1 : 0;
            if (dst && dstBuf == dst->virAddr)
                dstMmuFlag = 1;
            if (dst && dstBuf == dst->phyAddr)
                dstMmuFlag = 0;
#if defined(__arm64__) || defined(__aarch64__)
            NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned long)dstBuf,
                                       (unsigned long)dstBuf + dstVirW * dstVirH,
                                       (unsigned long)dstBuf + dstVirW * dstVirH * 5/4,
                                       dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
#else
            NormalRgaSetDstVirtualInfo(&rgaReg, (unsigned int)dstBuf,
                                       (unsigned int)dstBuf + dstVirW * dstVirH,
                                       (unsigned int)dstBuf + dstVirW * dstVirH * 5/4,
                                       dstVirW, dstVirH, &clip,
                                       RkRgaGetRgaFormat(relDstRect.format),0);
#endif
        }
        /*lut*/
        if (lutFd != -1) {
            lutMmuFlag = lutType ? 1 : 0;
            if (lut && lutFd == lut->fd)
                lutMmuFlag = lut->mmuFlag ? 1 : 0;
            NormalRgaSetPatVirtualInfo(&rgaReg, 0, 0, 0, lutVirW, lutVirH, &clip,
                                       RkRgaGetRgaFormat(relLutRect.format),0);
            /*lut fd*/
            NormalRgaSetFdsOffsets(&rgaReg, 0, lutFd, 0, 0);
        } else {
            if (lut && lut->hnd)
                lutMmuFlag = lutType ? 1 : 0;
            if (lut && lutBuf == lut->virAddr)
                lutMmuFlag = 1;
            if (lut && lutBuf == lut->phyAddr)
                lutMmuFlag = 0;
#if defined(__arm64__) || defined(__aarch64__)
            NormalRgaSetPatVirtualInfo(&rgaReg, (unsigned long)lutBuf,
                                       (unsigned long)lutBuf + lutVirW * lutVirH,
                                       (unsigned long)lutBuf + lutVirW * lutVirH * 5/4,
                                       lutVirW, lutVirH, &clip,
                                       RkRgaGetRgaFormat(relLutRect.format),0);
#else
            NormalRgaSetPatVirtualInfo(&rgaReg, (unsigned int)lutBuf,
                                       (unsigned int)lutBuf + lutVirW * lutVirH,
                                       (unsigned int)lutBuf + lutVirW * lutVirH * 5/4,
                                       lutVirW, lutVirH, &clip,
                                       RkRgaGetRgaFormat(relLutRect.format),0);
#endif
        }
    } else {
        if (src && src->hnd)
            srcMmuFlag = srcType ? 1 : 0;
        if (src && srcBuf == src->virAddr)
            srcMmuFlag = 1;
        if (src && srcBuf == src->phyAddr)
            srcMmuFlag = 0;
        if (srcFd != -1)
            srcMmuFlag = srcType ? 1 : 0;
        if (src && srcFd == src->fd)
            srcMmuFlag = src->mmuFlag ? 1 : 0;

        if (dst && dst->hnd)
            dstMmuFlag = dstType ? 1 : 0;
        if (dst && dstBuf == dst->virAddr)
            dstMmuFlag = 1;
        if (dst && dstBuf == dst->phyAddr)
            dstMmuFlag = 0;
        if (dstFd != -1)
            dstMmuFlag = dstType ? 1 : 0;
        if (dst && dstFd == dst->fd)
            dstMmuFlag = dst->mmuFlag ? 1 : 0;

        if (lut && lut->hnd)
            lutMmuFlag = lutType ? 1 : 0;
        if (lut && lutBuf == lut->virAddr)
            lutMmuFlag = 1;
        if (lut && lutBuf == lut->phyAddr)
            lutMmuFlag = 0;
        if (lutFd != -1)
            lutMmuFlag = lutType ? 1 : 0;
        if (lut && lutFd == lut->fd)
            lutMmuFlag = lut->mmuFlag ? 1 : 0;

#if defined(__arm64__) || defined(__aarch64__)
        NormalRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                   (unsigned long)srcBuf,
                                   (unsigned long)srcBuf + srcVirW * srcVirH,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                   (unsigned long)dstBuf,
                                   (unsigned long)dstBuf + dstVirW * dstVirH,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);

        /*lut*/
        NormalRgaSetPatVirtualInfo(&rgaReg, lutFd != -1 ? lutFd : 0,
                                   (unsigned long)lutBuf,
                                   (unsigned long)lutBuf + lutVirW * lutVirH,
                                   lutVirW, lutVirH, &clip,
                                   RkRgaGetRgaFormat(relLutRect.format),0);
#else
        NormalRgaSetSrcVirtualInfo(&rgaReg, srcFd != -1 ? srcFd : 0,
                                   (unsigned int)srcBuf,
                                   (unsigned int)srcBuf + srcVirW * srcVirH,
                                   srcVirW, srcVirH,
                                   RkRgaGetRgaFormat(relSrcRect.format),0);
        /*dst*/
        NormalRgaSetDstVirtualInfo(&rgaReg, dstFd != -1 ? dstFd : 0,
                                   (unsigned int)dstBuf,
                                   (unsigned int)dstBuf + dstVirW * dstVirH,
                                   dstVirW, dstVirH, &clip,
                                   RkRgaGetRgaFormat(relDstRect.format),0);
        /*lut*/
        NormalRgaSetPatVirtualInfo(&rgaReg, lutFd != -1 ? lutFd : 0,
                                  (unsigned int)lutBuf,
                                  (unsigned int)lutBuf + lutVirW * lutVirH,
                                  lutVirW, lutVirH, &clip,
                                  RkRgaGetRgaFormat(relLutRect.format),0);

#endif
    }

    /* set effective area of src and dst. */
    NormalRgaSetSrcActiveInfo(&rgaReg, srcActW, srcActH, srcXPos, srcYPos);
    NormalRgaSetDstActiveInfo(&rgaReg, dstActW, dstActH, dstXPos, dstYPos);
    NormalRgaSetPatActiveInfo(&rgaReg, lutActW, lutActH, lutXPos, lutYPos);

    if (srcMmuFlag || dstMmuFlag || lutMmuFlag) {
        NormalRgaMmuInfo(&rgaReg, 1, 0, 0, 0, 0, 2);
        NormalRgaMmuFlag(&rgaReg, srcMmuFlag, dstMmuFlag);
        /*set lut mmu_flag*/
        if (lutMmuFlag) {
            rgaReg.mmu_info.mmu_flag |= (0x1 << 11);
            rgaReg.mmu_info.mmu_flag |= (0x1 << 9);
        }

    }

#ifdef ANDROID
    if(is_out_log()) {
        ALOGD("srcMmuFlag = %d , dstMmuFlag = %d , lutMmuFlag = %d\n", srcMmuFlag, dstMmuFlag, lutMmuFlag);
        ALOGD("<<<<-------- rgaReg -------->>>>\n");
        NormalRgaLogOutRgaReq(rgaReg);
    }
#elif LINUX
#ifdef __DEBUG
    NormalRgaLogOutRgaReq(rgaReg);
#endif
#endif

    switch (RkRgaGetRgaFormat(relSrcRect.format)) {
        case RK_FORMAT_BPP1 :
            rgaReg.palette_mode = 0;
            break;
        case RK_FORMAT_BPP2 :
            rgaReg.palette_mode = 1;
            break;
        case RK_FORMAT_BPP4 :
            rgaReg.palette_mode = 2;
            break;
        case RK_FORMAT_BPP8 :
            rgaReg.palette_mode = 3;
            break;
    }

    if (!(lutFd == -1 && lutBuf == NULL)) {
        rgaReg.fading.g = 0xff;
        rgaReg.render_mode = update_palette_table_mode;

        if(ioctl(ctx->rgaFd, RGA_BLIT_SYNC, &rgaReg) != 0) {
            printf("update palette table mode ioctl err\n");
            return -1;
        }
    }

    rgaReg.render_mode = color_palette_mode;
    rgaReg.endian_mode = 1;

    if(ioctl(ctx->rgaFd, RGA_BLIT_SYNC, &rgaReg) != 0) {
      printf("color palette ioctl err\n");
        return -1;
    }

    return 0;
}

int NormalRgaScale() {
    return 1;
}

int NormalRgaRoate() {
    return 1;
}

int NormalRgaRoateScale() {
    return 1;
}