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
//     can be inspected directly;
//   * depth-tested zombie billboards with health bars add a small combat layer
//     on top of the renderer.
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
// Zombies and combat
// ---------------------------------------------------------------------------

constexpr int kPlayerMaxHealth = 100;
constexpr int kZombieMaxHealth = 100;
constexpr double kZombieRadius = 0.3;
constexpr double kZombieHeight = 0.9;  // feet to the top of the head, world units
constexpr double kZombieWidth = 0.45;  // matches the 32x64 sprite aspect ratio
constexpr double kZombieAggroRange = 9.0;
constexpr double kZombieAttackRange = 0.95;
constexpr double kZombieSpeed = 0.85;
constexpr int kZombieAttackDamage = 10;
constexpr double kZombieAttackCooldown = 1.1;
constexpr double kZombieRespawnSeconds = 8.0;
constexpr int kShotDamage = 34;
constexpr double kShotCooldown = 0.24;

struct Zombie {
    Vec2 spawn;
    Vec2 position;
    int health = kZombieMaxHealth;
    int maxHealth = kZombieMaxHealth;
    bool alive = true;
    double respawnTimer = 0.0;
    double attackTimer = 0.0;
    double hitFlash = 0.0;
    double walkPhase = 0.0;
};

// Fixed spawns keep both screenshots and the interactive demo reproducible.
// The first three stand in the starting view, the remaining three spread out
// through the atrium and the outer hall.
constexpr std::array<Vec2, 6> kZombieSpawns{{
    {8.5, 13.5}, {10.5, 9.5}, {14.5, 14.5},
    {7.5, 8.5},  {16.5, 15.5}, {2.5, 6.5},
}};
// Starting health is varied on purpose so the screenshot shows all three
// health-bar colours without having to fire a shot first.
constexpr std::array<int, 6> kZombieStartHealth{{100, 55, 30, 100, 80, 100}};

std::vector<Zombie> createZombies() {
    std::vector<Zombie> zombies;
    zombies.reserve(kZombieSpawns.size());
    for (std::size_t i = 0; i < kZombieSpawns.size(); ++i) {
        Zombie zombie;
        zombie.spawn = kZombieSpawns[i];
        zombie.position = zombie.spawn;
        zombie.maxHealth = kZombieMaxHealth;
        zombie.health = kZombieStartHealth[i];
        zombie.attackTimer = 0.6;
        zombies.push_back(zombie);
    }
    return zombies;
}

bool hasLineOfSight(const Grid& map, Vec2 from, Vec2 to) {
    const Vec2 delta{to.x - from.x, to.y - from.y};
    const double distance = std::hypot(delta.x, delta.y);
    if (distance < 1e-9) {
        return true;
    }
    const Vec2 direction{delta.x / distance, delta.y / distance};
    const RayHit hit = castRay(map, from, direction);
    return hit.material == 0 || hit.distance >= distance - 1e-6;
}

void tryMove(const Grid& map, Vec2& position, Vec2 delta) {
    if (isWalkableCentered(map, Vec2{position.x + delta.x, position.y})) {
        position.x += delta.x;
    }
    if (isWalkableCentered(map, Vec2{position.x, position.y + delta.y})) {
        position.y += delta.y;
    }
}

// Hitscan along the view direction: the closest living zombie whose body
// intersects the ray, unless a wall is closer.
int findShotTarget(const std::vector<Zombie>& zombies, const Grid& map, Vec2 origin,
                   Vec2 direction) {
    const double length = std::hypot(direction.x, direction.y);
    if (length < 1e-9) {
        return -1;
    }
    const Vec2 ray{direction.x / length, direction.y / length};
    const RayHit wall = castRay(map, origin, ray);
    int best = -1;
    double bestDistance = wall.material == 0 ? 1e30 : wall.distance;
    for (std::size_t i = 0; i < zombies.size(); ++i) {
        const Zombie& zombie = zombies[i];
        if (!zombie.alive) {
            continue;
        }
        const Vec2 relative{zombie.position.x - origin.x, zombie.position.y - origin.y};
        const double along = relative.x * ray.x + relative.y * ray.y;
        if (along <= 0.0 || along >= bestDistance) {
            continue;
        }
        const double perpendicular = std::fabs(relative.x * ray.y - relative.y * ray.x);
        if (perpendicular > kZombieRadius) {
            continue;
        }
        best = static_cast<int>(i);
        bestDistance = along;
    }
    return best;
}

// Returns true when this hit killed the zombie.
bool damageZombie(Zombie& zombie, int damage) {
    if (!zombie.alive) {
        return false;
    }
    zombie.health = std::max(0, zombie.health - damage);
    zombie.hitFlash = 0.16;
    if (zombie.health == 0) {
        zombie.alive = false;
        zombie.respawnTimer = kZombieRespawnSeconds;
        zombie.attackTimer = 0.0;
        return true;
    }
    return false;
}

// Moves zombies toward the player, handles melee attacks and respawns.  The
// return value is the damage the player should take this frame.
int updateZombies(std::vector<Zombie>& zombies, const Grid& map, Vec2 playerPosition,
                  double deltaSeconds, bool playerAlive) {
    int playerDamage = 0;
    for (Zombie& zombie : zombies) {
        zombie.hitFlash = std::max(0.0, zombie.hitFlash - deltaSeconds);
        if (!zombie.alive) {
            zombie.respawnTimer -= deltaSeconds;
            if (zombie.respawnTimer <= 0.0) {
                const double dx = zombie.spawn.x - playerPosition.x;
                const double dy = zombie.spawn.y - playerPosition.y;
                if (std::hypot(dx, dy) > 2.5 && isWalkableCentered(map, zombie.spawn, 0.25)) {
                    zombie.position = zombie.spawn;
                    zombie.health = zombie.maxHealth;
                    zombie.alive = true;
                    zombie.attackTimer = 0.5;
                    zombie.walkPhase = 0.0;
                }
            }
            continue;
        }

        zombie.attackTimer = std::max(0.0, zombie.attackTimer - deltaSeconds);
        const Vec2 relative{playerPosition.x - zombie.position.x,
                            playerPosition.y - zombie.position.y};
        const double distance = std::hypot(relative.x, relative.y);
        if (distance > 1e-9 && distance < kZombieAggroRange &&
            hasLineOfSight(map, zombie.position, playerPosition)) {
            if (distance > kZombieAttackRange) {
                const double step =
                    std::min(kZombieSpeed * deltaSeconds, distance - kZombieAttackRange);
                tryMove(map, zombie.position,
                        Vec2{relative.x / distance * step, relative.y / distance * step});
                zombie.walkPhase += deltaSeconds * 7.0;
            } else if (playerAlive && zombie.attackTimer <= 0.0) {
                playerDamage += kZombieAttackDamage;
                zombie.attackTimer = kZombieAttackCooldown;
            }
        }
    }

    // Keep the undead from stacking into a single billboard.
    for (std::size_t i = 0; i < zombies.size(); ++i) {
        if (!zombies[i].alive) {
            continue;
        }
        for (std::size_t j = i + 1; j < zombies.size(); ++j) {
            if (!zombies[j].alive) {
                continue;
            }
            const double dx = zombies[j].position.x - zombies[i].position.x;
            const double dy = zombies[j].position.y - zombies[i].position.y;
            const double distance = std::hypot(dx, dy);
            if (distance < 1e-6 || distance >= 0.55) {
                continue;
            }
            const double push = (0.55 - distance) * 0.5;
            const Vec2 normal{dx / distance, dy / distance};
            tryMove(map, zombies[i].position, Vec2{-normal.x * push, -normal.y * push});
            tryMove(map, zombies[j].position, Vec2{normal.x * push, normal.y * push});
        }
    }
    return playerDamage;
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

// ---------------------------------------------------------------------------
// Procedural zombie sprite: hand-drawn 32x64 pixel art, generated once
// ---------------------------------------------------------------------------

constexpr int kZombieSpriteWidth = 32;
constexpr int kZombieSpriteHeight = 64;

struct SpriteFrame {
    std::array<Color, kZombieSpriteWidth * kZombieSpriteHeight> pixels{};
    std::array<std::uint8_t, kZombieSpriteWidth * kZombieSpriteHeight> alpha{};
};

SpriteFrame makeZombieSprite(int variant) {
    SpriteFrame sprite;
    const auto indexOf = [](int x, int y) {
        return static_cast<std::size_t>(y) * kZombieSpriteWidth +
               static_cast<std::size_t>(x);
    };
    const auto put = [&](int x, int y, Color color) {
        if (x < 0 || y < 0 || x >= kZombieSpriteWidth || y >= kZombieSpriteHeight) {
            return;
        }
        const std::size_t index = indexOf(x, y);
        sprite.pixels[index] = color;
        sprite.alpha[index] = 255;
    };
    const auto rect = [&](int x, int y, int width, int height, Color color) {
        for (int py = y; py < y + height; ++py) {
            for (int px = x; px < x + width; ++px) {
                put(px, py, color);
            }
        }
    };
    const auto ellipse = [&](int centerX, int centerY, int radiusX, int radiusY, Color color) {
        for (int py = -radiusY; py <= radiusY; ++py) {
            for (int px = -radiusX; px <= radiusX; ++px) {
                const double nx = static_cast<double>(px) / radiusX;
                const double ny = static_cast<double>(py) / radiusY;
                if (nx * nx + ny * ny <= 1.0) {
                    put(centerX + px, centerY + py, color);
                }
            }
        }
    };

    const Color skin = makeColor(94, 132, 76);
    const Color skinLight = makeColor(128, 164, 100);
    const Color skinDark = makeColor(58, 88, 50);
    const Color shirt = makeColor(72, 80, 66);
    const Color shirtDark = makeColor(48, 55, 45);
    const Color pants = makeColor(47, 55, 70);
    const Color pantsDark = makeColor(30, 36, 48);
    const Color shoe = makeColor(28, 25, 22);
    const Color hair = makeColor(34, 42, 30);
    const Color blood = makeColor(112, 26, 26);
    const Color eye = makeColor(238, 66, 46);
    const Color socket = makeColor(24, 24, 20);
    const Color bone = makeColor(214, 204, 164);

    // Legs first, with a simple two-frame walk cycle.
    if (variant == 0) {
        rect(10, 45, 5, 15, pants);
        rect(9, 60, 7, 3, shoe);
        rect(17, 45, 5, 15, pants);
        rect(16, 60, 7, 3, shoe);
    } else {
        rect(9, 45, 5, 14, pants);
        rect(8, 59, 7, 3, shoe);
        rect(18, 45, 5, 16, pants);
        rect(17, 61, 7, 3, shoe);
    }
    put(11, 52, pantsDark);
    put(12, 52, pantsDark);
    put(19, 50, pantsDark);

    // Outstretched arms: ragged sleeve, bare forearm and a blocky hand.
    rect(2, 27, 6, 5, skin);
    rect(0, 28, 3, 4, skinLight);
    rect(7, 26, 3, 7, shirtDark);
    put(0, 29, skin);
    put(0, 31, skin);
    rect(24, 27, 6, 5, skin);
    rect(29, 28, 3, 4, skinLight);
    rect(22, 26, 3, 7, shirtDark);

    // Torso and torn shirt.
    rect(8, 24, 16, 3, shirt);
    rect(9, 27, 14, 17, shirt);
    rect(9, 44, 14, 2, shirtDark);
    rect(14, 22, 4, 3, skinDark);  // neck
    rect(14, 24, 4, 2, skin);      // open collar
    put(16, 29, shirtDark);
    put(16, 33, shirtDark);
    put(16, 37, shirtDark);
    put(16, 41, shirtDark);
    rect(11, 34, 2, 3, skin);      // tear
    rect(10, 34, 1, 3, shirtDark);
    rect(19, 38, 2, 2, skin);      // tear
    rect(14, 35, 3, 4, blood);
    put(13, 36, blood);
    put(18, 34, blood);
    put(20, 30, blood);
    rect(19, 28, 3, 3, makeColor(60, 66, 54));
    rect(15, 44, 2, 2, makeColor(120, 100, 60));

    // Head, hair and a damaged, hungry face.
    ellipse(16, 16, 8, 9, skin);
    rect(7, 15, 2, 3, skinDark);
    rect(23, 15, 2, 3, skinDark);
    ellipse(16, 10, 8, 4, hair);
    put(11, 13, hair);
    put(13, 12, hair);
    put(15, 13, hair);
    put(18, 12, hair);
    put(21, 13, hair);
    rect(10, 14, 4, 1, hair);
    rect(18, 14, 4, 1, hair);
    rect(10, 15, 4, 3, socket);
    rect(18, 15, 4, 3, socket);
    rect(11, 16, 2, 2, eye);
    rect(19, 16, 2, 2, eye);
    put(11, 16, makeColor(255, 196, 150));
    put(13, 9, skin);              // bare patch in the hair
    put(14, 9, blood);
    put(21, 12, skinDark);         // scar
    put(22, 13, skinDark);
    put(22, 14, skinDark);
    rect(15, 18, 2, 2, skinDark);  // nose
    rect(13, 21, 6, 2, makeColor(28, 18, 18));
    put(14, 21, bone);
    put(16, 21, bone);
    put(18, 21, bone);
    put(13, 23, skinDark);

    // Side shading before the silhouette outline gives the flat art some depth.
    for (int y = 0; y < kZombieSpriteHeight; ++y) {
        for (int x = 0; x < kZombieSpriteWidth; ++x) {
            const std::size_t index = indexOf(x, y);
            if (sprite.alpha[index] == 0) {
                continue;
            }
            const bool leftEdge = x == 0 || sprite.alpha[indexOf(x - 1, y)] == 0;
            const bool rightEdge =
                x == kZombieSpriteWidth - 1 || sprite.alpha[indexOf(x + 1, y)] == 0;
            if (rightEdge) {
                sprite.pixels[index] = shade(sprite.pixels[index], 0.72);
            } else if (leftEdge) {
                sprite.pixels[index] = mix(sprite.pixels[index], makeColor(255, 255, 255), 0.1);
            }
        }
    }

    // A one-pixel dark outline keeps the sprite readable against the floor.
    std::array<std::uint8_t, kZombieSpriteWidth * kZombieSpriteHeight> silhouette =
        sprite.alpha;
    for (int y = 0; y < kZombieSpriteHeight; ++y) {
        for (int x = 0; x < kZombieSpriteWidth; ++x) {
            const std::size_t index = indexOf(x, y);
            if (silhouette[index] != 0) {
                continue;
            }
            const bool nextToBody =
                (x > 0 && silhouette[indexOf(x - 1, y)] != 0) ||
                (x + 1 < kZombieSpriteWidth && silhouette[indexOf(x + 1, y)] != 0) ||
                (y > 0 && silhouette[indexOf(x, y - 1)] != 0) ||
                (y + 1 < kZombieSpriteHeight && silhouette[indexOf(x, y + 1)] != 0);
            if (nextToBody) {
                sprite.pixels[index] = makeColor(9, 12, 9);
                sprite.alpha[index] = 255;
            }
        }
    }
    return sprite;
}

const SpriteFrame& zombieSprite(int walkPhase) {
    static const std::array<SpriteFrame, 2> frames{{makeZombieSprite(0), makeZombieSprite(1)}};
    const int frame = ((walkPhase % 2) + 2) % 2;
    return frames[static_cast<std::size_t>(frame)];
}

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
// Zombie billboards and health bars
// ---------------------------------------------------------------------------

struct ZombieProjection {
    bool onScreen = false;
    double depth = 0.0;      // perpendicular distance from the camera plane
    double centerX = 0.0;    // screen x of the billboard centre
    double unit = 0.0;       // screen pixels per world unit at this depth
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
};

ZombieProjection projectZombie(Vec2 position, Vec2 player, double yawDegrees,
                               double fovDegrees, int width, int height) {
    ZombieProjection projection;
    const Vec2 direction = directionFromDegrees(yawDegrees);
    const double halfPlane = std::tan(fovDegrees * 0.5 * kDegToRad);
    const Vec2 plane{-direction.y * halfPlane, direction.x * halfPlane};
    const double projectionScale = width / (2.0 * halfPlane);
    const double horizon = height * 0.5;
    const Vec2 relative{position.x - player.x, position.y - player.y};

    // Inverse of the camera matrix, so a world position becomes camera space.
    const double determinant = plane.x * direction.y - direction.x * plane.y;
    if (std::fabs(determinant) < 1e-12) {
        return projection;
    }
    const double inverse = 1.0 / determinant;
    const double cameraX = inverse * (direction.y * relative.x - direction.x * relative.y);
    const double cameraDepth = inverse * (-plane.y * relative.x + plane.x * relative.y);
    projection.depth = cameraDepth;
    if (cameraDepth <= 0.05) {
        return projection;
    }

    projection.unit = projectionScale / cameraDepth;
    projection.centerX = width * 0.5 * (1.0 + cameraX / cameraDepth);
    projection.left = projection.centerX - 0.5 * kZombieWidth * projection.unit;
    projection.right = projection.centerX + 0.5 * kZombieWidth * projection.unit;
    projection.top = horizon - (kZombieHeight - kEyeHeight) * projection.unit;
    projection.bottom = horizon + kEyeHeight * projection.unit;
    projection.onScreen = projection.right > 0.0 && projection.left < width &&
                          projection.bottom > 0.0 && projection.top < height;
    return projection;
}

Color healthBarColor(double fraction) {
    if (fraction > 0.66) {
        return makeColor(92, 214, 96);
    }
    if (fraction > 0.33) {
        return makeColor(233, 186, 62);
    }
    return makeColor(226, 72, 58);
}

void drawHealthBar(Image& image, int x, int y, int width, int height, double fraction) {
    if (width <= 1 || height <= 0) {
        return;
    }
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    image.fillRect(x - 1, y - 1, width + 2, height + 2, makeColor(7, 9, 11), 0.95);
    image.fillRect(x, y, width, height, makeColor(52, 20, 20));
    const int fillWidth =
        static_cast<int>(std::lround((width - 2) * clamped));
    if (fillWidth > 0) {
        const Color fill = healthBarColor(clamped);
        image.fillRect(x + 1, y + 1, fillWidth, height - 2, fill);
        image.fillRect(x + 1, y + 1, fillWidth, 1,
                       mix(fill, makeColor(255, 255, 255), 0.35), 0.7);
    }
}

void renderZombies(Image& image, const std::vector<Zombie>& zombies, Vec2 position,
                   double yawDegrees, double fovDegrees,
                   const std::vector<double>& wallDepth) {
    struct OrderedZombie {
        ZombieProjection projection;
        std::size_t index = 0;
    };
    std::vector<OrderedZombie> ordered;
    ordered.reserve(zombies.size());
    for (std::size_t i = 0; i < zombies.size(); ++i) {
        if (!zombies[i].alive) {
            continue;
        }
        const ZombieProjection projection =
            projectZombie(zombies[i].position, position, yawDegrees, fovDegrees,
                          image.width(), image.height());
        if (projection.depth > 0.05 && projection.onScreen) {
            ordered.push_back(OrderedZombie{projection, i});
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const OrderedZombie& a, const OrderedZombie& b) {
                  return a.projection.depth > b.projection.depth;
              });

    for (const OrderedZombie& entry : ordered) {
        const Zombie& zombie = zombies[entry.index];
        const ZombieProjection& p = entry.projection;
        const SpriteFrame& sprite = zombieSprite(static_cast<int>(zombie.walkPhase));
        const int firstX =
            std::max(0, static_cast<int>(std::ceil(p.left)));
        const int lastX =
            std::min(image.width() - 1, static_cast<int>(std::floor(p.right)));
        const int firstY =
            std::max(0, static_cast<int>(std::ceil(p.top)));
        const int lastY =
            std::min(image.height() - 1, static_cast<int>(std::floor(p.bottom)));
        if (firstX > lastX || firstY > lastY) {
            continue;
        }

        const double widthSpan = std::max(p.right - p.left, 1e-9);
        const double heightSpan = std::max(p.bottom - p.top, 1e-9);
        double fog = 1.0 - 1.0 / (1.0 + 0.014 * p.depth * p.depth);
        fog = std::min(fog, 0.84);
        const bool flashing = zombie.hitFlash > 0.0;

        for (int x = firstX; x <= lastX; ++x) {
            if (p.depth >= wallDepth[static_cast<std::size_t>(x)]) {
                continue;
            }
            const double u = (x + 0.5 - p.left) / widthSpan;
            const int textureX =
                std::clamp(static_cast<int>(u * kZombieSpriteWidth), 0,
                           kZombieSpriteWidth - 1);
            for (int y = firstY; y <= lastY; ++y) {
                const double v = (y + 0.5 - p.top) / heightSpan;
                const int textureY =
                    std::clamp(static_cast<int>(v * kZombieSpriteHeight), 0,
                               kZombieSpriteHeight - 1);
                const std::size_t index =
                    static_cast<std::size_t>(textureY) * kZombieSpriteWidth +
                    static_cast<std::size_t>(textureX);
                if (sprite.alpha[index] == 0) {
                    continue;
                }
                Color color = sprite.pixels[index];
                if (flashing) {
                    color = mix(color, makeColor(255, 188, 168), 0.55);
                }
                color = mix(color, kFogColor, fog);
                image.at(x, y) = color;
            }
        }

        const int centerColumn =
            std::clamp(static_cast<int>(std::lround(p.centerX)), 0, image.width() - 1);
        if (p.depth >= wallDepth[static_cast<std::size_t>(centerColumn)]) {
            continue;
        }
        const int barWidth =
            std::clamp(static_cast<int>(std::lround(p.unit * kZombieWidth * 0.85)), 14, 160);
        const int barHeight = std::clamp(barWidth / 9, 2, 9);
        const int barGap = std::max(2, barWidth / 24);
        const int barX =
            static_cast<int>(std::lround(p.centerX - barWidth * 0.5));
        const int barY = std::max(1, static_cast<int>(std::lround(p.top - barHeight - barGap)));
        if (barY >= image.height()) {
            continue;
        }
        const auto isBarVisible = [&](int screenX) {
            const int column = std::clamp(screenX, 0, image.width() - 1);
            return p.depth < wallDepth[static_cast<std::size_t>(column)];
        };
        if (!isBarVisible(barX) || !isBarVisible(barX + barWidth - 1)) {
            continue;
        }
        drawHealthBar(image, barX, barY, barWidth, barHeight,
                      static_cast<double>(zombie.health) /
                          static_cast<double>(std::max(1, zombie.maxHealth)));
    }
}

void renderCrosshair(Image& image, bool firing) {
    const int centerX = image.width() / 2;
    const int centerY = image.height() / 2;
    const Color color = firing ? makeColor(255, 225, 120) : makeColor(232, 236, 240);
    for (int offset = 3; offset <= 7; ++offset) {
        image.blend(centerX + offset, centerY, color, 0.8);
        image.blend(centerX - offset, centerY, color, 0.8);
        image.blend(centerX, centerY + offset, color, 0.8);
        image.blend(centerX, centerY - offset, color, 0.8);
    }
    image.blend(centerX, centerY, color, 0.95);
}

void renderPlayerHud(Image& image, int health) {
    const int barWidth = std::clamp(image.width() / 4, 100, 260);
    const int barHeight = std::clamp(barWidth / 16, 6, 12);
    const int x = 12;
    const int y = image.height() - barHeight - 14;
    const int iconSize = barHeight + 4;
    image.fillRect(x - 8, y - 9, barWidth + iconSize + 30, barHeight + 18,
                   makeColor(5, 7, 10), 0.55);
    // Small medical-cross icon in front of the player health bar.
    image.fillRect(x, y - 2, iconSize, iconSize, makeColor(126, 32, 30));
    image.fillRect(x + iconSize / 2 - 1, y, 2, iconSize - 4, makeColor(240, 236, 230));
    image.fillRect(x + 2, y + iconSize / 2 - 3, iconSize - 4, 2,
                   makeColor(240, 236, 230));
    drawHealthBar(image, x + iconSize + 8, y, barWidth, barHeight,
                  static_cast<double>(health) / static_cast<double>(kPlayerMaxHealth));
}

// ---------------------------------------------------------------------------
// 3D renderer
// ---------------------------------------------------------------------------

void render3D(Image& image, const Grid& map, Vec2 position, double yawDegrees,
              double fovDegrees, const std::vector<Zombie>& zombies) {
    renderFloorAndCeiling(image, position, yawDegrees, fovDegrees);
    std::vector<double> wallDepth(static_cast<std::size_t>(image.width()), 1e30);

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
        wallDepth[static_cast<std::size_t>(x)] = hit.distance;
    }

    renderZombies(image, zombies, position, yawDegrees, fovDegrees, wallDepth);
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

void paintZombies(Image& image, const std::vector<Zombie>& zombies, int offsetX,
                  int offsetY, int scale, bool detailed) {
    for (const Zombie& zombie : zombies) {
        if (!zombie.alive) {
            continue;
        }
        const int centerX =
            static_cast<int>(std::lround(offsetX + zombie.position.x * scale));
        const int centerY =
            static_cast<int>(std::lround(offsetY + zombie.position.y * scale));
        const int radius = std::max(detailed ? 2 : 1, scale / 3);
        image.drawCircle(centerX, centerY, radius + 1, makeColor(10, 12, 12), 0.9);
        image.drawCircle(centerX, centerY, radius, makeColor(210, 72, 56), 1.0);
        image.blend(centerX, centerY, makeColor(255, 190, 90), 0.85);
        if (detailed) {
            const int barWidth = std::max(8, scale);
            const int barHeight = std::max(2, scale / 8);
            drawHealthBar(image, centerX - barWidth / 2,
                          centerY - radius - barHeight - 3, barWidth, barHeight,
                          static_cast<double>(zombie.health) /
                              static_cast<double>(std::max(1, zombie.maxHealth)));
        }
    }
}

void renderMinimap(Image& image, const Grid& map, Vec2 position, double yawDegrees,
                   double fovDegrees, const std::vector<Zombie>& zombies) {
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
    paintZombies(image, zombies, offsetX, offsetY, scale, false);
    paintPlayer(image, position, yawDegrees, offsetX, offsetY, scale);
}

void renderMapView(Image& image, const Grid& map, Vec2 position, double yawDegrees,
                   double fovDegrees, const std::vector<Zombie>& zombies) {
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
    paintZombies(image, zombies, offsetX, offsetY, scale, true);
    paintPlayer(image, position, yawDegrees, offsetX, offsetY, scale);
}

void renderWorld(Image& image, const Grid& map, Vec2 position, double yawDegrees,
                 double fovDegrees, const std::vector<Zombie>& zombies, bool mapView,
                 bool minimap, bool firing) {
    if (mapView) {
        renderMapView(image, map, position, yawDegrees, fovDegrees, zombies);
    } else {
        render3D(image, map, position, yawDegrees, fovDegrees, zombies);
        if (minimap) {
            renderMinimap(image, map, position, yawDegrees, fovDegrees, zombies);
        }
        renderCrosshair(image, firing);
    }
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
                 "  Left/Right      turn                 Space  fire\n"
                 "  Tab             top-down ray view    M    minimap\n"
                 "  R               restart              Q/Esc  quit\n");
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

bool selftestZombies(const Grid& map, std::string& error) {
    const std::vector<Zombie> zombies = createZombies();
    if (zombies.size() < 3) {
        error = "expected at least three zombies";
        return false;
    }
    for (std::size_t i = 0; i < zombies.size(); ++i) {
        const Zombie& zombie = zombies[i];
        if (!isWalkableCentered(map, zombie.spawn, 0.25)) {
            error = "zombie " + std::to_string(i) + " spawns inside a wall";
            return false;
        }
        if (!zombie.alive || zombie.health <= 0 || zombie.health > zombie.maxHealth) {
            error = "zombie " + std::to_string(i) + " has invalid initial health";
            return false;
        }
    }

    int onScreen = 0;
    for (const Zombie& zombie : zombies) {
        const ZombieProjection projection =
            projectZombie(zombie.position, kSpawn, kSpawnYaw, kDefaultFov, 960, 540);
        if (projection.onScreen) {
            ++onScreen;
        }
    }
    if (onScreen < 2) {
        error = "fewer than two zombies are visible from the default view";
        return false;
    }

    // A visible zombie can be shot, loses health and dies after enough hits.
    std::vector<Zombie> target(1);
    target[0].spawn = target[0].position = Vec2{8.5, 13.5};
    target[0].health = target[0].maxHealth = kZombieMaxHealth;
    const Vec2 toTarget{target[0].position.x - kSpawn.x, target[0].position.y - kSpawn.y};
    if (findShotTarget(target, map, kSpawn, toTarget) != 0) {
        error = "a zombie straight ahead cannot be shot";
        return false;
    }
    if (damageZombie(target[0], kShotDamage)) {
        error = "one shot killed a full-health zombie";
        return false;
    }
    damageZombie(target[0], kShotDamage);
    if (!damageZombie(target[0], kShotDamage)) {
        error = "three shots did not kill a full-health zombie";
        return false;
    }
    if (target[0].alive || target[0].health != 0) {
        error = "a killed zombie is still alive";
        return false;
    }
    if (findShotTarget(target, map, kSpawn, toTarget) != -1) {
        error = "a dead zombie can still be shot";
        return false;
    }

    // Walls block the hitscan.
    std::vector<Zombie> behindWall(1);
    behindWall[0].spawn = behindWall[0].position = Vec2{4.5, 14.5};
    if (findShotTarget(behindWall, map, kSpawn, Vec2{-1.0, 0.0}) != -1) {
        error = "a shot passed through a wall";
        return false;
    }

    // A zombie closes the distance, attacks in melee and later respawns.
    std::vector<Zombie> chaser(1);
    chaser[0].spawn = chaser[0].position = Vec2{7.0, 13.5};
    chaser[0].health = chaser[0].maxHealth = kZombieMaxHealth;
    const double distanceBefore =
        std::hypot(chaser[0].position.x - kSpawn.x, chaser[0].position.y - kSpawn.y);
    for (int frame = 0; frame < 20; ++frame) {
        updateZombies(chaser, map, kSpawn, 0.05, true);
    }
    const double distanceAfter =
        std::hypot(chaser[0].position.x - kSpawn.x, chaser[0].position.y - kSpawn.y);
    if (distanceAfter >= distanceBefore - 0.05) {
        error = "a zombie does not chase the player";
        return false;
    }
    chaser[0].position = Vec2{6.9, 14.3};
    chaser[0].attackTimer = 0.0;
    if (updateZombies(chaser, map, kSpawn, 0.01, true) != kZombieAttackDamage) {
        error = "a zombie in melee range does not damage the player";
        return false;
    }
    chaser[0].attackTimer = 0.0;
    if (updateZombies(chaser, map, kSpawn, 0.01, false) != 0) {
        error = "a dead player still takes melee damage";
        return false;
    }
    chaser[0] = Zombie{};
    chaser[0].spawn = chaser[0].position = Vec2{16.5, 15.5};
    chaser[0].alive = false;
    chaser[0].health = 0;
    chaser[0].respawnTimer = 0.0;
    updateZombies(chaser, map, kSpawn, 0.01, false);
    if (!chaser[0].alive || chaser[0].health != chaser[0].maxHealth ||
        std::fabs(chaser[0].position.x - 16.5) > 1e-9) {
        error = "a dead zombie does not respawn";
        return false;
    }

    const auto countChangedPixels = [](const Image& a, const Image& b) {
        int changed = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const Color left = a.at(x, y);
                const Color right = b.at(x, y);
                if (left.r != right.r || left.g != right.g || left.b != right.b) {
                    ++changed;
                }
            }
        }
        return changed;
    };

    Image emptyFrame(320, 180);
    Image zombieFrame(320, 180);
    render3D(emptyFrame, map, kSpawn, kSpawnYaw, kDefaultFov, {});
    render3D(zombieFrame, map, kSpawn, kSpawnYaw, kDefaultFov, zombies);
    if (countChangedPixels(emptyFrame, zombieFrame) < 500) {
        error = "zombies are not visible in the rendered frame";
        return false;
    }

    // The wall z-buffer must occlude a zombie standing behind a wall.
    Image plainFrame(320, 180);
    Image hiddenFrame(320, 180);
    render3D(plainFrame, map, kSpawn, 180.0, kDefaultFov, {});
    render3D(hiddenFrame, map, kSpawn, 180.0, kDefaultFov, behindWall);
    if (countChangedPixels(plainFrame, hiddenFrame) != 0) {
        error = "a zombie behind a wall is still drawn";
        return false;
    }

    Image barImage(80, 24);
    barImage.fill(makeColor(0, 0, 0));
    drawHealthBar(barImage, 10, 5, 60, 8, 0.5);
    const Color filled = barImage.at(15, 7);
    const Color depleted = barImage.at(45, 7);
    if (filled.g < 150 || depleted.r < 40 || depleted.g > 40) {
        error = "health bar colours do not match the health fraction";
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
    if (selftestZombies(map, error)) {
        std::printf("  [ok] zombies: spawns walkable, shots hit, walls occlude sprites\n");
        std::printf("  [ok] health bars: colours follow the health fraction\n");
    } else {
        std::printf("  [FAIL] zombies: %s\n", error.c_str());
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

    std::vector<Zombie> zombies = createZombies();
    int playerHealth = kPlayerMaxHealth;
    int kills = 0;
    bool playerDead = false;
    bool shootRequested = false;
    double shotCooldown = 0.0;
    double muzzleFlash = 0.0;
    double damageFlash = 0.0;

    double forwardIntent = 0.0;
    double strafeIntent = 0.0;
    double turnIntent = 0.0;
    double displayedFps = 0.0;
    auto previousFrame = std::chrono::steady_clock::now();

    const auto restart = [&]() {
        position = options.position;
        yaw = options.yawDegrees;
        zombies = createZombies();
        playerHealth = kPlayerMaxHealth;
        kills = 0;
        playerDead = false;
        shootRequested = false;
        shotCooldown = 0.0;
        muzzleFlash = 0.0;
        damageFlash = 0.0;
        forwardIntent = 0.0;
        strafeIntent = 0.0;
        turnIntent = 0.0;
    };

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
                case ' ':
                    shootRequested = true;
                    break;
                case 'r':
                case 'R':
                    restart();
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

        shotCooldown = std::max(0.0, shotCooldown - deltaSeconds);
        muzzleFlash = std::max(0.0, muzzleFlash - deltaSeconds * 6.0);
        damageFlash = std::max(0.0, damageFlash - deltaSeconds * 2.5);

        const double forwardStep = std::clamp(forwardIntent, -deltaSeconds, deltaSeconds);
        forwardIntent -= forwardStep;
        const double strafeStep = std::clamp(strafeIntent, -deltaSeconds, deltaSeconds);
        strafeIntent -= strafeStep;
        const double turnStep = std::clamp(turnIntent, -deltaSeconds, deltaSeconds);
        turnIntent -= turnStep;

        const Vec2 direction = directionFromDegrees(yaw);
        if (!playerDead) {
            if (std::fabs(forwardStep) > 0.0) {
                const double amount = forwardStep * 2.6;
                tryMove(map, position, Vec2{direction.x * amount, direction.y * amount});
            }
            if (std::fabs(strafeStep) > 0.0) {
                const double amount = strafeStep * 2.1;
                tryMove(map, position, Vec2{-direction.y * amount, direction.x * amount});
            }
        }
        if (std::fabs(turnStep) > 0.0) {
            yaw = normalizeDegrees(yaw + turnStep * 110.0);
        }

        const int incomingDamage =
            updateZombies(zombies, map, position, deltaSeconds, !playerDead);
        if (incomingDamage > 0) {
            playerHealth = std::max(0, playerHealth - incomingDamage);
            damageFlash = 0.6;
            if (playerHealth == 0) {
                playerDead = true;
            }
        }

        if (shootRequested && !playerDead && shotCooldown <= 0.0) {
            shotCooldown = kShotCooldown;
            muzzleFlash = 0.09;
            const int target =
                findShotTarget(zombies, map, position, directionFromDegrees(yaw));
            if (target >= 0 &&
                damageZombie(zombies[static_cast<std::size_t>(target)], kShotDamage)) {
                ++kills;
            }
        }
        shootRequested = false;

        const auto [columns, rows] = terminalSize();
        const int width = std::clamp(columns, 20, 400);
        const int sceneRows = std::clamp(rows - 1, 4, 200);
        const int height = sceneRows * 2;
        Image image(width, height);
        renderWorld(image, map, position, yaw, options.fovDegrees, zombies, mapMode,
                    minimap, muzzleFlash > 0.0);
        if (damageFlash > 0.0) {
            image.fillRect(0, 0, width, height, makeColor(150, 16, 16),
                           std::min(0.28, damageFlash * 0.4));
        }
        if (playerDead) {
            image.fillRect(0, 0, width, height, makeColor(48, 0, 0), 0.35);
        }
        renderPlayerHud(image, playerHealth);

        std::string output = renderTerminalFrame(image);
        char status[512];
        if (playerDead) {
            std::snprintf(status, sizeof(status),
                          "  raycasting | YOU DIED - press R to restart | kills=%d | Q quit",
                          kills);
        } else {
            std::snprintf(status, sizeof(status),
                          "  raycasting | HP %d | kills %d | %s | fps=%.0f | WASD move, "
                          "Space shoot, Tab map, M minimap, R restart, Q quit",
                          playerHealth, kills,
                          mapMode ? "top-down ray view" : "3D view", displayedFps);
        }
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
        const std::vector<Zombie> zombies = createZombies();
        Image image(options.width, options.height);
        renderWorld(image, map, options.position, options.yawDegrees, options.fovDegrees,
                    zombies, options.mapView, options.minimap, false);
        renderPlayerHud(image, kPlayerMaxHealth);
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
