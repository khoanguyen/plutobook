/*
 * Copyright (c) 2022-2026 Samuel Ugochukwu <sammycageagle@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "imageresource.h"
#include "textresource.h"
#include "svgdocument.h"
#include "graphicscontext.h"
#include "stringutils.h"

#include "plutobook.hpp"

#include <cairo.h>
#ifdef PLUTOBOOK_HAS_WEBP
#include <webp/decode.h>
#endif

#ifdef PLUTOBOOK_HAS_TURBOJPEG
#define STBI_NO_JPEG
#include <turbojpeg.h>
#endif

#ifdef CAIRO_HAS_PNG_FUNCTIONS
#define STBI_NO_PNG
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#include <cstring>
#include <cmath>

namespace plutobook {

RefPtr<ImageResource> ImageResource::create(Document* document, const Url& url)
{
    auto resource = ResourceLoader::loadUrl(url, document->customResourceFetcher());
    if(resource.isNull())
        return nullptr;
    auto image = decode(document->book(), resource.content(), resource.contentLength(), resource.mimeType(), resource.textEncoding(), url.base());
    if(image == nullptr) {
        plutobook_set_error_message("Unable to load image '%s': %s", ellipsize(url.value()).data(), plutobook_get_error_message());
        return nullptr;
    }

    return adoptPtr(new (document->heap()) ImageResource(std::move(image)));
}

RefPtr<Image> ImageResource::decode(Book* book, const char* data, size_t size, std::string_view mimeType, std::string_view textEncoding, std::string_view baseUrl)
{
    if(equalsIgnoringCase(mimeType, "image/svg+xml"))
        return SVGImage::create(book, TextResource::decode(data, size, mimeType, textEncoding), baseUrl);
    return BitmapImage::create(book, data, size);
}

bool ImageResource::supportsMimeType(std::string_view mimeType)
{
    return equalsIgnoringCase(mimeType, "image/jpeg")
        || equalsIgnoringCase(mimeType, "image/png")
#ifdef PLUTOBOOK_HAS_WEBP
        || equalsIgnoringCase(mimeType, "image/webp")
#endif
        || equalsIgnoringCase(mimeType, "image/svg+xml")
        || equalsIgnoringCase(mimeType, "image/gif")
        || equalsIgnoringCase(mimeType, "image/bmp");
}

static cairo_surface_t* createImageSurface(cairo_format_t format, int width, int height)
{
    auto surface = cairo_image_surface_create(format, width, height);
    if(auto status = cairo_surface_status(surface)) {
        plutobook_set_error_message("image decode error: %s", cairo_status_to_string(status));
        return nullptr;
    }

    return surface;
}

inline bool matchesPngSignature(const char* data, size_t size)
{
    return size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1A\n", 8) == 0;
}

inline bool matchesJpegSignature(const char* data, size_t size)
{
    return size >= 3 && std::memcmp(data, "\xFF\xD8\xFF", 3) == 0;
}

inline bool matchesWebpSignature(const char* data, size_t size)
{
    return size >= 14 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBPVP", 6) == 0;
}

static void attachMimeData(cairo_surface_t* surface, const char* data, size_t size)
{
    if(!matchesJpegSignature(data, size))
        return;
    auto destroy_func = [](void* data) {
        delete[] static_cast<uint8_t*>(data);
    };

    auto mimeData = new uint8_t[size];
    std::memcpy(mimeData, data, size);
    if(cairo_surface_set_mime_data(surface, CAIRO_MIME_TYPE_JPEG, mimeData, size, destroy_func, mimeData)) {
        destroy_func(mimeData);
    }
}

#ifdef CAIRO_HAS_PNG_FUNCTIONS
static cairo_surface_t* decodePngImage(const char* data, size_t size)
{
    struct ReadStream {
        const char* data;
        size_t size;
    };

    auto read_func = [](void* closure, uint8_t* data, uint32_t length) -> cairo_status_t {
        auto stream = static_cast<ReadStream*>(closure);
        if(length > stream->size)
            return CAIRO_STATUS_READ_ERROR;
        std::memcpy(data, stream->data, length);
        stream->data += length;
        stream->size -= length;
        return CAIRO_STATUS_SUCCESS;
    };

    ReadStream stream = { data, size };
    return cairo_image_surface_create_from_png_stream(read_func, &stream);
}
#endif // CAIRO_HAS_PNG_FUNCTIONS

#ifdef PLUTOBOOK_HAS_TURBOJPEG
static cairo_surface_t* decodeJpegImage(const char* data, size_t size)
{
    auto tj = tjInitDecompress();
    if(tj == nullptr) {
        plutobook_set_error_message("image decode error: %s", tjGetErrorStr());
        return nullptr;
    }

    int width, height;
    if(tjDecompressHeader(tj, (uint8_t*)(data), size, &width, &height) == -1) {
        plutobook_set_error_message("image decode error: %s", tjGetErrorStr());
        tjDestroy(tj);
        return nullptr;
    }

    auto surface = createImageSurface(CAIRO_FORMAT_RGB24, width, height);
    if(surface == nullptr) {
        tjDestroy(tj);
        return nullptr;
    }

    auto pixels = cairo_image_surface_get_data(surface);
    auto stride = cairo_image_surface_get_stride(surface);
    if(tjDecompress2(tj, (uint8_t*)(data), size, pixels, width, stride, height, TJPF_BGRX, 0) == -1) {
        plutobook_set_error_message("image decode error: %s", tjGetErrorStr());
        cairo_surface_destroy(surface);
        tjDestroy(tj);
        return nullptr;
    }

    cairo_surface_mark_dirty(surface);
    attachMimeData(surface, data, size);
    tjDestroy(tj);
    return surface;
}
#endif // PLUTOBOOK_HAS_TURBOJPEG

#ifdef PLUTOBOOK_HAS_WEBP
static cairo_surface_t* decodeWebpImage(const char* data, size_t size)
{
    WebPDecoderConfig config;
    if(!WebPInitDecoderConfig(&config)) {
        plutobook_set_error_message("image decode error: WebPInitDecoderConfig failed");
        return nullptr;
    }

    if(WebPGetFeatures((const uint8_t*)(data), size, &config.input) != VP8_STATUS_OK) {
        plutobook_set_error_message("image decode error: WebPGetFeatures failed");
        return nullptr;
    }

    auto surface = createImageSurface(CAIRO_FORMAT_ARGB32, config.input.width, config.input.height);
    if(surface == nullptr)
        return nullptr;
    auto surfaceData = cairo_image_surface_get_data(surface);
    auto surfaceWidth = cairo_image_surface_get_width(surface);
    auto surfaceHeight = cairo_image_surface_get_height(surface);
    auto surfaceStride = cairo_image_surface_get_stride(surface);

    config.output.colorspace = MODE_bgrA;
    config.output.u.RGBA.rgba = surfaceData;
    config.output.u.RGBA.stride = surfaceStride;
    config.output.u.RGBA.size = surfaceStride * surfaceHeight;
    config.output.width = surfaceWidth;
    config.output.height = surfaceHeight;
    config.output.is_external_memory = 1;
    if(WebPDecode((const uint8_t*)(data), size, &config) != VP8_STATUS_OK) {
        plutobook_set_error_message("image decode error: WebPDecode failed");
        cairo_surface_destroy(surface);
        return nullptr;
    }

    cairo_surface_mark_dirty(surface);
    return surface;
}
#endif // PLUTOBOOK_HAS_WEBP

static cairo_surface_t* decodeGenericImage(const char* data, size_t size)
{
    int width, height, channels;
    auto imageData = stbi_load_from_memory((const stbi_uc*)(data), size, &width, &height, &channels, STBI_rgb_alpha);
    if(imageData == nullptr) {
        plutobook_set_error_message("image decode error: %s", stbi_failure_reason());
        return nullptr;
    }

    auto surface = createImageSurface(CAIRO_FORMAT_ARGB32, width, height);
    if(surface == nullptr) {
        stbi_image_free(imageData);
        return nullptr;
    }

    auto surfaceData = cairo_image_surface_get_data(surface);
    auto surfaceStride = cairo_image_surface_get_stride(surface);

    const auto imageStride = width * 4;
    for(int y = 0; y < height; y++) {
        auto src = (const uint32_t*)(imageData + imageStride * y);
        auto dst = (uint32_t*)(surfaceData + surfaceStride * y);
        for(int x = 0; x < width; x++) {
            uint32_t pixel = src[x];
            uint32_t r = (pixel >> 0) & 0xFF;
            uint32_t g = (pixel >> 8) & 0xFF;
            uint32_t b = (pixel >> 16) & 0xFF;
            uint32_t a = (pixel >> 24) & 0xFF;

            uint32_t pr = (r * a) / 255;
            uint32_t pg = (g * a) / 255;
            uint32_t pb = (b * a) / 255;
            dst[x] = (a << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }

    cairo_surface_mark_dirty(surface);
    attachMimeData(surface, data, size);
    stbi_image_free(imageData);
    return surface;
}

static cairo_surface_t* decodeBitmapImage(const char* data, size_t size)
{
#ifdef CAIRO_HAS_PNG_FUNCTIONS
    if(matchesPngSignature(data, size))
        return decodePngImage(data, size);
#endif
#ifdef PLUTOBOOK_HAS_TURBOJPEG
    if(matchesJpegSignature(data, size))
        return decodeJpegImage(data, size);
#endif
#ifdef PLUTOBOOK_HAS_WEBP
    if(matchesWebpSignature(data, size))
        return decodeWebpImage(data, size);
#endif
    return decodeGenericImage(data, size);
}

RefPtr<BitmapImage> BitmapImage::create(Book* book, const char* data, size_t size)
{
    auto surface = decodeBitmapImage(data, size);
    if(surface == nullptr)
        return nullptr;
    if(auto status = cairo_surface_status(surface)) {
        plutobook_set_error_message("image decode error: %s", cairo_status_to_string(status));
        cairo_surface_destroy(surface);
        return nullptr;
    }

    return adoptPtr(new (book->heap()) BitmapImage(surface));
}

void Image::drawTiled(GraphicsContext& context, const Rect& destRect, const Rect& tileRect)
{
    const Size imageSize(size());
    if(imageSize.isEmpty() || destRect.isEmpty() || tileRect.isEmpty()) {
        return;
    }

    const Size scale(tileRect.w / imageSize.w, tileRect.h / imageSize.h);
    const Point phase = {
        destRect.x + std::fmod(std::fmod(-tileRect.x, tileRect.w) - tileRect.w, tileRect.w),
        destRect.y + std::fmod(std::fmod(-tileRect.y, tileRect.h) - tileRect.h, tileRect.h)
    };

    const Rect oneTileRect(phase, tileRect.size());
    if(!oneTileRect.contains(destRect)) {
        drawPattern(context, destRect, imageSize, scale, phase);
    } else {
        const Rect srcRect = {
            (destRect.x - oneTileRect.x) / scale.w,
            (destRect.y - oneTileRect.y) / scale.h,
            (destRect.w / scale.w),
            (destRect.h / scale.h)
        };

        draw(context, destRect, srcRect);
    }
}

void BitmapImage::draw(GraphicsContext& context, const Rect& dstRect, const Rect& srcRect)
{
    if(dstRect.isEmpty() || srcRect.isEmpty()) {
        return;
    }

    auto xScale = srcRect.w / dstRect.w;
    auto yScale = srcRect.h / dstRect.h;
    cairo_matrix_t matrix = {xScale, 0, 0, yScale, srcRect.x, srcRect.y};

    auto pattern = cairo_pattern_create_for_surface(m_surface);
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);

    auto canvas = context.canvas();
    cairo_save(canvas);
    cairo_set_fill_rule(canvas, CAIRO_FILL_RULE_WINDING);
    cairo_translate(canvas, dstRect.x, dstRect.y);
    cairo_rectangle(canvas, 0, 0, dstRect.w, dstRect.h);
    cairo_set_source(canvas, pattern);
    cairo_fill(canvas);
    cairo_restore(canvas);
    cairo_pattern_destroy(pattern);
}

void BitmapImage::drawPattern(GraphicsContext& context, const Rect& destRect, const Size& size, const Size& scale, const Point& phase)
{
    assert(!destRect.isEmpty() && !size.isEmpty() && !scale.isEmpty());

    cairo_matrix_t matrix;
    cairo_matrix_init(&matrix, scale.w, 0, 0, scale.h, phase.x, phase.y);
    cairo_matrix_invert(&matrix);

    auto pattern = cairo_pattern_create_for_surface(m_surface);
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

    auto canvas = context.canvas();
    cairo_save(canvas);
    cairo_set_fill_rule(canvas, CAIRO_FILL_RULE_WINDING);
    cairo_rectangle(canvas, destRect.x, destRect.y, destRect.w, destRect.h);
    cairo_set_source(canvas, pattern);
    cairo_fill(canvas);
    cairo_restore(canvas);
    cairo_pattern_destroy(pattern);
}

void BitmapImage::computeIntrinsicDimensions(float& intrinsicWidth, float& intrinsicHeight, double& intrinsicRatio)
{
    intrinsicWidth = cairo_image_surface_get_width(m_surface);
    intrinsicHeight = cairo_image_surface_get_height(m_surface);
    if(intrinsicHeight > 0) {
        intrinsicRatio = intrinsicWidth / intrinsicHeight;
    } else {
        intrinsicRatio = 0;
    }
}

Size BitmapImage::intrinsicSize() const
{
    auto width = cairo_image_surface_get_width(m_surface);
    auto height = cairo_image_surface_get_height(m_surface);

    return Size(width, height);
}

Size BitmapImage::size() const
{
    return intrinsicSize();
}

BitmapImage::~BitmapImage()
{
    cairo_surface_destroy(m_surface);
}

BitmapImage::BitmapImage(cairo_surface_t* surface)
    : m_surface(surface)
{
}

RefPtr<GradientImage> GradientImage::createLinear(Heap* heap, bool repeating, bool hasAngle, float angle,
    bool toLeft, bool toRight, bool toTop, bool toBottom, GradientColorStopList stops)
{
    auto image = adoptPtr(new (heap) GradientImage(GradientImageType::Linear, repeating, std::move(stops)));
    image->m_hasAngle = hasAngle;
    image->m_angle = angle;
    image->m_toLeft = toLeft;
    image->m_toRight = toRight;
    image->m_toTop = toTop;
    image->m_toBottom = toBottom;
    return image;
}

RefPtr<GradientImage> GradientImage::createRadial(Heap* heap, bool repeating, GradientShape shape, GradientSizing sizing,
    const GradientLength& radiusX, const GradientLength& radiusY,
    const GradientLength& centerX, const GradientLength& centerY, GradientColorStopList stops)
{
    auto image = adoptPtr(new (heap) GradientImage(GradientImageType::Radial, repeating, std::move(stops)));
    image->m_shape = shape;
    image->m_sizing = sizing;
    image->m_radiusX = radiusX;
    image->m_radiusY = radiusY;
    image->m_centerX = centerX;
    image->m_centerY = centerY;
    return image;
}

void GradientImage::computeIntrinsicDimensions(float& intrinsicWidth, float& intrinsicHeight, double& intrinsicRatio)
{
    /* A gradient has no intrinsic size or ratio, so it fills its positioning area. */
    intrinsicWidth = 0.f;
    intrinsicHeight = 0.f;
    intrinsicRatio = 0.0;
}

/* Resolves the color stops onto the gradient line, following the CSS rules: the
 * first and last positions default to the ends, a position never moves backwards
 * past the one before it, and runs without positions are spread evenly. The
 * offsets are returned normalised to the span the stops actually cover, which is
 * also the span a repeating gradient repeats over. */
static GradientStops resolveGradientStops(const GradientColorStopList& stops, float lineLength, bool repeating, float& firstOffset, float& lastOffset)
{
    const auto count = stops.size();
    std::vector<float> offsets(count, 0.f);
    std::vector<bool> resolved(count, false);
    for(size_t index = 0; index < count; ++index) {
        const auto& position = stops[index].position();
        if(!position.isSpecified())
            continue;
        if(position.isPercent())
            offsets[index] = position.value() / 100.f;
        else if(lineLength > 0.f)
            offsets[index] = position.value() / lineLength;
        resolved[index] = true;
    }

    if(!resolved.front()) {
        offsets.front() = 0.f;
        resolved.front() = true;
    }

    if(!resolved.back()) {
        offsets.back() = 1.f;
        resolved.back() = true;
    }

    auto maximum = offsets.front();
    for(size_t index = 1; index < count; ++index) {
        if(!resolved[index])
            continue;
        offsets[index] = std::max(offsets[index], maximum);
        maximum = offsets[index];
    }

    size_t index = 1;
    while(index < count) {
        if(resolved[index]) {
            ++index;
            continue;
        }

        auto end = index;
        while(end < count && !resolved[end])
            ++end;
        const auto before = offsets[index - 1];
        const auto after = offsets[end];
        const auto steps = end - index + 1;
        for(size_t position = index; position < end; ++position)
            offsets[position] = before + (after - before) * (position - index + 1) / steps;
        index = end;
    }

    firstOffset = offsets.front();
    lastOffset = offsets.back();

    GradientStops result;
    result.reserve(count);

    /* Every stop landing on the same position is a hard edge: the first color
     * applies before it and the last one after, which a span just wide enough to
     * hold the two ends reproduces. A repeating gradient has nothing to repeat in
     * that case and is a solid color instead, which the caller handles. */
    if(lastOffset <= firstOffset) {
        if(repeating)
            return result;
        lastOffset = firstOffset + std::max(1.f / std::max(1.f, lineLength), 1e-4f);
        for(size_t position = 0; position < count; ++position)
            result.emplace_back(position == 0 ? 0.f : 1.f, stops[position].color());
        return result;
    }

    const auto span = lastOffset - firstOffset;
    for(size_t position = 0; position < count; ++position) {
        const auto offset = (offsets[position] - firstOffset) / span;
        result.emplace_back(std::clamp(offset, 0.f, 1.f), stops[position].color());
    }

    return result;
}

void GradientImage::applyLinear(GraphicsContext& context, const Size& size) const
{
    auto angle = m_angle;
    if(!m_hasAngle && (m_toLeft || m_toRight || m_toTop || m_toBottom)) {
        if(m_toLeft && m_toTop) {
            angle = 360.f - rad2deg(std::atan2(size.w, size.h));
        } else if(m_toLeft && m_toBottom) {
            angle = 180.f + rad2deg(std::atan2(size.w, size.h));
        } else if(m_toRight && m_toTop) {
            angle = rad2deg(std::atan2(size.w, size.h));
        } else if(m_toRight && m_toBottom) {
            angle = 180.f - rad2deg(std::atan2(size.w, size.h));
        } else if(m_toLeft) {
            angle = 270.f;
        } else if(m_toRight) {
            angle = 90.f;
        } else if(m_toTop) {
            angle = 0.f;
        } else {
            angle = 180.f;
        }
    }

    /* Zero degrees points to the top and angles grow clockwise. */
    const auto radians = deg2rad(angle);
    const auto sinAngle = std::sin(radians);
    const auto cosAngle = std::cos(radians);
    const auto lineLength = std::abs(size.w * sinAngle) + std::abs(size.h * cosAngle);

    float firstOffset = 0.f;
    float lastOffset = 1.f;
    auto stops = resolveGradientStops(m_stops, lineLength, m_repeating, firstOffset, lastOffset);
    if(stops.empty()) {
        context.setColor(m_stops.back().color());
        context.fillRect(Rect(0, 0, size.w, size.h));
        return;
    }

    const auto centerX = size.w / 2.f;
    const auto centerY = size.h / 2.f;
    const auto halfX = sinAngle * lineLength / 2.f;
    const auto halfY = -cosAngle * lineLength / 2.f;

    LinearGradientValues values;
    values.x1 = centerX - halfX + (halfX * 2.f) * firstOffset;
    values.y1 = centerY - halfY + (halfY * 2.f) * firstOffset;
    values.x2 = centerX - halfX + (halfX * 2.f) * lastOffset;
    values.y2 = centerY - halfY + (halfY * 2.f) * lastOffset;

    context.setLinearGradient(values, stops, Transform(), m_repeating ? SpreadMethod::Repeat : SpreadMethod::Pad, 1.f);
    context.fillRect(Rect(0, 0, size.w, size.h));
}

void GradientImage::applyRadial(GraphicsContext& context, const Size& size) const
{
    const auto centerX = m_centerX.isSpecified() ? m_centerX.resolve(size.w) : size.w / 2.f;
    const auto centerY = m_centerY.isSpecified() ? m_centerY.resolve(size.h) : size.h / 2.f;

    const auto leftDistance = std::abs(centerX);
    const auto rightDistance = std::abs(size.w - centerX);
    const auto topDistance = std::abs(centerY);
    const auto bottomDistance = std::abs(size.h - centerY);

    float radiusX = 0.f;
    float radiusY = 0.f;
    switch(m_sizing) {
    case GradientSizing::Explicit:
        radiusX = m_radiusX.resolve(size.w);
        radiusY = m_shape == GradientShape::Circle ? radiusX : m_radiusY.resolve(size.h);
        break;
    case GradientSizing::ClosestSide:
    case GradientSizing::FarthestSide: {
        const auto closest = m_sizing == GradientSizing::ClosestSide;
        const auto sideX = closest ? std::min(leftDistance, rightDistance) : std::max(leftDistance, rightDistance);
        const auto sideY = closest ? std::min(topDistance, bottomDistance) : std::max(topDistance, bottomDistance);
        if(m_shape == GradientShape::Circle) {
            radiusX = radiusY = closest ? std::min(sideX, sideY) : std::max(sideX, sideY);
        } else {
            radiusX = sideX;
            radiusY = sideY;
        }

        break;
    }

    case GradientSizing::ClosestCorner:
    case GradientSizing::FarthestCorner: {
        const auto closest = m_sizing == GradientSizing::ClosestCorner;
        const auto cornerX = closest ? std::min(leftDistance, rightDistance) : std::max(leftDistance, rightDistance);
        const auto cornerY = closest ? std::min(topDistance, bottomDistance) : std::max(topDistance, bottomDistance);
        if(m_shape == GradientShape::Circle) {
            radiusX = radiusY = std::sqrt(cornerX * cornerX + cornerY * cornerY);
        } else {
            /* An ellipse through the corner keeps the aspect ratio of the
             * matching side-sized ellipse. */
            const auto sideX = closest ? std::min(leftDistance, rightDistance) : std::max(leftDistance, rightDistance);
            const auto sideY = closest ? std::min(topDistance, bottomDistance) : std::max(topDistance, bottomDistance);
            const auto ratio = sideY > 0.f ? sideX / sideY : 1.f;
            radiusX = std::sqrt(cornerX * cornerX + (cornerY * ratio) * (cornerY * ratio));
            radiusY = ratio > 0.f ? radiusX / ratio : radiusX;
        }

        break;
    }
    }

    float firstOffset = 0.f;
    float lastOffset = 1.f;
    auto stops = resolveGradientStops(m_stops, radiusX, m_repeating, firstOffset, lastOffset);
    if(stops.empty() || radiusX <= 0.f || radiusY <= 0.f) {
        context.setColor(m_stops.back().color());
        context.fillRect(Rect(0, 0, size.w, size.h));
        return;
    }

    RadialGradientValues values;
    values.cx = centerX;
    values.cy = centerY;
    values.fx = centerX;
    values.fy = centerY;
    values.r = radiusX * lastOffset;
    values.r0 = std::max(0.f, radiusX * firstOffset);

    /* Cairo draws circles, so an ellipse is a circle under a scale about its centre. */
    Transform transform;
    if(radiusY != radiusX) {
        transform.translate(centerX, centerY);
        transform.scale(1.f, radiusY / radiusX);
        transform.translate(-centerX, -centerY);
    }

    context.setRadialGradient(values, stops, transform, m_repeating ? SpreadMethod::Repeat : SpreadMethod::Pad, 1.f);
    context.fillRect(Rect(0, 0, size.w, size.h));
}

void GradientImage::apply(GraphicsContext& context, const Size& size) const
{
    if(m_stops.empty() || size.isEmpty())
        return;
    if(m_type == GradientImageType::Linear) {
        applyLinear(context, size);
    } else {
        applyRadial(context, size);
    }
}

void GradientImage::draw(GraphicsContext& context, const Rect& dstRect, const Rect& srcRect)
{
    if(dstRect.isEmpty() || srcRect.isEmpty()) {
        return;
    }

    const auto xScale = dstRect.w / srcRect.w;
    const auto yScale = dstRect.h / srcRect.h;

    context.save();
    context.clipRect(dstRect);
    context.translate(dstRect.x - srcRect.x * xScale, dstRect.y - srcRect.y * yScale);
    context.scale(xScale, yScale);
    apply(context, m_containerSize);
    context.restore();
}

void GradientImage::drawPattern(GraphicsContext& context, const Rect& destRect, const Size& size, const Size& scale, const Point& phase)
{
    assert(!destRect.isEmpty() && !size.isEmpty() && !scale.isEmpty());

    cairo_rectangle_t rectangle = {0, 0, size.w * scale.w, size.h * scale.h};
    auto surface = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &rectangle);
    auto canvas = cairo_create(surface);

    GradientImage* image = this;
    GraphicsContext tileContext(canvas);
    tileContext.scale(scale.w, scale.h);
    image->apply(tileContext, size);

    Transform transform;
    transform.translate(phase.x, phase.y);

    context.save();
    context.clipRect(destRect);
    context.setPattern(surface, transform);
    context.fillRect(destRect);
    context.restore();

    cairo_destroy(canvas);
    cairo_surface_destroy(surface);
}

RefPtr<SVGImage> SVGImage::create(Book* book, std::string_view content, std::string_view baseUrl)
{
    auto document = SVGDocument::create(book, ResourceLoader::completeUrl(baseUrl));
    if(!document->parse(content))
        return nullptr;
    if(!document->rootElement()->isOfType(svgNs, svgTag)) {
        plutobook_set_error_message("invalid SVG image: root element must be <svg> in the \"http://www.w3.org/2000/svg\" namespace");
        return nullptr;
    }

    return adoptPtr(new (book->heap()) SVGImage(std::move(document)));
}

void SVGImage::draw(GraphicsContext& context, const Rect& dstRect, const Rect& srcRect)
{
    if(dstRect.isEmpty() || srcRect.isEmpty()) {
        return;
    }

    auto xScale = dstRect.w / srcRect.w;
    auto yScale = dstRect.h / srcRect.h;

    auto xOffset = dstRect.x - (srcRect.x * xScale);
    auto yOffset = dstRect.y - (srcRect.y * yScale);

    context.save();
    context.clipRect(dstRect);
    context.translate(xOffset, yOffset);
    context.scale(xScale, yScale);
    m_document->render(context, srcRect);
    context.restore();
}

void SVGImage::drawPattern(GraphicsContext& context, const Rect& destRect, const Size& size, const Size& scale, const Point& phase)
{
    assert(!destRect.isEmpty() && !size.isEmpty() && !scale.isEmpty());

    cairo_matrix_t pattern_matrix;
    cairo_matrix_init(&pattern_matrix, 1, 0, 0, 1, -phase.x, -phase.y);

    cairo_rectangle_t pattern_rectangle = {0, 0, size.w * scale.w, size.h * scale.h};
    auto pattern_surface = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &pattern_rectangle);
    auto pattern_canvas = cairo_create(pattern_surface);

    auto pattern = cairo_pattern_create_for_surface(pattern_surface);
    cairo_pattern_set_matrix(pattern, &pattern_matrix);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

    GraphicsContext pattern_context(pattern_canvas);
    m_document->render(pattern_context, Rect::Infinite);

    auto canvas = context.canvas();
    cairo_save(canvas);
    cairo_set_fill_rule(canvas, CAIRO_FILL_RULE_WINDING);
    cairo_rectangle(canvas, destRect.x, destRect.y, destRect.w, destRect.h);
    cairo_set_source(canvas, pattern);
    cairo_fill(canvas);
    cairo_restore(canvas);

    cairo_pattern_destroy(pattern);
    cairo_destroy(pattern_canvas);
    cairo_surface_destroy(pattern_surface);
}

void SVGImage::computeIntrinsicDimensions(float& intrinsicWidth, float& intrinsicHeight, double& intrinsicRatio)
{
    rootElement()->computeIntrinsicDimensions(intrinsicWidth, intrinsicHeight, intrinsicRatio);
}

void SVGImage::setContainerSize(const Size& size)
{
    m_containerSize = size;
    if(m_document->setContainerSize(size.w, size.h)) {
        m_document->layout(nullptr);
    }
}

Size SVGImage::intrinsicSize() const
{
    float intrinsicWidth = 0.f;
    float intrinsicHeight = 0.f;
    double intrinsicRatio = 0.0;

    rootElement()->computeIntrinsicDimensions(intrinsicWidth, intrinsicHeight, intrinsicRatio);
    if(intrinsicRatio && (!intrinsicWidth || !intrinsicHeight)) {
        if(!intrinsicWidth && intrinsicHeight)
            intrinsicWidth = intrinsicHeight * intrinsicRatio;
        else if(intrinsicWidth && !intrinsicHeight) {
            intrinsicHeight = intrinsicWidth / intrinsicRatio;
        }
    }

    if(intrinsicWidth > 0 && intrinsicHeight > 0) {
        return Size(intrinsicWidth, intrinsicHeight);
    }

    auto viewBoxRect = rootElement()->viewBox();
    if(viewBoxRect.isValid())
        return viewBoxRect.size();
    return Size(300, 150);
}

Size SVGImage::size() const
{
    return m_containerSize;
}

SVGImage::~SVGImage() = default;

SVGImage::SVGImage(std::unique_ptr<SVGDocument> document)
    : m_document(std::move(document))
{
    m_document->build();
}

SVGSVGElement* SVGImage::rootElement() const
{
    auto element = m_document->rootElement();
    assert(element && element->isOfType(svgNs, svgTag));
    return static_cast<SVGSVGElement*>(element);
}

} // namespace plutobook
