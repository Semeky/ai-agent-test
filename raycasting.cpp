// raycasting.cpp - a small, dependency-free C++17 ray casting demo.
//
// The program renders a Wolfenstein-style 3D view of a 2D tile map using the
// classic DDA ray casting algorithm:
//
//   * one ray per screen column, cast across the horizontal field of view;
//   * the first wall hit gives a perpendicular distance, which becomes the
//     height of a vertical wall slice;
//   * every wall slice samples a procedural 64x64 texture, then gets distance
//     fog and side shading;
//   * a top-down view draws the map and the actual ray fan, so the algorithm
//     can be inspected directly.
//
// Run `raycasting` in a truecolor terminal for the interactive demo, or use
// `--render frame.ppm` to write a single frame.  `--selftest` checks the ray
// casting math against an independent brute-force ray march.
//
// Build:  c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic raycasting.cpp -o raycasting
// This file is intentionally self-contained: no third-party libraries needed.

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#define RAYCASTING_POSIX 1
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Small math / color helpers
// ---------------------------------------------------------------------------

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kEyeHeight = 0.5;  // camera half a wall above the floor

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

static_assert(sizeof(Color) == 3, "Color must be tightly packed");

constexpr Color makeColor(int r, int g, int b) {
    return Color{static_cast<std::uint8_t>(std::clamp(r, 0, 255)),
                 static_cast<std::uint8_t>(std::clamp(g, 0, 255)),
                 static_cast<std::uint8_t>(std::clamp(b, 0, 255))};
}

Color mix(Color from, Color to, double amount) {
    const double t = std::clamp(amount, 0.0, 1.0);
    auto channel = [t](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(std::lround(a + (b - a) * t));
    };
    return Color{channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b)};
}

Color shade(Color c, double factor) {
    const double f = std::clamp(factor, 0.0, 4.0);
    return Color{static_cast<std::uint8_t>(std::clamp(std::lround(c.r * f), 0L, 255L)),
                 static_cast<std::uint8_t>(std::clamp(std::lround(c.g * f), 0L, 255L)),
                 static_cast<std::uint8_t>(std::clamp(std::lround(c.b * f), 0L, 255L))};
}

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Deterministic value in [0, 1); used for texture grain and per-brick tones.
double noise01(int x, int y, int seed) {
    const std::uint32_t h = hash32(static_cast<std::uint32_t>(x) * 0x9e3779b1U ^
                                   static_cast<std::uint32_t>(y) * 0x85ebca77U ^
                                   static_cast<std::uint32_t>(seed) * 0xc2b2ae3dU);
    return static_cast<double>(h >> 8) / 16777216.0;
}

Vec2 directionFromDegrees(double degrees) {
    const double radians = degrees * kDegToRad;
    return Vec2{std::cos(radians), std::sin(radians)};
}

double normalizeDegrees(double degrees) {
    double result = std::fmod(degrees, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

constexpr int kMapWidth = 24;
constexpr int kMapHeight = 24;
constexpr int kTextureSize = 64;

// 0/. = empty floor, 1..5 = wall materials.  The map is a small "museum":
// a big outer hall with four colored rooms, a walled inner atrium and a
// golden fountain in the middle, entered through a doorway to the south.
const char* const kMapRows[kMapHeight] = {
    "111111111111111111111111",
    "1......................1",
    "1..44444......33333....1",
    "1..4...4......3...3....1",
    "1..4...4......3...3....1",
    "1..44444......33333....1",
    "1......................1",
    "1....11111111111111....1",
    "1....1............1....1",
    "1....1.22......22.1....1",
    "1....1.22......22.1....1",
    "1....1....5555....1....1",
    "1....1....5555....1....1",
    "1....1....5555....1....1",
    "1....1............1....1",
    "1....1............1....1",
    "1....11111....11111....1",
    "1......................1",
    "1..22222......44444....1",
    "1..2...2......4...4....1",
    "1..2...2......4...4....1",
    "1..22222......44444....1",
    "1......................1",
    "111111111111111111111111",
};

using Grid = std::array<std::array<std::uint8_t, kMapWidth>, kMapHeight>;

Grid loadMap(std::string& error) {
    Grid map{};
    for (int y = 0; y < kMapHeight; ++y) {
        const char* row = kMapRows[y];
        if (std::strlen(row) != static_cast<std::size_t>(kMapWidth)) {
            error = "map row " + std::to_string(y) + " has the wrong width";
            return Grid{};
        }
        for (int x = 0; x < kMapWidth; ++x) {
            const char tile = row[x];
            if (tile == '.') {
                map[y][x] = 0;
            } else if (tile >= '1' && tile <= '5') {
                map[y][x] = static_cast<std::uint8_t>(tile - '0');
            } else {
                error = "unknown tile '" + std::string(1, tile) + "' in map row " +
                        std::to_string(y);
                return Grid{};
            }
        }
    }
    return map;
}

bool isSolidAt(const Grid& map, double x, double y) {
    const int tileX = static_cast<int>(std::floor(x));
    const int tileY = static_cast<int>(std::floor(y));
    if (tileX < 0 || tileY < 0 || tileX >= kMapWidth || tileY >= kMapHeight) {
        return true;
    }
    return map[tileY][tileX] != 0;
}

bool isWalkableCentered(const Grid& map, Vec2 position, double radius = 0.22) {
    return !isSolidAt(map, position.x - radius, position.y - radius) &&
           !isSolidAt(map, position.x + radius, position.y - radius) &&
           !isSolidAt(map, position.x - radius, position.y + radius) &&
           !isSolidAt(map, position.x + radius, position.y + radius);
}

// ---------------------------------------------------------------------------
// Ray casting core
// ---------------------------------------------------------------------------

struct RayHit {
    double distance = 0.0;  // parameter along the (possibly unnormalized) ray
    int mapX = -1;
    int mapY = -1;
    std::uint8_t material = 0;  // 0 means no wall
    bool side = false;          // false = x-side wall, true = y-side wall
    double wallU = 0.0;         // horizontal texture coordinate in [0, 1)
};

// Classic DDA grid traversal (after Lode Vandevenne's raycasting tutorial).
// Returns the first wall encountered by origin + t * direction, where distance
// is the parameter t.  Because the camera plane is perpendicular to the view
// direction, this t is also the perpendicular wall distance used for
// projection (important: it prevents fisheye distortion).
RayHit castRay(const Grid& map, Vec2 origin, Vec2 direction) {
    RayHit hit;
    const double dirX = direction.x;
    const double dirY = direction.y;
    const double epsilon = 1e-12;

    int mapX = static_cast<int>(std::floor(origin.x));
    int mapY = static_cast<int>(std::floor(origin.y));

    const double deltaDistX = std::fabs(dirX) < epsilon ? 1e30 : std::fabs(1.0 / dirX);
    const double deltaDistY = std::fabs(dirY) < epsilon ? 1e30 : std::fabs(1.0 / dirY);

    const int stepX = dirX < 0.0 ? -1 : 1;
    const int stepY = dirY < 0.0 ? -1 : 1;

    double sideDistX = dirX < 0.0 ? (origin.x - mapX) * deltaDistX
                                  : (mapX + 1.0 - origin.x) * deltaDistX;
    double sideDistY = dirY < 0.0 ? (origin.y - mapY) * deltaDistY
                                  : (mapY + 1.0 - origin.y) * deltaDistY;

    bool side = false;
    for (int guard = 0; guard < 4 * kMapWidth * kMapHeight + 64; ++guard) {
        if (sideDistX < sideDistY) {
            sideDistX += deltaDistX;
            mapX += stepX;
            side = false;
        } else {
            sideDistY += deltaDistY;
            mapY += stepY;
            side = true;
        }

        if (mapX < 0 || mapY < 0 || mapX >= kMapWidth || mapY >= kMapHeight) {
            hit.distance = 1e30;
            return hit;
        }
        if (map[mapY][mapX] == 0) {
            continue;
        }

        const double distance =
            side ? (mapY - origin.y + (1.0 - stepY) * 0.5) / dirY
                 : (mapX - origin.x + (1.0 - stepX) * 0.5) / dirX;
        const double wallCoordinate =
            side ? origin.x + distance * dirX : origin.y + distance * dirY;
        double wallU = wallCoordinate - std::floor(wallCoordinate);
        if (wallU < 0.0) {
            wallU += 1.0;
        }
        if (wallU >= 1.0) {
            wallU -= 1.0;
        }

        hit.distance = std::max(0.0, distance);
        hit.mapX = mapX;
        hit.mapY = mapY;
        hit.material = map[mapY][mapX];
        hit.side = side;
        hit.wallU = wallU;
        return hit;
    }
    return hit;
}

// ---------------------------------------------------------------------------
// Procedural wall textures (64x64, tiling)
// ---------------------------------------------------------------------------

Color wallTexture(std::uint8_t material, int x, int y) {
    switch (material) {
        case 1: {  // red brick
            const int row = y / 16;
            const int offset = (row & 1) * 16;
            const int brickX = (x + offset) % 32;
            if (y % 16 < 2 || brickX < 2) {
                return makeColor(58, 52, 48);
            }
            const double tone = noise01((x + offset) / 32, row, 11);
            const int grain = static_cast<int>((noise01(x, y, 7) - 0.5) * 22.0);
            const int lift = static_cast<int>(tone * 28.0);
            return makeColor(140 + lift + grain, 63 + lift / 3 + grain, 49 + lift / 4 + grain);
        }
        case 2: {  // blue-grey stone blocks
            if (x % 16 < 2 || y % 16 < 2) {
                return makeColor(39, 45, 56);
            }
            const int block = (x / 16) + (y / 16) * 4;
            const double tone = noise01(block, 0, 23);
            const int grain = static_cast<int>((noise01(x, y, 31) - 0.5) * 26.0);
            const int lift = static_cast<int>(tone * 34.0);
            return makeColor(88 + lift + grain, 108 + lift + grain, 142 + lift + grain);
        }
        case 3: {  // green painted panels with a rib every 8 pixels
            if (x % 32 < 2 || y % 32 < 2) {
                return makeColor(24, 58, 42);
            }
            const double tone = noise01(x / 32, y / 32, 37);
            const int grain = static_cast<int>((noise01(x, y, 41) - 0.5) * 18.0);
            const int rib = (y % 8 < 1) ? -14 : 0;
            const int lift = static_cast<int>(tone * 28.0);
            return makeColor(47 + lift + grain + rib, 115 + lift + grain + rib,
                             82 + lift + grain + rib);
        }
        case 4: {  // warm wood planks
            const int plank = x % 8;
            if (plank < 1 || y % 32 < 1) {
                return makeColor(55, 37, 22);
            }
            const int grain =
                static_cast<int>(std::sin(y * 0.42 + x * 0.17 +
                                          noise01(x, y / 8, 53) * 6.0) *
                                 16.0);
            const int lift = static_cast<int>(noise01(x / 8, y / 32, 59) * 18.0);
            return makeColor(128 + lift + grain, 88 + lift / 2 + grain, 56 + lift / 3 + grain);
        }
        case 5: {  // golden fountain stone, ribbed and banded
            const int rib = x % 8;
            if (rib < 2) {
                return makeColor(76, 54, 18);
            }
            if (y % 16 < 2) {
                return makeColor(110, 82, 30);
            }
            const int shine = rib >= 3 && rib <= 5 ? 22 : 0;
            const int grain = static_cast<int>((noise01(x, y, 67) - 0.5) * 18.0);
            const int lift = static_cast<int>(noise01(x / 8, y / 16, 71) * 22.0);
            return makeColor(176 + lift + grain + shine, 132 + lift + grain + shine,
                             58 + shine / 2);
        }
        default:
            return makeColor(120, 120, 120);
    }
}

Color mapWallColor(std::uint8_t material) {
    switch (material) {
        case 1:
            return makeColor(179, 76, 57);
        case 2:
            return makeColor(91, 119, 158);
        case 3:
            return makeColor(68, 145, 102);
        case 4:
            return makeColor(160, 112, 64);
        case 5:
            return makeColor(204, 167, 74);
        default:
            return makeColor(110, 110, 110);
    }
}

// ---------------------------------------------------------------------------
// Image buffer and simple drawing helpers
// ---------------------------------------------------------------------------

class Image {
  public:
    Image(int width, int height)
        : width_(width),
          height_(height),
          pixels_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {}

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<Color>& pixels() const { return pixels_; }

    Color& at(int x, int y) {
        return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                       static_cast<std::size_t>(x)];
    }

    const Color& at(int x, int y) const {
        return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                       static_cast<std::size_t>(x)];
    }

    void fill(Color color) {
        std::fill(pixels_.begin(), pixels_.end(), color);
    }

    void blend(int x, int y, Color color, double alpha) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) {
            return;
        }
        at(x, y) = mix(at(x, y), color, alpha);
    }

    void fillRect(int x, int y, int width, int height, Color color, double alpha = 1.0) {
        const int x0 = std::max(0, x);
        const int y0 = std::max(0, y);
        const int x1 = std::min(width_, x + width);
        const int y1 = std::min(height_, y + height);
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                if (alpha >= 1.0) {
                    at(px, py) = color;
                } else {
                    at(px, py) = mix(at(px, py), color, alpha);
                }
            }
        }
    }

    void drawLine(int x0, int y0, int x1, int y1, Color color, double alpha = 1.0) {
        int dx = std::abs(x1 - x0);
        int dy = std::abs(y1 - y0);
        const int stepX = x0 < x1 ? 1 : -1;
        const int stepY = y0 < y1 ? 1 : -1;
        int error = dx - dy;
        for (;;) {
            blend(x0, y0, color, alpha);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int doubled = 2 * error;
            if (doubled > -dy) {
                error -= dy;
                x0 += stepX;
            }
            if (doubled < dx) {
                error += dx;
                y0 += stepY;
            }
        }
    }

    void drawCircle(int centerX, int centerY, int radius, Color color, double alpha = 1.0) {
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                if (x * x + y * y <= radius * radius) {
                    blend(centerX + x, centerY + y, color, alpha);
                }
            }
        }
    }

  private:
    int width_ = 0;
    int height_ = 0;
    std::vector<Color> pixels_;
};

const Color kFogColor = makeColor(10, 13, 19);

Color floorTexture(Vec2 world) {
    const double scaledX = world.x * 2.0;
    const double scaledY = world.y * 2.0;
    const double tileX = std::floor(scaledX);
    const double tileY = std::floor(scaledY);
    const double fractionX = scaledX - tileX;
    const double fractionY = scaledY - tileY;
    const bool alternate = (static_cast<int>(tileX) + static_cast<int>(tileY)) & 1;
    Color base = alternate ? makeColor(72, 64, 54) : makeColor(62, 56, 48);
    const int grain = static_cast<int>((noise01(static_cast<int>(scaledX * 8.0),
                                               static_cast<int>(scaledY * 8.0), 91) -
                                       0.5) *
                                      14.0);
    base = shade(base, 1.0 + grain / 255.0);
    if (fractionX < 0.025 || fractionY < 0.025) {
        base = makeColor(38, 33, 28);
    }
    return base;
}

Color ceilingTexture(Vec2 world) {
    const double tileX = std::floor(world.x);
    const double tileY = std::floor(world.y);
    const double fractionX = world.x - tileX;
    const double fractionY = world.y - tileY;

    Color base = makeColor(32, 38, 51);
    if (fractionX < 0.03 || fractionY < 0.03) {
        base = makeColor(20, 25, 35);
    }
    const int grain = static_cast<int>((noise01(static_cast<int>(world.x * 8.0),
                                               static_cast<int>(world.y * 8.0), 97) -
                                       0.5) *
                                      8.0);
    return shade(base, 1.0 + grain / 255.0);
}

// Perspective-correct floor and ceiling casting.  For every screen row the
// distance to the floor (or ceiling) plane is constant, so a single texture
// coordinate can be interpolated linearly across the row.  Walls are drawn on
// top of this background afterwards.
void renderFloorAndCeiling(Image& image, Vec2 position, double yawDegrees,
                           double fovDegrees) {
    const Vec2 direction = directionFromDegrees(yawDegrees);
    const double halfPlane = std::tan(fovDegrees * 0.5 * kDegToRad);
    const Vec2 plane{-direction.y * halfPlane, direction.x * halfPlane};
    const double projectionScale = image.width() / (2.0 * halfPlane);
    const double horizon = image.height() * 0.5;

    const double firstCameraX = 1.0 / image.width() - 1.0;
    const Vec2 firstRay{direction.x + plane.x * firstCameraX,
                        direction.y + plane.y * firstCameraX};
    const Vec2 rayStep{plane.x * (2.0 / image.width()),
                       plane.y * (2.0 / image.width())};

    for (int y = 0; y < image.height(); ++y) {
        const double offsetFromHorizon = (y + 0.5) - horizon;
        const double absoluteOffset = std::fabs(offsetFromHorizon);
        if (absoluteOffset < 1e-6) {
            image.fillRect(0, y, image.width(), 1, kFogColor);
            continue;
        }
        const bool floor = offsetFromHorizon > 0.0;
        const double rowDistance =
            projectionScale * kEyeHeight / absoluteOffset;
        double fog = 1.0 - 1.0 / (1.0 + 0.013 * rowDistance * rowDistance);
        fog = std::min(fog, 0.96);

        Vec2 world{position.x + firstRay.x * rowDistance,
                   position.y + firstRay.y * rowDistance};
        const double worldStepX = rayStep.x * rowDistance;
        const double worldStepY = rayStep.y * rowDistance;
        for (int x = 0; x < image.width(); ++x) {
            const Color texture = floor ? floorTexture(world) : ceilingTexture(world);
            image.at(x, y) = mix(texture, kFogColor, fog);
            world.x += worldStepX;
            world.y += worldStepY;
        }
    }
}

// ---------------------------------------------------------------------------
// 3D renderer
// ---------------------------------------------------------------------------

void render3D(Image& image, const Grid& map, Vec2 position, double yawDegrees,
              double fovDegrees) {
    renderFloorAndCeiling(image, position, yawDegrees, fovDegrees);

    const Vec2 direction = directionFromDegrees(yawDegrees);
    const double halfPlane = std::tan(fovDegrees * 0.5 * kDegToRad);
    const Vec2 plane{-direction.y * halfPlane, direction.x * halfPlane};
    const double projectionScale =
        image.width() / (2.0 * halfPlane);  // pixels per world unit at distance 1
    const double horizon = image.height() * 0.5;

    for (int x = 0; x < image.width(); ++x) {
        const double cameraX = 2.0 * (x + 0.5) / image.width() - 1.0;
        const Vec2 rayDirection{direction.x + plane.x * cameraX,
                                direction.y + plane.y * cameraX};
        const RayHit hit = castRay(map, position, rayDirection);
        if (hit.material == 0 || hit.distance >= 1e29) {
            continue;
        }

        const double wallHeight = projectionScale / std::max(hit.distance, 1e-9);
        const double drawStart = horizon - wallHeight * 0.5;
        const double drawEnd = drawStart + wallHeight;
        const double maxY = static_cast<double>(image.height() - 1);
        const int firstY =
            static_cast<int>(std::clamp(std::floor(drawStart), 0.0, maxY));
        const int lastY =
            static_cast<int>(std::clamp(std::ceil(drawEnd), 0.0, maxY));
        if (firstY > lastY) {
            continue;
        }

        int textureX = static_cast<int>(hit.wallU * kTextureSize);
        textureX = std::clamp(textureX, 0, kTextureSize - 1);
        if ((!hit.side && rayDirection.x > 0.0) || (hit.side && rayDirection.y < 0.0)) {
            textureX = kTextureSize - 1 - textureX;
        }

        const double physicalDistance =
            hit.distance * std::sqrt(rayDirection.x * rayDirection.x +
                                     rayDirection.y * rayDirection.y);
        double fog = 1.0 - 1.0 / (1.0 + 0.014 * physicalDistance * physicalDistance);
        fog = std::min(fog, 0.84);
        const double light = hit.side ? 0.78 : 1.05;

        for (int y = firstY; y <= lastY; ++y) {
            const double textureV =
                std::clamp((y + 0.5 - drawStart) / wallHeight, 0.0, 1.0);
            const int textureY =
                std::clamp(static_cast<int>(textureV * kTextureSize), 0, kTextureSize - 1);
            Color color = wallTexture(hit.material, textureX, textureY);
            color = shade(color, light);
            color = mix(color, kFogColor, fog);
            image.at(x, y) = color;
        }
    }
}

// ---------------------------------------------------------------------------
// Top-down map and ray fan (used for the map view and the minimap)
// ---------------------------------------------------------------------------

void paintMapCells(Image& image, const Grid& map, int offsetX, int offsetY, int scale,
                   bool gridLines) {
    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            const int pixelX = offsetX + x * scale;
            const int pixelY = offsetY + y * scale;
            if (map[y][x] != 0) {
                const Color base = mapWallColor(map[y][x]);
                image.fillRect(pixelX, pixelY, scale, scale, base);
                if (scale >= 4) {
                    image.fillRect(pixelX, pixelY, scale, 1, mix(base, makeColor(255, 255, 255), 0.18));
                    image.fillRect(pixelX, pixelY + scale - 1, scale, 1, shade(base, 0.65));
                    image.fillRect(pixelX, pixelY, 1, scale, mix(base, makeColor(255, 255, 255), 0.10));
                    image.fillRect(pixelX + scale - 1, pixelY, 1, scale, shade(base, 0.72));
                }
            } else {
                const Color floorColor = ((x + y) & 1) ? makeColor(19, 23, 30)
                                                       : makeColor(23, 27, 34);
                image.fillRect(pixelX, pixelY, scale, scale, floorColor);
                if (gridLines && scale >= 6) {
                    image.fillRect(pixelX, pixelY, 1, scale, makeColor(14, 17, 23));
                    image.fillRect(pixelX, pixelY, scale, 1, makeColor(14, 17, 23));
                }
            }
        }
    }
}

void paintRayFan(Image& image, const Grid& map, Vec2 position, double yawDegrees,
                 double fovDegrees, int offsetX, int offsetY, int scale, int rayCount,
                 bool highlightHits) {
    const Vec2 direction = directionFromDegrees(yawDegrees);
    const double halfPlane = std::tan(fovDegrees * 0.5 * kDegToRad);
    const Vec2 plane{-direction.y * halfPlane, direction.x * halfPlane};
    const double startX = offsetX + position.x * scale;
    const double startY = offsetY + position.y * scale;

    std::array<std::array<int, kMapWidth>, kMapHeight> hitCounts{};
    for (int i = 0; i < rayCount; ++i) {
        const double cameraX = rayCount == 1 ? 0.0
                                              : 2.0 * (i + 0.5) / rayCount - 1.0;
        const Vec2 rayDirection{direction.x + plane.x * cameraX,
                                direction.y + plane.y * cameraX};
        const RayHit hit = castRay(map, position, rayDirection);
        if (hit.material == 0) {
            continue;
        }
        const double endX = offsetX + (position.x + rayDirection.x * hit.distance) * scale;
        const double endY = offsetY + (position.y + rayDirection.y * hit.distance) * scale;
        image.drawLine(static_cast<int>(std::lround(startX)),
                       static_cast<int>(std::lround(startY)),
                       static_cast<int>(std::lround(endX)),
                       static_cast<int>(std::lround(endY)),
                       makeColor(255, 190, 86), 0.22);
        if (highlightHits) {
            ++hitCounts[hit.mapY][hit.mapX];
        }
    }

    if (highlightHits) {
        for (int y = 0; y < kMapHeight; ++y) {
            for (int x = 0; x < kMapWidth; ++x) {
                if (hitCounts[y][x] == 0) {
                    continue;
                }
                const double alpha = std::min(0.42, 0.05 * hitCounts[y][x]);
                image.fillRect(offsetX + x * scale, offsetY + y * scale, scale, scale,
                               makeColor(255, 196, 96), alpha);
            }
        }
    }
}

void paintPlayer(Image& image, Vec2 position, double yawDegrees, int offsetX, int offsetY,
                 int scale) {
    const double centerX = offsetX + position.x * scale;
    const double centerY = offsetY + position.y * scale;
    const Vec2 direction = directionFromDegrees(yawDegrees);
    const int radius = std::max(3, scale / 3);
    image.drawCircle(static_cast<int>(std::lround(centerX)),
                     static_cast<int>(std::lround(centerY)), radius + 1,
                     makeColor(10, 12, 16), 0.9);
    image.drawCircle(static_cast<int>(std::lround(centerX)),
                     static_cast<int>(std::lround(centerY)), radius,
                     makeColor(245, 245, 245), 1.0);
    image.drawLine(static_cast<int>(std::lround(centerX)),
                   static_cast<int>(std::lround(centerY)),
                   static_cast<int>(std::lround(centerX + direction.x * scale * 1.5)),
                   static_cast<int>(std::lround(centerY + direction.y * scale * 1.5)),
                   makeColor(255, 238, 190), 0.95);
}

void renderMinimap(Image& image, const Grid& map, Vec2 position, double yawDegrees,
                   double fovDegrees) {
    const int scale = std::clamp(std::min(image.width(), image.height()) / 70, 2, 10);
    const int offsetX = 12;
    const int offsetY = 12;
    const int mapWidth = kMapWidth * scale;
    const int mapHeight = kMapHeight * scale;

    image.fillRect(offsetX - 4, offsetY - 4, mapWidth + 8, mapHeight + 8,
                   makeColor(3, 5, 8), 0.62);
    image.fillRect(offsetX - 1, offsetY - 1, mapWidth + 2, 1, makeColor(90, 105, 128), 0.8);
    image.fillRect(offsetX - 1, offsetY + mapHeight, mapWidth + 2, 1,
                   makeColor(90, 105, 128), 0.8);
    image.fillRect(offsetX - 1, offsetY - 1, 1, mapHeight + 2, makeColor(90, 105, 128), 0.8);
    image.fillRect(offsetX + mapWidth, offsetY - 1, 1, mapHeight + 2,
                   makeColor(90, 105, 128), 0.8);

    paintMapCells(image, map, offsetX, offsetY, scale, false);
    paintRayFan(image, map, position, yawDegrees, fovDegrees, offsetX, offsetY, scale,
                std::max(24, 3 * kMapWidth), true);
    paintPlayer(image, position, yawDegrees, offsetX, offsetY, scale);
}

void renderMapView(Image& image, const Grid& map, Vec2 position, double yawDegrees,
                   double fovDegrees) {
    image.fill(makeColor(8, 10, 14));

    const int margin = std::max(8, std::min(image.width(), image.height()) / 24);
    const int scaleX = (image.width() - 2 * margin) / kMapWidth;
    const int scaleY = (image.height() - 2 * margin) / kMapHeight;
    const int scale = std::max(1, std::min(scaleX, scaleY));
    const int mapWidth = kMapWidth * scale;
    const int mapHeight = kMapHeight * scale;
    const int offsetX = (image.width() - mapWidth) / 2;
    const int offsetY = (image.height() - mapHeight) / 2;

    const int padding = std::max(4, scale);
    image.fillRect(offsetX - padding, offsetY - padding, mapWidth + 2 * padding,
                   mapHeight + 2 * padding, makeColor(28, 34, 44), 0.55);

    paintMapCells(image, map, offsetX, offsetY, scale, true);
    paintRayFan(image, map, position, yawDegrees, fovDegrees, offsetX, offsetY, scale,
                std::max(64, image.width() / 6), true);
    paintPlayer(image, position, yawDegrees, offsetX, offsetY, scale);
}

// ---------------------------------------------------------------------------
// PPM output
// ---------------------------------------------------------------------------

bool writePPM(const std::string& path, const Image& image) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", image.width(), image.height());
    const std::size_t bytes = image.pixels().size() * sizeof(Color);
    const bool written =
        std::fwrite(image.pixels().data(), 1, bytes, file) == bytes;
    if (std::fclose(file) != 0) {
        return false;
    }
    return written;
}

// ---------------------------------------------------------------------------
// Command line options
// ---------------------------------------------------------------------------

constexpr Vec2 kSpawn{6.5, 14.5};
constexpr double kSpawnYaw = 330.0;  // east-north-east (0 = east, 90 = south)
constexpr double kDefaultFov = 75.0;

struct Options {
    bool help = false;
    bool selftest = false;
    bool mapView = false;
    bool minimap = false;
    Vec2 position = kSpawn;
    double yawDegrees = kSpawnYaw;
    double fovDegrees = kDefaultFov;
    int width = 960;
    int height = 540;
    int fps = 30;
    std::string renderPath;
};

void printUsage(std::FILE* stream) {
    std::fprintf(stream,
                 "Ray casting demo (C++17)\n"
                 "\n"
                 "Usage:\n"
                 "  raycasting                      interactive demo (needs a terminal)\n"
                 "  raycasting --render frame.ppm   render one PPM frame\n"
                 "  raycasting --selftest           run built-in checks\n"
                 "\n"
                 "Options:\n"
                 "  --render <file.ppm>  write a binary PPM frame and exit\n"
                 "  --map                show the top-down map with the ray fan\n"
                 "  --minimap            start with the minimap enabled\n"
                 "  --width <px>         render width (default 960)\n"
                 "  --height <px>        render height (default 540)\n"
                 "  --fov <deg>          horizontal field of view (default 75)\n"
                 "  --pos <x>,<y>        start position (default 6.5,14.5)\n"
                 "  --angle <deg>        start angle, 0=east 90=south (default 330)\n"
                 "  --fps <n>            interactive frame cap (default 30)\n"
                 "  -h, --help           show this help\n"
                 "\n"
                 "Interactive controls:\n"
                 "  W/S or Up/Down  move forward/back    A/D  strafe\n"
                 "  Left/Right      turn                 Tab  top-down ray view\n"
                 "  M               minimap              Q/Esc  quit\n");
}

bool parseInt(const char* text, int& value) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < -2147483647L ||
        parsed > 2147483647L) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parseDouble(const char* text, double& value) {
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parsePosition(const char* text, Vec2& position) {
    char* endX = nullptr;
    const double x = std::strtod(text, &endX);
    if (endX == text || *endX != ',') {
        return false;
    }
    char* endY = nullptr;
    const double y = std::strtod(endX + 1, &endY);
    if (endY == endX + 1 || *endY != '\0' || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    position = Vec2{x, y};
    return true;
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto nextValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                error = std::string("missing value for ") + name;
                return nullptr;
            }
            ++i;
            return argv[i];
        };

        if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "--selftest") {
            options.selftest = true;
        } else if (argument == "--map") {
            options.mapView = true;
        } else if (argument == "--minimap") {
            options.minimap = true;
        } else if (argument == "--render") {
            const char* value = nextValue("--render");
            if (value == nullptr) {
                return false;
            }
            options.renderPath = value;
        } else if (argument == "--width" || argument == "--height" || argument == "--fps" ||
                   argument == "--fov" || argument == "--pos" || argument == "--angle") {
            const char* value = nextValue(argument.c_str());
            if (value == nullptr) {
                return false;
            }
            if (argument == "--width") {
                if (!parseInt(value, options.width)) {
                    error = "invalid width: " + std::string(value);
                    return false;
                }
            } else if (argument == "--height") {
                if (!parseInt(value, options.height)) {
                    error = "invalid height: " + std::string(value);
                    return false;
                }
            } else if (argument == "--fps") {
                if (!parseInt(value, options.fps)) {
                    error = "invalid fps: " + std::string(value);
                    return false;
                }
            } else if (argument == "--fov") {
                if (!parseDouble(value, options.fovDegrees)) {
                    error = "invalid fov: " + std::string(value);
                    return false;
                }
            } else if (argument == "--pos") {
                if (!parsePosition(value, options.position)) {
                    error = "invalid position (expected x,y): " + std::string(value);
                    return false;
                }
            } else if (argument == "--angle") {
                if (!parseDouble(value, options.yawDegrees)) {
                    error = "invalid angle: " + std::string(value);
                    return false;
                }
                options.yawDegrees = normalizeDegrees(options.yawDegrees);
            }
        } else {
            error = "unknown option: " + argument;
            return false;
        }
    }

    if (options.width < 16 || options.width > 4096) {
        error = "--width must be between 16 and 4096";
        return false;
    }
    if (options.height < 16 || options.height > 4096) {
        error = "--height must be between 16 and 4096";
        return false;
    }
    if (options.fps < 1 || options.fps > 120) {
        error = "--fps must be between 1 and 120";
        return false;
    }
    if (!(options.fovDegrees >= 30.0 && options.fovDegrees <= 120.0)) {
        error = "--fov must be between 30 and 120 degrees";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Selftest: compare DDA casting with an independent brute-force ray march
// ---------------------------------------------------------------------------

bool selftestMap(const Grid& map, std::string& error) {
    for (int x = 0; x < kMapWidth; ++x) {
        if (map[0][x] == 0 || map[kMapHeight - 1][x] == 0) {
            error = "map border is not closed";
            return false;
        }
    }
    for (int y = 0; y < kMapHeight; ++y) {
        if (map[y][0] == 0 || map[y][kMapWidth - 1] == 0) {
            error = "map border is not closed";
            return false;
        }
    }
    std::array<int, 6> materialCounts{};
    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            ++materialCounts[map[y][x]];
        }
    }
    for (int material = 1; material <= 5; ++material) {
        if (materialCounts[static_cast<std::size_t>(material)] == 0) {
            error = "map does not contain material " + std::to_string(material);
            return false;
        }
    }
    if (!isWalkableCentered(map, kSpawn)) {
        error = "spawn point is not walkable";
        return false;
    }
    return true;
}

bool selftestRays(const Grid& map, std::string& error) {
    std::vector<std::pair<int, int>> openCells;
    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            if (map[y][x] == 0) {
                openCells.emplace_back(x, y);
            }
        }
    }
    if (openCells.empty()) {
        error = "map has no open cells";
        return false;
    }

    std::mt19937 rng(0x5eed1234U);
    std::uniform_real_distribution<double> offsetDistribution(0.2, 0.8);
    std::uniform_real_distribution<double> angleDistribution(0.0, 2.0 * kPi);
    constexpr double kStep = 0.002;
    constexpr double kMaxDistance = 100.0;

    auto checkRay = [&](Vec2 origin, Vec2 direction, bool compareMarch, std::string& why) {
        const RayHit hit = castRay(map, origin, direction);
        if (hit.material == 0) {
            why = "ray did not hit a wall";
            return false;
        }
        if (hit.wallU < 0.0 || hit.wallU >= 1.0) {
            why = "wallU is outside [0, 1)";
            return false;
        }
        if (!(hit.distance < 1e29)) {
            why = "ray returned an infinite distance";
            return false;
        }

        // A point just past the hit must be inside the reported tile.
        const double probe = 1e-6;
        const int tileX = static_cast<int>(std::floor(origin.x + direction.x * (hit.distance + probe)));
        const int tileY = static_cast<int>(std::floor(origin.y + direction.y * (hit.distance + probe)));
        if (tileX != hit.mapX || tileY != hit.mapY) {
            why = "hit tile does not contain the hit point";
            return false;
        }

        // The wall face must be axis aligned: the hit point lies on a grid line.
        const double hitX = origin.x + direction.x * hit.distance;
        const double hitY = origin.y + direction.y * hit.distance;
        if (!hit.side) {
            const double fractional = std::fabs(hitX - std::round(hitX));
            if (fractional > 1e-6) {
                why = "x-side hit is not on a vertical grid line";
                return false;
            }
        } else {
            const double fractional = std::fabs(hitY - std::round(hitY));
            if (fractional > 1e-6) {
                why = "y-side hit is not on a horizontal grid line";
                return false;
            }
        }

        if (!compareMarch) {
            return true;
        }

        // Independent brute-force march: the first sample inside a wall must
        // bracket the DDA distance by at most one march step.
        double marched = -1.0;
        for (double t = 0.0; t < kMaxDistance; t += kStep) {
            const double x = origin.x + direction.x * t;
            const double y = origin.y + direction.y * t;
            const int mx = static_cast<int>(std::floor(x));
            const int my = static_cast<int>(std::floor(y));
            if (mx < 0 || my < 0 || mx >= kMapWidth || my >= kMapHeight) {
                break;
            }
            if (map[my][mx] != 0) {
                marched = t;
                break;
            }
        }
        if (marched < 0.0) {
            why = "brute-force march found no wall";
            return false;
        }
        constexpr double kTolerance = 1e-6;
        if (hit.distance > marched + kTolerance ||
            hit.distance < marched - kStep - kTolerance) {
            why = "DDA distance does not match brute-force march (dda=" +
                  std::to_string(hit.distance) + ", march=" + std::to_string(marched) + ")";
            return false;
        }
        return true;
    };

    std::string failure;
    for (int i = 0; i < 1200; ++i) {
        const auto& cell = openCells[static_cast<std::size_t>(rng()) % openCells.size()];
        const Vec2 origin{cell.first + offsetDistribution(rng),
                          cell.second + offsetDistribution(rng)};
        const double angle = angleDistribution(rng);
        if (!checkRay(origin, Vec2{std::cos(angle), std::sin(angle)}, true, failure)) {
            error = "random ray " + std::to_string(i) + ": " + failure;
            return false;
        }
    }

    // Axis-aligned rays make the delta-distance code take its zero-division
    // guard, so exercise those explicitly too.
    for (const double angle : {0.0, 90.0, 180.0, 270.0}) {
        const Vec2 direction = directionFromDegrees(angle);
        for (int i = 0; i < 24; ++i) {
            const auto& cell = openCells[static_cast<std::size_t>(rng()) % openCells.size()];
            const Vec2 origin{cell.first + 0.5, cell.second + 0.5};
            if (!checkRay(origin, direction, true, failure)) {
                error = "axis ray at " + std::to_string(angle) + " degrees: " + failure;
                return false;
            }
        }
    }
    return true;
}

bool selftestPPM(std::string& error) {
    const std::string path = "raycasting-selftest.ppm";
    Image image(5, 3);
    image.fill(makeColor(10, 20, 30));
    image.at(1, 1) = makeColor(200, 100, 50);
    if (!writePPM(path, image)) {
        error = "could not write a PPM test file";
        return false;
    }

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "could not reopen the PPM test file";
        return false;
    }
    char header[64] = {};
    const std::size_t headerLength = std::fread(header, 1, sizeof(header) - 1, file);
    std::fseek(file, 0, SEEK_END);
    const long fileSize = std::ftell(file);
    std::fclose(file);
    std::remove(path.c_str());

    const std::string expectedHeader = "P6\n5 3\n255\n";
    if (headerLength < expectedHeader.size() ||
        std::string(header, expectedHeader.size()) != expectedHeader) {
        error = "PPM header is malformed";
        return false;
    }
    const long expectedSize = static_cast<long>(expectedHeader.size()) + 5L * 3L * 3L;
    if (fileSize != expectedSize) {
        error = "PPM file size is " + std::to_string(fileSize) + ", expected " +
                std::to_string(expectedSize);
        return false;
    }
    return true;
}

bool runSelftest(const Grid& map) {
    std::string error;
    int failures = 0;
    std::printf("Ray casting demo selftest\n");

    if (selftestMap(map, error)) {
        std::printf("  [ok] map: 24x24 border, five materials, spawn walkable\n");
    } else {
        std::printf("  [FAIL] map: %s\n", error.c_str());
        ++failures;
    }
    if (selftestRays(map, error)) {
        std::printf("  [ok] ray casting: 1200 random rays match brute-force march\n");
        std::printf("  [ok] axis-aligned rays handle infinite delta distances\n");
    } else {
        std::printf("  [FAIL] ray casting: %s\n", error.c_str());
        ++failures;
    }
    if (selftestPPM(error)) {
        std::printf("  [ok] PPM writer: header and byte count\n");
    } else {
        std::printf("  [FAIL] PPM writer: %s\n", error.c_str());
        ++failures;
    }

    if (failures == 0) {
        std::printf("All tests passed.\n");
        return true;
    }
    std::printf("%d test(s) failed.\n", failures);
    return false;
}

// ---------------------------------------------------------------------------
// Terminal interaction (POSIX)
// ---------------------------------------------------------------------------

#ifdef RAYCASTING_POSIX

constexpr int kKeyUp = 1001;
constexpr int kKeyDown = 1002;
constexpr int kKeyLeft = 1003;
constexpr int kKeyRight = 1004;

void writeAll(int fileDescriptor, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fileDescriptor, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }
        if (written == 0) {
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
}

void writeAll(int fileDescriptor, const std::string& text) {
    writeAll(fileDescriptor, text.data(), text.size());
}

class RawTerminal {
  public:
    RawTerminal() {
        if (!isatty(STDIN_FILENO)) {
            return;
        }
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
            return;
        }
        termios raw = saved_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            return;
        }
        const char hideCursor[] = "\x1b[?25l";
        writeAll(STDOUT_FILENO, hideCursor, sizeof(hideCursor) - 1);
        active_ = true;
    }

    ~RawTerminal() { restore(); }

    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;

    bool active() const { return active_; }

    void restore() {
        if (!active_) {
            return;
        }
        const char showCursor[] = "\x1b[0m\x1b[?25h";
        writeAll(STDOUT_FILENO, showCursor, sizeof(showCursor) - 1);
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        active_ = false;
    }

    int readKey() {
        const int first = readByte();
        if (first < 0) {
            return -1;
        }
        if (first == 27) {
            // Some terminals deliver the ESC byte of an arrow-key sequence
            // separately.  Wait briefly before treating a lone ESC as quit.
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(STDIN_FILENO, &readSet);
            timeval timeout{};
            timeout.tv_usec = 15000;
            if (::select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) <= 0) {
                return 27;
            }
            const int second = readByte();
            if (second < 0) {
                return 27;
            }
            if (second != '[') {
                return -1;
            }
            const int third = readByte();
            if (third < 0) {
                return -1;
            }
            switch (third) {
                case 'A':
                    return kKeyUp;
                case 'B':
                    return kKeyDown;
                case 'C':
                    return kKeyRight;
                case 'D':
                    return kKeyLeft;
                default:
                    return -1;
            }
        }
        return first;
    }

  private:
    int readByte() {
        unsigned char byte = 0;
        ssize_t result = 0;
        do {
            result = ::read(STDIN_FILENO, &byte, 1);
        } while (result < 0 && errno == EINTR);
        return result == 1 ? static_cast<int>(byte) : -1;
    }

    termios saved_{};
    bool active_ = false;
};

std::pair<int, int> terminalSize() {
    winsize window{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0 &&
        window.ws_row > 0) {
        return {std::max(20, static_cast<int>(window.ws_col)),
                std::max(6, static_cast<int>(window.ws_row))};
    }
    return {100, 30};
}

std::string renderTerminalFrame(const Image& image) {
    std::string output;
    output.reserve(static_cast<std::size_t>(image.width()) *
                   static_cast<std::size_t>(image.height()) * 20);
    output += "\x1b[H";
    for (int y = 0; y + 1 < image.height(); y += 2) {
        for (int x = 0; x < image.width(); ++x) {
            const Color top = image.at(x, y);
            const Color bottom = image.at(x, y + 1);
            char buffer[64];
            const int length = std::snprintf(
                buffer, sizeof(buffer),
                "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80",
                static_cast<int>(top.r), static_cast<int>(top.g),
                static_cast<int>(top.b), static_cast<int>(bottom.r),
                static_cast<int>(bottom.g), static_cast<int>(bottom.b));
            if (length > 0) {
                output.append(buffer, static_cast<std::size_t>(length));
            }
        }
        output += "\x1b[0m\r\n";
    }
    return output;
}

void tryMove(const Grid& map, Vec2& position, Vec2 delta) {
    if (isWalkableCentered(map, Vec2{position.x + delta.x, position.y})) {
        position.x += delta.x;
    }
    if (isWalkableCentered(map, Vec2{position.x, position.y + delta.y})) {
        position.y += delta.y;
    }
}

int runInteractive(const Grid& map, Options options) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr,
                     "Interactive mode needs a terminal.\n"
                     "Use --render <file.ppm> or --selftest for non-interactive use.\n");
        return 1;
    }

    RawTerminal terminal;
    if (!terminal.active()) {
        std::fprintf(stderr, "Could not switch the terminal to raw mode.\n");
        return 1;
    }

    writeAll(STDOUT_FILENO, std::string("\x1b[2J\x1b[H"));

    Vec2 position = options.position;
    double yaw = options.yawDegrees;
    bool mapMode = options.mapView;
    bool minimap = options.minimap;
    bool running = true;

    double forwardIntent = 0.0;
    double strafeIntent = 0.0;
    double turnIntent = 0.0;
    double displayedFps = 0.0;
    auto previousFrame = std::chrono::steady_clock::now();

    while (running) {
        for (;;) {
            const int key = terminal.readKey();
            if (key < 0) {
                break;
            }
            switch (key) {
                case 'w':
                case 'W':
                case kKeyUp:
                    forwardIntent = 0.16;
                    break;
                case 's':
                case 'S':
                case kKeyDown:
                    forwardIntent = -0.16;
                    break;
                case 'a':
                case 'A':
                    strafeIntent = -0.13;
                    break;
                case 'd':
                case 'D':
                    strafeIntent = 0.13;
                    break;
                case kKeyLeft:
                    turnIntent = -0.14;
                    break;
                case kKeyRight:
                    turnIntent = 0.14;
                    break;
                case '\t':
                    mapMode = !mapMode;
                    break;
                case 'm':
                case 'M':
                    minimap = !minimap;
                    break;
                case 'q':
                case 'Q':
                case 27:
                case 3:  // Ctrl-C (ISIG is disabled in raw mode)
                    running = false;
                    break;
                default:
                    break;
            }
        }

        if (!running) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const double deltaSeconds =
            std::min(0.1, std::chrono::duration<double>(now - previousFrame).count());
        previousFrame = now;
        if (deltaSeconds > 1e-6) {
            displayedFps = displayedFps == 0.0
                               ? 1.0 / deltaSeconds
                               : displayedFps * 0.9 + (1.0 / deltaSeconds) * 0.1;
        }

        const double forwardStep = std::clamp(forwardIntent, -deltaSeconds, deltaSeconds);
        forwardIntent -= forwardStep;
        const double strafeStep = std::clamp(strafeIntent, -deltaSeconds, deltaSeconds);
        strafeIntent -= strafeStep;
        const double turnStep = std::clamp(turnIntent, -deltaSeconds, deltaSeconds);
        turnIntent -= turnStep;

        const Vec2 direction = directionFromDegrees(yaw);
        if (std::fabs(forwardStep) > 0.0) {
            const double amount = forwardStep * 2.6;
            tryMove(map, position, Vec2{direction.x * amount, direction.y * amount});
        }
        if (std::fabs(strafeStep) > 0.0) {
            const double amount = strafeStep * 2.1;
            tryMove(map, position, Vec2{-direction.y * amount, direction.x * amount});
        }
        if (std::fabs(turnStep) > 0.0) {
            yaw = normalizeDegrees(yaw + turnStep * 110.0);
        }

        const auto [columns, rows] = terminalSize();
        const int width = std::clamp(columns, 20, 400);
        const int sceneRows = std::clamp(rows - 1, 4, 200);
        const int height = sceneRows * 2;
        Image image(width, height);
        if (mapMode) {
            renderMapView(image, map, position, yaw, options.fovDegrees);
        } else {
            render3D(image, map, position, yaw, options.fovDegrees);
            if (minimap) {
                renderMinimap(image, map, position, yaw, options.fovDegrees);
            }
        }

        std::string output = renderTerminalFrame(image);
        char status[512];
        std::snprintf(status, sizeof(status),
                      "  raycasting | x=%.1f y=%.1f yaw=%.0f | %s | fps=%.0f | WASD move, "
                      "Left/Right turn | Tab rays | M minimap | Q quit",
                      position.x, position.y, normalizeDegrees(yaw),
                      mapMode ? "top-down ray view" : "3D view", displayedFps);
        std::string statusLine = status;
        if (static_cast<int>(statusLine.size()) > columns) {
            statusLine.resize(static_cast<std::size_t>(columns));
        }
        statusLine.resize(static_cast<std::size_t>(columns), ' ');
        output += "\x1b[0m\x1b[38;2;150;160;175m";
        output += statusLine;
        writeAll(STDOUT_FILENO, output);

        const auto frameEnd = std::chrono::steady_clock::now();
        const auto frameDuration = std::chrono::duration<double>(frameEnd - now);
        const auto budget = std::chrono::duration<double>(1.0 / options.fps);
        if (frameDuration < budget) {
            std::this_thread::sleep_for(budget - frameDuration);
        }
    }

    terminal.restore();
    writeAll(STDOUT_FILENO, std::string("\x1b[0m\x1b[?25h\x1b[2J\x1b[H"));
    std::printf("Ray casting demo closed (x=%.1f, y=%.1f, yaw=%.0f).\n", position.x,
                position.y, normalizeDegrees(yaw));
    return 0;
}

#else  // !RAYCASTING_POSIX

int runInteractive(const Grid&, Options) {
    std::fprintf(stderr,
                 "Interactive mode is only available on POSIX terminals.\n"
                 "Use --render <file.ppm> or --selftest instead.\n");
    return 1;
}

#endif  // RAYCASTING_POSIX

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        std::fprintf(stderr, "error: %s\n\n", error.c_str());
        printUsage(stderr);
        return 1;
    }
    if (options.help) {
        printUsage(stdout);
        return 0;
    }

    const Grid map = loadMap(error);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    if (options.selftest) {
        return runSelftest(map) ? 0 : 1;
    }

    if (!isWalkableCentered(map, options.position)) {
        std::fprintf(stderr, "error: start position %.2f,%.2f is not walkable\n",
                     options.position.x, options.position.y);
        return 1;
    }

    if (!options.renderPath.empty()) {
        Image image(options.width, options.height);
        if (options.mapView) {
            renderMapView(image, map, options.position, options.yawDegrees,
                          options.fovDegrees);
        } else {
            render3D(image, map, options.position, options.yawDegrees,
                     options.fovDegrees);
            if (options.minimap) {
                renderMinimap(image, map, options.position, options.yawDegrees,
                              options.fovDegrees);
            }
        }
        if (!writePPM(options.renderPath, image)) {
            std::fprintf(stderr, "error: could not write %s\n",
                         options.renderPath.c_str());
            return 1;
        }
        std::printf("Rendered %dx%d frame to %s\n", options.width, options.height,
                    options.renderPath.c_str());
        return 0;
    }

    return runInteractive(map, options);
}
