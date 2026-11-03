// Dependency-free software 3D rendering pipeline.
//
// The renderer implements the classic hardware stages in software: model,
// view and projection transforms, near-plane clipping, perspective divide,
// viewport mapping, depth-buffered triangle rasterization with perspective
// correct attribute interpolation, texture sampling with mip levels, and
// per-pixel directional lighting, specular highlights and distance fog.
// Supersampled anti-aliasing renders the scene at a higher resolution and
// resolves it down with a box filter.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.hpp"

namespace g3d {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

inline Color makeColor(int red, int green, int blue) {
    return Color{static_cast<std::uint8_t>(clampd(red, 0.0, 255.0)),
                 static_cast<std::uint8_t>(clampd(green, 0.0, 255.0)),
                 static_cast<std::uint8_t>(clampd(blue, 0.0, 255.0))};
}

inline bool operator==(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

inline bool operator!=(Color a, Color b) { return !(a == b); }

Color mix(Color from, Color to, double amount);
Color shade(Color color, double factor);
Color multiply(Color a, Color b);

class Image {
  public:
    Image(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    bool contains(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    Color& at(int x, int y);
    const Color& at(int x, int y) const;

    std::vector<Color>& pixels() { return pixels_; }
    const std::vector<Color>& pixels() const { return pixels_; }

    void fill(Color color);
    void fillVerticalGradient(Color top, Color bottom);
    void blend(int x, int y, Color color, double alpha);
    void fillRect(int x, int y, int width, int height, Color color, double alpha = 1.0);
    void drawLine(int x0, int y0, int x1, int y1, Color color, double alpha = 1.0);
    void drawCircle(int centerX, int centerY, int radius, Color color,
                    double alpha = 1.0);

  private:
    int width_ = 1;
    int height_ = 1;
    std::vector<Color> pixels_;
};

bool writePPM(const std::string& path, const Image& image);

// A tiling RGBA-free color texture with an automatically generated mip chain.
class Texture {
  public:
    Texture() = default;
    Texture(int width, int height, std::vector<Color> pixels);

    int width() const { return width_; }
    int height() const { return height_; }
    bool empty() const { return levels_.empty(); }

    // Bilinear, wrapping sample.  `lod` selects (and blends between) mip
    // levels, where 0 is the full-resolution level.
    Color sample(double u, double v, double lod = 0.0) const;

  private:
    struct Level {
        int width = 1;
        int height = 1;
        std::vector<Color> pixels;
    };

    Color sampleLevel(const Level& level, double u, double v) const;

    int width_ = 0;
    int height_ = 0;
    std::vector<Level> levels_;
};

struct Vertex {
    Vec3 position;
    Vec3 normal{0.0, 0.0, 1.0};
    Vec2 uv;
    // Per-vertex coverage used for soft shadows and particles.  It is
    // interpolated across the triangle and multiplied by the material
    // opacity to blend the fragment over the frame.
    double alpha = 1.0;
};

struct Triangle {
    Vertex v0;
    Vertex v1;
    Vertex v2;
};

struct Mesh {
    std::vector<Triangle> triangles;

    void addTriangle(const Vertex& a, const Vertex& b, const Vertex& c);
    // Adds a quad whose front face normal is cross(uAxis, vAxis).  The four
    // corners are origin, origin + uAxis*uLength, origin + both, origin +
    // vAxis*vLength; UVs span [uvStart, uvEnd].
    void addQuad(Vec3 origin, Vec3 uAxis, Vec3 vAxis, double uLength, double vLength,
                 Vec2 uvStart, Vec2 uvEnd);
    void addBox(Vec3 minimum, Vec3 maximum);
};

struct Material {
    const Texture* texture = nullptr;
    Color tint = makeColor(255, 255, 255);
    bool unlit = false;
    bool doubleSided = false;
    // Blinn-Phong highlight strength (0 = matte) and its exponent: higher
    // exponents produce a tighter, glossier highlight.
    double specular = 0.0;
    double shininess = 32.0;
    // Materials below full opacity are rendered in a transparent pass after
    // the opaque geometry, so soft shadows and particles blend over the scene.
    double opacity = 1.0;
};

struct DrawItem {
    const Mesh* mesh = nullptr;
    Mat4 model = identityMatrix();
    Material material;
};

struct Camera {
    Vec3 eye{0.0, 0.0, 0.0};
    Vec3 target{0.0, 0.0, -1.0};
    Vec3 up{0.0, 0.0, 1.0};
    double fovYDegrees = 60.0;
    double nearPlane = 0.06;
    double farPlane = 120.0;
};

struct DirectionalLight {
    Vec3 towardLight{0.35, 0.25, 0.9};  // normalized, world space
    Color color = makeColor(255, 244, 224);
    double intensity = 0.8;
};

struct Scene {
    Camera camera;
    std::vector<DrawItem> items;
    Color skyTop = makeColor(20, 25, 34);
    Color skyBottom = makeColor(42, 50, 64);
    double ambient = 0.62;
    DirectionalLight light;
    Color fogColor = makeColor(40, 46, 58);
    double fogStart = 16.0;
    double fogEnd = 60.0;
    double fogMaximum = 0.7;
    // 1 = no anti-aliasing, 2 = 2x2 supersampling, and so on.  Very large
    // frames are automatically reduced to keep the sample buffer bounded.
    int supersamples = 2;
};

void renderScene(Image& image, const Scene& scene);

// Radial screen-space blur used by the dash speed effect.  Samples are pulled
// toward the image centre; strength is the fraction of the centre distance
// sampled by the outermost tap.
void applyRadialBlur(Image& image, double strength);

// Camera basis helpers for billboards and other view-facing geometry.
struct CameraBasis {
    Vec3 forward;
    Vec3 right;
    Vec3 up;
};
CameraBasis cameraBasis(const Camera& camera);

}  // namespace g3d
