#include "hud.hpp"

#include <algorithm>
#include <cmath>

namespace g3d {
namespace {

Vec2 directionFromRadians(double radians) {
    return Vec2{std::cos(radians), std::sin(radians)};
}

const std::uint8_t kDigitGlyphs[10][5] = {
    {7, 5, 5, 5, 7},  // 0
    {2, 6, 2, 2, 7},  // 1
    {7, 1, 7, 4, 7},  // 2
    {7, 1, 7, 1, 7},  // 3
    {5, 5, 7, 1, 1},  // 4
    {7, 4, 7, 1, 7},  // 5
    {7, 4, 7, 5, 7},  // 6
    {7, 1, 1, 2, 2},  // 7
    {7, 5, 7, 5, 7},  // 8
    {7, 5, 7, 1, 7},  // 9
};
const std::uint8_t kComboGlyph[5] = {5, 5, 2, 5, 5};

void drawGlyph(Image& image, int x, int y, const std::uint8_t rows[5], int scale,
               Color color) {
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 3; ++column) {
            if ((rows[row] & static_cast<std::uint8_t>(1U << (2 - column))) != 0) {
                image.fillRect(x + column * scale, y + row * scale, scale, scale,
                               color);
            }
        }
    }
}

void drawNumberRight(Image& image, int rightX, int y, int value, int scale,
                     Color color) {
    int number = std::max(0, value);
    int digits[12] = {};
    int count = 0;
    do {
        digits[count++] = number % 10;
        number /= 10;
    } while (number > 0 && count < 12);
    const int pitch = 4 * scale;
    int x = rightX - (count * pitch - scale);
    for (int i = count - 1; i >= 0; --i) {
        drawGlyph(image, x, y, kDigitGlyphs[digits[i]], scale, color);
        x += pitch;
    }
}

void paintDashMeter(Image& image, const Game& game, int x, int y, int width,
                    int height) {
    const int unit = std::max(2, height / 3);
    const int iconWidth = 3 * unit;
    image.fillRect(x - 8, y - 6, width + iconWidth + 30, height + 12,
                   makeColor(5, 7, 10), 0.55);

    const bool dashing = game.player.dashTimer > 0.0;
    const bool ready = dashReady(game);
    const Color color = dashing ? makeColor(255, 246, 214)
                                : (ready ? makeColor(92, 226, 232)
                                         : makeColor(214, 150, 66));
    const int iconY = y + std::max(0, (height - 5 * unit) / 2);
    image.fillRect(x + 2 * unit, iconY, unit, unit, color);
    image.fillRect(x + unit, iconY + unit, 2 * unit, unit, color);
    image.fillRect(x, iconY + 2 * unit, 3 * unit, unit, color);
    image.fillRect(x + unit, iconY + 3 * unit, 2 * unit, unit, color);
    image.fillRect(x + 2 * unit, iconY + 4 * unit, unit, unit, color);

    const int barX = x + iconWidth + 8;
    image.fillRect(barX, y, width, height, makeColor(28, 34, 42));
    double fraction = 0.0;
    if (dashing) {
        fraction = clampd(game.player.dashTimer / kDashDuration, 0.0, 1.0);
    } else if (ready) {
        fraction = 1.0;
    } else {
        fraction = clampd(1.0 - game.player.dashCooldown / kDashCooldown, 0.0, 1.0);
    }
    image.fillRect(barX, y, static_cast<int>(std::lround(width * fraction)), height,
                   color);
    for (int tick = 1; tick < 4; ++tick) {
        image.fillRect(barX + width * tick / 4, y, 1, height,
                       makeColor(8, 10, 14), 0.85);
    }
    if (ready) {
        const double pulse = 0.22 + 0.18 * std::sin(game.elapsed * 7.0);
        image.fillRect(barX - 1, y - 1, width + 2, 1, color, pulse);
        image.fillRect(barX - 1, y + height, width + 2, 1, color, pulse);
    }
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

    if (!game.player.inVehicle) {
        const int dashWidth = std::clamp(barWidth * 3 / 5, 64, 150);
        const int dashHeight = std::max(4, barHeight - 2);
        paintDashMeter(image, game, x, y - barHeight - 17, dashWidth, dashHeight);
    }

    const int scoreScale = std::clamp(image.width() / 320, 2, 4);
    const int scoreRight = image.width() - 14;
    const int scoreY = 14;
    const std::string scoreText = std::to_string(std::max(0, game.score));
    const int scoreWidth = static_cast<int>(scoreText.size()) * 4 * scoreScale -
                           scoreScale;
    image.fillRect(scoreRight - scoreWidth - 10, scoreY - 7, scoreWidth + 20,
                   5 * scoreScale + 14, makeColor(5, 7, 10), 0.55);
    drawNumberRight(image, scoreRight, scoreY, game.score, scoreScale,
                    makeColor(255, 206, 104));
    if (game.combo > 1) {
        const int comboY = scoreY + 5 * scoreScale + 9;
        const int comboRight = scoreRight;
        drawGlyph(image, comboRight - 7 * scoreScale, comboY, kComboGlyph,
                  scoreScale, makeColor(120, 240, 244));
        drawNumberRight(image, comboRight, comboY, game.combo, scoreScale,
                        makeColor(120, 240, 244));
    }

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

void drawDashEffects(Image& image, const Game& game) {
    if (game.player.inVehicle || game.player.dashTimer <= 0.0) {
        return;
    }
    const double intensity =
        clampd(game.player.dashTimer / kDashDuration, 0.0, 1.0);
    const int centerX = image.width() / 2;
    const int centerY = image.height() / 2;
    const double radius =
        std::hypot(static_cast<double>(image.width()),
                   static_cast<double>(image.height())) *
        0.58;
    constexpr int lineCount = 26;
    for (int i = 0; i < lineCount; ++i) {
        const double wobble =
            std::sin(game.elapsed * 21.0 + static_cast<double>(i) * 2.37);
        const double angle =
            2.0 * kPi * static_cast<double>(i) / lineCount + wobble * 0.035;
        const double inner = radius * (0.30 + 0.10 * wobble);
        const double outer = radius * (0.66 + 0.18 * wobble);
        const Color color =
            i % 4 == 0 ? makeColor(255, 204, 118) : makeColor(235, 246, 255);
        image.drawLine(
            centerX + static_cast<int>(std::lround(std::cos(angle) * inner)),
            centerY + static_cast<int>(std::lround(std::sin(angle) * inner)),
            centerX + static_cast<int>(std::lround(std::cos(angle) * outer)),
            centerY + static_cast<int>(std::lround(std::sin(angle) * outer)), color,
            intensity * (0.10 + 0.12 * std::fabs(wobble)));
    }
    image.fillRect(0, 0, image.width(), image.height(), makeColor(255, 196, 96),
                   intensity * 0.035);
}

}  // namespace g3d
