/*
 * Copyright 2007 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#include "SkImageDecoder.h"
#include "SkImageEncoder.h"
#include "SkJpegUtility.h"
#include "SkColorPriv.h"
#include "SkDither.h"
#include "SkScaledBitmapSampler.h"
#include "SkStream.h"
#include "SkTemplates.h"
#include "SkTime.h"
#include "SkUtils.h"
#include "SkRTConf.h"
#include "SkRect.h"
#include "SkCanvas.h"
#include <math.h>

#ifdef HW_JPEG
#include "vdecoder.h"
#include "pdecoder.h"
#include "CdxParser.h"
#include "IonMemPool.h"
#include "memoryAdapter.h"
#endif


#include <stdio.h>
extern "C" {
    #include "jpeglib.h"
    #include "jerror.h"
}

#ifdef HW_JPEG
struct skJpegDecBitStream
{
    struct DecBitStream bitStream;

    // fStream is ref'ed and unref'ed
    SkStream*       fStream;
    // Unowned pointer to the decoder, used to check if the decoding process
    // has been cancelled.
    SkImageDecoder* fDecoder;

    int offset;
};
#endif

// These enable timing code that report milliseconds for an encoding/decoding
//#define TIME_ENCODE
//#define TIME_DECODE

// this enables our rgb->yuv code, which is faster than libjpeg on ARM
#define WE_CONVERT_TO_YUV

// If ANDROID_RGB is defined by in the jpeg headers it indicates that jpeg offers
// support for two additional formats (1) JCS_RGBA_8888 and (2) JCS_RGB_565.

#if defined(SK_DEBUG)
#define DEFAULT_FOR_SUPPRESS_JPEG_IMAGE_DECODER_WARNINGS false
#define DEFAULT_FOR_SUPPRESS_JPEG_IMAGE_DECODER_ERRORS false
#else  // !defined(SK_DEBUG)
#define DEFAULT_FOR_SUPPRESS_JPEG_IMAGE_DECODER_WARNINGS true
#define DEFAULT_FOR_SUPPRESS_JPEG_IMAGE_DECODER_ERRORS true
#endif  // defined(SK_DEBUG)
SK_CONF_DECLARE(bool, c_suppressJPEGImageDecoderWarnings,
                "images.jpeg.suppressDecoderWarnings",
                DEFAULT_FOR_SUPPRESS_JPEG_IMAGE_DECODER_WARNINGS,
                "Suppress most JPG warnings when calling decode functions.");
SK_CONF_DECLARE(bool, c_suppressJPEGImageDecoderErrors,
                "images.jpeg.suppressDecoderErrors",
                DEFAULT_FOR_SUPPRESS_JPEG_IMAGE_DECODER_ERRORS,
                "Suppress most JPG error messages when decode "
                "function fails.");

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static void overwrite_mem_buffer_size(jpeg_decompress_struct* cinfo) {
#ifdef SK_BUILD_FOR_ANDROID
    /* Check if the device indicates that it has a large amount of system memory
     * if so, increase the memory allocation to 30MB instead of the default 5MB.
     */
#ifdef ANDROID_LARGE_MEMORY_DEVICE
    cinfo->mem->max_memory_to_use = 30 * 1024 * 1024;
#else
    cinfo->mem->max_memory_to_use = 5 * 1024 * 1024;
#endif
#endif // SK_BUILD_FOR_ANDROID
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static void do_nothing_emit_message(jpeg_common_struct*, int) {
    /* do nothing */
}
static void do_nothing_output_message(j_common_ptr) {
    /* do nothing */
}

static void initialize_info(jpeg_decompress_struct* cinfo, skjpeg_source_mgr* src_mgr) {
    SkASSERT(cinfo != NULL);
    SkASSERT(src_mgr != NULL);
    jpeg_create_decompress(cinfo);
    overwrite_mem_buffer_size(cinfo);
    cinfo->src = src_mgr;
    /* To suppress warnings with a SK_DEBUG binary, set the
     * environment variable "skia_images_jpeg_suppressDecoderWarnings"
     * to "true".  Inside a program that links to skia:
     * SK_CONF_SET("images.jpeg.suppressDecoderWarnings", true); */
    if (c_suppressJPEGImageDecoderWarnings) {
        cinfo->err->emit_message = &do_nothing_emit_message;
    }
    /* To suppress error messages with a SK_DEBUG binary, set the
     * environment variable "skia_images_jpeg_suppressDecoderErrors"
     * to "true".  Inside a program that links to skia:
     * SK_CONF_SET("images.jpeg.suppressDecoderErrors", true); */
    if (c_suppressJPEGImageDecoderErrors) {
        cinfo->err->output_message = &do_nothing_output_message;
    }
}

#ifdef HW_JPEG 
static int GetCallingApkName(pid_t pid, char* strApkName, int nMaxNameSize)
{
    int fd;
    
    sprintf(strApkName, "/proc/%d/cmdline", pid);
    fd = ::open(strApkName, O_RDONLY);
    strApkName[0] = '\0';
    if (fd >= 0) 
    {
        ::read(fd, strApkName, nMaxNameSize);
        ::close(fd);
        //logd("Calling process is: %s", strApkName);
    }
    return 0;
}
#endif



#ifdef SK_BUILD_FOR_ANDROID
class SkJPEGImageIndex {
public:
    SkJPEGImageIndex(SkStreamRewindable* stream, SkImageDecoder* decoder)
        : fSrcMgr(stream, decoder)
        , fInfoInitialized(false)
        , fHuffmanCreated(false)
        , fDecompressStarted(false)
        {
            SkDEBUGCODE(fReadHeaderSucceeded = false;)
        }

    ~SkJPEGImageIndex() {
        if (fHuffmanCreated) {
            // Set to false before calling the libjpeg function, in case
            // the libjpeg function calls longjmp. Our setjmp handler may
            // attempt to delete this SkJPEGImageIndex, thus entering this
            // destructor again. Setting fHuffmanCreated to false first
            // prevents an infinite loop.
            fHuffmanCreated = false;
            jpeg_destroy_huffman_index(&fHuffmanIndex);
        }
        if (fDecompressStarted) {
            // Like fHuffmanCreated, set to false before calling libjpeg
            // function to prevent potential infinite loop.
            fDecompressStarted = false;
            jpeg_finish_decompress(&fCInfo);
        }
        if (fInfoInitialized) {
            this->destroyInfo();
        }
    }

    /**
     *  Destroy the cinfo struct.
     *  After this call, if a huffman index was already built, it
     *  can be used after calling initializeInfoAndReadHeader
     *  again. Must not be called after startTileDecompress except
     *  in the destructor.
     */
    void destroyInfo() {
        SkASSERT(fInfoInitialized);
        SkASSERT(!fDecompressStarted);
        // Like fHuffmanCreated, set to false before calling libjpeg
        // function to prevent potential infinite loop.
        fInfoInitialized = false;
        jpeg_destroy_decompress(&fCInfo);
        SkDEBUGCODE(fReadHeaderSucceeded = false;)
    }

    /**
     *  Initialize the cinfo struct.
     *  Calls jpeg_create_decompress, makes customizations, and
     *  finally calls jpeg_read_header. Returns true if jpeg_read_header
     *  returns JPEG_HEADER_OK.
     *  If cinfo was already initialized, destroyInfo must be called to
     *  destroy the old one. Must not be called after startTileDecompress.
     */
    bool initializeInfoAndReadHeader() {
        SkASSERT(!fInfoInitialized && !fDecompressStarted);
        initialize_info(&fCInfo, &fSrcMgr);
        fInfoInitialized = true;
        const bool success = (JPEG_HEADER_OK == jpeg_read_header(&fCInfo, true));
        SkDEBUGCODE(fReadHeaderSucceeded = success;)
        return success;
    }

    jpeg_decompress_struct* cinfo() { return &fCInfo; }

    huffman_index* huffmanIndex() { return &fHuffmanIndex; }

    /**
     *  Build the index to be used for tile based decoding.
     *  Must only be called after a successful call to
     *  initializeInfoAndReadHeader and must not be called more
     *  than once.
     */
    bool buildHuffmanIndex() {
        SkASSERT(fReadHeaderSucceeded);
        SkASSERT(!fHuffmanCreated);
        jpeg_create_huffman_index(&fCInfo, &fHuffmanIndex);
        SkASSERT(1 == fCInfo.scale_num && 1 == fCInfo.scale_denom);
        fHuffmanCreated = jpeg_build_huffman_index(&fCInfo, &fHuffmanIndex);
        return fHuffmanCreated;
    }

    /**
     *  Start tile based decoding. Must only be called after a
     *  successful call to buildHuffmanIndex, and must only be
     *  called once.
     */
    bool startTileDecompress() {
        SkASSERT(fHuffmanCreated);
        SkASSERT(fReadHeaderSucceeded);
        SkASSERT(!fDecompressStarted);
        if (jpeg_start_tile_decompress(&fCInfo)) {
            fDecompressStarted = true;
            return true;
        }
        return false;
    }

private:
    skjpeg_source_mgr  fSrcMgr;
    jpeg_decompress_struct fCInfo;
    huffman_index fHuffmanIndex;
    bool fInfoInitialized;
    bool fHuffmanCreated;
    bool fDecompressStarted;
    SkDEBUGCODE(bool fReadHeaderSucceeded;)
};
#endif

static int64_t GetNowUs() 
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (int64_t)tv.tv_sec * 1000000ll + tv.tv_usec;
}

class SkJPEGImageDecoder : public SkImageDecoder {
public:
#ifdef SK_BUILD_FOR_ANDROID
    SkJPEGImageDecoder() {
        fImageIndex = NULL;
        fImageWidth = 0;
        fImageHeight = 0;
        nTotalTime = 0;

#ifdef HW_JPEG       
        pVideo = NULL;
        pDecBitStream = NULL;
        
        hwDocede = 1;
        char strApkName[1024];
		pid_t pid = getpid();
    	GetCallingApkName(pid, strApkName, 1024);

    	if(!strcmp(strApkName, "com.android.cts.graphics"))
    	{
    		hwDocede = 0;
    	}
#endif

    }

    virtual ~SkJPEGImageDecoder() {
        SkDELETE(fImageIndex);
     
#ifdef HW_JPEG
		logv("++++ decode totaltime: %lld", nTotalTime);

        if(pVideo) DestroyVideoDecoder(pVideo);
        if(pDecBitStream) free(pDecBitStream);
#endif
    }
#endif

    virtual Format getFormat() const {
        return kJPEG_Format;
    }

protected:
#ifdef SK_BUILD_FOR_ANDROID
    virtual bool onBuildTileIndex(SkStreamRewindable *stream, int *width, int *height) SK_OVERRIDE;
    virtual bool onDecodeSubset(SkBitmap* bitmap, const SkIRect& rect) SK_OVERRIDE;
#endif
    virtual Result onDecode(SkStream* stream, SkBitmap* bm, Mode) SK_OVERRIDE;

private:
#ifdef SK_BUILD_FOR_ANDROID
    SkJPEGImageIndex* fImageIndex;
    int fImageWidth;
    int fImageHeight;
#endif

	int64_t nTotalTime;
#ifdef HW_JPEG
	VideoDecoder *pVideo;
	int           hwDocede;
	struct skJpegDecBitStream *pDecBitStream;
#endif

    /**
     *  Determine the appropriate bitmap colortype and out_color_space based on
     *  both the preference of the caller and the jpeg_color_space on the
     *  jpeg_decompress_struct passed in.
     *  Must be called after jpeg_read_header.
     */
    SkColorType getBitmapColorType(jpeg_decompress_struct*);

    typedef SkImageDecoder INHERITED;
};

//////////////////////////////////////////////////////////////////////////

/* Automatically clean up after throwing an exception */
class JPEGAutoClean {
public:
    JPEGAutoClean(): cinfo_ptr(NULL) {}
    ~JPEGAutoClean() {
        if (cinfo_ptr) {
            jpeg_destroy_decompress(cinfo_ptr);
        }
    }
    void set(jpeg_decompress_struct* info) {
        cinfo_ptr = info;
    }
private:
    jpeg_decompress_struct* cinfo_ptr;
};

///////////////////////////////////////////////////////////////////////////////

/*  If we need to better match the request, we might examine the image and
     output dimensions, and determine if the downsampling jpeg provided is
     not sufficient. If so, we can recompute a modified sampleSize value to
     make up the difference.

     To skip this additional scaling, just set sampleSize = 1; below.
 */
static int recompute_sampleSize(int sampleSize,
                                const jpeg_decompress_struct& cinfo) {
    return sampleSize * cinfo.output_width / cinfo.image_width;
}

static bool valid_output_dimensions(const jpeg_decompress_struct& cinfo) {
    /* These are initialized to 0, so if they have non-zero values, we assume
       they are "valid" (i.e. have been computed by libjpeg)
     */
    return 0 != cinfo.output_width && 0 != cinfo.output_height;
}

static bool skip_src_rows(jpeg_decompress_struct* cinfo, void* buffer, int count) {
    for (int i = 0; i < count; i++) {
        JSAMPLE* rowptr = (JSAMPLE*)buffer;
        int row_count = jpeg_read_scanlines(cinfo, &rowptr, 1);
        if (1 != row_count) {
            return false;
        }
    }
    return true;
}

#ifdef SK_BUILD_FOR_ANDROID
static bool skip_src_rows_tile(jpeg_decompress_struct* cinfo,
                               huffman_index *index, void* buffer, int count) {
    for (int i = 0; i < count; i++) {
        JSAMPLE* rowptr = (JSAMPLE*)buffer;
        int row_count = jpeg_read_tile_scanline(cinfo, index, &rowptr);
        if (1 != row_count) {
            return false;
        }
    }
    return true;
}
#endif

///////////////////////////////////////////////////////////////////////////////

// This guy exists just to aid in debugging, as it allows debuggers to just
// set a break-point in one place to see all error exists.
static void print_jpeg_decoder_errors(const jpeg_decompress_struct& cinfo,
                         int width, int height, const char caller[]) {
    if (!(c_suppressJPEGImageDecoderErrors)) {
        char buffer[JMSG_LENGTH_MAX];
        cinfo.err->format_message((const j_common_ptr)&cinfo, buffer);
        SkDebugf("libjpeg error %d <%s> from %s [%d %d]\n",
                 cinfo.err->msg_code, buffer, caller, width, height);
    }
}

static bool return_false(const jpeg_decompress_struct& cinfo,
                         const SkBitmap& bm, const char caller[]) {
    print_jpeg_decoder_errors(cinfo, bm.width(), bm.height(), caller);
    return false;
}

static SkImageDecoder::Result return_failure(const jpeg_decompress_struct& cinfo,
                                             const SkBitmap& bm, const char caller[]) {
    print_jpeg_decoder_errors(cinfo, bm.width(), bm.height(), caller);
    return SkImageDecoder::kFailure;
}

///////////////////////////////////////////////////////////////////////////////

// Convert a scanline of CMYK samples to RGBX in place. Note that this
// method moves the "scanline" pointer in its processing
static void convert_CMYK_to_RGB(uint8_t* scanline, unsigned int width) {
    // At this point we've received CMYK pixels from libjpeg. We
    // perform a crude conversion to RGB (based on the formulae
    // from easyrgb.com):
    //  CMYK -> CMY
    //    C = ( C * (1 - K) + K )      // for each CMY component
    //  CMY -> RGB
    //    R = ( 1 - C ) * 255          // for each RGB component
    // Unfortunately we are seeing inverted CMYK so all the original terms
    // are 1-. This yields:
    //  CMYK -> CMY
    //    C = ( (1-C) * (1 - (1-K) + (1-K) ) -> C = 1 - C*K
    // The conversion from CMY->RGB remains the same
    for (unsigned int x = 0; x < width; ++x, scanline += 4) {
        scanline[0] = SkMulDiv255Round(scanline[0], scanline[3]);
        scanline[1] = SkMulDiv255Round(scanline[1], scanline[3]);
        scanline[2] = SkMulDiv255Round(scanline[2], scanline[3]);
        scanline[3] = 255;
    }
}

/**
 *  Common code for setting the error manager.
 */
static void set_error_mgr(jpeg_decompress_struct* cinfo, skjpeg_error_mgr* errorManager) {
    SkASSERT(cinfo != NULL);
    SkASSERT(errorManager != NULL);
    cinfo->err = jpeg_std_error(errorManager);
    errorManager->error_exit = skjpeg_error_exit;
}

/**
 *  Common code for turning off upsampling and smoothing. Turning these
 *  off helps performance without showing noticable differences in the
 *  resulting bitmap.
 */
static void turn_off_visual_optimizations(jpeg_decompress_struct* cinfo) {
    SkASSERT(cinfo != NULL);
    /* this gives about 30% performance improvement. In theory it may
       reduce the visual quality, in practice I'm not seeing a difference
     */
    cinfo->do_fancy_upsampling = 0;

    /* this gives another few percents */
    cinfo->do_block_smoothing = 0;
}

/**
 * Common code for setting the dct method.
 */
static void set_dct_method(const SkImageDecoder& decoder, jpeg_decompress_struct* cinfo) {
    SkASSERT(cinfo != NULL);
#ifdef DCT_IFAST_SUPPORTED
    if (decoder.getPreferQualityOverSpeed()) {
        cinfo->dct_method = JDCT_ISLOW;
    } else {
        cinfo->dct_method = JDCT_IFAST;
    }
#else
    cinfo->dct_method = JDCT_ISLOW;
#endif
}

SkColorType SkJPEGImageDecoder::getBitmapColorType(jpeg_decompress_struct* cinfo) {
    SkASSERT(cinfo != NULL);

    SrcDepth srcDepth = k32Bit_SrcDepth;
    if (JCS_GRAYSCALE == cinfo->jpeg_color_space) {
        srcDepth = k8BitGray_SrcDepth;
    }

    SkColorType colorType = this->getPrefColorType(srcDepth, /*hasAlpha*/ false);
    switch (colorType) {
        case kAlpha_8_SkColorType:
            // Only respect A8 colortype if the original is grayscale,
            // in which case we will treat the grayscale as alpha
            // values.
            if (cinfo->jpeg_color_space != JCS_GRAYSCALE) {
                colorType = kN32_SkColorType;
            }
            break;
        case kN32_SkColorType:
            // Fall through.
        case kARGB_4444_SkColorType:
            // Fall through.
        case kRGB_565_SkColorType:
            // These are acceptable destination colortypes.
            break;
        default:
            // Force all other colortypes to 8888.
            colorType = kN32_SkColorType;
            break;
    }

    switch (cinfo->jpeg_color_space) {
        case JCS_CMYK:
            // Fall through.
        case JCS_YCCK:
            // libjpeg cannot convert from CMYK or YCCK to RGB - here we set up
            // so libjpeg will give us CMYK samples back and we will later
            // manually convert them to RGB
            cinfo->out_color_space = JCS_CMYK;
            break;
        case JCS_GRAYSCALE:
            if (kAlpha_8_SkColorType == colorType) {
                cinfo->out_color_space = JCS_GRAYSCALE;
                break;
            }
            // The data is JCS_GRAYSCALE, but the caller wants some sort of RGB
            // colortype. Fall through to set to the default.
        default:
            cinfo->out_color_space = JCS_RGB;
            break;
    }
    return colorType;
}

/**
 *  Based on the colortype and dither mode, adjust out_color_space and
 *  dither_mode of cinfo. Only does work in ANDROID_RGB
 */
static void adjust_out_color_space_and_dither(jpeg_decompress_struct* cinfo,
                                              SkColorType colorType,
                                              const SkImageDecoder& decoder) {
    SkASSERT(cinfo != NULL);
#ifdef ANDROID_RGB
    cinfo->dither_mode = JDITHER_NONE;
    if (JCS_CMYK == cinfo->out_color_space) {
        return;
    }
    switch (colorType) {
        case kN32_SkColorType:
            cinfo->out_color_space = JCS_RGBA_8888;
            break;
        case kRGB_565_SkColorType:
            cinfo->out_color_space = JCS_RGB_565;
            if (decoder.getDitherImage()) {
                cinfo->dither_mode = JDITHER_ORDERED;
            }
            break;
        default:
            break;
    }
#endif
}

/**
   Sets all pixels in given bitmap to SK_ColorWHITE for all rows >= y.
   Used when decoding fails partway through reading scanlines to fill
   remaining lines. */
static void fill_below_level(int y, SkBitmap* bitmap) {
    SkIRect rect = SkIRect::MakeLTRB(0, y, bitmap->width(), bitmap->height());
    SkCanvas canvas(*bitmap);
    canvas.clipRect(SkRect::Make(rect));
    canvas.drawColor(SK_ColorWHITE);
}

/**
 *  Get the config and bytes per pixel of the source data. Return
 *  whether the data is supported.
 */
static bool get_src_config(const jpeg_decompress_struct& cinfo,
                           SkScaledBitmapSampler::SrcConfig* sc,
                           int* srcBytesPerPixel) {
    SkASSERT(sc != NULL && srcBytesPerPixel != NULL);
    if (JCS_CMYK == cinfo.out_color_space) {
        // In this case we will manually convert the CMYK values to RGB
        *sc = SkScaledBitmapSampler::kRGBX;
        // The CMYK work-around relies on 4 components per pixel here
        *srcBytesPerPixel = 4;
    } else if (3 == cinfo.out_color_components && JCS_RGB == cinfo.out_color_space) {
        *sc = SkScaledBitmapSampler::kRGB;
        *srcBytesPerPixel = 3;
#ifdef ANDROID_RGB
    } else if (JCS_RGBA_8888 == cinfo.out_color_space) {
        *sc = SkScaledBitmapSampler::kRGBX;
        *srcBytesPerPixel = 4;
    } else if (JCS_RGB_565 == cinfo.out_color_space) {
        *sc = SkScaledBitmapSampler::kRGB_565;
        *srcBytesPerPixel = 2;
#endif
    } else if (1 == cinfo.out_color_components &&
               JCS_GRAYSCALE == cinfo.out_color_space) {
        *sc = SkScaledBitmapSampler::kGray;
        *srcBytesPerPixel = 1;
    } else {
        return false;
    }
    return true;
}

#ifdef HW_JPEG
// ************************************************************************************
// **** adapter for hardware decode read skStream
static int skBitStreamRead(struct DecBitStream* stream, void* buf, int len) {
    struct skJpegDecBitStream* skDecStream = (struct skJpegDecBitStream*)stream;
    /*
    if (skDecStream->fDecoder != NULL && skDecStream->fDecoder->shouldCancelDecode()) {
        return -1;
    }*/
    
    int bytes = skDecStream->fStream->read(buf, len);
    // note that JPEG is happy with less than the full read,
    // as long as the result is non-zero
    if (bytes == 0) {
        return bytes;
    }

    skDecStream->offset += bytes;
    return bytes;
}

static int skBitStreamSize(struct DecBitStream* stream) {
    struct skJpegDecBitStream* skDecStream = (struct skJpegDecBitStream*)stream;
    /*
    if (skDecStream->fDecoder != NULL && skDecStream->fDecoder->shouldCancelDecode()) {
        return -1;
    }*/
    
    if(skDecStream->fStream->hasLength())
	{
		return skDecStream->fStream->getLength();
	}

	return -1;
}

// seek to an absolute position
static int skBitStreamSeek(struct DecBitStream* stream, int offset) {
    struct skJpegDecBitStream* skDecStream = (struct skJpegDecBitStream*)stream;
    /*
    if (skDecStream->fDecoder != NULL && skDecStream->fDecoder->shouldCancelDecode()) {
        return -1;
    }*/

	int size  = skBitStreamSize(stream);
	if(size > 0 && offset > size)
	{
    	return -1;
    }

    //logd("+++ seek start offset : %d", offset);
    if(offset < skDecStream->offset)
    {
	    if(skDecStream->fStream->rewind() < 0)
	    {
	    	return -1;
	    }
	    
	    int bytes = skDecStream->fStream->skip(offset);
	    if(bytes < offset)
	    {
	    	loge("skBitStreamSeek failed");
	    	return -1;
	    }
    }
    else if(offset > skDecStream->offset)
    {
    	int skipByte = offset - skDecStream->offset;
    	int bytes = skDecStream->fStream->skip(skipByte);
	    if(bytes < skipByte)
	    {
	    	loge("skBitStreamSeek failed, skipByte: %d, bytes: %d", skipByte, bytes);
	    	return -1;
	    }
    }

    //logd("++++ seek success offset : %d", offset);
    
    skDecStream->offset = offset;
    return 0;
}

static int skBitStreamTell(struct DecBitStream* stream) {
    struct skJpegDecBitStream* skDecStream = (struct skJpegDecBitStream*)stream;
    /*
    if (skDecStream->fDecoder != NULL && skDecStream->fDecoder->shouldCancelDecode()) {
        return -1;
    }*/
    
    return skDecStream->offset;
}

struct skJpegDecBitStream* skBitStreamCreate(SkStream* stream, SkImageDecoder* decoder)
{
	struct skJpegDecBitStream* jpegStream = (struct skJpegDecBitStream*)malloc(sizeof(struct skJpegDecBitStream));
	if(jpegStream == NULL)
	{
		return NULL;
	}
	memset(jpegStream, 0x00, sizeof(struct skJpegDecBitStream));
	
	jpegStream->fStream = stream;
	jpegStream->fDecoder = decoder;
	jpegStream->bitStream.read = skBitStreamRead;
	jpegStream->bitStream.tell = skBitStreamTell;
	jpegStream->bitStream.seek = skBitStreamSeek;
	jpegStream->bitStream.size = skBitStreamSize;

	return jpegStream;
}

#endif

// **************************************************************************************
#ifdef HW_JPEG
//  **** hardware decode
SkImageDecoder::Result SkJPEGImageDecoder::onDecode(SkStream* stream, SkBitmap* bm, Mode mode) {
#ifdef TIME_DECODE
    SkAutoTime atm("JPEG Decode");
#endif

	//int64_t start, end;
	//start = GetNowUs();
	//logd("***************** onDecode start, streamLength: %d, bm->getSize: %d********************", stream->getLength(), bm->getSize());
    JPEGAutoClean autoClean;
    VideoPicture *videoPicture = NULL;
	int yBufferSize = 0;
	int sampleSize = 0;

    jpeg_decompress_struct  cinfo;
    skjpeg_source_mgr       srcManager(stream, this);

    skjpeg_error_mgr errorManager;
    set_error_mgr(&cinfo, &errorManager);

	// All objects need to be instantiated before this setjmp call so that
	// they will be cleaned up properly if an error occurs.
	if (setjmp(errorManager.fJmpBuf)) {
		return return_failure(cinfo, *bm, "setjmp");
	}
	
	initialize_info(&cinfo, &srcManager);
	autoClean.set(&cinfo);
	
	int status = jpeg_read_header(&cinfo, true);
	if (status != JPEG_HEADER_OK) {
		return return_failure(cinfo, *bm, "read_header");
	}

	sampleSize = this->getSampleSize();
	
	set_dct_method(*this, &cinfo);	
	SkASSERT(1 == cinfo.scale_num);
	cinfo.scale_denom = sampleSize;	
	turn_off_visual_optimizations(&cinfo);
	
	SkColorType colorType = this->getBitmapColorType(&cinfo);
	const SkAlphaType alphaType = kAlpha_8_SkColorType == colorType ?
									  kPremul_SkAlphaType : kOpaque_SkAlphaType;  

	//logd("+++ clorType; %d, mode: %d, sampleSize: %d", colorType, mode, sampleSize);
	adjust_out_color_space_and_dither(&cinfo, colorType, *this);

    // *********************** 1 *****************************************
    // **** first time call onDecode would come here to get the pic width and height
    // ***** it is software to read jpeg header,
    // ******************************************************************
	if (1 == sampleSize && SkImageDecoder::kDecodeBounds_Mode == mode) 
	{	
        // Assume an A8 bitmap is not opaque to avoid the check of each
        // individual pixel. It is very unlikely to be opaque, since
        // an opaque A8 bitmap would not be very interesting.
        // Otherwise, a jpeg image is opaque.

        // in hw dec, we should set colorType to RGBA8888 or ARGB8888, which hw supported
        //colorType = kRGBA_8888_SkColorType;
        //logd("image_width = %d, image_height = %d", cinfo.image_width, cinfo.image_height);
        bool success = bm->setInfo(SkImageInfo::Make(cinfo.image_width, cinfo.image_height,
                                                     colorType, alphaType));
        return success ? kSuccess : kFailure;
    }

	// if stream length > 8M, sbm will overflow, so goto soft dec
    if((stream->getLength() > 12*1024*1024) || (cinfo.image_width*cinfo.image_height<500*500))
    {
    	hwDocede = 0;
    	goto CMYK_decode;
    }

    // ****************************************************
	// ***** check whether it is the CMYK or progressive_mode *****
	// *****             change to soft decode                             *****
	// ****************************************************
	if(cinfo.progressive_mode == 1 || cinfo.num_components >3 )
	{
		logd("+++++++ pregressive(%d), out_color_components(%d), change soft decode", cinfo.progressive_mode, cinfo.out_color_components);
		hwDocede = 0;
		goto CMYK_decode;
	}

	// *** we must rewind after call jpeg_read_header when hw decode
	if(hwDocede && !stream->rewind())
	{
		logd("++++stream rewind failed, go to soft decode");
		hwDocede = 0;
		goto CMYK_decode;
	}

	if(!hwDocede)
	{
		goto CMYK_decode; 
	}
 
    // ***********************************
    // ******** hardware decode ************
    // ***********************************
	if(hwDocede)
	{
		pVideo = CreateVideoDecoder();
		if(!pVideo)
		{
			logd("create video decoder failed\n");
			hwDocede = 0;
			goto CMYK_decode;
		}
	}

	VConfig vConfig;
			
	if(hwDocede)
	{		
		memset(&vConfig, 0x00, sizeof(VConfig));
		vConfig.bDisable3D = 0;
		vConfig.bDispErrorFrame = 0;
		vConfig.bNoBFrames = 0;
		vConfig.bRotationEn = 0;
		if(sampleSize>1)
		{
			vConfig.bScaleDownEn = 1;
			vConfig.nHorizonScaleDownRatio = log(sampleSize) / log(2);
			vConfig.nVerticalScaleDownRatio = vConfig.nHorizonScaleDownRatio;
		}
		else
		{
			vConfig.bScaleDownEn = 0;
			vConfig.nHorizonScaleDownRatio = 0;
			vConfig.nVerticalScaleDownRatio = 0;
		}

		vConfig.nRotateDegree = 0;
		vConfig.nVbvBufferSize = stream->hasLength() ? ((stream->getLength()+1023) & ~1023) : 10*1024*1024;
		logd("====== vConfig.nVbvBufferSize: %d", vConfig.nVbvBufferSize);
		vConfig.eOutputPixelFormat = PIXEL_FORMAT_RGBA; // we do not care colorType, the pixels of bm is malloc here

		VideoStreamInfo videoInfo;
		memset(&videoInfo, 0x00, sizeof(VideoStreamInfo));
		videoInfo.eCodecFormat = VIDEO_CODEC_FORMAT_MJPEG; 

		if ((InitializeVideoDecoder(pVideo, &videoInfo, &vConfig)) != 0) 
		{
			logd("open dev failed, change to soft decode!\n");
			hwDocede = 0;
			goto CMYK_decode;
		}
	}


	if(hwDocede)
	{	
		int length;
		if(stream->hasLength())
		{
			length = stream->getLength();
		}
		else
		{
			length = 2*1024*1024;
		}
		
		char *buf, *ringBuf;
		int buflen, ringBufLen;

		if(RequestVideoStreamBuffer(pVideo, 
									length, 
									(char**)&buf,
									&buflen, 
									(char**)&ringBuf, 
									&ringBufLen, 
									0))
		{
			logd("Request Video Stream Buffer failed\n");
			return return_failure(cinfo, *bm, "RequestVideoStreamBuffer");
		}

		if(buflen + ringBufLen < length)
		{
			logd("#####Error: request buffer failed, buffer is not enough\n");		
			return return_failure(cinfo, *bm, "RequestVideoStreamBuffer");		
		}
		
		// copy stream to video decoder SBM
		// **** read  jpeg header data from srcManager.fBuffer
		if(buflen >= length)
		{
			stream->read(buf, length);
		}
		else
		{
			// read jpeg header fram srcManager
			stream->read(buf, buflen);
			stream->read(ringBuf, length-buflen);
		}

		VideoStreamDataInfo DataInfo;
		memset(&DataInfo, 0, sizeof(DataInfo));
		DataInfo.pData = buf;
		DataInfo.nLength = length;
		DataInfo.bIsFirstPart = 1;
		DataInfo.bIsLastPart = 1;		

		if (SubmitVideoStreamData(pVideo, &DataInfo, 0)) 
		{
			logd("#####Error: Submit Video Stream Data failed!\n"); 	
			return return_failure(cinfo, *bm, "SubmitVideoStreamData");				
		}

		// step : decode stream now
		int endofstream = 0;
		int dropBFrameifdelay = 0;
		int64_t currenttimeus = 0;
		int decodekeyframeonly = 0;


    	JpegSkiaConfig     pJpegSkiaConfig;
    	memset(&pJpegSkiaConfig, 0x00, sizeof(JpegSkiaConfig));

        //default value.
    	pJpegSkiaConfig.mode_selection = 0;
    	pJpegSkiaConfig.filed_alpha = 255;
    	pJpegSkiaConfig.imcu_int_minus1 = 15;
		pJpegSkiaConfig.nScaleDownRatio = vConfig.nHorizonScaleDownRatio;  // these two values must equal

    	DecoderSetSpecialData(pVideo, &pJpegSkiaConfig);

		int ret = DecodeVideoStream(pVideo, endofstream, decodekeyframeonly,
				dropBFrameifdelay, currenttimeus);

		switch (ret) 
		{
			case VDECODE_RESULT_KEYFRAME_DECODED:
			case VDECODE_RESULT_FRAME_DECODED: 
			case VDECODE_RESULT_NO_FRAME_BUFFER:
			{
				videoPicture = RequestPicture(pVideo, 0);
				if(!videoPicture)
				{
					return return_failure(cinfo, *bm, "RequestPicture");			
				}

				cinfo.output_width  = videoPicture->nWidth;
				cinfo.output_height = videoPicture->nHeight;

				// for mode ==1 ,sampleSize != 1
				if (SkImageDecoder::kDecodeBounds_Mode == mode && valid_output_dimensions(cinfo))
				{
					SkScaledBitmapSampler smpl(cinfo.output_width, cinfo.output_height,
											   recompute_sampleSize(sampleSize, cinfo));
					// Assume an A8 bitmap is not opaque to avoid the check of each
					// individual pixel. It is very unlikely to be opaque, since
					// an opaque A8 bitmap would not be very interesting.
					// Otherwise, a jpeg image is opaque.
					bool success = bm->setInfo(SkImageInfo::Make(smpl.scaledWidth(), smpl.scaledHeight(),
																 colorType, alphaType));
					return success ? kSuccess : kFailure;
				}

				if(videoPicture)
				{
					MemAdapterFlushCache((void*) videoPicture->pData0, videoPicture->nWidth* videoPicture->nHeight*4);
				}
				else
				{
					return return_failure(cinfo, *bm, "RequestPictureFailed");
				}

				break;
			}

			case VDECODE_RESULT_OK:
			case VDECODE_RESULT_CONTINUE:
			case VDECODE_RESULT_NO_BITSTREAM:
			case VDECODE_RESULT_RESOLUTION_CHANGE:
			case VDECODE_RESULT_UNSUPPORTED:
			default:
				logd("video decode Error: %d!\n", ret);
				return return_failure(cinfo, *bm, "DecodeVideoStream");
		}

		cinfo.scale_denom = sampleSize;

		turn_off_visual_optimizations(&cinfo);

		// set colorType to RGBA, which hw supported
		colorType = kRGBA_8888_SkColorType;
	    bm->setInfo(SkImageInfo::Make(videoPicture->nWidth, videoPicture->nHeight, colorType, alphaType));
		adjust_out_color_space_and_dither(&cinfo, colorType, *this);
		if (1 == sampleSize && SkImageDecoder::kDecodeBounds_Mode == mode)
		{
			// Assume an A8 bitmap is not opaque to avoid the check of each
	        // individual pixel. It is very unlikely to be opaque, since
	        // an opaque A8 bitmap would not be very interesting.
	        // Otherwise, a jpeg image is opaque.
			bool success = bm->setInfo(SkImageInfo::Make(cinfo.image_width, cinfo.image_height,
			     colorType, alphaType));
	        return success ? kSuccess : kFailure;
		}
		
		if (!this->allocPixelRef(bm, NULL)) 
		{
			logd("allocPixelRel failed");
			return return_failure(cinfo, *bm, "allocPixelRef");
		}

		yBufferSize = videoPicture->nWidth* videoPicture->nHeight*4;
		if((unsigned int)yBufferSize != bm->getSize())
		{
			logd("error: yBufferSize(%d) != bm->getSize():%d", yBufferSize, bm->getSize());
		}

		SkAutoLockPixels alp(*bm);
		char* tmp = (char*)bm->getPixels();
		memset(tmp, 0xFF, bm->getSize());
		if((unsigned int)yBufferSize < bm->getSize())
		{
			memcpy(tmp, videoPicture->pData0, yBufferSize);	
		}
		else
		{
			memcpy(tmp, videoPicture->pData0, bm->getSize());	
		}

		if(ReturnPicture(pVideo, videoPicture))
		{
			logd("return picture failed\n");
			return return_failure(cinfo, *bm, "ReturnPicFailed");
		}
	
		if(0) // save output rgba file	
		{
			FILE* rgbfp = fopen("/mnt/sdcard/ondecode_rgb.dat", "w");
			if(!rgbfp) 
			{
				logd("open file /mnt/sdcard/rgb.dat failed, errno(%d)", errno);
				return kFailure;
			}
			logd("bm->getsize = %d", bm->getSize());
			int i = 0;
			unsigned char *ptr = (unsigned char*)bm->getPixels();

			fwrite(bm->getPixels(), bm->getSize(), 1, rgbfp);
			fclose(rgbfp);
		}
		// *************** hardware decode end ********************

		//end = GetNowUs();
		//logd("onDecode hw time diff: %lld", end-start);

		return kSuccess;
	}
	
	
soft_decode:
	// **********************************************************
	// ******************* software decode *************************
	// **********************************************************

	
	// All objects need to be instantiated before this setjmp call so that
	// they will be cleaned up properly if an error occurs.
	if (setjmp(errorManager.fJmpBuf)) {
		return return_failure(cinfo, *bm, "setjmp");
	}
	
	initialize_info(&cinfo, &srcManager);
	autoClean.set(&cinfo);
	
	status = jpeg_read_header(&cinfo, true);
	if (status != JPEG_HEADER_OK) {
		return return_failure(cinfo, *bm, "read_header");
	}
	
	/*	Try to fulfill the requested sampleSize. Since jpeg can do it (when it
		can) much faster that we, just use their num/denom api to approximate
		the size.
	*/
		
	set_dct_method(*this, &cinfo);
	
	SkASSERT(1 == cinfo.scale_num);
	cinfo.scale_denom = sampleSize;
	
	turn_off_visual_optimizations(&cinfo);
			
	adjust_out_color_space_and_dither(&cinfo, colorType, *this);

	
CMYK_decode:

    /*  image_width and image_height are the original dimensions, available
        after jpeg_read_header(). To see the scaled dimensions, we have to call
        jpeg_start_decompress(), and then read output_width and output_height.
    */
    if (!jpeg_start_decompress(&cinfo)) {
        /*  If we failed here, we may still have enough information to return
            to the caller if they just wanted (subsampled bounds). If sampleSize
            was 1, then we would have already returned. Thus we just check if
            we're in kDecodeBounds_Mode, and that we have valid output sizes.

            One reason to fail here is that we have insufficient stream data
            to complete the setup. However, output dimensions seem to get
            computed very early, which is why this special check can pay off.
         */
        if (SkImageDecoder::kDecodeBounds_Mode == mode && valid_output_dimensions(cinfo)) {
            SkScaledBitmapSampler smpl(cinfo.output_width, cinfo.output_height,
                                       recompute_sampleSize(sampleSize, cinfo));
            // Assume an A8 bitmap is not opaque to avoid the check of each
            // individual pixel. It is very unlikely to be opaque, since
            // an opaque A8 bitmap would not be very interesting.
            // Otherwise, a jpeg image is opaque.
            bool success = bm->setInfo(SkImageInfo::Make(smpl.scaledWidth(), smpl.scaledHeight(),
                                                         colorType, alphaType));
            return success ? kSuccess : kFailure;
        } else {
            return return_failure(cinfo, *bm, "start_decompress");
        }
    }
    sampleSize = recompute_sampleSize(sampleSize, cinfo);

#ifdef SK_SUPPORT_LEGACY_IMAGEDECODER_CHOOSER
    // should we allow the Chooser (if present) to pick a colortype for us???
    if (!this->chooseFromOneChoice(colorType, cinfo.output_width, cinfo.output_height)) {
        return return_failure(cinfo, *bm, "chooseFromOneChoice");
    }
#endif

    SkScaledBitmapSampler sampler(cinfo.output_width, cinfo.output_height, sampleSize);
    // Assume an A8 bitmap is not opaque to avoid the check of each
    // individual pixel. It is very unlikely to be opaque, since
    // an opaque A8 bitmap would not be very interesting.
    // Otherwise, a jpeg image is opaque.
    bm->setInfo(SkImageInfo::Make(sampler.scaledWidth(), sampler.scaledHeight(),
                                  colorType, alphaType));
    if (SkImageDecoder::kDecodeBounds_Mode == mode) {
        return kSuccess;
    }
    if (!this->allocPixelRef(bm, NULL)) {
        return return_failure(cinfo, *bm, "allocPixelRef");
    }

    SkAutoLockPixels alp(*bm);

#ifdef ANDROID_RGB
    /* short-circuit the SkScaledBitmapSampler when possible, as this gives
       a significant performance boost.
    */
    if (sampleSize == 1 &&
        ((kN32_SkColorType == colorType && cinfo.out_color_space == JCS_RGBA_8888) ||
         (kRGB_565_SkColorType == colorType && cinfo.out_color_space == JCS_RGB_565)))
    {
        JSAMPLE* rowptr = (JSAMPLE*)bm->getPixels();
        INT32 const bpr =  bm->rowBytes();

        while (cinfo.output_scanline < cinfo.output_height) {
            int row_count = jpeg_read_scanlines(&cinfo, &rowptr, 1);
            if (0 == row_count) {
                // if row_count == 0, then we didn't get a scanline,
                // so return early.  We will return a partial image.
                fill_below_level(cinfo.output_scanline, bm);
                cinfo.output_scanline = cinfo.output_height;
                jpeg_finish_decompress(&cinfo);
                return kPartialSuccess;
            }
            if (this->shouldCancelDecode()) {
                return return_failure(cinfo, *bm, "shouldCancelDecode");
            }
            rowptr += bpr;
        }
        jpeg_finish_decompress(&cinfo);
        return kSuccess;
    }
#endif

    // check for supported formats
    SkScaledBitmapSampler::SrcConfig sc;
    int srcBytesPerPixel;

    if (!get_src_config(cinfo, &sc, &srcBytesPerPixel)) {
        return return_failure(cinfo, *bm, "jpeg colorspace");
    }

    if (!sampler.begin(bm, sc, *this)) {
        return return_failure(cinfo, *bm, "sampler.begin");
    }

    SkAutoMalloc srcStorage(cinfo.output_width * srcBytesPerPixel);
    uint8_t* srcRow = (uint8_t*)srcStorage.get();

    //  Possibly skip initial rows [sampler.srcY0]
    if (!skip_src_rows(&cinfo, srcRow, sampler.srcY0())) {
        return return_failure(cinfo, *bm, "skip rows");
    }

    // now loop through scanlines until y == bm->height() - 1
    for (int y = 0;; y++) {
        JSAMPLE* rowptr = (JSAMPLE*)srcRow;
        int row_count = jpeg_read_scanlines(&cinfo, &rowptr, 1);
        if (0 == row_count) {
            // if row_count == 0, then we didn't get a scanline,
            // so return early.  We will return a partial image.
            fill_below_level(y, bm);
            cinfo.output_scanline = cinfo.output_height;
            jpeg_finish_decompress(&cinfo);
            return kSuccess;
        }
        if (this->shouldCancelDecode()) {
            return return_failure(cinfo, *bm, "shouldCancelDecode");
        }

        if (JCS_CMYK == cinfo.out_color_space) {
            convert_CMYK_to_RGB(srcRow, cinfo.output_width);
        }

        sampler.next(srcRow);
        if (bm->height() - 1 == y) {
            // we're done
            break;
        }

        if (!skip_src_rows(&cinfo, srcRow, sampler.srcDY() - 1)) {
            return return_failure(cinfo, *bm, "skip rows");
        }
    }

    // we formally skip the rest, so we don't get a complaint from libjpeg
    if (!skip_src_rows(&cinfo, srcRow,
                       cinfo.output_height - cinfo.output_scanline)) {
        return return_failure(cinfo, *bm, "skip rows");
    }
    jpeg_finish_decompress(&cinfo);

    return kSuccess;
}

#ifdef SK_BUILD_FOR_ANDROID
bool SkJPEGImageDecoder::onBuildTileIndex(SkStreamRewindable* stream, int *width, int *height) {

    SkAutoTDelete<SkJPEGImageIndex> imageIndex(SkNEW_ARGS(SkJPEGImageIndex, (stream, this)));

	//logd("***************** onBuildTileIndex %p *******************************", this);

	// ****************************************************
	// *********** get the pic width and height   ****************
	// **** skStream must seekable, or CMYK cannot be decoded ****
	// ****************************************************
	//int64_t start, end;
	//start = GetNowUs();
        
	jpeg_decompress_struct* cinfo = imageIndex->cinfo();

    skjpeg_error_mgr sk_err;
    set_error_mgr(cinfo, &sk_err);

	pDecBitStream = skBitStreamCreate(stream, this);
    // if stream length > 6M, sbm will overflow, so goto soft dec
    if(stream->getLength() > 22*1024*1024)
    {
    	hwDocede = 0;
    }

    if(!hwDocede)
    {
    	goto soft_decode;
    }

	if(hwDocede)
	{
		// All objects need to be instantiated before this setjmp call so that
		// they will be cleaned up properly if an error occurs.
		if (setjmp(sk_err.fJmpBuf)) {
		    return false;
		}

		// create the cinfo used to create/build the huffmanIndex
		if (!imageIndex->initializeInfoAndReadHeader()) {
		    return false;
		}

	    if(cinfo->progressive_mode == 1 || cinfo->num_components >3)
		{
			logd("+++++++ pregressive(%d), out_color_components(%d), change soft decode", cinfo->progressive_mode, cinfo->num_components);
			hwDocede = 0;
			goto CMYK_decode;
		}
	}

	// *** we must rewind after call jpeg_read_header when hw decode
	if(hwDocede && !stream->rewind())
	{
		logd("++++stream rewind failed, error");
		hwDocede = 0;
		goto CMYK_decode;
	}

    // *****************************************************
    // ********              hardware decode            ****************
    // **********             build index              ******************
    // *****************************************************
	if(hwDocede)
	{	
		pVideo = CreateVideoDecoder();
		if(!pVideo)
		{
			logd("create video decoder failed\n");
			hwDocede = 0;
		}
	}

	VConfig vConfig;
			
	if(hwDocede)
	{		
		memset(&vConfig, 0x00, sizeof(VConfig));
		vConfig.bDisable3D = 0;
		vConfig.bDispErrorFrame = 0;
		vConfig.bNoBFrames = 0;
		vConfig.bRotationEn = 0;
		vConfig.nRotateDegree = 0;
		vConfig.eOutputPixelFormat = PIXEL_FORMAT_RGBA;
		vConfig.nVbvBufferSize = 512;   //stream->hasLength() ? stream->getLength(): 10*1024*1024;
		vConfig.bVirMallocSbm = 1;  // malloc sbm, or memory leak

		VideoStreamInfo videoInfo;
		memset(&videoInfo, 0x00, sizeof(VideoStreamInfo));
		videoInfo.eCodecFormat = VIDEO_CODEC_FORMAT_JPEG;

		if ((InitializeVideoDecoder(pVideo, &videoInfo, &vConfig)) != 0) 
		{
			logd("open dev failed, change to soft decode!\n");
			hwDocede = 0;
		}
	}

	if(hwDocede)
	{
		/*
		int length;
		if(stream->hasLength())
		{
			length = stream->getLength();
		}
		else
		{
			length = 2*1024*1024;
		}
		
		char *buf, *ringBuf;
		int buflen, ringBufLen;

		if(RequestVideoStreamBuffer(pVideo, 
									length, 
									(char**)&buf,
									&buflen, 
									(char**)&ringBuf, 
									&ringBufLen, 
									0))
		{
			logd("Request Video Stream Buffer failed\n");
			return false;
		}

		if(buflen + ringBufLen < length)
		{
			logd("#####Error: request buffer failed, buffer is not enough\n");		
			return false;		
		}

		// copy stream to video decoder SBM
		// **** read  jpeg header data from srcManager.fBuffer
		if(buflen >= length)
		{
			stream->read(buf, length);
		}
		else
		{
			// read jpeg header fram srcManager
			stream->read(buf, buflen);
			stream->read(ringBuf, length-buflen);
		}
		

		VideoStreamDataInfo DataInfo;
		memset(&DataInfo, 0, sizeof(DataInfo));
		DataInfo.pData = buf;
		DataInfo.nLength = length;
		DataInfo.bIsFirstPart = 1;
		DataInfo.bIsLastPart = 1;		

		if (SubmitVideoStreamData(pVideo, &DataInfo, 0)) 
		{
			logd("#####Error: Submit Video Stream Data failed!\n"); 	
			return false;				
		}
		*/

		// step : decode stream now
		int endofstream = 0;
		int dropBFrameifdelay = 0;
		int64_t currenttimeus = 0;
		int decodekeyframeonly = 0;

    	JpegSkiaConfig     pJpegSkiaConfig;
    	memset(&pJpegSkiaConfig, 0x00, sizeof(JpegSkiaConfig));

        //default value.
    	pJpegSkiaConfig.mode_selection = 1;
    	pJpegSkiaConfig.filed_alpha = 255;
    	pJpegSkiaConfig.imcu_int_minus1 = 15;
    	pJpegSkiaConfig.region_top = 0;
    	pJpegSkiaConfig.region_bot = 0;
    	pJpegSkiaConfig.region_left = 0;
    	pJpegSkiaConfig.region_right = 0;
    	pJpegSkiaConfig.bitStream = &pDecBitStream->bitStream;

    	DecoderSetSpecialData(pVideo, &pJpegSkiaConfig);

		int ret = DecodeVideoStream(pVideo, endofstream, decodekeyframeonly,
				dropBFrameifdelay, currenttimeus);

		switch (ret) 
		{
			case VDECODE_RESULT_KEYFRAME_DECODED:			
			case VDECODE_RESULT_FRAME_DECODED: 
			case VDECODE_RESULT_NO_FRAME_BUFFER:
			{
				break;
			}

			case VDECODE_RESULT_CONTINUE:
			case VDECODE_RESULT_NO_BITSTREAM:
			case VDECODE_RESULT_RESOLUTION_CHANGE:
			case VDECODE_RESULT_UNSUPPORTED:
			case VDECODE_RESULT_OK:
			default:
				logd("video decode Error: %d!\n", ret);
				hwDocede = 0;
				stream->rewind();
				goto soft_decode;
				break;
		}
		
		SkASSERT(1 == cinfo->scale_num);
	    fImageWidth = cinfo->image_width;   // maybe not right, it is output_width
	    fImageHeight = cinfo->image_height;

	    if (width) {
	        *width = fImageWidth;
	    }
	    if (height) {
	        *height = fImageHeight;
	    }

		SkDELETE(fImageIndex);
		fImageIndex = imageIndex.detach();

		//end = GetNowUs();
		//nTotalTime += (end-start);
		//logd("hw time buildIndex: %lld", end -start);
	    return true;
	}
	
	// *************** end of hw decode **************************

soft_decode:
	//***********************************************
	//************ soft decode ************************
	//**************************************************

    // All objects need to be instantiated before this setjmp call so that
    // they will be cleaned up properly if an error occurs.
    if (setjmp(sk_err.fJmpBuf)) {
        return false;
    }

    // create the cinfo used to create/build the huffmanIndex
    if (!imageIndex->initializeInfoAndReadHeader()) {
        return false;
    }

CMYK_decode:
    if (!imageIndex->buildHuffmanIndex()) {
        return false;
    }

    // destroy the cinfo used to create/build the huffman index
    imageIndex->destroyInfo();

    // Init decoder to image decode mode
    if (!imageIndex->initializeInfoAndReadHeader()) {
        return false;
    }


    // FIXME: This sets cinfo->out_color_space, which we may change later
    // based on the config in onDecodeSubset. This should be fine, since
    // jpeg_init_read_tile_scanline will check out_color_space again after
    // that change (when it calls jinit_color_deconverter).
    (void) this->getBitmapColorType(cinfo);

    turn_off_visual_optimizations(cinfo);

    // instead of jpeg_start_decompress() we start a tiled decompress
    if (!imageIndex->startTileDecompress()) {
        return false;
    }

    SkASSERT(1 == cinfo->scale_num);
    fImageWidth = cinfo->output_width;
    fImageHeight = cinfo->output_height;

    if (width) {
        *width = fImageWidth;
    }
    if (height) {
        *height = fImageHeight;
    }

	//logd("++++++++ softDecode Image width:%d, height:%d", fImageWidth, fImageHeight);
    SkDELETE(fImageIndex);
    fImageIndex = imageIndex.detach();

    return true;
}

// ****************************************************************
// ****** SkIRect& region: it is the region of original pic *******************
// ****** ************************************** *******************
bool SkJPEGImageDecoder::onDecodeSubset(SkBitmap* bm, const SkIRect& region) {
    if (!hwDocede && (NULL == fImageIndex)) {
        return false;
    }

    //int64_t start, end;
    //start = GetNowUs();
	//logd("***************** decodeSubset, region(%d, %d, %d, %d), bm->getSize:%d**************", region.fLeft, region.fRight, region.fBottom, region.fTop, bm->getSize());

    VideoPicture *videoPicture = NULL;
    jpeg_decompress_struct* cinfo = fImageIndex->cinfo();

    SkIRect rect = SkIRect::MakeWH(fImageWidth, fImageHeight);
    if (!rect.intersect(region)) {
        // If the requested region is entirely outside the image return false
        return false;
    }

    skjpeg_error_mgr errorManager;
    set_error_mgr(cinfo, &errorManager);

    if (setjmp(errorManager.fJmpBuf)) {
        return false;
    }

    int sampleSize = this->getSampleSize();
    int requestedSampleSize = 1 << (int)(log(sampleSize) / log(2));
    cinfo->scale_denom = requestedSampleSize;

    set_dct_method(*this, cinfo);

    SkColorType colorType = this->getBitmapColorType(cinfo);
    adjust_out_color_space_and_dither(cinfo, colorType, *this);

	int inputIndexSize = 512*1024;	// donot change this value, it is equal with IN_INDEX_TABLE_BUFFER in jpeg_dec_lib_plus.h
	int vbvBufferSize = 6*1024*1024;  //
	if((region.right() - region.left()) > 3000 && (region.bottom() - region.top())> 3000)
	{
		vbvBufferSize = 10*1024*1024;
	}
	unsigned long* pictureData = NULL;
	unsigned char* inputIndexBuffer = NULL;
	unsigned char* vbvBuffer = NULL;

	int entryMode = 0; // it is the same as swapOnly in soft dec
	SkBitmap bitmap;
	int wid = (region.right() - region.left()) / requestedSampleSize;
	int hei = (region.bottom() - region.top()) / requestedSampleSize;
	unsigned int pictureDataLen = wid * hei *4; //bm->getSize(); //fImageWidth* fImageHeight; //

	// we should decode in entry mode in this case
	if(hwDocede && (
	    		((rect == region) && (bm->isNull())) 
	    		|| (colorType!=kRGBA_8888_SkColorType) 
				|| (bm->getSize() != pictureDataLen)
				))
	{
		entryMode = 1;
		colorType = kRGBA_8888_SkColorType;
		bitmap.setInfo(SkImageInfo::Make(rect.width()/requestedSampleSize, rect.height()/requestedSampleSize, colorType,
										 kAlpha_8_SkColorType == colorType ?
											 kPremul_SkAlphaType : kOpaque_SkAlphaType));
		if (!this->allocPixelRef(&bitmap, NULL)) 
		{
        	if(pictureData)      IMPoolPfree(pictureData);
	    	if(inputIndexBuffer) IMPoolPfree(inputIndexBuffer);
	    	if(vbvBuffer)        IMPoolPfree(vbvBuffer);
	    	logd("return false");
            return return_false(*cinfo, bitmap, "allocPixelRef");
        }
	}

	if(hwDocede)
	{
		// step : decode stream now
		int endofstream = 0;
		int dropBFrameifdelay = 0;
		int64_t currenttimeus = 0;
		int decodekeyframeonly = 0;

		JpegSkiaConfig     pJpegSkiaConfig;
		memset(&pJpegSkiaConfig, 0x00, sizeof(JpegSkiaConfig));

		pictureData = (unsigned long*)IMPoolPalloc(pictureDataLen);		
		if(!pictureData)
		{
			loge("palloc failed, pictureDataLen: %d", pictureDataLen);
			if(pictureData)      IMPoolPfree(pictureData);
	    	if(inputIndexBuffer) IMPoolPfree(inputIndexBuffer);
	    	if(vbvBuffer)        IMPoolPfree(vbvBuffer);
			return return_failure(*cinfo, *bm, "IMPoolPalloc");
		}
		// we must memset in pic boundray, or twikling in boundary
		if((region.bottom()>fImageHeight) || (region.right()>fImageWidth))
		{
			memset(pictureData, 0x00, pictureDataLen);
			IMPoolFlushCache(pictureData, pictureDataLen);
		}

		inputIndexBuffer = (unsigned char*)IMPoolPalloc(inputIndexSize);
		if(!inputIndexBuffer)
		{
			loge("palloc failed");
			if(pictureData)      IMPoolPfree(pictureData);
	    	if(inputIndexBuffer) IMPoolPfree(inputIndexBuffer);
	    	if(vbvBuffer)        IMPoolPfree(vbvBuffer);
			return return_failure(*cinfo, *bm, "IMPoolPalloc");
		}

		vbvBuffer = (unsigned char*)IMPoolPalloc(vbvBufferSize);
		if(!vbvBuffer)
		{
			loge("palloc failed");
			if(pictureData)      IMPoolPfree(pictureData);
	    	if(inputIndexBuffer) IMPoolPfree(inputIndexBuffer);
	    	if(vbvBuffer)        IMPoolPfree(vbvBuffer);
			return return_failure(*cinfo, *bm, "IMPoolPalloc");
		}
		//logd("vbvBuffer: 0x%p, inputIndexBuffer: 0x%p, pictureData: 0x%p", vbvBuffer, inputIndexBuffer, pictureData);

	    //default value.
		pJpegSkiaConfig.mode_selection      = 2;
		pJpegSkiaConfig.filed_alpha         = 255;
		pJpegSkiaConfig.imcu_int_minus1     = 15;
		pJpegSkiaConfig.region_top          = region.fTop;
		pJpegSkiaConfig.region_left         = region.fLeft;		
		pJpegSkiaConfig.region_bot          = region.fBottom;
		pJpegSkiaConfig.region_right        = region.fRight;
		
		pJpegSkiaConfig.nScaleDownRatio     = log(requestedSampleSize) / log(2);
		pJpegSkiaConfig.pFrameBuffer        = pictureData;	
		pJpegSkiaConfig.pInputIndexBuffer   = inputIndexBuffer;
		pJpegSkiaConfig.nInputIndexSize     = inputIndexSize;
		pJpegSkiaConfig.pTileVbvBuffer      = vbvBuffer;
		pJpegSkiaConfig.nTileVbvVBufferSize = vbvBufferSize;

		DecoderSetSpecialData(pVideo, &pJpegSkiaConfig);

		int ret = DecodeVideoStream(pVideo, endofstream, decodekeyframeonly,
				dropBFrameifdelay, currenttimeus);
		switch (ret) 
		{
			case VDECODE_RESULT_KEYFRAME_DECODED:
			case VDECODE_RESULT_FRAME_DECODED: 
			case VDECODE_RESULT_NO_FRAME_BUFFER:
			{
				IMPoolFlushCache((void*) pictureData, pictureDataLen);
				break;
			}

			case VDECODE_RESULT_OK:
			case VDECODE_RESULT_CONTINUE:
			case VDECODE_RESULT_NO_BITSTREAM:
			case VDECODE_RESULT_RESOLUTION_CHANGE:
			case VDECODE_RESULT_UNSUPPORTED:
			default:
				logd("video decode Error: %d!\n", ret);
				if(pictureData)      IMPoolPfree(pictureData);
	    		if(inputIndexBuffer) IMPoolPfree(inputIndexBuffer);
	    		if(vbvBuffer)        IMPoolPfree(vbvBuffer);
				return return_failure(*cinfo, *bm, "DecodeVideoStream");
		}

		if(entryMode)
		{
			SkAutoLockPixels alp(bitmap);
			char* tmp = (char*)bitmap.getPixels();
			memset(tmp, 0x00, bitmap.getSize());
			if(pictureDataLen > bm->getSize()){	
				loge("maybe error");
			}
			if(pictureDataLen < bitmap.getSize()) {
				memcpy(tmp, pictureData, pictureDataLen);
			} else {
				memcpy(tmp, pictureData, bitmap.getSize());
			}
		    
	        bm->swap(bitmap); 

	        if(0) // save stream
			{ 
				char location[1024];
				sprintf(location, "/mnt/sdcard/entry_tileDecode_%d_%d_%d_%d.es", region.fLeft, region.fRight, region.fTop, region.fBottom);
			    FILE *rgbfp = fopen(location,"wb");
				if(!rgbfp) 
				{
					logd("open file /mnt/sdcard/rgb.dat failed, errno(%d)", errno);
					return kFailure;
				}
				SkAutoLockPixels alp(*bm);
				logd("bm->getsize = %d, bm->getPixels: %p, pictureDataLen: %d", bm->getSize(), bm->getPixels(), pictureDataLen);
				
				fwrite(bm->getPixels(), bm->getSize(), 1, rgbfp);
				//fwrite(pictureData, pictureDataLen, 1, rgbfp);

			    fclose(rgbfp);
		    }

	        if(pictureData)      IMPoolPfree(pictureData);
		    if(inputIndexBuffer) IMPoolPfree(inputIndexBuffer);
		    if(vbvBuffer)        IMPoolPfree(vbvBuffer);

		    //end = GetNowUs();
			//nTotalTime += (end-start);
			//logd("+++hw subDecode time:%lld, nTotalTime: %lld", end -start, nTotalTime);
	        return true;
		}
		
		SkAutoLockPixels alp(*bm);

		char* tmp = (char*)bm->getPixels();
		memset(tmp, 0x00, bm->getSize());
		if(pictureDataLen > bm->getSize()){	
			loge("maybe error");
		}
		if(pictureDataLen < bm->getSize()) {
			memcpy(tmp, pictureData, pictureDataLen);
		} else {
			memcpy(tmp, pictureData, bm->getSize());
		}
			
		if(0) // save stream
		{
			char location[1024];
			sprintf(location, "/mnt/sdcard/tileDecode_%d_%d_%d_%d.es", region.fLeft, region.fRight, region.fTop, region.fBottom);
		    FILE *rgbfp = fopen(location,"wb");
			if(!rgbfp) 
			{
				logd("open file /mnt/sdcard/rgb.dat failed, errno(%d)", errno);
				if(pictureData)      IMPoolPfree(pictureData);
	    		if(inputIndexBuffer) IMPoolPfree(inputIndexBuffer);
	    		if(vbvBuffer)        IMPoolPfree(vbvBuffer);
				return kFailure;
			}
			//logd("bm->getsize = %d, pictureDataLen: %d", bm->getSize(), pictureDataLen);
			//fwrite(bm->getPixels(), bm->getSize(), 1, rgbfp);
			fwrite(pictureData, pictureDataLen, 1, rgbfp);

		    fclose(rgbfp);
	    }

	    IMPoolPfree(pictureData);
	    IMPoolPfree(inputIndexBuffer);
	    IMPoolPfree(vbvBuffer);

		//end = GetNowUs();
		//nTotalTime += (end-start);
		//logd("+++hw subDecode time:%lld, nTotalTime: %lld", end -start, nTotalTime);
		

		return true;
	}

soft_decode:

    int startX = rect.fLeft;
    int startY = rect.fTop;
    int width = rect.width();
    int height = rect.height();

    jpeg_init_read_tile_scanline(cinfo, fImageIndex->huffmanIndex(),
                                 &startX, &startY, &width, &height);
    int skiaSampleSize = recompute_sampleSize(requestedSampleSize, *cinfo);
    int actualSampleSize = skiaSampleSize * (DCTSIZE / cinfo->min_DCT_scaled_size);

	//logd("--- width:%d, height:%d, skiaSampleSize:%d", width, height, skiaSampleSize);
    SkScaledBitmapSampler sampler(width, height, skiaSampleSize);

    // Assume an A8 bitmap is not opaque to avoid the check of each
    // individual pixel. It is very unlikely to be opaque, since
    // an opaque A8 bitmap would not be very interesting.
    // Otherwise, a jpeg image is opaque.
    bitmap.setInfo(SkImageInfo::Make(sampler.scaledWidth(), sampler.scaledHeight(), colorType,
                                     kAlpha_8_SkColorType == colorType ?
                                         kPremul_SkAlphaType : kOpaque_SkAlphaType));

    // Check ahead of time if the swap(dest, src) is possible or not.
    // If yes, then we will stick to AllocPixelRef since it's cheaper with the
    // swap happening. If no, then we will use alloc to allocate pixels to
    // prevent garbage collection.
    int w = rect.width() / actualSampleSize;
    int h = rect.height() / actualSampleSize;
    bool swapOnly = (rect == region) && bm->isNull() &&
                    (w == bitmap.width()) && (h == bitmap.height()) &&
                    ((startX - rect.x()) / actualSampleSize == 0) &&
                    ((startY - rect.y()) / actualSampleSize == 0);
    if (swapOnly) {
        if (!this->allocPixelRef(&bitmap, NULL)) {
	    	logd("return false");
            return return_false(*cinfo, bitmap, "allocPixelRef");
        }
    } else {
        if (!bitmap.allocPixels()) {
	    	logd("return false  222");
            return return_false(*cinfo, bitmap, "allocPixels");
        }
    }

    SkAutoLockPixels alp(bitmap);

#ifdef ANDROID_RGB
    /* short-circuit the SkScaledBitmapSampler when possible, as this gives
       a significant performance boost.
    */
    if (skiaSampleSize == 1 &&
        ((kN32_SkColorType == colorType && cinfo->out_color_space == JCS_RGBA_8888) ||
         (kRGB_565_SkColorType == colorType && cinfo->out_color_space == JCS_RGB_565)))
    {
        JSAMPLE* rowptr = (JSAMPLE*)bitmap.getPixels();
        INT32 const bpr = bitmap.rowBytes();
        int rowTotalCount = 0;

        while (rowTotalCount < height) {
            int rowCount = jpeg_read_tile_scanline(cinfo,
                                                   fImageIndex->huffmanIndex(),
                                                   &rowptr);
            // if rowCount == 0, then we didn't get a scanline, so abort.
            // onDecodeSubset() relies on onBuildTileIndex(), which
            // needs a complete image to succeed.
            if (0 == rowCount) {
                return return_false(*cinfo, bitmap, "read_scanlines");
            }
            if (this->shouldCancelDecode()) {
                return return_false(*cinfo, bitmap, "shouldCancelDecode");
            }
            rowTotalCount += rowCount;
            rowptr += bpr;
        }

        if (swapOnly) {
            bm->swap(bitmap);
        } else {
            cropBitmap(bm, &bitmap, actualSampleSize, region.x(), region.y(),
                       region.width(), region.height(), startX, startY);
        }

        return true;
    }
#endif

    // check for supported formats
    SkScaledBitmapSampler::SrcConfig sc;
    int srcBytesPerPixel;

    if (!get_src_config(*cinfo, &sc, &srcBytesPerPixel)) {
        return return_false(*cinfo, *bm, "jpeg colorspace");
    }

    if (!sampler.begin(&bitmap, sc, *this)) {
        return return_false(*cinfo, bitmap, "sampler.begin");
    }

    SkAutoMalloc  srcStorage(width * srcBytesPerPixel);
    uint8_t* srcRow = (uint8_t*)srcStorage.get();

    //  Possibly skip initial rows [sampler.srcY0]
    if (!skip_src_rows_tile(cinfo, fImageIndex->huffmanIndex(), srcRow, sampler.srcY0())) {
        return return_false(*cinfo, bitmap, "skip rows");
    }

    // now loop through scanlines until y == bitmap->height() - 1
    for (int y = 0;; y++) {
        JSAMPLE* rowptr = (JSAMPLE*)srcRow;
        int row_count = jpeg_read_tile_scanline(cinfo, fImageIndex->huffmanIndex(), &rowptr);
        // if row_count == 0, then we didn't get a scanline, so abort.
        // onDecodeSubset() relies on onBuildTileIndex(), which
        // needs a complete image to succeed.
        if (0 == row_count) {
            return return_false(*cinfo, bitmap, "read_scanlines");
        }
        if (this->shouldCancelDecode()) {
            return return_false(*cinfo, bitmap, "shouldCancelDecode");
        }

        if (JCS_CMYK == cinfo->out_color_space) {
            convert_CMYK_to_RGB(srcRow, width);
        }

        sampler.next(srcRow);
        if (bitmap.height() - 1 == y) {
            // we're done
            break;
        }

        if (!skip_src_rows_tile(cinfo, fImageIndex->huffmanIndex(), srcRow,
                                sampler.srcDY() - 1)) {
            return return_false(*cinfo, bitmap, "skip rows");
        }
    }
    if (swapOnly) {
        bm->swap(bitmap);
    } else {
        cropBitmap(bm, &bitmap, actualSampleSize, region.x(), region.y(),
                   region.width(), region.height(), startX, startY);
    }
    return true;
}
#endif

#else
// soft decode
SkImageDecoder::Result SkJPEGImageDecoder::onDecode(SkStream* stream, SkBitmap* bm, Mode mode) {
#ifdef TIME_DECODE
    SkAutoTime atm("JPEG Decode");
#endif

	//int64_t start, end;
	//start = GetNowUs();
	
    JPEGAutoClean autoClean;

    jpeg_decompress_struct  cinfo;
    skjpeg_source_mgr       srcManager(stream, this);

    skjpeg_error_mgr errorManager;
    set_error_mgr(&cinfo, &errorManager);

    // All objects need to be instantiated before this setjmp call so that
    // they will be cleaned up properly if an error occurs.
    if (setjmp(errorManager.fJmpBuf)) {
        return return_failure(cinfo, *bm, "setjmp");
    }

    initialize_info(&cinfo, &srcManager);
    autoClean.set(&cinfo);

    int status = jpeg_read_header(&cinfo, true);
    if (status != JPEG_HEADER_OK) {
        return return_failure(cinfo, *bm, "read_header");
    }

    /*  Try to fulfill the requested sampleSize. Since jpeg can do it (when it
        can) much faster that we, just use their num/denom api to approximate
        the size.
    */
    int sampleSize = this->getSampleSize();

    set_dct_method(*this, &cinfo);

    SkASSERT(1 == cinfo.scale_num);
    cinfo.scale_denom = sampleSize;

    turn_off_visual_optimizations(&cinfo);

    const SkColorType colorType = this->getBitmapColorType(&cinfo);
    const SkAlphaType alphaType = kAlpha_8_SkColorType == colorType ?
                                      kPremul_SkAlphaType : kOpaque_SkAlphaType;

    adjust_out_color_space_and_dither(&cinfo, colorType, *this);

    if (1 == sampleSize && SkImageDecoder::kDecodeBounds_Mode == mode) {
        // Assume an A8 bitmap is not opaque to avoid the check of each
        // individual pixel. It is very unlikely to be opaque, since
        // an opaque A8 bitmap would not be very interesting.
        // Otherwise, a jpeg image is opaque.
        bool success = bm->setInfo(SkImageInfo::Make(cinfo.image_width, cinfo.image_height,
                                                     colorType, alphaType));
        return success ? kSuccess : kFailure;
    }

    /*  image_width and image_height are the original dimensions, available
        after jpeg_read_header(). To see the scaled dimensions, we have to call
        jpeg_start_decompress(), and then read output_width and output_height.
    */
    if (!jpeg_start_decompress(&cinfo)) {
        /*  If we failed here, we may still have enough information to return
            to the caller if they just wanted (subsampled bounds). If sampleSize
            was 1, then we would have already returned. Thus we just check if
            we're in kDecodeBounds_Mode, and that we have valid output sizes.

            One reason to fail here is that we have insufficient stream data
            to complete the setup. However, output dimensions seem to get
            computed very early, which is why this special check can pay off.
         */
        if (SkImageDecoder::kDecodeBounds_Mode == mode && valid_output_dimensions(cinfo)) {
            SkScaledBitmapSampler smpl(cinfo.output_width, cinfo.output_height,
                                       recompute_sampleSize(sampleSize, cinfo));
            // Assume an A8 bitmap is not opaque to avoid the check of each
            // individual pixel. It is very unlikely to be opaque, since
            // an opaque A8 bitmap would not be very interesting.
            // Otherwise, a jpeg image is opaque.
            bool success = bm->setInfo(SkImageInfo::Make(smpl.scaledWidth(), smpl.scaledHeight(),
                                                         colorType, alphaType));
            return success ? kSuccess : kFailure;
        } else {
            return return_failure(cinfo, *bm, "start_decompress");
        }
    }
    sampleSize = recompute_sampleSize(sampleSize, cinfo);

#ifdef SK_SUPPORT_LEGACY_IMAGEDECODER_CHOOSER
    // should we allow the Chooser (if present) to pick a colortype for us???
    if (!this->chooseFromOneChoice(colorType, cinfo.output_width, cinfo.output_height)) {
        return return_failure(cinfo, *bm, "chooseFromOneChoice");
    }
#endif

    SkScaledBitmapSampler sampler(cinfo.output_width, cinfo.output_height, sampleSize);
    // Assume an A8 bitmap is not opaque to avoid the check of each
    // individual pixel. It is very unlikely to be opaque, since
    // an opaque A8 bitmap would not be very interesting.
    // Otherwise, a jpeg image is opaque.
    bm->setInfo(SkImageInfo::Make(sampler.scaledWidth(), sampler.scaledHeight(),
                                  colorType, alphaType));
    if (SkImageDecoder::kDecodeBounds_Mode == mode) {
        return kSuccess;
    }
    if (!this->allocPixelRef(bm, NULL)) {
        return return_failure(cinfo, *bm, "allocPixelRef");
    }

    SkAutoLockPixels alp(*bm);

#ifdef ANDROID_RGB
    /* short-circuit the SkScaledBitmapSampler when possible, as this gives
       a significant performance boost.
    */
    if (sampleSize == 1 &&
        ((kN32_SkColorType == colorType && cinfo.out_color_space == JCS_RGBA_8888) ||
         (kRGB_565_SkColorType == colorType && cinfo.out_color_space == JCS_RGB_565)))
    {
        JSAMPLE* rowptr = (JSAMPLE*)bm->getPixels();
        INT32 const bpr =  bm->rowBytes();

        while (cinfo.output_scanline < cinfo.output_height) {
            int row_count = jpeg_read_scanlines(&cinfo, &rowptr, 1);
            if (0 == row_count) {
                // if row_count == 0, then we didn't get a scanline,
                // so return early.  We will return a partial image.
                fill_below_level(cinfo.output_scanline, bm);
                cinfo.output_scanline = cinfo.output_height;
                jpeg_finish_decompress(&cinfo);
                return kPartialSuccess;
            }
            if (this->shouldCancelDecode()) {
                return return_failure(cinfo, *bm, "shouldCancelDecode");
            }
            rowptr += bpr;
        }
        jpeg_finish_decompress(&cinfo);
        //end = GetNowUs();
        //logd("onDecode softdec time: %lld", end-start);
        return kSuccess;
    }
#endif

    // check for supported formats
    SkScaledBitmapSampler::SrcConfig sc;
    int srcBytesPerPixel;

    if (!get_src_config(cinfo, &sc, &srcBytesPerPixel)) {
        return return_failure(cinfo, *bm, "jpeg colorspace");
    }

    if (!sampler.begin(bm, sc, *this)) {
        return return_failure(cinfo, *bm, "sampler.begin");
    }

    SkAutoMalloc srcStorage(cinfo.output_width * srcBytesPerPixel);
    uint8_t* srcRow = (uint8_t*)srcStorage.get();

    //  Possibly skip initial rows [sampler.srcY0]
    if (!skip_src_rows(&cinfo, srcRow, sampler.srcY0())) {
        return return_failure(cinfo, *bm, "skip rows");
    }

    // now loop through scanlines until y == bm->height() - 1
    for (int y = 0;; y++) {
        JSAMPLE* rowptr = (JSAMPLE*)srcRow;
        int row_count = jpeg_read_scanlines(&cinfo, &rowptr, 1);
        if (0 == row_count) {
            // if row_count == 0, then we didn't get a scanline,
            // so return early.  We will return a partial image.
            fill_below_level(y, bm);
            cinfo.output_scanline = cinfo.output_height;
            jpeg_finish_decompress(&cinfo);
            return kSuccess;
        }
        if (this->shouldCancelDecode()) {
            return return_failure(cinfo, *bm, "shouldCancelDecode");
        }

        if (JCS_CMYK == cinfo.out_color_space) {
            convert_CMYK_to_RGB(srcRow, cinfo.output_width);
        }

        sampler.next(srcRow);
        if (bm->height() - 1 == y) {
            // we're done
            break;
        }

        if (!skip_src_rows(&cinfo, srcRow, sampler.srcDY() - 1)) {
            return return_failure(cinfo, *bm, "skip rows");
        }
    }

    // we formally skip the rest, so we don't get a complaint from libjpeg
    if (!skip_src_rows(&cinfo, srcRow,
                       cinfo.output_height - cinfo.output_scanline)) {
        return return_failure(cinfo, *bm, "skip rows");
    }
    jpeg_finish_decompress(&cinfo);
    
	//end = GetNowUs();
	//logd("onDecode softdec time: %lld", end-start);

    return kSuccess;
}

#ifdef SK_BUILD_FOR_ANDROID
bool SkJPEGImageDecoder::onBuildTileIndex(SkStreamRewindable* stream, int *width, int *height) {

	//int64_t start, end;
	//start = GetNowUs();
    SkAutoTDelete<SkJPEGImageIndex> imageIndex(SkNEW_ARGS(SkJPEGImageIndex, (stream, this)));
    jpeg_decompress_struct* cinfo = imageIndex->cinfo();

    skjpeg_error_mgr sk_err;
    set_error_mgr(cinfo, &sk_err);

    // All objects need to be instantiated before this setjmp call so that
    // they will be cleaned up properly if an error occurs.
    if (setjmp(sk_err.fJmpBuf)) {
        return false;
    }

    // create the cinfo used to create/build the huffmanIndex
    if (!imageIndex->initializeInfoAndReadHeader()) {
        return false;
    }

    if (!imageIndex->buildHuffmanIndex()) {
        return false;
    }

    // destroy the cinfo used to create/build the huffman index
    imageIndex->destroyInfo();

    // Init decoder to image decode mode
    if (!imageIndex->initializeInfoAndReadHeader()) {
        return false;
    }

    // FIXME: This sets cinfo->out_color_space, which we may change later
    // based on the config in onDecodeSubset. This should be fine, since
    // jpeg_init_read_tile_scanline will check out_color_space again after
    // that change (when it calls jinit_color_deconverter).
    (void) this->getBitmapColorType(cinfo);

    turn_off_visual_optimizations(cinfo);

    // instead of jpeg_start_decompress() we start a tiled decompress
    if (!imageIndex->startTileDecompress()) {
        return false;
    }

    SkASSERT(1 == cinfo->scale_num);
    fImageWidth = cinfo->output_width;
    fImageHeight = cinfo->output_height;

    if (width) {
        *width = fImageWidth;
    }
    if (height) {
        *height = fImageHeight;
    }

    SkDELETE(fImageIndex);
    fImageIndex = imageIndex.detach();

	//end = GetNowUs();
	//nTotalTime += (end-start);
	//logd("soft buildIndex time: %lld", end-start);

	
    return true;
}

bool SkJPEGImageDecoder::onDecodeSubset(SkBitmap* bm, const SkIRect& region) {
    if (NULL == fImageIndex) {
        return false;
    }
 
	//logd("***************** decodeSubset, region(%d, %d, %d, %d), bm->getSize:%d**************", region.fLeft, region.fRight, region.fBottom, region.fTop, bm->getSize());
    //int64_t start, end;
    //start = GetNowUs();
    
    jpeg_decompress_struct* cinfo = fImageIndex->cinfo();

    SkIRect rect = SkIRect::MakeWH(fImageWidth, fImageHeight);
    if (!rect.intersect(region)) {
        // If the requested region is entirely outside the image return false
        return false;
    }


    skjpeg_error_mgr errorManager;
    set_error_mgr(cinfo, &errorManager);

    if (setjmp(errorManager.fJmpBuf)) {
        return false;
    }

    int requestedSampleSize = this->getSampleSize();
    cinfo->scale_denom = requestedSampleSize;

    set_dct_method(*this, cinfo);

    const SkColorType colorType = this->getBitmapColorType(cinfo);
    adjust_out_color_space_and_dither(cinfo, colorType, *this);

    int startX = rect.fLeft;
    int startY = rect.fTop;
    int width = rect.width();
    int height = rect.height();

    jpeg_init_read_tile_scanline(cinfo, fImageIndex->huffmanIndex(),
                                 &startX, &startY, &width, &height);
    int skiaSampleSize = recompute_sampleSize(requestedSampleSize, *cinfo);
    int actualSampleSize = skiaSampleSize * (DCTSIZE / cinfo->min_DCT_scaled_size);

    SkScaledBitmapSampler sampler(width, height, skiaSampleSize);

    SkBitmap bitmap;
    // Assume an A8 bitmap is not opaque to avoid the check of each
    // individual pixel. It is very unlikely to be opaque, since
    // an opaque A8 bitmap would not be very interesting.
    // Otherwise, a jpeg image is opaque.
    bitmap.setInfo(SkImageInfo::Make(sampler.scaledWidth(), sampler.scaledHeight(), colorType,
                                     kAlpha_8_SkColorType == colorType ?
                                         kPremul_SkAlphaType : kOpaque_SkAlphaType));

    // Check ahead of time if the swap(dest, src) is possible or not.
    // If yes, then we will stick to AllocPixelRef since it's cheaper with the
    // swap happening. If no, then we will use alloc to allocate pixels to
    // prevent garbage collection.
    int w = rect.width() / actualSampleSize;
    int h = rect.height() / actualSampleSize;
    bool swapOnly = (rect == region) && bm->isNull() &&
                    (w == bitmap.width()) && (h == bitmap.height()) &&
                    ((startX - rect.x()) / actualSampleSize == 0) &&
                    ((startY - rect.y()) / actualSampleSize == 0);
    if (swapOnly) {
        if (!this->allocPixelRef(&bitmap, NULL)) {
            return return_false(*cinfo, bitmap, "allocPixelRef");
        }
    } else {
        if (!bitmap.allocPixels()) {
            return return_false(*cinfo, bitmap, "allocPixels");
        }
    }

    SkAutoLockPixels alp(bitmap);

#ifdef ANDROID_RGB
    /* short-circuit the SkScaledBitmapSampler when possible, as this gives
       a significant performance boost.
    */
    if (skiaSampleSize == 1 &&
        ((kN32_SkColorType == colorType && cinfo->out_color_space == JCS_RGBA_8888) ||
         (kRGB_565_SkColorType == colorType && cinfo->out_color_space == JCS_RGB_565)))
    {
        JSAMPLE* rowptr = (JSAMPLE*)bitmap.getPixels();
        INT32 const bpr = bitmap.rowBytes();
        int rowTotalCount = 0;

        while (rowTotalCount < height) {
            int rowCount = jpeg_read_tile_scanline(cinfo,
                                                   fImageIndex->huffmanIndex(),
                                                   &rowptr);
            // if rowCount == 0, then we didn't get a scanline, so abort.
            // onDecodeSubset() relies on onBuildTileIndex(), which
            // needs a complete image to succeed.
            if (0 == rowCount) {
                return return_false(*cinfo, bitmap, "read_scanlines");
            }
            if (this->shouldCancelDecode()) {
                return return_false(*cinfo, bitmap, "shouldCancelDecode");
            }
            rowTotalCount += rowCount;
            rowptr += bpr;
        }

        if (swapOnly) {
            bm->swap(bitmap);
        } else {
            cropBitmap(bm, &bitmap, actualSampleSize, region.x(), region.y(),
                       region.width(), region.height(), startX, startY);
        }

        //end = GetNowUs();
        //nTotalTime += (end-start);
        //logd("soft onDecodeSub  time: %lld, nTotalTime: %lld", end-start, nTotalTime);
        
        return true;
    }
#endif

    // check for supported formats
    SkScaledBitmapSampler::SrcConfig sc;
    int srcBytesPerPixel;

    if (!get_src_config(*cinfo, &sc, &srcBytesPerPixel)) {
        return return_false(*cinfo, *bm, "jpeg colorspace");
    }

    if (!sampler.begin(&bitmap, sc, *this)) {
        return return_false(*cinfo, bitmap, "sampler.begin");
    }

    SkAutoMalloc  srcStorage(width * srcBytesPerPixel);
    uint8_t* srcRow = (uint8_t*)srcStorage.get();

    //  Possibly skip initial rows [sampler.srcY0]
    if (!skip_src_rows_tile(cinfo, fImageIndex->huffmanIndex(), srcRow, sampler.srcY0())) {
        return return_false(*cinfo, bitmap, "skip rows");
    }

    // now loop through scanlines until y == bitmap->height() - 1
    for (int y = 0;; y++) {
        JSAMPLE* rowptr = (JSAMPLE*)srcRow;
        int row_count = jpeg_read_tile_scanline(cinfo, fImageIndex->huffmanIndex(), &rowptr);
        // if row_count == 0, then we didn't get a scanline, so abort.
        // onDecodeSubset() relies on onBuildTileIndex(), which
        // needs a complete image to succeed.
        if (0 == row_count) {
            return return_false(*cinfo, bitmap, "read_scanlines");
        }
        if (this->shouldCancelDecode()) {
            return return_false(*cinfo, bitmap, "shouldCancelDecode");
        }

        if (JCS_CMYK == cinfo->out_color_space) {
            convert_CMYK_to_RGB(srcRow, width);
        }

        sampler.next(srcRow);
        if (bitmap.height() - 1 == y) {
            // we're done
            break;
        }

        if (!skip_src_rows_tile(cinfo, fImageIndex->huffmanIndex(), srcRow,
                                sampler.srcDY() - 1)) {
            return return_false(*cinfo, bitmap, "skip rows");
        }
    }
    if (swapOnly) {
        bm->swap(bitmap);
    } else {
        cropBitmap(bm, &bitmap, actualSampleSize, region.x(), region.y(),
                   region.width(), region.height(), startX, startY);
    }

    //end = GetNowUs();
    //nTotalTime += (end-start);
    //logd("soft onDecodeSub  time: %lld, nTotalTime: %lld", end-start, nTotalTime);
    
    return true;
}
#endif

#endif



///////////////////////////////////////////////////////////////////////////////

#include "SkColorPriv.h"

// taken from jcolor.c in libjpeg
#if 0   // 16bit - precise but slow
    #define CYR     19595   // 0.299
    #define CYG     38470   // 0.587
    #define CYB      7471   // 0.114

    #define CUR    -11059   // -0.16874
    #define CUG    -21709   // -0.33126
    #define CUB     32768   // 0.5

    #define CVR     32768   // 0.5
    #define CVG    -27439   // -0.41869
    #define CVB     -5329   // -0.08131

    #define CSHIFT  16
#else      // 8bit - fast, slightly less precise
    #define CYR     77    // 0.299
    #define CYG     150    // 0.587
    #define CYB      29    // 0.114

    #define CUR     -43    // -0.16874
    #define CUG    -85    // -0.33126
    #define CUB     128    // 0.5

    #define CVR      128   // 0.5
    #define CVG     -107   // -0.41869
    #define CVB      -21   // -0.08131

    #define CSHIFT  8
#endif

static void rgb2yuv_32(uint8_t dst[], SkPMColor c) {
    int r = SkGetPackedR32(c);
    int g = SkGetPackedG32(c);
    int b = SkGetPackedB32(c);

    int  y = ( CYR*r + CYG*g + CYB*b ) >> CSHIFT;
    int  u = ( CUR*r + CUG*g + CUB*b ) >> CSHIFT;
    int  v = ( CVR*r + CVG*g + CVB*b ) >> CSHIFT;

    dst[0] = SkToU8(y);
    dst[1] = SkToU8(u + 128);
    dst[2] = SkToU8(v + 128);
}

static void rgb2yuv_4444(uint8_t dst[], U16CPU c) {
    int r = SkGetPackedR4444(c);
    int g = SkGetPackedG4444(c);
    int b = SkGetPackedB4444(c);

    int  y = ( CYR*r + CYG*g + CYB*b ) >> (CSHIFT - 4);
    int  u = ( CUR*r + CUG*g + CUB*b ) >> (CSHIFT - 4);
    int  v = ( CVR*r + CVG*g + CVB*b ) >> (CSHIFT - 4);

    dst[0] = SkToU8(y);
    dst[1] = SkToU8(u + 128);
    dst[2] = SkToU8(v + 128);
}

static void rgb2yuv_16(uint8_t dst[], U16CPU c) {
    int r = SkGetPackedR16(c);
    int g = SkGetPackedG16(c);
    int b = SkGetPackedB16(c);

    int  y = ( 2*CYR*r + CYG*g + 2*CYB*b ) >> (CSHIFT - 2);
    int  u = ( 2*CUR*r + CUG*g + 2*CUB*b ) >> (CSHIFT - 2);
    int  v = ( 2*CVR*r + CVG*g + 2*CVB*b ) >> (CSHIFT - 2);

    dst[0] = SkToU8(y);
    dst[1] = SkToU8(u + 128);
    dst[2] = SkToU8(v + 128);
}

///////////////////////////////////////////////////////////////////////////////

typedef void (*WriteScanline)(uint8_t* SK_RESTRICT dst,
                              const void* SK_RESTRICT src, int width,
                              const SkPMColor* SK_RESTRICT ctable);

static void Write_32_YUV(uint8_t* SK_RESTRICT dst,
                         const void* SK_RESTRICT srcRow, int width,
                         const SkPMColor*) {
    const uint32_t* SK_RESTRICT src = (const uint32_t*)srcRow;
    while (--width >= 0) {
#ifdef WE_CONVERT_TO_YUV
        rgb2yuv_32(dst, *src++);
#else
        uint32_t c = *src++;
        dst[0] = SkGetPackedR32(c);
        dst[1] = SkGetPackedG32(c);
        dst[2] = SkGetPackedB32(c);
#endif
        dst += 3;
    }
}

static void Write_4444_YUV(uint8_t* SK_RESTRICT dst,
                           const void* SK_RESTRICT srcRow, int width,
                           const SkPMColor*) {
    const SkPMColor16* SK_RESTRICT src = (const SkPMColor16*)srcRow;
    while (--width >= 0) {
#ifdef WE_CONVERT_TO_YUV
        rgb2yuv_4444(dst, *src++);
#else
        SkPMColor16 c = *src++;
        dst[0] = SkPacked4444ToR32(c);
        dst[1] = SkPacked4444ToG32(c);
        dst[2] = SkPacked4444ToB32(c);
#endif
        dst += 3;
    }
}

static void Write_16_YUV(uint8_t* SK_RESTRICT dst,
                         const void* SK_RESTRICT srcRow, int width,
                         const SkPMColor*) {
    const uint16_t* SK_RESTRICT src = (const uint16_t*)srcRow;
    while (--width >= 0) {
#ifdef WE_CONVERT_TO_YUV
        rgb2yuv_16(dst, *src++);
#else
        uint16_t c = *src++;
        dst[0] = SkPacked16ToR32(c);
        dst[1] = SkPacked16ToG32(c);
        dst[2] = SkPacked16ToB32(c);
#endif
        dst += 3;
    }
}

static void Write_Index_YUV(uint8_t* SK_RESTRICT dst,
                            const void* SK_RESTRICT srcRow, int width,
                            const SkPMColor* SK_RESTRICT ctable) {
    const uint8_t* SK_RESTRICT src = (const uint8_t*)srcRow;
    while (--width >= 0) {
#ifdef WE_CONVERT_TO_YUV
        rgb2yuv_32(dst, ctable[*src++]);
#else
        uint32_t c = ctable[*src++];
        dst[0] = SkGetPackedR32(c);
        dst[1] = SkGetPackedG32(c);
        dst[2] = SkGetPackedB32(c);
#endif
        dst += 3;
    }
}

static WriteScanline ChooseWriter(const SkBitmap& bm) {
    switch (bm.colorType()) {
        case kN32_SkColorType:
            return Write_32_YUV;
        case kRGB_565_SkColorType:
            return Write_16_YUV;
        case kARGB_4444_SkColorType:
            return Write_4444_YUV;
        case kIndex_8_SkColorType:
            return Write_Index_YUV;
        default:
            return NULL;
    }
}

class SkJPEGImageEncoder : public SkImageEncoder {
protected:
    virtual bool onEncode(SkWStream* stream, const SkBitmap& bm, int quality) {
#ifdef TIME_ENCODE
        SkAutoTime atm("JPEG Encode");
#endif

        SkAutoLockPixels alp(bm);
        if (NULL == bm.getPixels()) {
            return false;
        }

        jpeg_compress_struct    cinfo;
        skjpeg_error_mgr        sk_err;
        skjpeg_destination_mgr  sk_wstream(stream);

        // allocate these before set call setjmp
        SkAutoMalloc    oneRow;
        SkAutoLockColors ctLocker;

        cinfo.err = jpeg_std_error(&sk_err);
        sk_err.error_exit = skjpeg_error_exit;
        if (setjmp(sk_err.fJmpBuf)) {
            return false;
        }

        // Keep after setjmp or mark volatile.
        const WriteScanline writer = ChooseWriter(bm);
        if (NULL == writer) {
            return false;
        }

        jpeg_create_compress(&cinfo);
        cinfo.dest = &sk_wstream;
        cinfo.image_width = bm.width();
        cinfo.image_height = bm.height();
        cinfo.input_components = 3;
#ifdef WE_CONVERT_TO_YUV
        cinfo.in_color_space = JCS_YCbCr;
#else
        cinfo.in_color_space = JCS_RGB;
#endif
        cinfo.input_gamma = 1;

        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, quality, TRUE /* limit to baseline-JPEG values */);
#ifdef DCT_IFAST_SUPPORTED
        cinfo.dct_method = JDCT_IFAST;
#endif

        jpeg_start_compress(&cinfo, TRUE);

        const int       width = bm.width();
        uint8_t*        oneRowP = (uint8_t*)oneRow.reset(width * 3);

        const SkPMColor* colors = ctLocker.lockColors(bm);
        const void*      srcRow = bm.getPixels();

        while (cinfo.next_scanline < cinfo.image_height) {
            JSAMPROW row_pointer[1];    /* pointer to JSAMPLE row[s] */

            writer(oneRowP, srcRow, width, colors);
            row_pointer[0] = oneRowP;
            (void) jpeg_write_scanlines(&cinfo, row_pointer, 1);
            srcRow = (const void*)((const char*)srcRow + bm.rowBytes());
        }

        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);

        return true;
    }
};

///////////////////////////////////////////////////////////////////////////////
DEFINE_DECODER_CREATOR(JPEGImageDecoder);
DEFINE_ENCODER_CREATOR(JPEGImageEncoder);
///////////////////////////////////////////////////////////////////////////////

static bool is_jpeg(SkStreamRewindable* stream) {
    static const unsigned char gHeader[] = { 0xFF, 0xD8, 0xFF };
    static const size_t HEADER_SIZE = sizeof(gHeader);

    char buffer[HEADER_SIZE];
    size_t len = stream->read(buffer, HEADER_SIZE);

    if (len != HEADER_SIZE) {
        return false;   // can't read enough
    }
    if (memcmp(buffer, gHeader, HEADER_SIZE)) {
        return false;
    }
    return true;
}


static SkImageDecoder* sk_libjpeg_dfactory(SkStreamRewindable* stream) {
    if (is_jpeg(stream)) {
        return SkNEW(SkJPEGImageDecoder);
    }
    return NULL;
}

static SkImageDecoder::Format get_format_jpeg(SkStreamRewindable* stream) {
    if (is_jpeg(stream)) {
        return SkImageDecoder::kJPEG_Format;
    }
    return SkImageDecoder::kUnknown_Format;
}

static SkImageEncoder* sk_libjpeg_efactory(SkImageEncoder::Type t) {
    return (SkImageEncoder::kJPEG_Type == t) ? SkNEW(SkJPEGImageEncoder) : NULL;
}

static SkImageDecoder_DecodeReg gDReg(sk_libjpeg_dfactory);
static SkImageDecoder_FormatReg gFormatReg(get_format_jpeg);
static SkImageEncoder_EncodeReg gEReg(sk_libjpeg_efactory);
