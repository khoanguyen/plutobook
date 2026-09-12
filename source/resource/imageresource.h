/*
 * Copyright (c) 2022-2026 Samuel Ugochukwu <sammycageagle@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef PLUTOBOOK_IMAGERESOURCE_H
#define PLUTOBOOK_IMAGERESOURCE_H

#include "resource.h"
#include "geometry.h"
#include "color.h"

#include <memory>

typedef struct _cairo_surface cairo_surface_t;

namespace plutobook {

class Image;
class Document;
class Book;

class ImageResource final : public Resource {
public:
    static RefPtr<ImageResource> create(Document* document, const Url& url);
    static RefPtr<Image> decode(Book* book, const char* data, size_t size, std::string_view mimeType, std::string_view textEncoding, std::string_view baseUrl);
    static bool supportsMimeType(std::string_view mimeType);
    const RefPtr<Image>& image() const { return m_image; }
    Type type() const final { return Type::Image; }

private:
    ImageResource(RefPtr<Image> image) : m_image(std::move(image)) {}
    RefPtr<Image> m_image;
};

template<>
struct is_a<ImageResource> {
    static bool check(const Resource& value) { return value.type() == Resource::Type::Image; }
};

class GraphicsContext;

class Image : public HeapMember, public RefCounted<Image> {
public:
    Image() = default;
    virtual ~Image() = default;

    virtual bool isBitmapImage() const { return false; }
    virtual bool isSVGImage() const { return false; }

    void drawTiled(GraphicsContext& context, const Rect& destRect, const Rect& tileRect);

    virtual void draw(GraphicsContext& context, const Rect& dstRect, const Rect& srcRect) = 0;
    virtual void drawPattern(GraphicsContext& context, const Rect& destRect, const Size& size, const Size& scale, const Point& phase) = 0;
    virtual void computeIntrinsicDimensions(float& intrinsicWidth, float& intrinsicHeight, double& intrinsicRatio) = 0;

    virtual void setContainerSize(const Size& size) = 0;
    virtual Size intrinsicSize() const = 0;
    virtual Size size() const = 0;
};

class BitmapImage final : public Image {
public:
    static RefPtr<BitmapImage> create(Book* book, const char* data, size_t size);

    bool isBitmapImage() const final { return true; }

    void draw(GraphicsContext& context, const Rect& dstRect, const Rect& srcRect) final;
    void drawPattern(GraphicsContext& context, const Rect& destRect, const Size& size, const Size& scale, const Point& phase) final;
    void computeIntrinsicDimensions(float& intrinsicWidth, float& intrinsicHeight, double& intrinsicRatio) final;

    void setContainerSize(const Size& size) final {};
    Size intrinsicSize() const final;
    Size size() const final;

    ~BitmapImage() final;

private:
    BitmapImage(cairo_surface_t* surface);
    cairo_surface_t* m_surface;
};

template<>
struct is_a<BitmapImage> {
    static bool check(const Image& value) { return value.isBitmapImage(); }
};

class SVGDocument;
class SVGSVGElement;

/* A CSS gradient behaves as an image with no intrinsic size: it is resolved
 * against the box it fills, so the geometry keywords and lengths survive here
 * until paint time, when the container size is known. */
enum class GradientImageType : uint8_t {
    Linear,
    Radial
};

enum class GradientShape : uint8_t {
    Circle,
    Ellipse
};

enum class GradientSizing : uint8_t {
    ClosestSide,
    ClosestCorner,
    FarthestSide,
    FarthestCorner,
    Explicit
};

class GradientLength {
public:
    GradientLength() = default;
    GradientLength(float value, bool percent)
        : m_value(value), m_percent(percent), m_specified(true)
    {}

    float value() const { return m_value; }
    bool isPercent() const { return m_percent; }
    bool isSpecified() const { return m_specified; }
    float resolve(float reference) const { return m_percent ? m_value * reference / 100.f : m_value; }

private:
    float m_value{0.f};
    bool m_percent{false};
    bool m_specified{false};
};

class GradientColorStop {
public:
    GradientColorStop(const Color& color, const GradientLength& position)
        : m_color(color), m_position(position)
    {}

    const Color& color() const { return m_color; }
    const GradientLength& position() const { return m_position; }

private:
    Color m_color;
    GradientLength m_position;
};

using GradientColorStopList = std::vector<GradientColorStop>;

class GradientImage final : public Image {
public:
    static RefPtr<GradientImage> createLinear(Heap* heap, bool repeating, bool hasAngle, float angle,
        bool toLeft, bool toRight, bool toTop, bool toBottom, GradientColorStopList stops);
    static RefPtr<GradientImage> createRadial(Heap* heap, bool repeating, GradientShape shape, GradientSizing sizing,
        const GradientLength& radiusX, const GradientLength& radiusY,
        const GradientLength& centerX, const GradientLength& centerY, GradientColorStopList stops);

    void draw(GraphicsContext& context, const Rect& dstRect, const Rect& srcRect) final;
    void drawPattern(GraphicsContext& context, const Rect& destRect, const Size& size, const Size& scale, const Point& phase) final;
    void computeIntrinsicDimensions(float& intrinsicWidth, float& intrinsicHeight, double& intrinsicRatio) final;

    void setContainerSize(const Size& size) final { m_containerSize = size; }
    Size intrinsicSize() const final { return m_containerSize; }
    Size size() const final { return m_containerSize; }

private:
    GradientImage(GradientImageType type, bool repeating, GradientColorStopList stops)
        : m_type(type), m_repeating(repeating), m_stops(std::move(stops))
    {}

    void apply(GraphicsContext& context, const Size& size) const;
    void applyLinear(GraphicsContext& context, const Size& size) const;
    void applyRadial(GraphicsContext& context, const Size& size) const;

    GradientImageType m_type;
    bool m_repeating;
    GradientColorStopList m_stops;
    Size m_containerSize;

    /* linear */
    bool m_hasAngle{false};
    float m_angle{180.f};
    bool m_toLeft{false};
    bool m_toRight{false};
    bool m_toTop{false};
    bool m_toBottom{false};

    /* radial */
    GradientShape m_shape{GradientShape::Ellipse};
    GradientSizing m_sizing{GradientSizing::FarthestCorner};
    GradientLength m_radiusX;
    GradientLength m_radiusY;
    GradientLength m_centerX;
    GradientLength m_centerY;
};

class SVGImage final : public Image {
public:
    static RefPtr<SVGImage> create(Book* book, std::string_view content, std::string_view baseUrl);

    bool isSVGImage() const final { return true; }

    void draw(GraphicsContext& context, const Rect& dstRect, const Rect& srcRect) final;
    void drawPattern(GraphicsContext& context, const Rect& destRect, const Size& size, const Size& scale, const Point& phase) final;
    void computeIntrinsicDimensions(float& intrinsicWidth, float& intrinsicHeight, double& intrinsicRatio) final;

    void setContainerSize(const Size& size) final;
    Size intrinsicSize() const final;
    Size size() const final;

    ~SVGImage() final;

private:
    SVGImage(std::unique_ptr<SVGDocument> document);
    SVGSVGElement* rootElement() const;
    std::unique_ptr<SVGDocument> m_document;
    Size m_containerSize;
};

template<>
struct is_a<SVGImage> {
    static bool check(const Image& value) { return value.isSVGImage(); }
};

} // namespace plutobook

#endif // PLUTOBOOK_IMAGERESOURCE_H
