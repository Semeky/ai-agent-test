#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace g3d {
namespace {

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

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7feb352dU;
    x ^= x >> 15U;
    x *= 0x846ca68bU;
    x ^= x >> 16U;
    return x;
}

double noise01(int x, int y, int seed) {
    const std::uint32_t value = hash32(static_cast<std::uint32_t>(x) * 374761393U +
                                       static_cast<std::uint32_t>(y) * 668265263U +
                                       static_cast<std::uint32_t>(seed) * 2246822519U);
    return static_cast<double>(value & 0xffffffU) / static_cast<double>(0x1000000U);
}

Vec2 directionFromRadians(double radians) {
    return Vec2{std::cos(radians), std::sin(radians)};
}

double normalizeRadians(double radians) {
    double result = std::fmod(radians, 2.0 * kPi);
    if (result < 0.0) {
        result += 2.0 * kPi;
    }
    return result;
}

double cellHeight(std::uint8_t material) {
    if (material == 0) {
        return 0.0;
    }
    return material == 5 ? kFountainHeight : kWallHeight;
}

void addParticle(Game& game, Vec2 position, double height, Vec2 velocity,
                 double verticalSpeed, double life, double size, Color color,
                 bool glow) {
    if (game.particles.size() >= kMaxParticles) {
        return;
    }
    Particle particle;
    particle.position = position;
    particle.height = height;
    particle.velocity = velocity;
    particle.verticalSpeed = verticalSpeed;
    particle.life = life;
    particle.maxLife = life;
    particle.size = size;
    particle.color = color;
    particle.glow = glow;
    game.particles.push_back(particle);
}

void spawnBlood(Game& game, Vec2 position, Vec2 direction, int count) {
    const double baseAngle = std::atan2(direction.y, direction.x);
    const int tick =
        static_cast<int>(std::fmod(game.elapsed * 1000.0, 100000.0));
    for (int i = 0; i < count; ++i) {
        const double a = noise01(i, 17, tick);
        const double b = noise01(i, 29, tick + 7);
        const double angle = baseAngle + (a - 0.5) * 2.1;
        const double speed = 0.7 + b * 2.8;
        addParticle(game, position, 0.75 + a * 0.65,
                    Vec2{std::cos(angle) * speed, std::sin(angle) * speed},
                    0.4 + b * 2.4, 0.45 + b * 0.55, 0.035 + a * 0.045,
                    mix(makeColor(128, 12, 12), makeColor(210, 46, 30), b), false);
    }
}

void spawnDust(Game& game, Vec2 position, Vec2 direction, int count, bool trail) {
    const double baseAngle = std::atan2(direction.y, direction.x);
    const int tick =
        static_cast<int>(std::fmod(game.elapsed * 1000.0, 100000.0));
    for (int i = 0; i < count; ++i) {
        const double a = noise01(i, 41, tick + 3);
        const double b = noise01(i, 53, tick + 11);
        const double angle = baseAngle + kPi + (a - 0.5) * 1.3;
        const double speed = 0.35 + b * 1.2;
        addParticle(game, position, 0.12 + a * 0.35,
                    Vec2{std::cos(angle) * speed, std::sin(angle) * speed},
                    0.25 + b * 0.75, 0.3 + b * 0.45, 0.045 + a * 0.05,
                    trail ? mix(makeColor(255, 186, 74), makeColor(255, 238, 196), b)
                          : mix(makeColor(104, 96, 84), makeColor(168, 154, 136), b),
                    true);
    }
}

void updateParticles(Game& game, double deltaSeconds) {
    std::size_t writeIndex = 0;
    for (std::size_t i = 0; i < game.particles.size(); ++i) {
        Particle particle = game.particles[i];
        particle.life -= deltaSeconds;
        if (particle.life <= 0.0) {
            continue;
        }
        if (particle.glow) {
            particle.velocity = particle.velocity * std::exp(-2.4 * deltaSeconds);
            particle.verticalSpeed += 0.9 * deltaSeconds;
        } else {
            particle.velocity = particle.velocity * std::exp(-1.6 * deltaSeconds);
            particle.verticalSpeed -= 9.8 * deltaSeconds;
            particle.height += particle.verticalSpeed * deltaSeconds;
            if (particle.height < 0.02 && particle.verticalSpeed < 0.0) {
                particle.height = 0.02;
                particle.verticalSpeed *= -0.22;
                particle.velocity = particle.velocity * 0.55;
            }
        }
        particle.position += particle.velocity * deltaSeconds;
        game.particles[writeIndex++] = particle;
    }
    game.particles.resize(writeIndex);
}

void awardKill(Game& game, bool dashKill) {
    ++game.kills;
    if (dashKill) {
        game.combo = std::min(5, game.combo + 1);
        game.comboTimer = 1.8;
        game.score += static_cast<int>(kDashScoreBonus) * game.combo;
    } else {
        game.combo = 0;
        game.comboTimer = 0.0;
        game.score += 100;
    }
    game.soundEvents.push_back(SoundEvent::Kill);
}

double distanceToVehicle(const Vehicle& vehicle, Vec2 point) {
    const Vec2 forward = directionFromRadians(vehicle.heading);
    const Vec2 rear = vehicle.position - forward * 0.5;
    const Vec2 front = vehicle.position + forward * 0.5;
    const Vec2 axis = front - rear;
    const double axisLengthSquared = lengthSquared(axis);
    double t = 0.0;
    if (axisLengthSquared > 1e-12) {
        t = clampd(dot(point - rear, axis) / axisLengthSquared, 0.0, 1.0);
    }
    return length(point - (rear + axis * t));
}

double distanceToSegment(Vec2 point, Vec2 from, Vec2 to) {
    const Vec2 axis = to - from;
    const double lengthSquaredAxis = lengthSquared(axis);
    if (lengthSquaredAxis < 1e-12) {
        return length(point - from);
    }
    const double t = clampd(dot(point - from, axis) / lengthSquaredAxis, 0.0, 1.0);
    return length(point - (from + axis * t));
}

bool circleHitsVehicle(const Vehicle& vehicle, Vec2 point, double radius) {
    return distanceToVehicle(vehicle, point) < radius + 0.38;
}

struct MoveResult {
    bool blockedX = false;
    bool blockedY = false;
};

bool playerPositionClear(const Game& game, Vec2 position) {
    if (!isWalkableCentered(game.map, position, kPlayerRadius)) {
        return false;
    }
    for (const Vehicle& vehicle : game.vehicles) {
        if (circleHitsVehicle(vehicle, position, kPlayerRadius)) {
            return false;
        }
    }
    return true;
}

MoveResult movePlayer(Game& game, Vec2 delta) {
    MoveResult result;
    const Vec2 tryX{game.player.position.x + delta.x, game.player.position.y};
    if (playerPositionClear(game, tryX)) {
        game.player.position.x = tryX.x;
    } else if (std::fabs(delta.x) > 1e-12) {
        result.blockedX = true;
    }
    const Vec2 tryY{game.player.position.x, game.player.position.y + delta.y};
    if (playerPositionClear(game, tryY)) {
        game.player.position.y = tryY.y;
    } else if (std::fabs(delta.y) > 1e-12) {
        result.blockedY = true;
    }
    return result;
}

bool vehicleClear(const Grid& map, const std::vector<Vehicle>& vehicles, int index,
                  Vec2 position, double heading) {
    const Vec2 forward = directionFromRadians(heading);
    const Vec2 ownPoints[2] = {position + forward * 0.5, position - forward * 0.5};
    for (const Vec2& point : ownPoints) {
        if (!isWalkableCentered(map, point, kVehicleRadius)) {
            return false;
        }
    }
    for (int other = 0; other < static_cast<int>(vehicles.size()); ++other) {
        if (other == index) {
            continue;
        }
        const Vec2 otherForward = directionFromRadians(vehicles[static_cast<std::size_t>(other)].heading);
        const Vec2 otherPoints[2] = {
            vehicles[static_cast<std::size_t>(other)].position + otherForward * 0.5,
            vehicles[static_cast<std::size_t>(other)].position - otherForward * 0.5};
        for (const Vec2& a : ownPoints) {
            for (const Vec2& b : otherPoints) {
                if (length(a - b) < 0.58) {
                    return false;
                }
            }
        }
    }
    return true;
}

MoveResult moveVehicle(Game& game, int index, Vec2 delta) {
    MoveResult result;
    Vehicle& vehicle = game.vehicles[static_cast<std::size_t>(index)];
    const Vec2 tryX{vehicle.position.x + delta.x, vehicle.position.y};
    if (vehicleClear(game.map, game.vehicles, index, tryX, vehicle.heading)) {
        vehicle.position.x = tryX.x;
    } else if (std::fabs(delta.x) > 1e-12) {
        result.blockedX = true;
    }
    const Vec2 tryY{vehicle.position.x, vehicle.position.y + delta.y};
    if (vehicleClear(game.map, game.vehicles, index, tryY, vehicle.heading)) {
        vehicle.position.y = tryY.y;
    } else if (std::fabs(delta.y) > 1e-12) {
        result.blockedY = true;
    }
    return result;
}

void updateVehicle(Game& game, int index, double throttle, double steerInput,
                   double deltaSeconds) {
    Vehicle& vehicle = game.vehicles[static_cast<std::size_t>(index)];
    if (throttle > 0.0) {
        if (vehicle.speed < 0.0) {
            vehicle.speed += kVehicleBraking * throttle * deltaSeconds;
        } else {
            vehicle.speed += kVehicleAcceleration * throttle * deltaSeconds;
        }
    } else if (throttle < 0.0) {
        if (vehicle.speed > 0.05) {
            vehicle.speed += throttle * kVehicleBraking * deltaSeconds;
        } else {
            vehicle.speed += throttle * kVehicleReverseAcceleration * deltaSeconds;
        }
    } else {
        const double drag = (kVehicleDrag + 0.5) * deltaSeconds;
        if (std::fabs(vehicle.speed) <= drag) {
            vehicle.speed = 0.0;
        } else {
            vehicle.speed -= vehicle.speed > 0.0 ? drag : -drag;
        }
    }
    vehicle.speed = clampd(vehicle.speed, -kMaxReverseSpeed, kMaxVehicleSpeed);

    const double speedFraction =
        std::min(1.0, std::fabs(vehicle.speed) / kMaxVehicleSpeed);
    const double targetSteer =
        clampd(steerInput, -1.0, 1.0) * kVehicleMaxSteer * (1.0 - 0.45 * speedFraction);
    const double steerRate =
        (std::fabs(steerInput) > 0.05 ? 3.4 : 5.0) * deltaSeconds;
    vehicle.steer += clampd(targetSteer - vehicle.steer, -steerRate, steerRate);

    if (std::fabs(vehicle.speed) > 0.02) {
        const double candidateHeading =
            normalizeRadians(vehicle.heading +
                             (vehicle.speed / kVehicleWheelBase) *
                                 std::tan(vehicle.steer) * deltaSeconds);
        if (vehicleClear(game.map, game.vehicles, index, vehicle.position,
                         candidateHeading)) {
            vehicle.heading = candidateHeading;
        } else {
            vehicle.steer *= 0.5;
        }
    }

    const Vec2 forward = directionFromRadians(vehicle.heading);
    const Vec2 delta = forward * (vehicle.speed * deltaSeconds);
    const MoveResult moved = moveVehicle(game, index, delta);
    if (moved.blockedX && moved.blockedY) {
        vehicle.speed = std::fabs(vehicle.speed) > 1.5 ? -vehicle.speed * 0.2 : 0.0;
    } else if (moved.blockedX || moved.blockedY) {
        const bool blockedForward =
            std::fabs(delta.x) >= std::fabs(delta.y) ? moved.blockedX : moved.blockedY;
        vehicle.speed *= blockedForward ? 0.45 : 0.9;
    }
    vehicle.wheelSpin = normalizeRadians(vehicle.wheelSpin +
                                         vehicle.speed * deltaSeconds / 0.27);
}

void applyVehicleImpacts(Game& game) {
    for (const Vehicle& vehicle : game.vehicles) {
        const double speedAbs = std::fabs(vehicle.speed);
        if (speedAbs < kVehicleImpactMinSpeed) {
            continue;
        }
        const Vec2 forward = directionFromRadians(vehicle.heading);
        const Vec2 motion = forward * (vehicle.speed >= 0.0 ? 1.0 : -1.0);
        for (Zombie& zombie : game.zombies) {
            if (!zombie.alive || zombie.impactCooldown > 0.0) {
                continue;
            }
            if (distanceToVehicle(vehicle, zombie.position) > 0.62) {
                continue;
            }
            const int damage = 10 + static_cast<int>(std::lround(
                                       (speedAbs - kVehicleImpactMinSpeed) * 26.0));
            Vec2 impulseDirection = normalized(
                motion * 0.75 + normalized(zombie.position - vehicle.position) * 0.25);
            if (lengthSquared(impulseDirection) < 1e-9) {
                impulseDirection = motion;
            }
            const Vec2 impulse =
                impulseDirection * (2.2 + speedAbs * 0.85);
            const bool killed = damageZombie(
                zombie, damage, impulse, speedAbs >= kVehicleImpactFlattenSpeed);
            zombie.stunTimer = std::max(zombie.stunTimer, 0.55 + speedAbs * 0.08);
            zombie.impactCooldown = kVehicleImpactCooldown;
            spawnBlood(game, zombie.position, impulseDirection,
                       10 + static_cast<int>(speedAbs * 1.5));
            spawnDust(game, zombie.position, impulseDirection, 4, false);
            game.shake = std::max(game.shake, 0.35 + speedAbs * 0.06);
            game.soundEvents.push_back(SoundEvent::Impact);
            if (killed) {
                awardKill(game, false);
            }
        }
    }
}

void updatePlayerDash(Game& game, double deltaSeconds) {
    Player& player = game.player;
    if (player.dashTimer <= 0.0) {
        return;
    }
    player.dashTimer = std::max(0.0, player.dashTimer - deltaSeconds);
    const double step = kDashSpeed * deltaSeconds;
    const Vec2 before = player.position;
    const MoveResult moved = movePlayer(game, player.dashDirection * step);
    const double travelled = length(player.position - before);
    if ((moved.blockedX || moved.blockedY) && travelled < step * 0.5) {
        player.dashTimer = 0.0;
        game.shake = std::max(game.shake, 0.5);
        spawnDust(game, player.position, player.dashDirection, 5, false);
    }

    player.dashTrailTimer -= deltaSeconds;
    if (player.dashTrailTimer <= 0.0) {
        player.dashTrailTimer = 0.035;
        spawnDust(game, player.position, player.dashDirection, 2, true);
    }

    const Vec2 after = player.position;
    for (Zombie& zombie : game.zombies) {
        if (!zombie.alive || zombie.lastDashId == player.dashId) {
            continue;
        }
        if (distanceToSegment(zombie.position, before, after) > kDashHitRadius) {
            continue;
        }
        zombie.lastDashId = player.dashId;
        const bool killed =
            damageZombie(zombie, kDashDamage, player.dashDirection * kDashKnockback,
                         false);
        zombie.stunTimer = std::max(zombie.stunTimer, 0.7);
        spawnBlood(game, zombie.position, player.dashDirection, killed ? 16 : 10);
        spawnDust(game, zombie.position, player.dashDirection, 3, false);
        game.shake = std::max(game.shake, 0.6);
        game.soundEvents.push_back(SoundEvent::Impact);
        if (killed) {
            awardKill(game, true);
            player.dashCooldown = std::max(0.0, player.dashCooldown - 0.22);
        }
    }

    game.shake = std::max(
        game.shake, 0.22 * (player.dashTimer / kDashDuration));
    if (player.dashTimer <= 0.0) {
        spawnDust(game, player.position, player.dashDirection, 4, false);
    }
}

std::vector<Vehicle> createVehicles() {
    std::vector<Vehicle> vehicles;
    vehicles.reserve(kVehicleSpawns.size());
    for (std::size_t i = 0; i < kVehicleSpawns.size(); ++i) {
        Vehicle vehicle;
        vehicle.id = static_cast<int>(i);
        vehicle.spawn = kVehicleSpawns[i];
        vehicle.spawnHeading = kVehicleSpawnHeadings[i] * kDegToRad;
        vehicle.position = vehicle.spawn;
        vehicle.heading = vehicle.spawnHeading;
        vehicles.push_back(vehicle);
    }
    return vehicles;
}

std::vector<Zombie> createZombies(Vec2 startPosition) {
    std::vector<Zombie> zombies;
    zombies.reserve(kZombieSpawns.size());
    for (std::size_t i = 0; i < kZombieSpawns.size(); ++i) {
        Zombie zombie;
        zombie.spawn = kZombieSpawns[i];
        zombie.position = zombie.spawn;
        zombie.maxHealth = kZombieMaxHealth;
        zombie.health = kZombieStartHealth[i];
        zombie.attackTimer = 0.6;
        const Vec2 toPlayer = startPosition - zombie.spawn;
        zombie.heading = std::atan2(toPlayer.y, toPlayer.x);
        zombies.push_back(zombie);
    }
    return zombies;
}

void tryMoveZombie(const Game& game, Zombie& zombie, Vec2 delta) {
    const auto clear = [&](Vec2 candidate) {
        if (!isWalkableCentered(game.map, candidate, 0.24)) {
            return false;
        }
        for (const Vehicle& vehicle : game.vehicles) {
            const double current = distanceToVehicle(vehicle, zombie.position);
            const double next = distanceToVehicle(vehicle, candidate);
            // A zombie that is already overlapping a hull (for example after a
            // vehicle hit) must be able to move away; only block motion that
            // pushes it deeper into the car.
            if (next < 0.62 && next < current - 1e-9) {
                return false;
            }
        }
        return true;
    };
    const Vec2 tryX{zombie.position.x + delta.x, zombie.position.y};
    if (clear(tryX)) {
        zombie.position.x = tryX.x;
    }
    const Vec2 tryY{zombie.position.x, zombie.position.y + delta.y};
    if (clear(tryY)) {
        zombie.position.y = tryY.y;
    }
}

int updateZombies(Game& game, double deltaSeconds) {
    int playerDamage = 0;
    const Vec2 target = game.player.position;
    for (Zombie& zombie : game.zombies) {
        zombie.hitFlash = std::max(0.0, zombie.hitFlash - deltaSeconds);
        if (!zombie.alive) {
            zombie.respawnTimer -= deltaSeconds;
            if (zombie.ragdoll) {
                zombie.deathElapsed += deltaSeconds;
                if (lengthSquared(zombie.knockback) > 1e-8) {
                    tryMoveZombie(game, zombie, zombie.knockback * deltaSeconds);
                    zombie.knockback =
                        zombie.knockback * std::exp(-3.0 * deltaSeconds);
                    if (length(zombie.knockback) < 0.05) {
                        zombie.knockback = Vec2{};
                    }
                }
            }
            if (zombie.respawnTimer <= 0.0) {
                const Vec2 toPlayer = zombie.spawn - target;
                if (length(toPlayer) > 2.5 &&
                    isWalkableCentered(game.map, zombie.spawn, 0.25)) {
                    zombie.position = zombie.spawn;
                    zombie.health = zombie.maxHealth;
                    zombie.alive = true;
                    zombie.ragdoll = false;
                    zombie.flattened = false;
                    zombie.deathElapsed = 0.0;
                    zombie.knockback = Vec2{};
                    zombie.stunTimer = 0.0;
                    zombie.impactCooldown = 0.0;
                    zombie.lastDashId = -1;
                    zombie.attackTimer = 0.5;
                    zombie.walkPhase = 0.0;
                }
            }
            continue;
        }

        zombie.attackTimer = std::max(0.0, zombie.attackTimer - deltaSeconds);
        zombie.impactCooldown = std::max(0.0, zombie.impactCooldown - deltaSeconds);
        zombie.stunTimer = std::max(0.0, zombie.stunTimer - deltaSeconds);
        if (lengthSquared(zombie.knockback) > 1e-8) {
            tryMoveZombie(game, zombie, zombie.knockback * deltaSeconds);
            zombie.knockback = zombie.knockback * std::exp(-5.0 * deltaSeconds);
            if (length(zombie.knockback) < 0.05) {
                zombie.knockback = Vec2{};
            }
        }
        if (zombie.stunTimer > 0.0) {
            continue;
        }

        const Vec2 relative{target.x - zombie.position.x, target.y - zombie.position.y};
        const double distance = length(relative);
        if (distance > 1e-9) {
            zombie.heading = std::atan2(relative.y, relative.x);
        }
        if (distance > 1e-9 && distance < kZombieAggroRange &&
            hasLineOfSight(game.map, zombie.position, target)) {
            if (distance > kZombieAttackRange) {
                const double step =
                    std::min(kZombieSpeed * deltaSeconds, distance - kZombieAttackRange);
                tryMoveZombie(game, zombie,
                              Vec2{relative.x / distance * step,
                                   relative.y / distance * step});
                zombie.walkPhase += deltaSeconds * 7.0;
            } else if (!game.playerDead && !game.player.inVehicle &&
                       game.player.dashTimer <= 0.0 &&
                       zombie.attackTimer <= 0.0) {
                playerDamage += kZombieAttackDamage;
                zombie.attackTimer = kZombieAttackCooldown;
            }
        }

        for (const Vehicle& vehicle : game.vehicles) {
            const double carDistance = distanceToVehicle(vehicle, zombie.position);
            if (carDistance < 0.62 && carDistance > 1e-9) {
                const Vec2 forward = directionFromRadians(vehicle.heading);
                const Vec2 rear = vehicle.position - forward * 0.5;
                const Vec2 front = vehicle.position + forward * 0.5;
                const Vec2 axis = front - rear;
                const double t = clampd(dot(zombie.position - rear, axis) /
                                            std::max(1e-9, lengthSquared(axis)),
                                        0.0, 1.0);
                const Vec2 closest = rear + axis * t;
                const Vec2 push = zombie.position - closest;
                const Vec2 direction = normalized(push);
                tryMoveZombie(game, zombie, direction * (0.64 - carDistance));
            }
        }
    }

    for (std::size_t i = 0; i < game.zombies.size(); ++i) {
        if (!game.zombies[i].alive) {
            continue;
        }
        for (std::size_t j = i + 1; j < game.zombies.size(); ++j) {
            if (!game.zombies[j].alive) {
                continue;
            }
            const double dx = game.zombies[j].position.x - game.zombies[i].position.x;
            const double dy = game.zombies[j].position.y - game.zombies[i].position.y;
            const double distance = std::hypot(dx, dy);
            if (distance < 1e-6 || distance >= 0.55) {
                continue;
            }
            const double push = (0.55 - distance) * 0.5;
            const Vec2 normal{dx / distance, dy / distance};
            tryMoveZombie(game, game.zombies[i], Vec2{-normal.x * push, -normal.y * push});
            tryMoveZombie(game, game.zombies[j], Vec2{normal.x * push, normal.y * push});
        }
    }
    return playerDamage;
}

Texture makeWallTexture(std::uint8_t material) {
    constexpr int size = 64;
    std::vector<Color> pixels(static_cast<std::size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Color color = makeColor(120, 120, 120);
            switch (material) {
                case 1: {  // red brick
                    const int row = y / 16;
                    const int offset = (row & 1) * 16;
                    const int brickX = (x + offset) % 32;
                    if (y % 16 < 2 || brickX < 2) {
                        color = makeColor(58, 52, 48);
                    } else {
                        const double tone = noise01((x + offset) / 32, row, 11);
                        const int grain =
                            static_cast<int>((noise01(x, y, 7) - 0.5) * 22.0);
                        const int lift = static_cast<int>(tone * 28.0);
                        color = makeColor(140 + lift + grain, 63 + lift / 3 + grain,
                                          49 + lift / 4 + grain);
                    }
                    break;
                }
                case 2: {  // blue-grey stone blocks
                    if (x % 16 < 2 || y % 16 < 2) {
                        color = makeColor(39, 45, 56);
                    } else {
                        const int block = (x / 16) + (y / 16) * 4;
                        const double tone = noise01(block, 0, 23);
                        const int grain =
                            static_cast<int>((noise01(x, y, 31) - 0.5) * 26.0);
                        const int lift = static_cast<int>(tone * 34.0);
                        color = makeColor(88 + lift + grain, 108 + lift + grain,
                                          142 + lift + grain);
                    }
                    break;
                }
                case 3: {  // green painted panels
                    if (x % 32 < 2 || y % 32 < 2) {
                        color = makeColor(24, 58, 42);
                    } else {
                        const double tone = noise01(x / 32, y / 32, 37);
                        const int grain =
                            static_cast<int>((noise01(x, y, 41) - 0.5) * 18.0);
                        const int rib = (y % 8 < 1) ? -14 : 0;
                        const int lift = static_cast<int>(tone * 28.0);
                        color = makeColor(47 + lift + grain + rib,
                                          115 + lift + grain + rib,
                                          82 + lift + grain + rib);
                    }
                    break;
                }
                case 4: {  // warm wood planks
                    const int plank = x % 8;
                    if (plank < 1 || y % 32 < 1) {
                        color = makeColor(55, 37, 22);
                    } else {
                        const int grain = static_cast<int>(
                            std::sin(y * 0.42 + x * 0.17 +
                                     noise01(x, y / 8, 53) * 6.0) *
                            16.0);
                        const int lift =
                            static_cast<int>(noise01(x / 8, y / 32, 59) * 18.0);
                        color = makeColor(128 + lift + grain, 88 + lift / 2 + grain,
                                          56 + lift / 3 + grain);
                    }
                    break;
                }
                case 5: {  // golden fountain stone
                    const int rib = x % 8;
                    if (rib < 2) {
                        color = makeColor(76, 54, 18);
                    } else if (y % 16 < 2) {
                        color = makeColor(110, 82, 30);
                    } else {
                        const int shine = rib >= 3 && rib <= 5 ? 22 : 0;
                        const int grain =
                            static_cast<int>((noise01(x, y, 67) - 0.5) * 18.0);
                        const int lift =
                            static_cast<int>(noise01(x / 8, y / 16, 71) * 22.0);
                        color = makeColor(176 + lift + grain + shine,
                                          132 + lift + grain + shine, 58 + shine / 2);
                    }
                    break;
                }
                default:
                    color = makeColor(120, 120, 120);
                    break;
            }
            pixels[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)] =
                color;
        }
    }
    return Texture(size, size, std::move(pixels));
}

Texture makeFloorTexture() {
    constexpr int size = 64;
    std::vector<Color> pixels(static_cast<std::size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int tileX = x / 16;
            const int tileY = y / 16;
            const bool border = x % 16 == 0 || y % 16 == 0;
            int red = ((tileX + tileY) & 1) != 0 ? 50 : 58;
            int green = red + 5;
            int blue = red + 14;
            if (border) {
                red = 27;
                green = 31;
                blue = 39;
            }
            const int grain = static_cast<int>((noise01(x, y, 101) - 0.5) * 9.0);
            pixels[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)] =
                makeColor(red + grain, green + grain, blue + grain);
        }
    }
    return Texture(size, size, std::move(pixels));
}

Texture makeCeilingTexture() {
    constexpr int size = 64;
    std::vector<Color> pixels(static_cast<std::size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool beam = x % 32 < 3 || y % 32 < 3;
            const int grain = static_cast<int>((noise01(x, y, 131) - 0.5) * 7.0);
            const Color color =
                beam ? makeColor(42 + grain, 41 + grain, 44 + grain)
                     : makeColor(64 + grain, 60 + grain, 64 + grain);
            pixels[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)] =
                color;
        }
    }
    return Texture(size, size, std::move(pixels));
}

Texture makeWaterTexture() {
    constexpr int size = 64;
    std::vector<Color> pixels(static_cast<std::size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double ripple =
                std::sin(x * 0.42 + y * 0.19) * 12.0 +
                std::sin(y * 0.73 - x * 0.11 + 1.7) * 8.0;
            const double glint = std::pow(
                std::max(0.0, std::sin(x * 0.31 + y * 0.53 + 0.6)), 12.0) * 40.0;
            const int grain = static_cast<int>((noise01(x, y, 151) - 0.5) * 6.0);
            pixels[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)] =
                makeColor(24 + grain + static_cast<int>(ripple) / 2 +
                              static_cast<int>(glint),
                          74 + grain + static_cast<int>(ripple) + static_cast<int>(glint),
                          100 + grain + static_cast<int>(ripple) * 2 +
                              static_cast<int>(glint));
        }
    }
    return Texture(size, size, std::move(pixels));
}

Mesh makeDiscMesh(int segments, double rimAlpha = 1.0, double radius = 1.0) {
    Mesh mesh;
    const Vec3 normal{0.0, 0.0, 1.0};
    const Vec2 uv{0.5, 0.5};
    for (int i = 0; i < segments; ++i) {
        const double a0 = 2.0 * kPi * static_cast<double>(i) / segments;
        const double a1 = 2.0 * kPi * static_cast<double>(i + 1) / segments;
        const Vec3 p0{std::cos(a0) * radius, std::sin(a0) * radius, 0.0};
        const Vec3 p1{std::cos(a1) * radius, std::sin(a1) * radius, 0.0};
        mesh.addTriangle(Vertex{Vec3{0.0, 0.0, 0.0}, normal, uv, 1.0},
                         Vertex{p0, normal, uv, rimAlpha},
                         Vertex{p1, normal, uv, rimAlpha});
    }
    return mesh;
}

void addCylinderY(Mesh& mesh, double radius, double halfWidth, int segments) {
    const Vec3 axis{0.0, 2.0 * halfWidth, 0.0};
    for (int i = 0; i < segments; ++i) {
        const double a0 = 2.0 * kPi * static_cast<double>(i) / segments;
        const double a1 = 2.0 * kPi * static_cast<double>(i + 1) / segments;
        const Vec3 p0{std::cos(a0) * radius, -halfWidth, std::sin(a0) * radius};
        const Vec3 p1{std::cos(a1) * radius, -halfWidth, std::sin(a1) * radius};
        const Vec3 tangent = p1 - p0;
        mesh.addQuad(p0, axis, tangent, 1.0, 1.0, Vec2{0.0, 0.0}, Vec2{1.0, 1.0});

        const Vec3 backNormal{0.0, -1.0, 0.0};
        const Vec3 frontNormal{0.0, 1.0, 0.0};
        const Vec2 uv{0.5, 0.5};
        const Vec3 backCenter{0.0, -halfWidth, 0.0};
        const Vec3 frontCenter{0.0, halfWidth, 0.0};
        mesh.addTriangle(Vertex{backCenter, backNormal, uv}, Vertex{p0, backNormal, uv},
                         Vertex{p1, backNormal, uv});
        mesh.addTriangle(Vertex{frontCenter, frontNormal, uv},
                         Vertex{p1, frontNormal, uv}, Vertex{p0, frontNormal, uv});
    }
}

Color zombieSkin(double flash) {
    return mix(makeColor(96, 140, 80), makeColor(255, 236, 220), flash);
}

Color zombieCloth(double flash) {
    return mix(makeColor(70, 80, 70), makeColor(255, 236, 220), flash);
}

Color zombiePants(double flash) {
    return mix(makeColor(52, 61, 80), makeColor(255, 236, 220), flash);
}

Color vehiclePaint(int id) {
    return id == 0 ? makeColor(206, 116, 44) : makeColor(40, 122, 150);
}

void addPart(Scene& scene, const Mesh& mesh, const Mat4& model, Color color,
             bool unlit = false, const Texture* texture = nullptr,
             double specular = 0.0, double shininess = 32.0, double opacity = 1.0) {
    DrawItem item;
    item.mesh = &mesh;
    item.model = model;
    item.material.tint = color;
    item.material.unlit = unlit;
    item.material.texture = texture;
    item.material.specular = specular;
    item.material.shininess = shininess;
    item.material.opacity = opacity;
    scene.items.push_back(item);
}

}  // namespace

Grid loadMap(std::string& error) {
    Grid map{};
    for (int y = 0; y < kMapHeight; ++y) {
        const char* row = kMapRows[y];
        const std::size_t rowLength = std::char_traits<char>::length(row);
        if (rowLength != static_cast<std::size_t>(kMapWidth)) {
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

bool circleTouchesSolid(const Grid& map, Vec2 center, double radius) {
    return isSolidAt(map, center.x - radius, center.y - radius) ||
           isSolidAt(map, center.x + radius, center.y - radius) ||
           isSolidAt(map, center.x - radius, center.y + radius) ||
           isSolidAt(map, center.x + radius, center.y + radius);
}

bool isWalkableCentered(const Grid& map, Vec2 position, double radius) {
    return !circleTouchesSolid(map, position, radius);
}

GridHit traceGrid(const Grid& map, Vec2 origin, Vec2 direction) {
    GridHit hit;
    const double dirX = direction.x;
    const double dirY = direction.y;
    const double epsilon = 1e-12;
    if (std::fabs(dirX) < epsilon && std::fabs(dirY) < epsilon) {
        return hit;
    }

    int mapX = static_cast<int>(std::floor(origin.x));
    int mapY = static_cast<int>(std::floor(origin.y));
    const double deltaDistX = std::fabs(dirX) < epsilon ? 1e30 : std::fabs(1.0 / dirX);
    const double deltaDistY = std::fabs(dirY) < epsilon ? 1e30 : std::fabs(1.0 / dirY);
    const int stepX = dirX < 0.0 ? -1 : 1;
    const int stepY = dirY < 0.0 ? -1 : 1;
    double sideDistX =
        dirX < 0.0 ? (origin.x - mapX) * deltaDistX : (mapX + 1.0 - origin.x) * deltaDistX;
    double sideDistY =
        dirY < 0.0 ? (origin.y - mapY) * deltaDistY : (mapY + 1.0 - origin.y) * deltaDistY;

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

bool hasLineOfSight(const Grid& map, Vec2 from, Vec2 to) {
    const Vec2 delta{to.x - from.x, to.y - from.y};
    const double distance = length(delta);
    if (distance < 1e-9) {
        return true;
    }
    const GridHit hit = traceGrid(map, from, delta / distance);
    return hit.material == 0 || hit.distance >= distance - 1e-6;
}

Vec3 mapToWorld(Vec2 position, double z) {
    return Vec3{position.x, static_cast<double>(kMapHeight) - position.y, z};
}

Vec3 mapDirectionToWorld(double mapHeadingDegrees) {
    const double worldYaw = -mapHeadingDegrees * kDegToRad;
    return Vec3{std::cos(worldYaw), std::sin(worldYaw), 0.0};
}

double mapYawToWorldYaw(double mapYawDegrees) {
    return -mapYawDegrees;
}

Game createGame() {
    Game game;
    std::string error;
    game.map = loadMap(error);
    game.vehicles = createVehicles();
    game.zombies = createZombies(game.startPosition);
    game.player.position = game.startPosition;
    game.player.yawDegrees = game.startYawDegrees;
    return game;
}

void resetGame(Game& game) {
    game.player.position = game.startPosition;
    game.player.yawDegrees = game.startYawDegrees;
    game.player.health = kPlayerMaxHealth;
    game.player.inVehicle = false;
    game.player.vehicleIndex = -1;
    game.player.dashDirection = Vec2{1.0, 0.0};
    game.player.dashTimer = 0.0;
    game.player.dashCooldown = 0.0;
    game.player.dashTrailTimer = 0.0;
    game.player.dashId = 0;
    game.vehicles = createVehicles();
    game.zombies = createZombies(game.startPosition);
    game.kills = 0;
    game.playerDead = false;
    game.shotCooldown = 0.0;
    game.muzzleFlash = 0.0;
    game.damageFlash = 0.0;
    game.score = 0;
    game.combo = 0;
    game.comboTimer = 0.0;
    game.shake = 0.0;
    game.elapsed = 0.0;
    game.particles.clear();
    game.soundEvents.clear();
}

bool startDash(Game& game, Vec2 direction) {
    Player& player = game.player;
    if (game.playerDead || player.inVehicle || player.dashTimer > 0.0 ||
        player.dashCooldown > 0.0) {
        return false;
    }
    if (lengthSquared(direction) < 1e-9) {
        direction = directionFromDegrees(player.yawDegrees);
    }
    player.dashDirection = normalized(direction);
    if (lengthSquared(player.dashDirection) < 1e-9) {
        return false;
    }
    player.dashTimer = kDashDuration;
    player.dashCooldown = kDashCooldown;
    player.dashTrailTimer = 0.0;
    ++player.dashId;
    game.shake = std::max(game.shake, 0.55);
    spawnDust(game, player.position, player.dashDirection, 7, true);
    game.soundEvents.push_back(SoundEvent::Dash);
    return true;
}

bool dashReady(const Game& game) {
    return !game.playerDead && !game.player.inVehicle &&
           game.player.dashTimer <= 0.0 && game.player.dashCooldown <= 0.0;
}

int nearbyVehicle(const Game& game, double maxDistance) {
    int best = -1;
    double bestDistance = maxDistance;
    for (std::size_t i = 0; i < game.vehicles.size(); ++i) {
        const double distance = distanceToVehicle(game.vehicles[i], game.player.position);
        if (distance < bestDistance) {
            best = static_cast<int>(i);
            bestDistance = distance;
        }
    }
    return best;
}

bool enterVehicle(Game& game, int vehicleIndex) {
    if (game.player.inVehicle || vehicleIndex < 0 ||
        vehicleIndex >= static_cast<int>(game.vehicles.size())) {
        return false;
    }
    const Vehicle& vehicle = game.vehicles[static_cast<std::size_t>(vehicleIndex)];
    if (std::fabs(vehicle.speed) > 1.2) {
        return false;
    }
    if (distanceToVehicle(vehicle, game.player.position) > 2.2) {
        return false;
    }
    game.player.inVehicle = true;
    game.player.vehicleIndex = vehicleIndex;
    game.player.position = vehicle.position;
    return true;
}

bool exitVehicle(Game& game) {
    if (!game.player.inVehicle) {
        return false;
    }
    const int index = game.player.vehicleIndex;
    if (index < 0 || index >= static_cast<int>(game.vehicles.size())) {
        game.player.inVehicle = false;
        game.player.vehicleIndex = -1;
        return true;
    }
    Vehicle& vehicle = game.vehicles[static_cast<std::size_t>(index)];
    if (std::fabs(vehicle.speed) > 1.2) {
        return false;
    }
    const Vec2 forward = directionFromRadians(vehicle.heading);
    const Vec2 right = perpendicular(forward);
    const Vec2 candidates[6] = {
        vehicle.position + right * 1.0,
        vehicle.position - right * 1.0,
        vehicle.position - forward * 1.3,
        vehicle.position + forward * 1.3,
        vehicle.position + right * 0.8 - forward * 0.6,
        vehicle.position - right * 0.8 + forward * 0.6,
    };
    for (const Vec2& candidate : candidates) {
        if (!isWalkableCentered(game.map, candidate, kPlayerRadius)) {
            continue;
        }
        bool clear = true;
        for (const Vehicle& other : game.vehicles) {
            if (circleHitsVehicle(other, candidate, kPlayerRadius)) {
                clear = false;
                break;
            }
        }
        if (!clear) {
            continue;
        }
        game.player.position = candidate;
        game.player.yawDegrees = normalizeDegrees(vehicle.heading / kDegToRad);
        game.player.inVehicle = false;
        game.player.vehicleIndex = -1;
        return true;
    }
    return false;
}

int findShotTarget(const Game& game, Vec2 origin, Vec2 direction) {
    const double directionLength = length(direction);
    if (directionLength < 1e-9) {
        return -1;
    }
    const Vec2 ray = direction / directionLength;
    const GridHit wall = traceGrid(game.map, origin, ray);
    int best = -1;
    double bestDistance = wall.material == 0 ? 1e30 : wall.distance;
    for (std::size_t i = 0; i < game.zombies.size(); ++i) {
        const Zombie& zombie = game.zombies[i];
        if (!zombie.alive) {
            continue;
        }
        const Vec2 relative{zombie.position.x - origin.x, zombie.position.y - origin.y};
        const double along = dot(relative, ray);
        if (along <= 0.0 || along >= bestDistance) {
            continue;
        }
        const double perpendicularDistance = std::fabs(cross(relative, ray));
        if (perpendicularDistance > kZombieRadius) {
            continue;
        }
        best = static_cast<int>(i);
        bestDistance = along;
    }
    return best;
}

bool damageZombie(Zombie& zombie, int damage, Vec2 impulse, bool flattened) {
    if (!zombie.alive) {
        return false;
    }
    zombie.health = std::max(0, zombie.health - damage);
    zombie.hitFlash = 0.16;
    if (lengthSquared(impulse) > 1e-9) {
        zombie.knockback += impulse;
        zombie.stunTimer = std::max(zombie.stunTimer, 0.45);
    }
    if (zombie.health == 0) {
        zombie.alive = false;
        zombie.ragdoll = true;
        zombie.flattened = flattened;
        zombie.deathElapsed = 0.0;
        zombie.deathSpin = impulse.x >= 0.0 ? 1.0 : -1.0;
        zombie.respawnTimer = kZombieRespawnSeconds;
        zombie.attackTimer = 0.0;
        return true;
    }
    return false;
}

void updateGame(Game& game, const Input& input, double deltaSeconds) {
    if (deltaSeconds <= 0.0) {
        return;
    }
    game.soundEvents.clear();
    game.elapsed += deltaSeconds;
    game.shotCooldown = std::max(0.0, game.shotCooldown - deltaSeconds);
    game.muzzleFlash = std::max(0.0, game.muzzleFlash - deltaSeconds * 6.0);
    game.damageFlash = std::max(0.0, game.damageFlash - deltaSeconds * 2.5);
    game.shake = std::max(0.0, game.shake - deltaSeconds * 2.1);
    game.player.dashCooldown =
        std::max(0.0, game.player.dashCooldown - deltaSeconds);
    if (game.comboTimer > 0.0) {
        game.comboTimer -= deltaSeconds;
        if (game.comboTimer <= 0.0) {
            game.comboTimer = 0.0;
            game.combo = 0;
        }
    }

    if (input.enterExit && game.player.dashTimer <= 0.0) {
        if (game.player.inVehicle) {
            exitVehicle(game);
        } else {
            const int index = nearbyVehicle(game);
            if (index >= 0) {
                enterVehicle(game, index);
            }
        }
    }

    if (!game.playerDead) {
        if (game.player.inVehicle) {
            updateVehicle(game, game.player.vehicleIndex, clampd(input.forward, -1.0, 1.0),
                         clampd(input.strafe + input.turn, -1.0, 1.0), deltaSeconds);
            game.player.position =
                game.vehicles[static_cast<std::size_t>(game.player.vehicleIndex)].position;
        } else {
            if (input.dash) {
                startDash(game, input.dashDirection);
            }
            if (game.player.dashTimer > 0.0) {
                updatePlayerDash(game, deltaSeconds);
            } else {
                const Vec2 forward = directionFromDegrees(game.player.yawDegrees);
                const Vec2 right = perpendicular(forward);
                const Vec2 delta =
                    forward * (clampd(input.forward, -1.0, 1.0) * 2.6 * deltaSeconds) +
                    right * (clampd(input.strafe, -1.0, 1.0) * 2.1 * deltaSeconds);
                if (lengthSquared(delta) > 1e-12) {
                    movePlayer(game, delta);
                }
                game.player.yawDegrees =
                    normalizeDegrees(game.player.yawDegrees +
                                     clampd(input.turn, -1.0, 1.0) * 110.0 * deltaSeconds);

                if (input.shoot && game.shotCooldown <= 0.0) {
                    game.shotCooldown = kShotCooldown;
                    game.muzzleFlash = 0.09;
                    const Vec2 shotDirection =
                        directionFromDegrees(game.player.yawDegrees);
                    const int target =
                        findShotTarget(game, game.player.position, shotDirection);
                    if (target >= 0 &&
                        damageZombie(
                            game.zombies[static_cast<std::size_t>(target)],
                            kShotDamage, shotDirection * 2.2)) {
                        awardKill(game, false);
                    }
                }
            }
        }
    } else if (game.player.inVehicle) {
        game.player.position =
            game.vehicles[static_cast<std::size_t>(game.player.vehicleIndex)].position;
    }

    applyVehicleImpacts(game);
    const int incomingDamage = updateZombies(game, deltaSeconds);
    updateParticles(game, deltaSeconds);
    if (incomingDamage > 0 && !game.playerDead) {
        game.player.health = std::max(0, game.player.health - incomingDamage);
        game.damageFlash = 0.6;
        if (game.player.health == 0) {
            game.playerDead = true;
        }
    }
}

Camera firstPersonCamera(const Game& game, double horizontalFovDegrees, double aspect) {
    Camera camera;
    camera.eye = mapToWorld(game.player.position, kEyeHeight);
    camera.target =
        camera.eye + mapDirectionToWorld(game.player.yawDegrees) * 12.0;
    camera.up = Vec3{0.0, 0.0, 1.0};
    camera.fovYDegrees =
        verticalFovFromHorizontal(horizontalFovDegrees, aspect);
    return camera;
}

Camera chaseCamera(const Game& game, double horizontalFovDegrees, double aspect) {
    Camera camera;
    const int index = game.player.vehicleIndex;
    if (index < 0 || index >= static_cast<int>(game.vehicles.size())) {
        return firstPersonCamera(game, horizontalFovDegrees, aspect);
    }
    const Vehicle& vehicle = game.vehicles[static_cast<std::size_t>(index)];
    const Vec2 forward = directionFromRadians(vehicle.heading);
    const Vec2 backward = -forward;
    const GridHit clearance = traceGrid(game.map, vehicle.position, backward);
    const double distance = clampd(clearance.distance - 0.4, 0.2, 3.2);
    camera.eye = mapToWorld(vehicle.position + backward * distance, 2.35);
    camera.target = mapToWorld(vehicle.position + forward * 0.6, 0.8);
    camera.up = Vec3{0.0, 0.0, 1.0};
    camera.fovYDegrees =
        verticalFovFromHorizontal(horizontalFovDegrees, aspect);
    return camera;
}

Camera overheadCamera(double aspect) {
    Camera camera;
    const double fovY = 52.0;
    const double halfTan = std::tan(fovY * 0.5 * kDegToRad);
    const double neededVertical =
        (static_cast<double>(kMapHeight) + 1.5) / (2.0 * halfTan);
    const double neededHorizontal =
        (static_cast<double>(kMapWidth) + 1.5) / (2.0 * halfTan * aspect);
    const double eyeZ = std::max(neededVertical, neededHorizontal) * 1.03;
    camera.eye = Vec3{kMapWidth * 0.5, kMapHeight * 0.5, eyeZ};
    camera.target = Vec3{kMapWidth * 0.5, kMapHeight * 0.5, 0.0};
    camera.up = Vec3{0.0, 1.0, 0.0};
    camera.fovYDegrees = fovY;
    camera.farPlane = 150.0;
    return camera;
}

void applyCameraShake(Camera& camera, const Game& game) {
    if (game.shake <= 1e-6) {
        return;
    }
    const CameraBasis basis = cameraBasis(camera);
    const double strength = game.shake * 0.11;
    const double t = game.elapsed * 43.0;
    const Vec3 offset =
        basis.right * (std::sin(t * 1.7) * strength) +
        basis.up * (std::cos(t * 2.3) * strength * 0.72);
    camera.eye += offset;
    camera.target += offset * 0.35 +
                     basis.right * (std::sin(t * 3.1) * strength * 0.25);
}

Color mapMaterialColor(std::uint8_t material) {
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

void WorldAssets::build(const Grid& map) {
    for (int material = 1; material <= 5; ++material) {
        wallTextures[static_cast<std::size_t>(material)] =
            makeWallTexture(static_cast<std::uint8_t>(material));
        wallMeshes[static_cast<std::size_t>(material)] = Mesh{};
        wallTopMeshes[static_cast<std::size_t>(material)] = Mesh{};
    }
    floorTexture = makeFloorTexture();
    ceilingTexture = makeCeilingTexture();
    waterTexture = makeWaterTexture();
    floorMesh = Mesh{};
    ceilingMesh = Mesh{};
    waterMesh = Mesh{};
    particleMesh = makeDiscMesh(12, 0.0, 0.5);

    floorMesh.addQuad(Vec3{0.0, 0.0, 0.0}, Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0},
                      static_cast<double>(kMapWidth), static_cast<double>(kMapHeight),
                      Vec2{0.0, 0.0},
                      Vec2{static_cast<double>(kMapWidth),
                           static_cast<double>(kMapHeight)});
    ceilingMesh.addQuad(
        Vec3{0.0, static_cast<double>(kMapHeight), kWallHeight}, Vec3{1.0, 0.0, 0.0},
        Vec3{0.0, -1.0, 0.0}, static_cast<double>(kMapWidth),
        static_cast<double>(kMapHeight), Vec2{0.0, 0.0},
        Vec2{static_cast<double>(kMapWidth), static_cast<double>(kMapHeight)});

    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            const std::uint8_t material = map[y][x];
            if (material == 0) {
                continue;
            }
            const double height = cellHeight(material);
            const double yMin = static_cast<double>(kMapHeight - y - 1);
            const double yMax = yMin + 1.0;
            const double xMin = static_cast<double>(x);
            const double xMax = xMin + 1.0;
            Mesh& wallMesh = wallMeshes[static_cast<std::size_t>(material)];

            const auto addFace = [&](double neighborHeight, Vec3 origin, Vec3 uAxis,
                                     Vec3 vAxis) {
                if (neighborHeight >= height - 1e-9) {
                    return;
                }
                wallMesh.addQuad(origin, uAxis, vAxis, 1.0,
                                 height - neighborHeight,
                                 Vec2{0.0, neighborHeight}, Vec2{1.0, height});
            };

            const std::uint8_t east =
                x + 1 < kMapWidth ? map[y][x + 1] : std::uint8_t{0};
            const std::uint8_t west =
                x > 0 ? map[y][x - 1] : std::uint8_t{0};
            const std::uint8_t north =
                y > 0 ? map[y - 1][x] : std::uint8_t{0};
            const std::uint8_t south =
                y + 1 < kMapHeight ? map[y + 1][x] : std::uint8_t{0};

            addFace(cellHeight(east), Vec3{xMax, yMin, 0.0}, Vec3{0.0, 1.0, 0.0},
                    Vec3{0.0, 0.0, 1.0});
            addFace(cellHeight(west), Vec3{xMin, yMax, 0.0}, Vec3{0.0, -1.0, 0.0},
                    Vec3{0.0, 0.0, 1.0});
            addFace(cellHeight(north), Vec3{xMax, yMax, 0.0}, Vec3{-1.0, 0.0, 0.0},
                    Vec3{0.0, 0.0, 1.0});
            addFace(cellHeight(south), Vec3{xMin, yMin, 0.0}, Vec3{1.0, 0.0, 0.0},
                    Vec3{0.0, 0.0, 1.0});

            if (material == 5) {
                waterMesh.addQuad(Vec3{xMin, yMin, kFountainHeight - 0.02},
                                  Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0}, 1.0, 1.0,
                                  Vec2{0.0, 0.0}, Vec2{1.0, 1.0});
            } else {
                wallTopMeshes[static_cast<std::size_t>(material)].addQuad(
                    Vec3{xMin, yMin, height}, Vec3{1.0, 0.0, 0.0},
                    Vec3{0.0, 1.0, 0.0}, 1.0, 1.0, Vec2{0.0, 0.0}, Vec2{1.0, 1.0});
            }
        }
    }
}

ZombieAssets::ZombieAssets() {
    torso.addBox(Vec3{-0.10, -0.17, 0.78}, Vec3{0.10, 0.17, 1.30});
    head.addBox(Vec3{-0.11, -0.115, 1.36}, Vec3{0.11, 0.115, 1.62});
    leftLeg.addBox(Vec3{-0.075, -0.07, -0.78}, Vec3{0.075, 0.07, 0.0});
    rightLeg.addBox(Vec3{-0.075, -0.07, -0.78}, Vec3{0.075, 0.07, 0.0});
    leftArm.addBox(Vec3{-0.055, -0.055, -0.56}, Vec3{0.055, 0.055, 0.0});
    rightArm.addBox(Vec3{-0.055, -0.055, -0.56}, Vec3{0.055, 0.055, 0.0});
    leftEye.addBox(Vec3{-0.006, -0.022, -0.02}, Vec3{0.012, 0.022, 0.02});
    rightEye.addBox(Vec3{-0.006, -0.022, -0.02}, Vec3{0.012, 0.022, 0.02});
    shadow = makeDiscMesh(14, 0.0);
    healthQuad.addQuad(Vec3{-0.5, -0.5, 0.0}, Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0},
                       1.0, 1.0, Vec2{0.0, 0.0}, Vec2{1.0, 1.0});
    bloodPool = makeDiscMesh(18, 0.0);
}

VehicleAssets::VehicleAssets() {
    body.addBox(Vec3{-0.78, -0.36, 0.30}, Vec3{0.78, 0.36, 0.60});
    body.addBox(Vec3{0.78, -0.33, 0.28}, Vec3{0.88, 0.33, 0.44});
    body.addBox(Vec3{-0.88, -0.33, 0.28}, Vec3{-0.78, 0.33, 0.44});
    cabin.addBox(Vec3{-0.30, -0.30, 0.60}, Vec3{0.36, 0.30, 0.98});
    roof.addBox(Vec3{-0.27, -0.28, 0.98}, Vec3{0.33, 0.28, 1.04});
    addCylinderY(wheel, 0.27, 0.09, 12);
    addCylinderY(hub, 0.11, 0.095, 10);
    headlight.addBox(Vec3{0.80, -0.28, 0.46}, Vec3{0.865, -0.16, 0.545});
    headlight.addBox(Vec3{0.80, 0.16, 0.46}, Vec3{0.865, 0.28, 0.545});
    taillight.addBox(Vec3{-0.865, -0.30, 0.46}, Vec3{-0.80, -0.20, 0.545});
    taillight.addBox(Vec3{-0.865, 0.20, 0.46}, Vec3{-0.80, 0.30, 0.545});
    shadow = makeDiscMesh(18, 0.0);
}

void appendWorldGeometry(Scene& scene, const WorldAssets& assets, bool includeCeiling) {
    const Color white = makeColor(255, 255, 255);
    addPart(scene, assets.floorMesh, identityMatrix(), white, false,
            &assets.floorTexture, 0.12, 28.0);
    for (int material = 1; material <= 5; ++material) {
        // Bricks and wood stay mostly matte; stone and gold catch a sheen.
        const double specular = material == 5 ? 0.45 : (material == 2 ? 0.12 : 0.04);
        const double shininess = material == 5 ? 48.0 : 24.0;
        addPart(scene, assets.wallMeshes[static_cast<std::size_t>(material)],
                identityMatrix(), white, false,
                &assets.wallTextures[static_cast<std::size_t>(material)], specular,
                shininess);
        if (!assets.wallTopMeshes[static_cast<std::size_t>(material)].triangles.empty()) {
            addPart(scene, assets.wallTopMeshes[static_cast<std::size_t>(material)],
                    identityMatrix(),
                    mapMaterialColor(static_cast<std::uint8_t>(material)), false,
                    nullptr, specular, shininess);
        }
    }
    if (includeCeiling) {
        addPart(scene, assets.ceilingMesh, identityMatrix(), white, false,
                &assets.ceilingTexture, 0.02, 16.0);
    }
    addPart(scene, assets.waterMesh, identityMatrix(), white, false,
            &assets.waterTexture, 0.9, 110.0);
}

void appendVehicles(Scene& scene, const Game& game, const VehicleAssets& assets) {
    for (const Vehicle& vehicle : game.vehicles) {
        const Mat4 transform = translationMatrix(mapToWorld(vehicle.position, 0.0)) *
                               rotationZMatrix(-vehicle.heading);
        const Color paint = vehiclePaint(vehicle.id);

        addPart(scene, assets.shadow,
                translationMatrix(mapToWorld(vehicle.position, 0.02)) *
                    rotationZMatrix(-vehicle.heading) *
                    scaleMatrix(Vec3{1.05, 0.62, 1.0}),
                makeColor(12, 14, 18), true, nullptr, 0.0, 32.0, 0.65);
        addPart(scene, assets.body, transform, paint, false, nullptr, 0.55, 52.0);
        addPart(scene, assets.cabin, transform, makeColor(44, 60, 80), false,
                nullptr, 0.8, 90.0);
        addPart(scene, assets.roof, transform, paint, false, nullptr, 0.55, 52.0);
        addPart(scene, assets.headlight, transform, makeColor(255, 244, 186), true);
        addPart(scene, assets.taillight, transform, makeColor(214, 48, 38), true);

        for (int wheelIndex = 0; wheelIndex < 4; ++wheelIndex) {
            const double side = wheelIndex % 2 == 0 ? 1.0 : -1.0;
            const bool front = wheelIndex < 2;
            const Vec3 local{front ? 0.52 : -0.52, side * 0.40, 0.28};
            Mat4 wheelModel = transform * translationMatrix(local);
            if (front) {
                wheelModel = wheelModel * rotationZMatrix(-vehicle.steer);
            }
            wheelModel = wheelModel * rotationYMatrix(vehicle.wheelSpin);
            addPart(scene, assets.wheel, wheelModel, makeColor(26, 26, 29), false,
                    nullptr, 0.10, 18.0);
            addPart(scene, assets.hub, wheelModel, makeColor(156, 158, 164), false,
                    nullptr, 0.6, 40.0);
        }
    }
}

void appendZombies(Scene& scene, const Game& game, const ZombieAssets& assets,
                   bool showHealthBars) {
    const CameraBasis basis = cameraBasis(scene.camera);
    const Vec3 towardCamera = -basis.forward;
    Mat4 billboard = identityMatrix();
    billboard.m[0][0] = basis.right.x;
    billboard.m[1][0] = basis.right.y;
    billboard.m[2][0] = basis.right.z;
    billboard.m[0][1] = basis.up.x;
    billboard.m[1][1] = basis.up.y;
    billboard.m[2][1] = basis.up.z;
    billboard.m[0][2] = towardCamera.x;
    billboard.m[1][2] = towardCamera.y;
    billboard.m[2][2] = towardCamera.z;

    for (const Zombie& zombie : game.zombies) {
        const double flash = std::min(1.0, zombie.hitFlash / 0.16) * 0.65;
        if (!zombie.alive) {
            if (!zombie.ragdoll || zombie.deathElapsed >= kRagdollSeconds) {
                continue;
            }
            const double fall = smoothstep(0.0, 0.6, zombie.deathElapsed);
            const double settle = smoothstep(0.6, 1.6, zombie.deathElapsed);
            const double fade =
                clampd((kRagdollSeconds - zombie.deathElapsed) / 0.45, 0.0, 1.0);
            Mat4 corpse =
                translationMatrix(mapToWorld(zombie.position, 0.05 + 0.05 * fall)) *
                rotationZMatrix(-zombie.heading + zombie.deathSpin * 0.24 * fall);
            if (zombie.flattened) {
                corpse = corpse * scaleMatrix(Vec3{1.30, 1.30, 0.34});
            } else {
                corpse = corpse * rotationYMatrix(-1.45 * fall);
            }
            corpse = corpse * scaleMatrix(Vec3{fade, fade, fade});

            addPart(scene, assets.bloodPool,
                    translationMatrix(mapToWorld(zombie.position, 0.012)) *
                        rotationZMatrix(-zombie.heading) *
                        scaleMatrix(Vec3{0.55 + 0.35 * settle,
                                         0.40 + 0.25 * settle, 1.0}),
                    mix(makeColor(80, 8, 8), makeColor(138, 22, 14), settle), true,
                    nullptr, 0.0, 32.0, 0.9);
            addPart(scene, assets.shadow,
                    translationMatrix(mapToWorld(zombie.position, 0.018)) *
                        scaleMatrix(Vec3{0.50, 0.34, 1.0}),
                    makeColor(12, 14, 18), true, nullptr, 0.0, 32.0, 0.6);
            const double splay = zombie.flattened ? 1.35 : 0.95;
            addPart(scene, assets.leftLeg,
                    corpse * translationMatrix(Vec3{0.0, 0.10, 0.78}) *
                        rotationYMatrix(-splay),
                    zombiePants(flash));
            addPart(scene, assets.rightLeg,
                    corpse * translationMatrix(Vec3{0.0, -0.10, 0.78}) *
                        rotationYMatrix(splay),
                    zombiePants(flash));
            addPart(scene, assets.torso, corpse, zombieCloth(flash));
            addPart(scene, assets.leftArm,
                    corpse * translationMatrix(Vec3{0.0, 0.23, 1.28}) *
                        rotationYMatrix(-1.9 - splay * 0.2),
                    zombieSkin(flash));
            addPart(scene, assets.rightArm,
                    corpse * translationMatrix(Vec3{0.0, -0.23, 1.28}) *
                        rotationYMatrix(-1.9 + splay * 0.2),
                    zombieSkin(flash));
            addPart(scene, assets.head, corpse, zombieSkin(flash));
            addPart(scene, assets.leftEye,
                    corpse * translationMatrix(Vec3{0.112, 0.055, 1.51}),
                    makeColor(238, 66, 46), true);
            addPart(scene, assets.rightEye,
                    corpse * translationMatrix(Vec3{0.112, -0.055, 1.51}),
                    makeColor(238, 66, 46), true);
            continue;
        }

        const Vec3 basePosition = mapToWorld(zombie.position, 0.0);
        const Mat4 base =
            translationMatrix(basePosition) * rotationZMatrix(-zombie.heading);
        const double swing = std::sin(zombie.walkPhase) * 0.55;
        const double bob = std::fabs(std::sin(zombie.walkPhase)) * 0.025;

        addPart(scene, assets.shadow,
                translationMatrix(mapToWorld(zombie.position, 0.02)) *
                    scaleMatrix(Vec3{0.36, 0.36, 1.0}),
                makeColor(12, 14, 18), true, nullptr, 0.0, 32.0, 0.6);
        addPart(scene, assets.leftLeg,
                base * translationMatrix(Vec3{0.0, 0.10, 0.78}) *
                    rotationYMatrix(swing),
                zombiePants(flash));
        addPart(scene, assets.rightLeg,
                base * translationMatrix(Vec3{0.0, -0.10, 0.78}) *
                    rotationYMatrix(-swing),
                zombiePants(flash));
        addPart(scene, assets.torso,
                base * translationMatrix(Vec3{0.0, 0.0, bob}), zombieCloth(flash));
        addPart(scene, assets.leftArm,
                base * translationMatrix(Vec3{0.0, 0.23, 1.28}) *
                    rotationYMatrix(-1.05 + swing * 0.25),
                zombieSkin(flash));
        addPart(scene, assets.rightArm,
                base * translationMatrix(Vec3{0.0, -0.23, 1.28}) *
                    rotationYMatrix(-1.05 - swing * 0.25),
                zombieSkin(flash));
        addPart(scene, assets.head,
                base * translationMatrix(Vec3{0.0, 0.0, bob}), zombieSkin(flash));
        addPart(scene, assets.leftEye,
                base * translationMatrix(Vec3{0.112, 0.055, 1.51 + bob}),
                makeColor(238, 66, 46), true);
        addPart(scene, assets.rightEye,
                base * translationMatrix(Vec3{0.112, -0.055, 1.51 + bob}),
                makeColor(238, 66, 46), true);

        if (!showHealthBars) {
            continue;
        }
        const double fraction =
            static_cast<double>(zombie.health) /
            static_cast<double>(std::max(1, zombie.maxHealth));
        const Vec3 center = mapToWorld(zombie.position, 1.74);
        const Color fillColor = fraction > 0.6
                                    ? makeColor(88, 208, 96)
                                    : (fraction > 0.3 ? makeColor(232, 180, 62)
                                                      : makeColor(218, 66, 54));
        addPart(scene, assets.healthQuad,
                translationMatrix(center) * billboard *
                    scaleMatrix(Vec3{0.52, 0.085, 1.0}),
                makeColor(8, 10, 12), true);
        const double fillWidth = 0.46 * clampd(fraction, 0.0, 1.0);
        const Vec3 fillCenter =
            center + basis.right * (-0.23 + fillWidth * 0.5) + towardCamera * 0.008;
        addPart(scene, assets.healthQuad,
                translationMatrix(fillCenter) * billboard *
                    scaleMatrix(Vec3{std::max(0.02, fillWidth), 0.05, 1.0}),
                fillColor, true);
    }
}

void appendParticles(Scene& scene, const Game& game, const WorldAssets& assets) {
    if (game.particles.empty()) {
        return;
    }
    const CameraBasis basis = cameraBasis(scene.camera);
    const Vec3 towardCamera = -basis.forward;
    Mat4 billboard = identityMatrix();
    billboard.m[0][0] = basis.right.x;
    billboard.m[1][0] = basis.right.y;
    billboard.m[2][0] = basis.right.z;
    billboard.m[0][1] = basis.up.x;
    billboard.m[1][1] = basis.up.y;
    billboard.m[2][1] = basis.up.z;
    billboard.m[0][2] = towardCamera.x;
    billboard.m[1][2] = towardCamera.y;
    billboard.m[2][2] = towardCamera.z;

    for (const Particle& particle : game.particles) {
        const double fade =
            clampd(particle.life / std::max(1e-6, particle.maxLife), 0.0, 1.0);
        const double age = 1.0 - fade;
        const double size =
            particle.size *
            (particle.glow ? (0.7 + age * 1.7) : (1.0 - age * 0.35));
        addPart(scene, assets.particleMesh,
                translationMatrix(mapToWorld(particle.position, particle.height)) *
                    billboard * scaleMatrix(Vec3{size, size, 1.0}),
                particle.color, true, nullptr, 0.0, 32.0, fade);
    }
}

}  // namespace g3d
