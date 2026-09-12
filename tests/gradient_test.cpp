#include <plutobook.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int size = 200;
using Pixels = std::vector<uint32_t>;

Pixels render(const std::string& background, const std::string& extra = {})
{
    plutobook::Book book({size * plutobook::units::px, size * plutobook::units::px},
        plutobook::PageMargins::None, plutobook::MediaType::Screen);
    const auto html = "<!doctype html><html><meta charset='utf-8'><style>"
        "body{margin:0;background:white}"
        "#box{width:" + std::to_string(size) + "px;height:" + std::to_string(size) + "px;background:"
        + background + ";" + extra + "}"
        "</style><body><div id='box'></div></body></html>";
    if(!book.loadHtml(html))
        throw std::runtime_error("HTML load failed");
    plutobook::ImageCanvas canvas(size, size);
    book.renderDocument(canvas);
    Pixels pixels(size * size);
    for(int y = 0; y < size; ++y)
        memcpy(pixels.data() + y * size, canvas.data() + y * canvas.stride(), size * 4);
    return pixels;
}

struct Rgb {
    int r{0}, g{0}, b{0}, a{0};
};

Rgb at(const Pixels& pixels, int x, int y)
{
    const auto pixel = pixels[y * size + x];
    const auto alpha = (pixel >> 24) & 255;
    if(alpha == 0)
        return Rgb{0, 0, 0, 0};
    /* The canvas is premultiplied. */
    return Rgb{
        static_cast<int>(((pixel >> 16) & 255) * 255 / alpha),
        static_cast<int>(((pixel >> 8) & 255) * 255 / alpha),
        static_cast<int>((pixel & 255) * 255 / alpha),
        static_cast<int>(alpha)
    };
}

bool near(int value, int expected, int tolerance = 12)
{
    return value >= expected - tolerance && value <= expected + tolerance;
}

// Orange #ff8000 to blue #0040ff, so the red and blue channels alone say how far
// along the gradient a pixel sits.
const std::string ramp = "#ff8000,#0040ff";

bool isOrange(const Rgb& color) { return near(color.r, 255) && near(color.b, 0); }
bool isBlue(const Rgb& color) { return near(color.r, 0) && near(color.b, 255); }
bool isMixed(const Rgb& color) { return !isOrange(color) && !isBlue(color); }
bool isWhite(const Rgb& color) { return near(color.r, 255) && near(color.g, 255) && near(color.b, 255); }

void check(bool value, const char* message)
{
    if(!value)
        throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
}

int main()
{
    try {
        const auto toRight = render("linear-gradient(to right," + ramp + ")");
        check(isOrange(at(toRight, 2, 100)) && isBlue(at(toRight, size - 3, 100))
            && isMixed(at(toRight, 100, 100)),
            "linear-gradient runs along the axis given by to right");
        check(at(toRight, 100, 20).r == at(toRight, 100, 180).r,
            "a horizontal gradient is constant down the box");

        const auto ninety = render("linear-gradient(90deg," + ramp + ")");
        check(ninety == toRight, "90deg matches to right");
        check(render("linear-gradient(0.25turn," + ramp + ")") == toRight, "0.25turn matches 90deg");
        check(render("linear-gradient(100grad," + ramp + ")") == toRight, "100grad matches 90deg");

        const auto toTop = render("linear-gradient(to top," + ramp + ")");
        check(isBlue(at(toTop, 100, 2)) && isOrange(at(toTop, 100, size - 3)),
            "to top puts the last color at the top edge");
        check(render("linear-gradient(0deg," + ramp + ")") == toTop, "0deg matches to top");
        check(render("linear-gradient(" + ramp + ")") == render("linear-gradient(to bottom," + ramp + ")"),
            "an omitted direction means to bottom");

        const auto corner = render("linear-gradient(to bottom right," + ramp + ")");
        check(isOrange(at(corner, 2, 2)) && isBlue(at(corner, size - 3, size - 3))
            && isMixed(at(corner, size - 3, 2)) && isMixed(at(corner, 2, size - 3)),
            "to bottom right runs corner to corner");

        const auto stops = render("linear-gradient(to right,#ff8000 25%,#0040ff 75%)");
        check(isOrange(at(stops, 40, 100)) && isBlue(at(stops, 160, 100)) && isMixed(at(stops, 100, 100)),
            "color stop positions hold their color outside the stop range");

        const auto hard = render("linear-gradient(to right,#ff8000 50%,#0040ff 50%)");
        check(isOrange(at(hard, 90, 100)) && isBlue(at(hard, 110, 100)),
            "two stops at one position make a hard edge rather than a solid fill");

        const auto pixels = render("linear-gradient(to right,#ff8000 40px,#0040ff 80px)");
        check(isOrange(at(pixels, 30, 100)) && isBlue(at(pixels, 90, 100)) && isMixed(at(pixels, 60, 100)),
            "stop positions accept lengths as well as percentages");

        const auto clamped = render("linear-gradient(to right,#ff8000 60%,#00ff00 20%,#0040ff)");
        check(isOrange(at(clamped, 100, 100)) && near(at(clamped, 122, 100).g, 255, 60),
            "a stop never moves back past the one before it");

        const auto fade = render("linear-gradient(to right,#ff8000,transparent)");
        check(isOrange(at(fade, 2, 100)) && isWhite(at(fade, size - 3, 100)),
            "a transparent stop fades the gradient out, letting the page show through");

        const auto ellipse = render("radial-gradient(" + ramp + ")");
        check(isOrange(at(ellipse, 100, 100)) && isBlue(at(ellipse, 2, 2)),
            "radial-gradient starts at the centre and ends at the farthest corner");
        // In a square box an ellipse and a circle coincide, so the shape keywords
        // only differ over a box that is twice as wide as it is tall.
        const std::string wide = "width:200px;height:100px";
        const auto wideEllipse = render("radial-gradient(" + ramp + ")", wide);
        check(near(at(wideEllipse, 160, 50).r, at(wideEllipse, 100, 80).r, 4),
            "the default ending shape is an ellipse: equal fractions of each radius match");
        const auto wideCircle = render("radial-gradient(circle," + ramp + ")", wide);
        check(near(at(wideCircle, 140, 50).r, at(wideCircle, 100, 10).r, 4)
            && !near(at(wideCircle, 160, 50).r, at(wideCircle, 100, 80).r, 20),
            "a circle ends the same distance away on both axes");

        const auto closestSide = render("radial-gradient(circle closest-side," + ramp + ")");
        check(isBlue(at(closestSide, 100, 2)) && isBlue(at(closestSide, 2, 100)),
            "closest-side reaches the nearest edge");
        const auto atCorner = render("radial-gradient(circle 40px at left top," + ramp + ")");
        check(isOrange(at(atCorner, 0, 0)) && isBlue(at(atCorner, 60, 60)),
            "at <position> moves the centre and an explicit radius sizes the circle");
        const auto explicitEllipse = render("radial-gradient(ellipse 80px 20px," + ramp + ")");
        check(isMixed(at(explicitEllipse, 140, 100)) && isBlue(at(explicitEllipse, 100, 130)),
            "two radii describe an ellipse");

        const auto repeatingLinear = render("repeating-linear-gradient(to right,#ff8000 0 20px,#0040ff 20px 40px)");
        check(isOrange(at(repeatingLinear, 10, 100)) && isBlue(at(repeatingLinear, 30, 100))
            && isOrange(at(repeatingLinear, 50, 100)) && isBlue(at(repeatingLinear, 70, 100)),
            "repeating-linear-gradient repeats the stop range across the box");
        const auto offsetRepeat = render("repeating-linear-gradient(to right,#ff8000 20%,#0040ff 40%)");
        check(isMixed(at(offsetRepeat, 60, 100)) && isMixed(at(offsetRepeat, 100, 100)),
            "a repeating gradient whose first stop is away from the start still repeats");
        const auto repeatingRadial = render("repeating-radial-gradient(circle,#ff8000 0 20px,#0040ff 20px 40px)");
        check(isOrange(at(repeatingRadial, 100, 100)) && isBlue(at(repeatingRadial, 100, 70))
            && isOrange(at(repeatingRadial, 100, 55)),
            "repeating-radial-gradient repeats outwards in rings");
        check(render("repeating-linear-gradient(to right,#ff8000 50%,#0040ff 50%)") == render("#0040ff"),
            "a repeating gradient with nothing to repeat is the color of its last stop");

        const auto tiled = render("linear-gradient(to right," + ramp + ")", "background-size:50px 50px");
        check(isOrange(at(tiled, 0, 25)) && isBlue(at(tiled, 49, 25))
            && isOrange(at(tiled, 50, 25)) && isBlue(at(tiled, 99, 25)),
            "background-size and repeat tile a gradient like any other image");
        const auto once = render("linear-gradient(to right," + ramp + ") no-repeat", "background-size:50px 50px");
        check(isBlue(at(once, 48, 25)) && isWhite(at(once, 60, 25)),
            "no-repeat paints the gradient once");

        check(render("linear-gradient(#ff8000)") == render("none"), "a single color stop is invalid");
        check(render("linear-gradient(45,#ff8000,#0040ff)") == render("none"), "a unitless angle is invalid");
        check(render("radial-gradient(circle 10px 20px,#ff8000,#0040ff)") == render("none"),
            "two radii are invalid for a circle");
        check(render("radial-gradient(50%,#ff8000,#0040ff)") == render("none"),
            "a percentage is invalid as a circle radius");
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
