#include "hud.hpp"

#include <algorithm>
#include <cmath>

namespace g3d {
namespace {

Vec2 directionFromRadians(double radians) {
    return Vec2{std::cos(radians), std::sin(radians)};
}

void paintMapCells(Image& image, const Grid& map, int offsetX, int offsetY, int scale) {
    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            const int pixelX = offsetX + x * scale;
            const int pixelY = offsetY + y * scale;
            if (map[y][x] != 0) {
                const Color base = mapMaterialColor(map[y][x]);
                image.fillRect(pixelX, pixelY, scale, scale, base);
                if (scale >= 4) {
                    image.fillRect(pixelX, pixelY, scale, 1,
                                   mix(base, makeColor(255, 255, 255), 0.18));
                    image.fillRect(pixelX, pixelY + scale - 1, scale, 1,
                                   shade(base, 0.65));
                }
            } else {
                const Color floorColor = ((x + y) & 1) ? makeColor(19, 23, 30)
                                                       : makeColor(23, 27, 34);
                image.fillRect(pixelX, pixelY, scale, scale, floorColor);
            }
        }
    }
}

void paintPlayerMarker(Image& image, Vec2 position, double yawDegrees, int offsetX,
                       int offsetY, int scale) {
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
                   static_cast<int>(std::lround(centerX + direction.x * scale * 1.6)),
                   static_cast<int>(std::lround(centerY + direction.y * scale * 1.6)),
                   makeColor(255, 238, 190), 0.95);
}

void paintZombieMarkers(Image& image, const Game& game, int offsetX, int offsetY,
                        int scale) {
    for (const Zombie& zombie : game.zombies) {
        if (!zombie.alive) {
            continue;
        }
        const int centerX =
            static_cast<int>(std::lround(offsetX + zombie.position.x * scale));
        const int centerY =
            static_cast<int>(std::lround(offsetY + zombie.position.y * scale));
        const int radius = std::max(1, scale / 3);
        image.drawCircle(centerX, centerY, radius + 1, makeColor(10, 12, 12), 0.9);
        image.drawCircle(centerX, centerY, radius, makeColor(210, 72, 56), 1.0);
        image.blend(centerX, centerY, makeColor(255, 190, 90), 0.85);
    }
}

void paintVehicleMarkers(Image& image, const Game& game, int offsetX, int offsetY,
                         int scale) {
    for (std::size_t index = 0; index < game.vehicles.size(); ++index) {
        const Vehicle& vehicle = game.vehicles[index];
        if (game.player.inVehicle &&
            game.player.vehicleIndex == static_cast<int>(index)) {
            continue;
        }
        const Vec2 forward = directionFromRadians(vehicle.heading);
        const Vec2 rear = vehicle.position - forward * 0.55;
        const Vec2 front = vehicle.position + forward * 0.55;
        image.drawLine(static_cast<int>(std::lround(offsetX + rear.x * scale)),
                       static_cast<int>(std::lround(offsetY + rear.y * scale)),
                       static_cast<int>(std::lround(offsetX + front.x * scale)),
                       static_cast<int>(std::lround(offsetY + front.y * scale)),
                       makeColor(238, 168, 74), 0.95);
        image.drawCircle(static_cast<int>(std::lround(offsetX + vehicle.position.x * scale)),
                         static_cast<int>(std::lround(offsetY + vehicle.position.y * scale)),
                         std::max(2, scale / 4), makeColor(255, 210, 120), 1.0);
    }
}

}  // namespace

void drawHealthBar(Image& image, int x, int y, int width, int height,
                   double fraction) {
    const double clamped = clampd(fraction, 0.0, 1.0);
    const Color color = clamped > 0.6
                            ? makeColor(88, 208, 96)
                            : (clamped > 0.3 ? makeColor(232, 180, 62)
                                             : makeColor(218, 66, 54));
    image.fillRect(x - 1, y - 1, width + 2, height + 2, makeColor(6, 8, 11), 0.9);
    image.fillRect(x, y, static_cast<int>(std::lround(width * clamped)), height, color);
    if (width > 8) {
        image.fillRect(x, y, static_cast<int>(std::lround(width * clamped)),
                       std::max(1, height / 3), mix(color, makeColor(255, 255, 255),
                                                    0.25));
    }
}

void drawPlayerHud(Image& image, const Game& game) {
    const int barWidth = std::clamp(image.width() / 4, 100, 260);
    const int barHeight = std::clamp(barWidth / 16, 6, 12);
    const int x = 12;
    const int y = image.height() - barHeight - 14;
    const int iconSize = barHeight + 4;
    image.fillRect(x - 8, y - 9, barWidth + iconSize + 30, barHeight + 18,
                   makeColor(5, 7, 10), 0.55);
    image.fillRect(x, y - 2, iconSize, iconSize, makeColor(126, 32, 30));
    image.fillRect(x + iconSize / 2 - 1, y, 2, iconSize - 4, makeColor(240, 236, 230));
    image.fillRect(x + 2, y + iconSize / 2 - 3, iconSize - 4, 2,
                   makeColor(240, 236, 230));
    drawHealthBar(image, x + iconSize + 8, y, barWidth, barHeight,
                  static_cast<double>(game.player.health) / kPlayerMaxHealth);

    if (!game.player.inVehicle || game.player.vehicleIndex < 0 ||
        game.player.vehicleIndex >= static_cast<int>(game.vehicles.size())) {
        return;
    }
    const Vehicle& vehicle =
        game.vehicles[static_cast<std::size_t>(game.player.vehicleIndex)];
    const int meterWidth = std::clamp(image.width() / 5, 90, 220);
    const int meterHeight = std::clamp(meterWidth / 24, 5, 9);
    const int meterX = image.width() - meterWidth - 16;
    const int meterY = image.height() - meterHeight - 16;
    image.fillRect(meterX - 8, meterY - 10, meterWidth + 16, meterHeight + 20,
                   makeColor(5, 7, 10), 0.55);
    image.fillRect(meterX, meterY, meterWidth, meterHeight, makeColor(28, 34, 42));
    const double fraction =
        clampd(std::fabs(vehicle.speed) / kMaxVehicleSpeed, 0.0, 1.0);
    const Color speedColor = fraction > 0.75
                                 ? makeColor(224, 108, 62)
                                 : makeColor(86, 186, 216);
    image.fillRect(meterX, meterY,
                   static_cast<int>(std::lround(meterWidth * fraction)), meterHeight,
                   speedColor);
    for (int tick = 1; tick < 4; ++tick) {
        image.fillRect(meterX + meterWidth * tick / 4, meterY, 1, meterHeight,
                       makeColor(8, 10, 14), 0.85);
    }
}

void drawCrosshair(Image& image, bool firing) {
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

void drawMinimap(Image& image, const Game& game) {
    const int scale = std::clamp(std::min(image.width(), image.height()) / 70, 2, 10);
    const int offsetX = 12;
    const int offsetY = 12;
    const int mapWidth = kMapWidth * scale;
    const int mapHeight = kMapHeight * scale;

    image.fillRect(offsetX - 4, offsetY - 4, mapWidth + 8, mapHeight + 8,
                   makeColor(3, 5, 8), 0.62);
    image.fillRect(offsetX - 1, offsetY - 1, mapWidth + 2, 1, makeColor(90, 105, 128),
                   0.8);
    image.fillRect(offsetX - 1, offsetY + mapHeight, mapWidth + 2, 1,
                   makeColor(90, 105, 128), 0.8);
    image.fillRect(offsetX - 1, offsetY - 1, 1, mapHeight + 2, makeColor(90, 105, 128),
                   0.8);
    image.fillRect(offsetX + mapWidth, offsetY - 1, 1, mapHeight + 2,
                   makeColor(90, 105, 128), 0.8);

    paintMapCells(image, game.map, offsetX, offsetY, scale);
    paintVehicleMarkers(image, game, offsetX, offsetY, scale);
    paintZombieMarkers(image, game, offsetX, offsetY, scale);
    if (game.player.inVehicle && game.player.vehicleIndex >= 0 &&
        game.player.vehicleIndex < static_cast<int>(game.vehicles.size())) {
        const Vehicle& vehicle =
            game.vehicles[static_cast<std::size_t>(game.player.vehicleIndex)];
        paintPlayerMarker(image, vehicle.position,
                          normalizeDegrees(vehicle.heading / kDegToRad), offsetX,
                          offsetY, scale);
    } else {
        paintPlayerMarker(image, game.player.position, game.player.yawDegrees, offsetX,
                          offsetY, scale);
    }
}

}  // namespace g3d
