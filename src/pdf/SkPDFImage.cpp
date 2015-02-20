/*
 * Copyright 2010 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkPDFImage.h"

#include "SkBitmap.h"
#include "SkColor.h"
#include "SkColorPriv.h"
#include "SkData.h"
#include "SkFlate.h"
#include "SkPDFBitmap.h"
#include "SkPDFCatalog.h"
#include "SkPixelRef.h"
#include "SkRect.h"
#include "SkStream.h"
#include "SkString.h"
#include "SkUnPreMultiply.h"

static size_t get_uncompressed_size(const SkBitmap& bitmap,
                                    const SkIRect& srcRect) {
    switch (bitmap.colorType()) {
        case kIndex_8_SkColorType:
            return srcRect.width() * srcRect.height();
        case kARGB_4444_SkColorType:
            return ((srcRect.width() * 3 + 1) / 2) * srcRect.height();
        case kRGB_565_SkColorType:
            return srcRect.width() * 3 * srcRect.height();
        case kRGBA_8888_SkColorType:
        case kBGRA_8888_SkColorType:
            return srcRect.width() * 3 * srcRect.height();
        case kAlpha_8_SkColorType:
            return 1;
        default:
            SkASSERT(false);
            return 0;
    }
}

static SkStream* extract_index8_image(const SkBitmap& bitmap,
                                      const SkIRect& srcRect) {
    const int rowBytes = srcRect.width();
    SkStream* stream = SkNEW_ARGS(SkMemoryStream,
                                  (get_uncompressed_size(bitmap, srcRect)));
    uint8_t* dst = (uint8_t*)stream->getMemoryBase();

    for (int y = srcRect.fTop; y < srcRect.fBottom; y++) {
        memcpy(dst, bitmap.getAddr8(srcRect.fLeft, y), rowBytes);
        dst += rowBytes;
    }
    return stream;
}

static SkStream* extract_argb4444_data(const SkBitmap& bitmap,
                                       const SkIRect& srcRect,
                                       bool extractAlpha,
                                       bool* isOpaque,
                                       bool* isTransparent) {
    SkStream* stream;
    uint8_t* dst = NULL;
    if (extractAlpha) {
        const int alphaRowBytes = (srcRect.width() + 1) / 2;
        stream = SkNEW_ARGS(SkMemoryStream,
                            (alphaRowBytes * srcRect.height()));
    } else {
        stream = SkNEW_ARGS(SkMemoryStream,
                            (get_uncompressed_size(bitmap, srcRect)));
    }
    dst = (uint8_t*)stream->getMemoryBase();

    for (int y = srcRect.fTop; y < srcRect.fBottom; y++) {
        uint16_t* src = bitmap.getAddr16(0, y);
        int x;
        for (x = srcRect.fLeft; x + 1 < srcRect.fRight; x += 2) {
            if (extractAlpha) {
                dst[0] = (SkGetPackedA4444(src[x]) << 4) |
                    SkGetPackedA4444(src[x + 1]);
                *isOpaque &= dst[0] == SK_AlphaOPAQUE;
                *isTransparent &= dst[0] == SK_AlphaTRANSPARENT;
                dst++;
            } else {
                dst[0] = (SkGetPackedR4444(src[x]) << 4) |
                    SkGetPackedG4444(src[x]);
                dst[1] = (SkGetPackedB4444(src[x]) << 4) |
                    SkGetPackedR4444(src[x + 1]);
                dst[2] = (SkGetPackedG4444(src[x + 1]) << 4) |
                    SkGetPackedB4444(src[x + 1]);
                dst += 3;
            }
        }
        if (srcRect.width() & 1) {
            if (extractAlpha) {
                dst[0] = (SkGetPackedA4444(src[x]) << 4);
                *isOpaque &= dst[0] == (SK_AlphaOPAQUE & 0xF0);
                *isTransparent &= dst[0] == (SK_AlphaTRANSPARENT & 0xF0);
                dst++;

            } else {
                dst[0] = (SkGetPackedR4444(src[x]) << 4) |
                    SkGetPackedG4444(src[x]);
                dst[1] = (SkGetPackedB4444(src[x]) << 4);
                dst += 2;
            }
        }
    }
    return stream;
}

static SkStream* extract_rgb565_image(const SkBitmap& bitmap,
                                      const SkIRect& srcRect) {
    SkStream* stream = SkNEW_ARGS(SkMemoryStream,
                                  (get_uncompressed_size(bitmap,
                                                     srcRect)));
    uint8_t* dst = (uint8_t*)stream->getMemoryBase();
    for (int y = srcRect.fTop; y < srcRect.fBottom; y++) {
        uint16_t* src = bitmap.getAddr16(0, y);
        for (int x = srcRect.fLeft; x < srcRect.fRight; x++) {
            dst[0] = SkGetPackedR16(src[x]);
            dst[1] = SkGetPackedG16(src[x]);
            dst[2] = SkGetPackedB16(src[x]);
            dst += 3;
        }
    }
    return stream;
}

static uint32_t get_argb8888_neighbor_avg_color(const SkBitmap& bitmap,
                                                int xOrig,
                                                int yOrig);

static SkStream* extract_argb8888_data(const SkBitmap& bitmap,
                                       const SkIRect& srcRect,
                                       bool extractAlpha,
                                       bool* isOpaque,
                                       bool* isTransparent) {
    size_t streamSize = extractAlpha ? srcRect.width() * srcRect.height()
                                     : get_uncompressed_size(bitmap, srcRect);
    SkStream* stream = SkNEW_ARGS(SkMemoryStream, (streamSize));
    uint8_t* dst = (uint8_t*)stream->getMemoryBase();

    const SkUnPreMultiply::Scale* scaleTable = SkUnPreMultiply::GetScaleTable();

    for (int y = srcRect.fTop; y < srcRect.fBottom; y++) {
        uint32_t* src = bitmap.getAddr32(0, y);
        for (int x = srcRect.fLeft; x < srcRect.fRight; x++) {
            SkPMColor c = src[x];
            U8CPU alpha = SkGetPackedA32(c);
            if (extractAlpha) {
                *isOpaque &= alpha == SK_AlphaOPAQUE;
                *isTransparent &= alpha == SK_AlphaTRANSPARENT;
                *dst++ = alpha;
            } else {
                if (SK_AlphaTRANSPARENT == alpha) {
                    // It is necessary to average the color component of
                    // transparent pixels with their surrounding neighbors
                    // since the PDF renderer may separately re-sample the
                    // alpha and color channels when the image is not
                    // displayed at its native resolution. Since an alpha of
                    // zero gives no information about the color component,
                    // the pathological case is a white image with sharp
                    // transparency bounds - the color channel goes to black,
                    // and the should-be-transparent pixels are rendered
                    // as grey because of the separate soft mask and color
                    // resizing.
                    c = get_argb8888_neighbor_avg_color(bitmap, x, y);
                    *dst++ = SkGetPackedR32(c);
                    *dst++ = SkGetPackedG32(c);
                    *dst++ = SkGetPackedB32(c);
                } else {
                    SkUnPreMultiply::Scale s = scaleTable[alpha];
                    *dst++ = SkUnPreMultiply::ApplyScale(s, SkGetPackedR32(c));
                    *dst++ = SkUnPreMultiply::ApplyScale(s, SkGetPackedG32(c));
                    *dst++ = SkUnPreMultiply::ApplyScale(s, SkGetPackedB32(c));
                }
            }
        }
    }
    SkASSERT(dst == streamSize + (uint8_t*)stream->getMemoryBase());
    return stream;
}

static SkStream* extract_a8_alpha(const SkBitmap& bitmap,
                                  const SkIRect& srcRect,
                                  bool* isOpaque,
                                  bool* isTransparent) {
    const int alphaRowBytes = srcRect.width();
    SkStream* stream = SkNEW_ARGS(SkMemoryStream,
                                  (alphaRowBytes * srcRect.height()));
    uint8_t* alphaDst = (uint8_t*)stream->getMemoryBase();

    for (int y = srcRect.fTop; y < srcRect.fBottom; y++) {
        uint8_t* src = bitmap.getAddr8(0, y);
        for (int x = srcRect.fLeft; x < srcRect.fRight; x++) {
            alphaDst[0] = src[x];
            *isOpaque &= alphaDst[0] == SK_AlphaOPAQUE;
            *isTransparent &= alphaDst[0] == SK_AlphaTRANSPARENT;
            alphaDst++;
        }
    }
    return stream;
}

static SkStream* create_black_image() {
    SkStream* stream = SkNEW_ARGS(SkMemoryStream, (1));
    ((uint8_t*)stream->getMemoryBase())[0] = 0;
    return stream;
}

/**
 * Extract either the color or image data from a SkBitmap into a SkStream.
 * @param bitmap        Bitmap to extract data from.
 * @param srcRect       Region in the bitmap to extract.
 * @param extractAlpha  Set to true to extract the alpha data or false to
 *                      extract the color data.
 * @param isTransparent Pointer to a bool to output whether the alpha is
 *                      completely transparent. May be NULL. Only valid when
 *                      extractAlpha == true.
 * @return              Unencoded image data, or NULL if either data was not
 *                      available or alpha data was requested but the image was
 *                      entirely transparent or opaque.
 */
static SkStream* extract_image_data(const SkBitmap& bitmap,
                                    const SkIRect& srcRect,
                                    bool extractAlpha, bool* isTransparent) {
    SkColorType colorType = bitmap.colorType();
    if (extractAlpha && (kIndex_8_SkColorType == colorType ||
                         kRGB_565_SkColorType == colorType)) {
        if (isTransparent != NULL) {
            *isTransparent = false;
        }
        return NULL;
    }

    SkAutoLockPixels lock(bitmap);
    if (NULL == bitmap.getPixels()) {
        return NULL;
    }

    bool isOpaque = true;
    bool transparent = extractAlpha;
    SkAutoTDelete<SkStream> stream;

    switch (colorType) {
        case kIndex_8_SkColorType:
            if (!extractAlpha) {
                stream.reset(extract_index8_image(bitmap, srcRect));
            }
            break;
        case kARGB_4444_SkColorType:
            stream.reset(extract_argb4444_data(bitmap, srcRect, extractAlpha,
                                               &isOpaque, &transparent));
            break;
        case kRGB_565_SkColorType:
            if (!extractAlpha) {
                stream.reset(extract_rgb565_image(bitmap, srcRect));
            }
            break;
        case kN32_SkColorType:
            stream.reset(extract_argb8888_data(bitmap, srcRect, extractAlpha,
                                               &isOpaque, &transparent));
            break;
        case kAlpha_8_SkColorType:
            if (!extractAlpha) {
                stream.reset(create_black_image());
            } else {
                stream.reset(extract_a8_alpha(bitmap, srcRect,
                                              &isOpaque, &transparent));
            }
            break;
        default:
            SkASSERT(false);
    }

    if (isTransparent != NULL) {
        *isTransparent = transparent;
    }
    if (extractAlpha && (transparent || isOpaque)) {
        return NULL;
    }
    return stream.detach();
}

static SkPDFArray* make_indexed_color_space(SkColorTable* table) {
    SkPDFArray* result = new SkPDFArray();
    result->reserve(4);
    result->appendName("Indexed");
    result->appendName("DeviceRGB");
    result->appendInt(table->count() - 1);

    // Potentially, this could be represented in fewer bytes with a stream.
    // Max size as a string is 1.5k.
    SkString index;
    for (int i = 0; i < table->count(); i++) {
        char buf[3];
        SkColor color = SkUnPreMultiply::PMColorToColor((*table)[i]);
        buf[0] = SkGetPackedR32(color);
        buf[1] = SkGetPackedG32(color);
        buf[2] = SkGetPackedB32(color);
        index.append(buf, 3);
    }
    result->append(new SkPDFString(index))->unref();
    return result;
}

/**
 * Removes the alpha component of an ARGB color (including unpremultiply) while
 * keeping the output in the same format as the input.
 */
static uint32_t remove_alpha_argb8888(uint32_t pmColor) {
    SkColor color = SkUnPreMultiply::PMColorToColor(pmColor);
    return SkPackARGB32NoCheck(SK_AlphaOPAQUE,
                               SkColorGetR(color),
                               SkColorGetG(color),
                               SkColorGetB(color));
}

static uint16_t remove_alpha_argb4444(uint16_t pmColor) {
    return SkPixel32ToPixel4444(
            remove_alpha_argb8888(SkPixel4444ToPixel32(pmColor)));
}

static uint32_t get_argb8888_neighbor_avg_color(const SkBitmap& bitmap,
                                                int xOrig, int yOrig) {
    uint8_t count = 0;
    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;

    for (int y = yOrig - 1; y <= yOrig + 1; y++) {
        if (y < 0 || y >= bitmap.height()) {
            continue;
        }
        uint32_t* src = bitmap.getAddr32(0, y);
        for (int x = xOrig - 1; x <= xOrig + 1; x++) {
            if (x < 0 || x >= bitmap.width()) {
                continue;
            }
            if (SkGetPackedA32(src[x]) != SK_AlphaTRANSPARENT) {
                uint32_t color = remove_alpha_argb8888(src[x]);
                r += SkGetPackedR32(color);
                g += SkGetPackedG32(color);
                b += SkGetPackedB32(color);
                count++;
            }
        }
    }

    if (count == 0) {
        return SkPackARGB32NoCheck(SK_AlphaOPAQUE, 0, 0, 0);
    } else {
        return SkPackARGB32NoCheck(SK_AlphaOPAQUE,
                                   r / count, g / count, b / count);
    }
}

static uint16_t get_argb4444_neighbor_avg_color(const SkBitmap& bitmap,
                                                int xOrig, int yOrig) {
    uint8_t count = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    for (int y = yOrig - 1; y <= yOrig + 1; y++) {
        if (y < 0 || y >= bitmap.height()) {
            continue;
        }
        uint16_t* src = bitmap.getAddr16(0, y);
        for (int x = xOrig - 1; x <= xOrig + 1; x++) {
            if (x < 0 || x >= bitmap.width()) {
                continue;
            }
            if ((SkGetPackedA4444(src[x]) & 0x0F) != SK_AlphaTRANSPARENT) {
                uint16_t color = remove_alpha_argb4444(src[x]);
                r += SkGetPackedR4444(color);
                g += SkGetPackedG4444(color);
                b += SkGetPackedB4444(color);
                count++;
            }
        }
    }

    if (count == 0) {
        return SkPackARGB4444(SK_AlphaOPAQUE & 0x0F, 0, 0, 0);
    } else {
        return SkPackARGB4444(SK_AlphaOPAQUE & 0x0F,
                                   r / count, g / count, b / count);
    }
}

static SkBitmap unpremultiply_bitmap(const SkBitmap& bitmap,
                                     const SkIRect& srcRect) {
    SkBitmap outBitmap;
    outBitmap.allocPixels(bitmap.info().makeWH(srcRect.width(), srcRect.height()));
    int dstRow = 0;

    SkAutoLockPixels outBitmapPixelLock(outBitmap);
    SkAutoLockPixels bitmapPixelLock(bitmap);
    switch (bitmap.colorType()) {
        case kARGB_4444_SkColorType: {
            for (int y = srcRect.fTop; y < srcRect.fBottom; y++) {
                uint16_t* dst = outBitmap.getAddr16(0, dstRow);
                uint16_t* src = bitmap.getAddr16(0, y);
                for (int x = srcRect.fLeft; x < srcRect.fRight; x++) {
                    uint8_t a = SkGetPackedA4444(src[x]);
                    // It is necessary to average the color component of
                    // transparent pixels with their surrounding neighbors
                    // since the PDF renderer may separately re-sample the
                    // alpha and color channels when the image is not
                    // displayed at its native resolution. Since an alpha of
                    // zero gives no information about the color component,
                    // the pathological case is a white image with sharp
                    // transparency bounds - the color channel goes to black,
                    // and the should-be-transparent pixels are rendered
                    // as grey because of the separate soft mask and color
                    // resizing.
                    if (a == (SK_AlphaTRANSPARENT & 0x0F)) {
                        *dst = get_argb4444_neighbor_avg_color(bitmap, x, y);
                    } else {
                        *dst = remove_alpha_argb4444(src[x]);
                    }
                    dst++;
                }
                dstRow++;
            }
            break;
        }
        case kN32_SkColorType: {
            for (int y = srcRect.fTop; y < srcRect.fBottom; y++) {
                uint32_t* dst = outBitmap.getAddr32(0, dstRow);
                uint32_t* src = bitmap.getAddr32(0, y);
                for (int x = srcRect.fLeft; x < srcRect.fRight; x++) {
                    uint8_t a = SkGetPackedA32(src[x]);
                    if (a == SK_AlphaTRANSPARENT) {
                        *dst = get_argb8888_neighbor_avg_color(bitmap, x, y);
                    } else {
                        *dst = remove_alpha_argb8888(src[x]);
                    }
                    dst++;
                }
                dstRow++;
            }
            break;
        }
        default:
            SkASSERT(false);
    }

    outBitmap.setImmutable();

    return outBitmap;
}

// static
SkPDFImage* SkPDFImage::CreateImage(const SkBitmap& bitmap,
                                    const SkIRect& srcRect) {
    if (bitmap.colorType() == kUnknown_SkColorType) {
        return NULL;
    }

    bool isTransparent = false;
    SkAutoTDelete<SkStream> alphaData;
    if (!bitmap.isOpaque()) {
        // Note that isOpaque is not guaranteed to return false for bitmaps
        // with alpha support but a completely opaque alpha channel,
        // so alphaData may still be NULL if we have a completely opaque
        // (or transparent) bitmap.
        alphaData.reset(
                extract_image_data(bitmap, srcRect, true, &isTransparent));
    }
    if (isTransparent) {
        return NULL;
    }

    SkPDFImage* image;
    SkColorType colorType = bitmap.colorType();
    if (alphaData.get() != NULL && (kN32_SkColorType == colorType ||
                                    kARGB_4444_SkColorType == colorType)) {
        if (kN32_SkColorType == colorType) {
            image = SkNEW_ARGS(SkPDFImage, (NULL, bitmap, false,
                                            SkIRect::MakeWH(srcRect.width(),
                                                            srcRect.height())));
        } else {
            SkBitmap unpremulBitmap = unpremultiply_bitmap(bitmap, srcRect);
            image = SkNEW_ARGS(SkPDFImage, (NULL, unpremulBitmap, false,
                                            SkIRect::MakeWH(srcRect.width(),
                                                            srcRect.height())));
        }
    } else {
        image = SkNEW_ARGS(SkPDFImage, (NULL, bitmap, false, srcRect));
    }
    if (alphaData.get() != NULL) {
        SkAutoTUnref<SkPDFImage> mask(
                SkNEW_ARGS(SkPDFImage, (alphaData.get(), bitmap, true, srcRect)));
        image->insert("SMask", new SkPDFObjRef(mask))->unref();
    }
    return image;
}

SkPDFImage::~SkPDFImage() {}

SkPDFImage::SkPDFImage(SkStream* stream,
                       const SkBitmap& bitmap,
                       bool isAlpha,
                       const SkIRect& srcRect)
    : fIsAlpha(isAlpha),
      fSrcRect(srcRect) {

    if (bitmap.isImmutable()) {
        fBitmap = bitmap;
    } else {
        bitmap.deepCopyTo(&fBitmap);
        fBitmap.setImmutable();
    }

    if (stream != NULL) {
        this->setData(stream);
        fStreamValid = true;
    } else {
        fStreamValid = false;
    }

    SkColorType colorType = fBitmap.colorType();

    insertName("Type", "XObject");
    insertName("Subtype", "Image");

    bool alphaOnly = (kAlpha_8_SkColorType == colorType);

    if (!isAlpha && alphaOnly) {
        // For alpha only images, we stretch a single pixel of black for
        // the color/shape part.
        SkAutoTUnref<SkPDFInt> one(new SkPDFInt(1));
        insert("Width", one.get());
        insert("Height", one.get());
    } else {
        insertInt("Width", fSrcRect.width());
        insertInt("Height", fSrcRect.height());
    }

    if (isAlpha || alphaOnly) {
        insertName("ColorSpace", "DeviceGray");
    } else if (kIndex_8_SkColorType == colorType) {
        SkAutoLockPixels alp(fBitmap);
        insert("ColorSpace",
               make_indexed_color_space(fBitmap.getColorTable()))->unref();
    } else {
        insertName("ColorSpace", "DeviceRGB");
    }

    int bitsPerComp = 8;
    if (kARGB_4444_SkColorType == colorType) {
        bitsPerComp = 4;
    }
    insertInt("BitsPerComponent", bitsPerComp);

    if (kRGB_565_SkColorType == colorType) {
        SkASSERT(!isAlpha);
        SkAutoTUnref<SkPDFInt> zeroVal(new SkPDFInt(0));
        SkAutoTUnref<SkPDFScalar> scale5Val(
                new SkPDFScalar(8.2258f));  // 255/2^5-1
        SkAutoTUnref<SkPDFScalar> scale6Val(
                new SkPDFScalar(4.0476f));  // 255/2^6-1
        SkAutoTUnref<SkPDFArray> decodeValue(new SkPDFArray());
        decodeValue->reserve(6);
        decodeValue->append(zeroVal.get());
        decodeValue->append(scale5Val.get());
        decodeValue->append(zeroVal.get());
        decodeValue->append(scale6Val.get());
        decodeValue->append(zeroVal.get());
        decodeValue->append(scale5Val.get());
        insert("Decode", decodeValue.get());
    }
}

SkPDFImage::SkPDFImage(SkPDFImage& pdfImage)
    : SkPDFStream(pdfImage),
      fBitmap(pdfImage.fBitmap),
      fIsAlpha(pdfImage.fIsAlpha),
      fSrcRect(pdfImage.fSrcRect),
      fStreamValid(pdfImage.fStreamValid) {
    // Nothing to do here - the image params are already copied in SkPDFStream's
    // constructor, and the bitmap will be regenerated and encoded in
    // populate.
}

bool SkPDFImage::populate(SkPDFCatalog* catalog) {
    if (getState() == kUnused_State) {
        // Initializing image data for the first time.
        // Fallback method
        if (!fStreamValid) {
            SkAutoTDelete<SkStream> stream(
                    extract_image_data(fBitmap, fSrcRect, fIsAlpha, NULL));
            this->setData(stream);
            fStreamValid = true;
        }
        return INHERITED::populate(catalog);
    }
#ifndef SK_NO_FLATE
    else if (getState() == kNoCompression_State) {
        // Compression has not been requested when the stream was first created,
        // but the new catalog wants it compressed.
        if (!getSubstitute()) {
            SkPDFStream* substitute = SkNEW_ARGS(SkPDFImage, (*this));
            setSubstitute(substitute);
            catalog->setSubstitute(this, substitute);
        }
        return false;
    }
#endif  // SK_NO_FLATE
    return true;
}

#if 0  // reenable when we can figure out the JPEG colorspace
namespace {
/**
 *  This PDFObject assumes that its constructor was handed
 *  Jpeg-encoded data that can be directly embedded into a PDF.
 */
class PDFJPEGImage : public SkPDFObject {
    SkAutoTUnref<SkData> fData;
    int fWidth;
    int fHeight;
public:
    PDFJPEGImage(SkData* data, int width, int height)
        : fData(SkRef(data)), fWidth(width), fHeight(height) {}
    virtual void emitObject(
            SkWStream* stream,
            SkPDFCatalog* catalog, bool indirect) SK_OVERRIDE {
        if (indirect) {
            this->emitIndirectObject(stream, catalog);
            return;
        }
        SkASSERT(fData.get());
        const char kPrefaceFormat[] =
            "<<"
            "/Type /XObject\n"
            "/Subtype /Image\n"
            "/Width %d\n"
            "/Height %d\n"
            "/ColorSpace /DeviceRGB\n"  // or DeviceGray
            "/BitsPerComponent 8\n"
            "/Filter /DCTDecode\n"
            "/ColorTransform 0\n"
            "/Length " SK_SIZE_T_SPECIFIER "\n"
            ">> stream\n";
        SkString preface(
                SkStringPrintf(kPrefaceFormat, fWidth, fHeight, fData->size()));
        const char kPostface[] = "\nendstream";
        stream->write(preface.c_str(), preface.size());
        stream->write(fData->data(), fData->size());
        stream->write(kPostface, sizeof(kPostface));
    }
};

/**
 *  If the bitmap is not subsetted, return its encoded data, if
 *  availible.
 */
static inline SkData* ref_encoded_data(const SkBitmap& bm) {
    if ((NULL == bm.pixelRef())
        || !bm.pixelRefOrigin().isZero()
        || (bm.info().dimensions() != bm.pixelRef()->info().dimensions())) {
        return NULL;
    }
    return bm.pixelRef()->refEncodedData();
}

/*
 *  This functions may give false negatives but no false positives.
 */
static bool is_jfif_jpeg(SkData* data) {
    if (!data || (data->size() < 11)) {
        return false;
    }
    const uint8_t bytesZeroToThree[] = {0xFF, 0xD8, 0xFF, 0xE0};
    const uint8_t bytesSixToTen[] = {'J', 'F', 'I', 'F', 0};
    // 0   1   2   3   4   5   6   7   8   9   10
    // FF  D8  FF  E0  ??  ??  'J' 'F' 'I' 'F' 00 ...
    return ((0 == memcmp(data->bytes(), bytesZeroToThree,
                         sizeof(bytesZeroToThree)))
            && (0 == memcmp(data->bytes() + 6, bytesSixToTen,
                            sizeof(bytesSixToTen))));
}
}  // namespace
#endif

SkPDFObject* SkPDFCreateImageObject(
        const SkBitmap& bitmap,
        const SkIRect& subset) {
    if (SkPDFObject* pdfBitmap = SkPDFBitmap::Create(bitmap, subset)) {
        return pdfBitmap;
    }
#if 0  // reenable when we can figure out the JPEG colorspace
    if (SkIRect::MakeWH(bitmap.width(), bitmap.height()) == subset) {
        SkAutoTUnref<SkData> encodedData(ref_encoded_data(bitmap));
        if (is_jfif_jpeg(encodedData)) {
            return SkNEW_ARGS(PDFJPEGImage,
                              (encodedData, bitmap.width(), bitmap.height()));
        }
    }
#endif
    return SkPDFImage::CreateImage(bitmap, subset);
}
