#include "renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace g3d {
namespace {

int wrapIndex(int value, int size) {
    if (size <= 0) {
        return 0;
    }
    const int result = value % size;
    return result < 0 ? result + size : result;
}

struct ViewVertex {
    Vec3 view;
    Vec3 world;
    Vec3 normal;
    Vec2 uv;
};

struct ScreenVertex {
    Vec3 world;
    Vec3 normal;
    Vec2 uv;
    double x = 0.0;
    double y = 0.0;
    double invW = 0.0;
};

double edgeFunction(double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

ViewVertex interpolateViewVertex(const ViewVertex& a, const ViewVertex& b, double t) {
    ViewVertex result;
    result.view = a.view + (b.view - a.view) * t;
    result.world = a.world + (b.world - a.world) * t;
    result.normal = a.normal + (b.normal - a.normal) * t;
    result.uv = a.uv + (b.uv - a.uv) * t;
    return result;
}

void clipAgainstNearPlane(const std::vector<ViewVertex>& input, double nearPlane,
                          std::vector<ViewVertex>& output) {
    output.clear();
    if (input.empty()) {
        return;
    }
    const auto inside = [nearPlane](const ViewVertex& vertex) {
        return vertex.view.z <= -nearPlane;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        const ViewVertex& current = input[i];
        const ViewVertex& next = input[(i + 1) % input.size()];
        const bool currentInside = inside(current);
        const bool nextInside = inside(next);
        if (currentInside) {
            output.push_back(current);
        }
        if (currentInside != nextInside) {
            const double denominator = next.view.z - current.view.z;
            if (std::fabs(denominator) > 1e-15) {
                const double t = (-nearPlane - current.view.z) / denominator;
                output.push_back(interpolateViewVertex(current, next, t));
            }
        }
    }
}

}  // namespace

Color mix(Color from, Color to, double amount) {
    const double t = clampd(amount, 0.0, 1.0);
    return makeColor(static_cast<int>(std::lround(from.r + (to.r - from.r) * t)),
                     static_cast<int>(std::lround(from.g + (to.g - from.g) * t)),
                     static_cast<int>(std::lround(from.b + (to.b - from.b) * t)));
}

Color shade(Color color, double factor) {
    return makeColor(static_cast<int>(std::lround(color.r * factor)),
                     static_cast<int>(std::lround(color.g * factor)),
                     static_cast<int>(std::lround(color.b * factor)));
}

Color multiply(Color a, Color b) {
    return makeColor((static_cast<int>(a.r) * static_cast<int>(b.r) + 127) / 255,
                     (static_cast<int>(a.g) * static_cast<int>(b.g) + 127) / 255,
                     (static_cast<int>(a.b) * static_cast<int>(b.b) + 127) / 255);
}

Image::Image(int width, int height)
    : width_(std::max(1, width)),
      height_(std::max(1, height)),
      pixels_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)) {}

Color& Image::at(int x, int y) {
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                   static_cast<std::size_t>(x)];
}

const Color& Image::at(int x, int y) const {
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                   static_cast<std::size_t>(x)];
}

void Image::fill(Color color) { std::fill(pixels_.begin(), pixels_.end(), color); }

void Image::fillVerticalGradient(Color top, Color bottom) {
    for (int y = 0; y < height_; ++y) {
        const double t = height_ > 1 ? static_cast<double>(y) / (height_ - 1) : 0.0;
        const Color row = mix(top, bottom, t);
        for (int x = 0; x < width_; ++x) {
            at(x, y) = row;
        }
    }
}

void Image::blend(int x, int y, Color color, double alpha) {
    if (!contains(x, y)) {
        return;
    }
    at(x, y) = mix(at(x, y), color, alpha);
}

void Image::fillRect(int x, int y, int width, int height, Color color, double alpha) {
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

void Image::drawLine(int x0, int y0, int x1, int y1, Color color, double alpha) {
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

void Image::drawCircle(int centerX, int centerY, int radius, Color color, double alpha) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                blend(centerX + x, centerY + y, color, alpha);
            }
        }
    }
}

bool writePPM(const std::string& path, const Image& image) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", image.width(), image.height());
    const std::size_t bytes = image.pixels().size() * sizeof(Color);
    const bool written = std::fwrite(image.pixels().data(), 1, bytes, file) == bytes;
    if (std::fclose(file) != 0) {
        return false;
    }
    return written;
}

Texture::Texture(int width, int height, std::vector<Color> pixels)
    : width_(std::max(1, width)), height_(std::max(1, height)) {
    const std::size_t expected =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    pixels.resize(expected, makeColor(128, 128, 128));
    Level base;
    base.width = width_;
    base.height = height_;
    base.pixels = std::move(pixels);
    levels_.push_back(std::move(base));
    while (levels_.back().width > 1 || levels_.back().height > 1) {
        const Level& source = levels_.back();
        const int nextWidth = std::max(1, source.width / 2);
        const int nextHeight = std::max(1, source.height / 2);
        Level next;
        next.width = nextWidth;
        next.height = nextHeight;
        next.pixels.resize(static_cast<std::size_t>(nextWidth) *
                           static_cast<std::size_t>(nextHeight));
        for (int y = 0; y < nextHeight; ++y) {
            for (int x = 0; x < nextWidth; ++x) {
                int red = 0;
                int green = 0;
                int blue = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(source.width - 1, x * 2 + dx);
                        const int sy = std::min(source.height - 1, y * 2 + dy);
                        const Color& color = source.pixels[static_cast<std::size_t>(sy) *
                                                               static_cast<std::size_t>(
                                                                   source.width) +
                                                           static_cast<std::size_t>(sx)];
                        red += color.r;
                        green += color.g;
                        blue += color.b;
                    }
                }
                next.pixels[static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(nextWidth) +
                            static_cast<std::size_t>(x)] =
                    makeColor((red + 2) / 4, (green + 2) / 4, (blue + 2) / 4);
            }
        }
        levels_.push_back(std::move(next));
    }
}

Color Texture::sampleLevel(const Level& level, double u, double v) const {
    const double wrappedU = u - std::floor(u);
    const double wrappedV = v - std::floor(v);
    const double x = wrappedU * level.width - 0.5;
    const double y = wrappedV * level.height - 0.5;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - std::floor(x);
    const double fy = y - std::floor(y);
    const auto texel = [&](int sampleX, int sampleY) {
        return level.pixels[static_cast<std::size_t>(wrapIndex(sampleY, level.height)) *
                                static_cast<std::size_t>(level.width) +
                            static_cast<std::size_t>(wrapIndex(sampleX, level.width))];
    };
    const Color c00 = texel(x0, y0);
    const Color c10 = texel(x0 + 1, y0);
    const Color c01 = texel(x0, y0 + 1);
    const Color c11 = texel(x0 + 1, y0 + 1);
    const Color top = mix(c00, c10, fx);
    const Color bottom = mix(c01, c11, fx);
    return mix(top, bottom, fy);
}

Color Texture::sample(double u, double v, double lod) const {
    if (levels_.empty()) {
        return makeColor(255, 255, 255);
    }
    const double maximum = static_cast<double>(levels_.size() - 1);
    const double clamped = clampd(lod, 0.0, maximum);
    const int lower = static_cast<int>(std::floor(clamped));
    const int upper = std::min(lower + 1, static_cast<int>(levels_.size() - 1));
    const double fraction = clamped - static_cast<double>(lower);
    const Color a = sampleLevel(levels_[static_cast<std::size_t>(lower)], u, v);
    if (fraction <= 1e-9 || lower == upper) {
        return a;
    }
    const Color b = sampleLevel(levels_[static_cast<std::size_t>(upper)], u, v);
    return mix(a, b, fraction);
}

void Mesh::addTriangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    triangles.push_back(Triangle{a, b, c});
}

void Mesh::addQuad(Vec3 origin, Vec3 uAxis, Vec3 vAxis, double uLength, double vLength,
                   Vec2 uvStart, Vec2 uvEnd) {
    const Vec3 normal = normalized(cross(uAxis, vAxis));
    const Vec3 p0 = origin;
    const Vec3 p1 = origin + uAxis * uLength;
    const Vec3 p2 = origin + uAxis * uLength + vAxis * vLength;
    const Vec3 p3 = origin + vAxis * vLength;
    Vertex v0{p0, normal, Vec2{uvStart.x, uvStart.y}};
    Vertex v1{p1, normal, Vec2{uvEnd.x, uvStart.y}};
    Vertex v2{p2, normal, Vec2{uvEnd.x, uvEnd.y}};
    Vertex v3{p3, normal, Vec2{uvStart.x, uvEnd.y}};
    addTriangle(v0, v1, v2);
    addTriangle(v0, v2, v3);
}

void Mesh::addBox(Vec3 minimum, Vec3 maximum) {
    const Vec3 dx{maximum.x - minimum.x, 0.0, 0.0};
    const Vec3 dy{0.0, maximum.y - minimum.y, 0.0};
    const Vec3 dz{0.0, 0.0, maximum.z - minimum.z};
    const Vec2 uvA{0.0, 0.0};
    const Vec2 uvB{1.0, 1.0};
    // +x and -x.
    addQuad(Vec3{maximum.x, minimum.y, minimum.z}, dy, dz, 1.0, 1.0, uvA, uvB);
    addQuad(Vec3{minimum.x, maximum.y, minimum.z}, Vec3{-dy.x, -dy.y, -dy.z}, dz, 1.0,
            1.0, uvA, uvB);
    // +y and -y.
    addQuad(Vec3{maximum.x, maximum.y, minimum.z}, Vec3{-dx.x, -dx.y, -dx.z}, dz, 1.0,
            1.0, uvA, uvB);
    addQuad(Vec3{minimum.x, minimum.y, minimum.z}, dx, dz, 1.0, 1.0, uvA, uvB);
    // +z and -z.
    addQuad(Vec3{minimum.x, minimum.y, maximum.z}, dx, dy, 1.0, 1.0, uvA, uvB);
    addQuad(Vec3{minimum.x, maximum.y, minimum.z}, dx, Vec3{0.0, -dy.y, 0.0}, 1.0, 1.0,
            uvA, uvB);
}

void renderScene(Image& image, const Scene& scene) {
    const int width = image.width();
    const int height = image.height();
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    const Camera& camera = scene.camera;
    const Mat4 projection = perspectiveMatrix(camera.fovYDegrees, aspect,
                                              camera.nearPlane, camera.farPlane);
    const Mat4 view = lookAtMatrix(camera.eye, camera.target, camera.up);

    std::vector<double> depth(static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height),
                              0.0);
    image.fillVerticalGradient(scene.skyTop, scene.skyBottom);

    const Vec3 lightDirection = normalized(scene.light.towardLight);
    const double lightRed = static_cast<double>(scene.light.color.r) / 255.0;
    const double lightGreen = static_cast<double>(scene.light.color.g) / 255.0;
    const double lightBlue = static_cast<double>(scene.light.color.b) / 255.0;
    const double lodScale = 0.08 * 960.0 / static_cast<double>(width);

    std::vector<ViewVertex> polygon;
    std::vector<ViewVertex> clipped;
    polygon.reserve(8);
    clipped.reserve(8);
    std::vector<ScreenVertex> screenPolygon;
    screenPolygon.reserve(8);

    for (const DrawItem& item : scene.items) {
        if (item.mesh == nullptr || item.mesh->triangles.empty()) {
            continue;
        }
        const Mat4 modelView = view * item.model;
        for (const Triangle& triangle : item.mesh->triangles) {
            const Vertex* vertices[3] = {&triangle.v0, &triangle.v1, &triangle.v2};
            Vec3 worldPositions[3];
            for (int i = 0; i < 3; ++i) {
                worldPositions[i] = transformPoint(item.model, vertices[i]->position);
            }
            Vec3 geometricNormal =
                cross(worldPositions[1] - worldPositions[0],
                      worldPositions[2] - worldPositions[0]);
            if (lengthSquared(geometricNormal) < 1e-18) {
                continue;
            }
            geometricNormal = normalized(geometricNormal);
            if (!item.material.doubleSided &&
                dot(geometricNormal, camera.eye - worldPositions[0]) <= 0.0) {
                continue;
            }

            polygon.clear();
            for (int i = 0; i < 3; ++i) {
                ViewVertex vertex;
                vertex.view = transformPoint(modelView, vertices[i]->position);
                vertex.world = worldPositions[i];
                vertex.normal = normalized(
                    transformDirection(item.model, vertices[i]->normal));
                vertex.uv = vertices[i]->uv;
                polygon.push_back(vertex);
            }
            clipAgainstNearPlane(polygon, camera.nearPlane, clipped);
            if (clipped.size() < 3) {
                continue;
            }

            screenPolygon.clear();
            bool valid = true;
            for (const ViewVertex& vertex : clipped) {
                const Vec4 clip =
                    projection * Vec4{vertex.view.x, vertex.view.y, vertex.view.z, 1.0};
                if (clip.w <= 1e-9) {
                    valid = false;
                    break;
                }
                ScreenVertex screen;
                screen.world = vertex.world;
                screen.normal = vertex.normal;
                screen.uv = vertex.uv;
                screen.invW = 1.0 / clip.w;
                screen.x = (clip.x / clip.w * 0.5 + 0.5) * static_cast<double>(width);
                screen.y = (0.5 - clip.y / clip.w * 0.5) * static_cast<double>(height);
                screenPolygon.push_back(screen);
            }
            if (!valid) {
                continue;
            }

            for (std::size_t fan = 1; fan + 1 < screenPolygon.size(); ++fan) {
                const ScreenVertex& p0 = screenPolygon[0];
                const ScreenVertex& p1 = screenPolygon[fan];
                const ScreenVertex& p2 = screenPolygon[fan + 1];
                const double area = edgeFunction(p0.x, p0.y, p1.x, p1.y, p2.x, p2.y);
                if (std::fabs(area) < 1e-12) {
                    continue;
                }
                const double inverseArea = 1.0 / area;
                const double minX = std::max(
                    0.0, std::floor(std::min({p0.x, p1.x, p2.x})));
                const double maxX = std::min(
                    static_cast<double>(width - 1),
                    std::ceil(std::max({p0.x, p1.x, p2.x})));
                const double minY = std::max(
                    0.0, std::floor(std::min({p0.y, p1.y, p2.y})));
                const double maxY = std::min(
                    static_cast<double>(height - 1),
                    std::ceil(std::max({p0.y, p1.y, p2.y})));
                if (minX > maxX || minY > maxY) {
                    continue;
                }
                const int x0 = static_cast<int>(minX);
                const int x1 = static_cast<int>(maxX);
                const int y0 = static_cast<int>(minY);
                const int y1 = static_cast<int>(maxY);

                for (int y = y0; y <= y1; ++y) {
                    const double py = static_cast<double>(y) + 0.5;
                    const std::size_t row =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
                    for (int x = x0; x <= x1; ++x) {
                        const double px = static_cast<double>(x) + 0.5;
                        double b0 = edgeFunction(p1.x, p1.y, p2.x, p2.y, px, py) *
                                    inverseArea;
                        double b1 = edgeFunction(p2.x, p2.y, p0.x, p0.y, px, py) *
                                    inverseArea;
                        double b2 = edgeFunction(p0.x, p0.y, p1.x, p1.y, px, py) *
                                    inverseArea;
                        // Barycentrics are edge/area, so they are positive
                        // inside the triangle whatever the winding sign is.
                        if (b0 < 0.0 || b1 < 0.0 || b2 < 0.0) {
                            continue;
                        }
                        const double invW = b0 * p0.invW + b1 * p1.invW + b2 * p2.invW;
                        if (invW <= 1e-12) {
                            continue;
                        }
                        const std::size_t index = row + static_cast<std::size_t>(x);
                        if (invW <= depth[index]) {
                            continue;
                        }
                        const double scale = 1.0 / invW;
                        const Vec3 world = (p0.world * (p0.invW * b0) +
                                            p1.world * (p1.invW * b1) +
                                            p2.world * (p2.invW * b2)) *
                                           scale;
                        const Vec3 normal = (p0.normal * (p0.invW * b0) +
                                             p1.normal * (p1.invW * b1) +
                                             p2.normal * (p2.invW * b2)) *
                                            scale;
                        const Vec2 uv = (p0.uv * (p0.invW * b0) +
                                         p1.uv * (p1.invW * b1) +
                                         p2.uv * (p2.invW * b2)) *
                                        scale;
                        const double distance = length(world - camera.eye);
                        const double lod = std::log2(
                            std::max(1.0, distance * lodScale));
                        Color albedo = item.material.texture != nullptr
                                           ? item.material.texture->sample(uv.x, uv.y, lod)
                                           : makeColor(255, 255, 255);
                        albedo = multiply(albedo, item.material.tint);
                        if (!item.material.unlit) {
                            const Vec3 unitNormal = normalized(normal);
                            const double diffuse =
                                std::max(0.0, dot(unitNormal, lightDirection)) *
                                    scene.light.intensity +
                                std::max(0.0, -unitNormal.z) * 0.25;
                            const double red = scene.ambient + diffuse * lightRed;
                            const double green = scene.ambient + diffuse * lightGreen;
                            const double blue = scene.ambient + diffuse * lightBlue;
                            albedo = makeColor(
                                static_cast<int>(albedo.r * red),
                                static_cast<int>(albedo.g * green),
                                static_cast<int>(albedo.b * blue));
                            const double fog =
                                smoothstep(scene.fogStart, scene.fogEnd, distance) *
                                scene.fogMaximum;
                            albedo = mix(albedo, scene.fogColor, fog);
                        }
                        image.at(x, y) = albedo;
                        depth[index] = invW;
                    }
                }
            }
        }
    }
}

CameraBasis cameraBasis(const Camera& camera) {
    CameraBasis basis;
    basis.forward = normalized(camera.target - camera.eye);
    basis.right = normalized(cross(basis.forward, camera.up));
    basis.up = cross(basis.right, basis.forward);
    return basis;
}

}  // namespace g3d
