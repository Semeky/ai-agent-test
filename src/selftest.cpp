#include "selftest.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace g3d {
namespace {

bool approximately(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

int countDifferentPixels(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) {
        return -1;
    }
    int changed = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.at(x, y) != b.at(x, y)) {
                ++changed;
            }
        }
    }
    return changed;
}

int countColoredPixels(const Image& image) {
    int colored = 0;
    for (const Color& color : image.pixels()) {
        if (color.r != 0 || color.g != 0 || color.b != 0) {
            ++colored;
        }
    }
    return colored;
}

std::size_t uniqueColorCount(const Image& image) {
    std::set<std::uint32_t> colors;
    for (const Color& color : image.pixels()) {
        colors.insert((static_cast<std::uint32_t>(color.r) << 16U) |
                      (static_cast<std::uint32_t>(color.g) << 8U) |
                      static_cast<std::uint32_t>(color.b));
    }
    return colors.size();
}

struct TestAssets {
    WorldAssets world;
    ZombieAssets zombies;
    VehicleAssets vehicles;

    explicit TestAssets(const Grid& map) { world.build(map); }
};

void addTestTriangle(Mesh& mesh, double z, bool clockwise) {
    const Vec3 normal{0.0, 0.0, clockwise ? -1.0 : 1.0};
    const Vec2 uv{0.0, 0.0};
    const Vertex a{Vec3{-1.0, -1.0, z}, normal, uv};
    const Vertex b{Vec3{1.0, -1.0, z}, normal, uv};
    const Vertex c{Vec3{0.0, 1.0, z}, normal, uv};
    if (clockwise) {
        mesh.addTriangle(a, c, b);
    } else {
        mesh.addTriangle(a, b, c);
    }
}

Scene makeSimpleScene(const Mesh& mesh, Color color) {
    Scene scene;
    scene.skyTop = makeColor(0, 0, 0);
    scene.skyBottom = makeColor(0, 0, 0);
    scene.camera.eye = Vec3{0.0, 0.0, 0.0};
    scene.camera.target = Vec3{0.0, 0.0, -1.0};
    scene.camera.up = Vec3{0.0, 1.0, 0.0};
    scene.camera.fovYDegrees = 60.0;
    DrawItem item;
    item.mesh = &mesh;
    item.material.tint = color;
    item.material.unlit = true;
    scene.items.push_back(item);
    return scene;
}

void renderGameFrame(Image& image, const Game& game, const TestAssets& assets,
                     bool mapMode, bool withEntities, bool withCeiling) {
    Scene scene;
    const double aspect =
        static_cast<double>(image.width()) / static_cast<double>(image.height());
    if (mapMode) {
        scene.camera = overheadCamera(aspect);
    } else if (game.player.inVehicle) {
        scene.camera = chaseCamera(game, kDefaultFov, aspect);
    } else {
        scene.camera = firstPersonCamera(game, kDefaultFov, aspect);
    }
    appendWorldGeometry(scene, assets.world, withCeiling);
    if (withEntities) {
        appendVehicles(scene, game, assets.vehicles);
        appendZombies(scene, game, assets.zombies, !mapMode);
        appendParticles(scene, game, assets.world);
    }
    renderScene(image, scene);
}

bool selftestMath(std::string& error) {
    const Mat4 identity = identityMatrix();
    const Vec3 point = transformPoint(identity, Vec3{1.0, 2.0, 3.0});
    if (!approximately(point.x, 1.0) || !approximately(point.y, 2.0) ||
        !approximately(point.z, 3.0)) {
        error = "identity transform changed a point";
        return false;
    }

    const Vec3 rotated =
        transformPoint(rotationZMatrix(kPi * 0.5), Vec3{1.0, 0.0, 0.0});
    if (!approximately(rotated.x, 0.0, 1e-9) || !approximately(rotated.y, 1.0, 1e-9)) {
        error = "rotation around Z is wrong";
        return false;
    }

    const Mat4 projection =
        perspectiveMatrix(60.0, 1.0, 0.1, 100.0);
    const Vec4 center = projection * Vec4{0.0, 0.0, -5.0, 1.0};
    if (!approximately(center.x, 0.0, 1e-9) || !approximately(center.y, 0.0, 1e-9) ||
        !approximately(center.w, 5.0, 1e-9)) {
        error = "perspective projection of the view axis is wrong";
        return false;
    }
    const Vec4 behind = projection * Vec4{0.0, 0.0, 5.0, 1.0};
    if (!(behind.w < 0.0)) {
        error = "a point behind the camera is not clipped by w";
        return false;
    }

    const Mat4 view =
        lookAtMatrix(Vec3{0.0, 0.0, 5.0}, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0});
    const Vec3 origin = transformPoint(view, Vec3{0.0, 0.0, 0.0});
    if (!approximately(origin.x, 0.0, 1e-9) || !approximately(origin.y, 0.0, 1e-9) ||
        !approximately(origin.z, -5.0, 1e-9)) {
        error = "look-at view matrix is wrong";
        return false;
    }
    if (!approximately(verticalFovFromHorizontal(90.0, 1.0), 90.0, 1e-9)) {
        error = "horizontal/vertical field of view conversion is wrong";
        return false;
    }
    return true;
}

bool selftestTexture(std::string& error) {
    Texture texture(2, 2, {makeColor(255, 0, 0), makeColor(0, 255, 0),
                           makeColor(0, 0, 255), makeColor(255, 255, 255)});
    const Color red = texture.sample(0.25, 0.25, 0.0);
    if (red.r < 250 || red.g > 5 || red.b > 5) {
        error = "texture sampling does not hit the first texel";
        return false;
    }
    const Color white = texture.sample(0.75, 0.75, 0.0);
    if (white.r < 250 || white.g < 250 || white.b < 250) {
        error = "texture sampling does not hit the last texel";
        return false;
    }
    const Color wrapped = texture.sample(1.25, 1.25, 0.0);
    if (wrapped != red) {
        error = "texture coordinates do not wrap";
        return false;
    }
    const Color averaged = texture.sample(0.5, 0.5, 10.0);
    if (std::abs(static_cast<int>(averaged.r) - 128) > 2 ||
        std::abs(static_cast<int>(averaged.g) - 128) > 2 ||
        std::abs(static_cast<int>(averaged.b) - 128) > 2) {
        error = "mip level does not average a 2x2 texture";
        return false;
    }
    return true;
}

bool selftestRasterizer(std::string& error) {
    Image frame(64, 64);
    Mesh nearMesh;
    addTestTriangle(nearMesh, -3.0, false);
    const Scene nearScene = makeSimpleScene(nearMesh, makeColor(220, 40, 30));
    renderScene(frame, nearScene);
    if (frame.at(32, 32) != makeColor(220, 40, 30)) {
        error = "a front-facing triangle is not rasterized at the screen center";
        return false;
    }
    if (frame.at(1, 1) != makeColor(0, 0, 0)) {
        error = "the sky clear color is missing outside the triangle";
        return false;
    }

    // A farther blue triangle is drawn first, then a nearer red one.  The
    // depth buffer must keep the red pixels.
    Mesh blueMesh;
    addTestTriangle(blueMesh, -5.0, false);
    Mesh redMesh;
    addTestTriangle(redMesh, -3.0, false);
    Scene layered = makeSimpleScene(blueMesh, makeColor(30, 40, 220));
    DrawItem redItem;
    redItem.mesh = &redMesh;
    redItem.material.tint = makeColor(220, 40, 30);
    redItem.material.unlit = true;
    layered.items.push_back(redItem);
    Image depthFrame(64, 64);
    renderScene(depthFrame, layered);
    if (depthFrame.at(32, 32) != makeColor(220, 40, 30)) {
        error = "the depth buffer does not keep the nearest triangle";
        return false;
    }

    // Back-face culling: the same triangle wound the other way disappears.
    Mesh culledMesh;
    addTestTriangle(culledMesh, -3.0, true);
    const Scene culled = makeSimpleScene(culledMesh, makeColor(220, 40, 30));
    Image culledFrame(64, 64);
    renderScene(culledFrame, culled);
    if (countColoredPixels(culledFrame) != 0) {
        error = "back-face culling draws a clockwise triangle";
        return false;
    }

    // A triangle crossing the near plane must be clipped, not dropped or
    // projected to infinity.
    Mesh clippedMesh;
    const Vec3 normal{0.0, 0.0, 1.0};
    const Vec2 uv{0.0, 0.0};
    clippedMesh.addTriangle(Vertex{Vec3{-2.0, -2.0, -3.0}, normal, uv},
                            Vertex{Vec3{2.0, -2.0, -3.0}, normal, uv},
                            Vertex{Vec3{0.0, 2.0, 0.5}, normal, uv});
    const Scene clippedScene =
        makeSimpleScene(clippedMesh, makeColor(240, 240, 240));
    Image clippedFrame(64, 64);
    renderScene(clippedFrame, clippedScene);
    if (countColoredPixels(clippedFrame) < 20) {
        error = "a triangle crossing the near plane was not clipped and drawn";
        return false;
    }
    return true;
}

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
            ++materialCounts[static_cast<std::size_t>(map[y][x])];
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

    const Game game = createGame();
    if (game.vehicles.size() < 2) {
        error = "expected at least two vehicles";
        return false;
    }
    for (std::size_t i = 0; i < game.vehicles.size(); ++i) {
        const Vehicle& vehicle = game.vehicles[i];
        const Vec2 forward{std::cos(vehicle.heading), std::sin(vehicle.heading)};
        if (!isWalkableCentered(map, vehicle.position + forward * 0.5, kVehicleRadius) ||
            !isWalkableCentered(map, vehicle.position - forward * 0.5, kVehicleRadius)) {
            error = "vehicle " + std::to_string(i) + " spawns inside a wall";
            return false;
        }
        for (std::size_t j = i + 1; j < game.vehicles.size(); ++j) {
            if (length(game.vehicles[i].position - game.vehicles[j].position) < 1.2) {
                error = "vehicles spawn on top of each other";
                return false;
            }
        }
    }
    for (const Zombie& zombie : game.zombies) {
        if (!isWalkableCentered(map, zombie.spawn, 0.25)) {
            error = "a zombie spawns inside a wall";
            return false;
        }
        for (const Vehicle& vehicle : game.vehicles) {
            const Vec2 forward{std::cos(vehicle.heading), std::sin(vehicle.heading)};
            for (const double sign : {1.0, -1.0}) {
                const Vec2 wheel = vehicle.position + forward * sign * 0.5;
                if (length(zombie.spawn - wheel) < 0.65) {
                    error = "a zombie spawns on a vehicle";
                    return false;
                }
            }
        }
    }
    return true;
}

bool selftestGridTrace(const Grid& map, std::string& error) {
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

    auto checkRay = [&](Vec2 origin, Vec2 direction, std::string& why) {
        const GridHit hit = traceGrid(map, origin, direction);
        if (hit.material == 0) {
            why = "ray did not hit a wall";
            return false;
        }
        if (hit.distance > 1e29) {
            why = "ray returned an infinite distance";
            return false;
        }
        double marched = -1.0;
        for (double t = 0.0; t < kMaxDistance; t += kStep) {
            const int mx = static_cast<int>(std::floor(origin.x + direction.x * t));
            const int my = static_cast<int>(std::floor(origin.y + direction.y * t));
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
        if (hit.distance > marched + 1e-6 || hit.distance < marched - kStep - 1e-6) {
            why = "ray distance does not match the brute-force march";
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
        if (!checkRay(origin, Vec2{std::cos(angle), std::sin(angle)}, failure)) {
            error = "random ray " + std::to_string(i) + ": " + failure;
            return false;
        }
    }
    for (const double angle : {0.0, 90.0, 180.0, 270.0}) {
        const Vec2 direction = directionFromDegrees(angle);
        for (int i = 0; i < 24; ++i) {
            const auto& cell = openCells[static_cast<std::size_t>(rng()) % openCells.size()];
            const Vec2 origin{cell.first + 0.5, cell.second + 0.5};
            if (!checkRay(origin, direction, failure)) {
                error = "axis ray at " + std::to_string(angle) + " degrees: " + failure;
                return false;
            }
        }
    }
    return true;
}

bool selftestWeaponsAndZombies(std::string& error) {
    Game game = createGame();
    const Vec2 toFirstZombie =
        game.zombies[0].position - game.player.position;
    if (findShotTarget(game, game.player.position, toFirstZombie) != 0) {
        error = "a zombie straight ahead cannot be shot";
        return false;
    }

    Zombie target;
    target.spawn = target.position = Vec2{4.5, 14.5};
    Game wallGame = createGame();
    wallGame.zombies.clear();
    wallGame.zombies.push_back(target);
    if (findShotTarget(wallGame, kSpawn, Vec2{-1.0, 0.0}) != -1) {
        error = "a shot passed through a wall";
        return false;
    }

    Zombie victim;
    victim.health = victim.maxHealth = kZombieMaxHealth;
    if (damageZombie(victim, kShotDamage)) {
        error = "one shot killed a full-health zombie";
        return false;
    }
    damageZombie(victim, kShotDamage);
    if (!damageZombie(victim, kShotDamage)) {
        error = "three shots did not kill a full-health zombie";
        return false;
    }
    if (victim.alive || victim.health != 0) {
        error = "a killed zombie is still alive";
        return false;
    }

    // Chasing, melee damage and respawns through the real update path.
    Game chase = createGame();
    chase.vehicles.clear();
    chase.zombies.clear();
    Zombie chaser;
    chaser.spawn = chaser.position = Vec2{7.0, 13.5};
    chaser.health = chaser.maxHealth = kZombieMaxHealth;
    chase.zombies.push_back(chaser);
    const double distanceBefore = length(chaser.position - chase.player.position);
    for (int frame = 0; frame < 20; ++frame) {
        updateGame(chase, Input{}, 0.05);
    }
    const double distanceAfter =
        length(chase.zombies[0].position - chase.player.position);
    if (distanceAfter >= distanceBefore - 0.05) {
        error = "a zombie does not chase the player";
        return false;
    }

    chase.zombies[0].position = Vec2{6.9, 14.3};
    chase.zombies[0].attackTimer = 0.0;
    const int healthBefore = chase.player.health;
    updateGame(chase, Input{}, 0.01);
    if (chase.player.health > healthBefore - kZombieAttackDamage) {
        error = "a zombie in melee range does not damage the player";
        return false;
    }

    Game respawn = createGame();
    respawn.vehicles.clear();
    respawn.zombies.clear();
    Zombie dead;
    dead.spawn = dead.position = Vec2{16.5, 15.5};
    dead.alive = false;
    dead.health = 0;
    dead.respawnTimer = 0.0;
    respawn.zombies.push_back(dead);
    updateGame(respawn, Input{}, 0.01);
    if (!respawn.zombies[0].alive ||
        respawn.zombies[0].health != respawn.zombies[0].maxHealth) {
        error = "a dead zombie does not respawn";
        return false;
    }
    return true;
}

bool selftestVehicles(const Grid& map, std::string& error) {
    Game game = createGame();
    game.zombies.clear();
    if (nearbyVehicle(game) >= 0) {
        error = "a vehicle is reachable from the default spawn without walking";
        return false;
    }
    if (enterVehicle(game, 0)) {
        error = "a vehicle can be entered from far away";
        return false;
    }

    Vehicle& car = game.vehicles[0];
    game.player.position = car.position + Vec2{0.0, 0.9};
    if (!enterVehicle(game, 0)) {
        error = "a nearby vehicle cannot be entered";
        return false;
    }
    game.player.position = car.position;

    auto resetCar = [&]() {
        car.position = Vec2{7.0, 8.5};
        car.heading = 0.0;
        car.speed = 0.0;
        car.steer = 0.0;
        game.player.position = car.position;
    };

    Input accelerate;
    accelerate.forward = 1.0;
    resetCar();
    for (int frame = 0; frame < 120; ++frame) {
        updateGame(game, accelerate, 1.0 / 60.0);
    }
    if (car.speed < 4.0) {
        error = "the vehicle does not accelerate";
        return false;
    }
    if (car.speed > kMaxVehicleSpeed + 1e-6) {
        error = "the vehicle exceeds its maximum speed";
        return false;
    }
    if (car.position.x < 9.5) {
        error = "the vehicle does not move forward";
        return false;
    }

    resetCar();
    car.speed = 4.0;
    Input steer;
    steer.forward = 1.0;
    steer.strafe = 1.0;
    const double headingBefore = car.heading;
    for (int frame = 0; frame < 30; ++frame) {
        updateGame(game, steer, 1.0 / 60.0);
    }
    if (car.heading - headingBefore < 0.1) {
        error = "steering does not change the vehicle heading";
        return false;
    }

    resetCar();
    car.speed = 5.0;
    Input brake;
    brake.forward = -1.0;
    for (int frame = 0; frame < 60; ++frame) {
        updateGame(game, brake, 1.0 / 60.0);
    }
    if (car.speed > 0.5) {
        error = "braking does not slow the vehicle";
        return false;
    }

    // Give the reverse run a long clear lane: the car starts facing east and
    // rolls backwards west along the open north side of the atrium.
    resetCar();
    car.position = Vec2{16.0, 8.5};
    game.player.position = car.position;
    for (int frame = 0; frame < 60; ++frame) {
        updateGame(game, brake, 1.0 / 60.0);
    }
    if (car.speed > -1.0) {
        error = "reversing does not move the vehicle backwards";
        return false;
    }

    resetCar();
    Input steerStanding;
    steerStanding.strafe = 1.0;
    const double standingHeading = car.heading;
    for (int frame = 0; frame < 30; ++frame) {
        updateGame(game, steerStanding, 1.0 / 60.0);
    }
    if (std::fabs(car.heading - standingHeading) > 1e-9) {
        error = "the vehicle turns while standing still";
        return false;
    }

    // Drive east along the open atrium until the east wall stops the car.
    resetCar();
    for (int frame = 0; frame < 300; ++frame) {
        updateGame(game, accelerate, 1.0 / 60.0);
    }
    if (car.position.x > 17.5) {
        error = "the vehicle drove through the east wall";
        return false;
    }
    if (!isWalkableCentered(map, car.position, kVehicleRadius)) {
        error = "the vehicle ended up inside a wall";
        return false;
    }
    if (!std::isfinite(car.position.x) || !std::isfinite(car.position.y)) {
        error = "vehicle position is not finite";
        return false;
    }

    // Vehicle-on-vehicle collision.
    resetCar();
    game.vehicles[1].position = Vec2{9.5, 8.5};
    game.vehicles[1].heading = kPi;
    game.vehicles[1].speed = 0.0;
    for (int frame = 0; frame < 120; ++frame) {
        updateGame(game, accelerate, 1.0 / 60.0);
    }
    if (length(game.vehicles[0].position - game.vehicles[1].position) < 1.2) {
        error = "vehicles drive through each other";
        return false;
    }

    // Exiting places the player on walkable ground outside the car.
    car.speed = 0.0;
    game.player.position = car.position;
    if (!exitVehicle(game)) {
        error = "a stopped vehicle cannot be exited";
        return false;
    }
    if (game.player.inVehicle) {
        error = "the player is still flagged as driving after exiting";
        return false;
    }
    if (!isWalkableCentered(map, game.player.position, kPlayerRadius)) {
        error = "the player exits into a wall";
        return false;
    }
    if (length(game.player.position - car.position) < 0.5) {
        error = "the player exits inside the vehicle";
        return false;
    }

    car.speed = 3.0;
    game.player.position = car.position + Vec2{0.0, 0.8};
    if (enterVehicle(game, 0)) {
        error = "a moving vehicle can be entered";
        return false;
    }
    return true;
}

bool selftestDash(std::string& error) {
    Game game = createGame();
    game.vehicles.clear();
    game.zombies.clear();
    game.player.position = Vec2{6.5, 14.5};
    game.player.yawDegrees = 0.0;
    if (!dashReady(game)) {
        error = "dash is not ready at spawn";
        return false;
    }
    if (!startDash(game, Vec2{1.0, 0.0})) {
        error = "the dash did not start";
        return false;
    }
    if (game.player.dashTimer <= 0.0 || game.player.dashCooldown <= 0.0) {
        error = "the dash did not set its timer and cooldown";
        return false;
    }
    if (game.soundEvents.empty() || game.soundEvents.back() != SoundEvent::Dash) {
        error = "the dash did not queue its audio event";
        return false;
    }
    const Vec2 start = game.player.position;
    for (int frame = 0; frame < 5; ++frame) {
        updateGame(game, Input{}, 1.0 / 60.0);
    }
    if (length(game.player.position - start) < 0.7) {
        error = "the dash did not move the player fast enough";
        return false;
    }
    if (game.player.dashTimer <= 0.0) {
        error = "the dash ended too early";
        return false;
    }
    if (startDash(game, Vec2{1.0, 0.0})) {
        error = "a second dash ignored the cooldown";
        return false;
    }
    for (int frame = 0; frame < 140; ++frame) {
        updateGame(game, Input{}, 1.0 / 60.0);
    }
    if (!dashReady(game)) {
        error = "the dash cooldown does not recover";
        return false;
    }

    // A dash that hits a zombie damages it, awards score and spawns effects.
    Game combat = createGame();
    combat.vehicles.clear();
    combat.zombies.clear();
    combat.player.position = Vec2{6.5, 14.5};
    combat.player.yawDegrees = 0.0;
    Zombie target;
    target.spawn = target.position = Vec2{7.2, 14.5};
    target.health = 55;
    target.maxHealth = kZombieMaxHealth;
    combat.zombies.push_back(target);
    Input dashInput;
    dashInput.dash = true;
    dashInput.dashDirection = Vec2{1.0, 0.0};
    updateGame(combat, dashInput, 1.0 / 60.0);
    for (int frame = 0; frame < 5; ++frame) {
        updateGame(combat, Input{}, 1.0 / 60.0);
    }
    if (!combat.zombies[0].ragdoll) {
        error = "a dash hit did not kill a damaged zombie";
        return false;
    }
    if (combat.kills != 1 || combat.score <= 0) {
        error = "a dash kill did not award kills and score";
        return false;
    }
    if (combat.particles.empty()) {
        error = "a dash kill did not spawn blood or dust particles";
        return false;
    }
    if (combat.zombies[0].flattened) {
        error = "a dash kill was flattened like a vehicle impact";
        return false;
    }

    // A low frame rate must not tunnel the dash through a zombie.
    Game sweep = createGame();
    sweep.vehicles.clear();
    sweep.zombies.clear();
    sweep.player.position = Vec2{6.5, 14.5};
    sweep.player.yawDegrees = 0.0;
    Zombie middle;
    middle.spawn = middle.position = Vec2{7.6, 14.5};
    middle.health = 55;
    middle.maxHealth = kZombieMaxHealth;
    sweep.zombies.push_back(middle);
    Input sweepInput;
    sweepInput.dash = true;
    sweepInput.dashDirection = Vec2{1.0, 0.0};
    updateGame(sweep, sweepInput, 0.1);
    if (!sweep.zombies[0].ragdoll) {
        error = "the dash tunneled past a zombie at a large time step";
        return false;
    }

    // The active dash grants invulnerability, then the same zombie can hit.
    Game dodge = createGame();
    dodge.vehicles.clear();
    dodge.zombies.clear();
    dodge.player.position = Vec2{6.5, 14.5};
    dodge.player.yawDegrees = 0.0;
    Zombie attacker;
    attacker.spawn = attacker.position = Vec2{5.7, 14.5};
    attacker.health = kZombieMaxHealth;
    attacker.maxHealth = kZombieMaxHealth;
    attacker.attackTimer = 0.0;
    dodge.zombies.push_back(attacker);
    if (!startDash(dodge, Vec2{1.0, 0.0})) {
        error = "the dodge test could not start a dash";
        return false;
    }
    const int healthBefore = dodge.player.health;
    updateGame(dodge, Input{}, 0.01);
    if (dodge.player.health != healthBefore) {
        error = "a zombie hit the player during the dash invulnerability window";
        return false;
    }
    dodge.player.dashTimer = 0.0;
    dodge.player.dashCooldown = 1.0;
    dodge.zombies[0].position = dodge.player.position - Vec2{0.8, 0.0};
    dodge.zombies[0].attackTimer = 0.0;
    dodge.zombies[0].stunTimer = 0.0;
    updateGame(dodge, Input{}, 0.01);
    if (dodge.player.health >= healthBefore) {
        error = "the zombie could not attack once the dash ended";
        return false;
    }
    return true;
}

bool selftestVehicleCombat(std::string& error) {
    const auto setupImpact = [](Game& game, Vec2 carPosition, double speed,
                                Vec2 zombiePosition) {
        game.zombies.clear();
        game.player.inVehicle = false;
        game.player.vehicleIndex = -1;
        Zombie zombie;
        zombie.spawn = zombie.position = zombiePosition;
        zombie.health = zombie.maxHealth = kZombieMaxHealth;
        zombie.attackTimer = 90.0;
        game.zombies.push_back(zombie);
        Vehicle& car = game.vehicles[0];
        car.position = carPosition;
        car.heading = 0.0;
        car.speed = speed;
        car.steer = 0.0;
        game.player.inVehicle = true;
        game.player.vehicleIndex = 0;
        game.player.position = carPosition;
    };

    Game low = createGame();
    setupImpact(low, Vec2{8.8, 8.5}, 2.0, Vec2{9.8, 8.5});
    for (int frame = 0; frame < 30 && low.zombies[0].health == kZombieMaxHealth;
         ++frame) {
        updateGame(low, Input{}, 1.0 / 60.0);
    }
    if (low.zombies[0].health == kZombieMaxHealth) {
        error = "a slow vehicle did not damage a zombie";
        return false;
    }
    if (low.zombies[0].health == 0) {
        error = "a slow vehicle killed a full-health zombie too easily";
        return false;
    }
    if (lengthSquared(low.zombies[0].knockback) <= 0.0) {
        error = "a vehicle impact did not knock the zombie back";
        return false;
    }
    if (low.particles.empty()) {
        error = "a vehicle impact did not spawn particles";
        return false;
    }
    if (low.kills != 0) {
        error = "a non-lethal vehicle hit counted as a kill";
        return false;
    }

    Game high = createGame();
    setupImpact(high, Vec2{8.8, 8.5}, kMaxVehicleSpeed, Vec2{11.6, 8.5});
    bool sawImpactSound = false;
    for (int frame = 0; frame < 90 && !high.zombies[0].ragdoll; ++frame) {
        updateGame(high, Input{}, 1.0 / 60.0);
        for (SoundEvent event : high.soundEvents) {
            if (event == SoundEvent::Impact) {
                sawImpactSound = true;
            }
        }
    }
    if (!high.zombies[0].ragdoll) {
        error = "a fast vehicle impact did not kill the zombie";
        return false;
    }
    if (!sawImpactSound) {
        error = "a vehicle impact did not queue an audio event";
        return false;
    }
    if (high.zombies[0].health != 0 || high.kills != 1 || high.score <= 0) {
        error = "a vehicle kill did not damage, kill and score correctly";
        return false;
    }
    if (!high.zombies[0].flattened) {
        error = "a fast vehicle impact did not flatten the corpse";
        return false;
    }
    if (high.particles.empty()) {
        error = "a vehicle kill did not spawn blood or dust";
        return false;
    }

    // Respawn clears the ragdoll and flattening state.
    Zombie& corpse = high.zombies[0];
    corpse.respawnTimer = 0.0;
    high.vehicles[0].position = Vec2{2.5, 2.5};
    high.player.position = high.vehicles[0].position;
    updateGame(high, Input{}, 0.01);
    if (!corpse.alive || corpse.ragdoll || corpse.flattened) {
        error = "a respawned zombie kept its ragdoll state";
        return false;
    }
    return true;
}

bool selftestScene(const TestAssets& assets, std::string& error) {
    Game game = createGame();

    Image worldOnly(320, 180);
    renderGameFrame(worldOnly, game, assets, false, false, true);
    const int colored = countColoredPixels(worldOnly);
    if (colored < 30000) {
        error = "the world frame is mostly empty";
        return false;
    }
    if (uniqueColorCount(worldOnly) < 100) {
        error = "the world frame has too few distinct colors";
        return false;
    }

    Image withEntities(320, 180);
    renderGameFrame(withEntities, game, assets, false, true, true);
    if (countDifferentPixels(worldOnly, withEntities) < 300) {
        error = "zombies and vehicles are not visible in the first-person frame";
        return false;
    }

    // A zombie behind a wall must be fully occluded, including its health bar.
    Game hiddenGame = createGame();
    for (Zombie& zombie : hiddenGame.zombies) {
        zombie.alive = false;
    }
    hiddenGame.zombies[0].alive = true;
    hiddenGame.zombies[0].position = Vec2{4.5, 14.5};
    hiddenGame.zombies[0].health = 40;
    hiddenGame.player.yawDegrees = 180.0;
    Scene hiddenScene;
    hiddenScene.camera =
        firstPersonCamera(hiddenGame, kDefaultFov, static_cast<double>(320) / 180.0);
    appendWorldGeometry(hiddenScene, assets.world, true);
    Image hiddenBase(320, 180);
    renderScene(hiddenBase, hiddenScene);
    Scene zombieScene = hiddenScene;
    appendZombies(zombieScene, hiddenGame, assets.zombies, true);
    Image hiddenZombie(320, 180);
    renderScene(hiddenZombie, zombieScene);
    if (countDifferentPixels(hiddenBase, hiddenZombie) != 0) {
        error = "a zombie behind a wall is still drawn";
        return false;
    }

    // Third-person chase camera while driving.
    Game driving = createGame();
    driving.zombies.clear();
    driving.player.position = driving.vehicles[0].position + Vec2{0.0, 0.9};
    if (!enterVehicle(driving, 0)) {
        error = "the vehicle cannot be entered for the chase-camera test";
        return false;
    }
    Image drivingFrame(320, 180);
    renderGameFrame(drivingFrame, driving, assets, false, true, true);
    if (countDifferentPixels(withEntities, drivingFrame) < 1000) {
        error = "the chase camera does not change the rendered view";
        return false;
    }

    // Overhead 3D map view without the ceiling.
    Image overhead(320, 180);
    renderGameFrame(overhead, driving, assets, true, true, false);
    if (countDifferentPixels(drivingFrame, overhead) < 1000) {
        error = "the overhead 3D map view is not rendered";
        return false;
    }
    if (uniqueColorCount(overhead) < 40) {
        error = "the overhead 3D map view has too few distinct colors";
        return false;
    }

    // A killed zombie must render as a ragdoll instead of disappearing at once.
    Game effects = createGame();
    effects.zombies.clear();
    effects.player.yawDegrees = 0.0;
    Zombie victim;
    victim.spawn = victim.position = Vec2{8.0, 14.5};
    victim.health = victim.maxHealth = kZombieMaxHealth;
    effects.zombies.push_back(victim);
    Image aliveFrame(320, 180);
    renderGameFrame(aliveFrame, effects, assets, false, true, true);
    damageZombie(effects.zombies[0], kZombieMaxHealth, Vec2{1.0, 0.0}, true);
    for (int frame = 0; frame < 8; ++frame) {
        updateGame(effects, Input{}, 0.05);
    }
    Image ragdollFrame(320, 180);
    renderGameFrame(ragdollFrame, effects, assets, false, true, true);
    if (countDifferentPixels(aliveFrame, ragdollFrame) < 100) {
        error = "a ragdoll corpse is not rendered";
        return false;
    }

    // Camera shake and dash motion blur must change the rendered frame.
    Game shakeGame = createGame();
    shakeGame.shake = 1.0;
    const Camera stillCamera =
        firstPersonCamera(shakeGame, kDefaultFov, static_cast<double>(320) / 180.0);
    Camera shakenCamera = stillCamera;
    applyCameraShake(shakenCamera, shakeGame);
    if (length(shakenCamera.eye - stillCamera.eye) < 1e-6) {
        error = "camera shake does not move the camera";
        return false;
    }
    Image sharp(64, 36);
    sharp.fillVerticalGradient(makeColor(10, 20, 40), makeColor(220, 160, 60));
    Image blurred = sharp;
    applyRadialBlur(blurred, 0.4);
    if (countDifferentPixels(sharp, blurred) < 10) {
        error = "the dash radial blur does not change the frame";
        return false;
    }
    return true;
}

bool selftestPPM(std::string& error) {
    const std::string path = "game3d-selftest.ppm";
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
    const long expectedSize =
        static_cast<long>(expectedHeader.size()) + 5L * 3L * 3L;
    if (fileSize != expectedSize) {
        error = "PPM file size is " + std::to_string(fileSize) + ", expected " +
                std::to_string(expectedSize);
        return false;
    }
    return true;
}

}  // namespace

bool runSelftest(const Grid& map) {
    std::string error;
    int failures = 0;
    std::printf("3D engine, dash and vehicle-combat selftest\n");

    if (selftestMath(error)) {
        std::printf("  [ok] math: transforms, projection, view matrix, field of view\n");
    } else {
        std::printf("  [FAIL] math: %s\n", error.c_str());
        ++failures;
    }
    if (selftestTexture(error)) {
        std::printf("  [ok] textures: bilinear sampling, wrapping and mip levels\n");
    } else {
        std::printf("  [FAIL] textures: %s\n", error.c_str());
        ++failures;
    }
    if (selftestRasterizer(error)) {
        std::printf("  [ok] pipeline: rasterization, depth buffer, culling, near clip\n");
    } else {
        std::printf("  [FAIL] pipeline: %s\n", error.c_str());
        ++failures;
    }
    if (selftestMap(map, error)) {
        std::printf("  [ok] map: closed border, five materials, walkable spawns\n");
        std::printf("  [ok] entities: vehicles and zombies start in open space\n");
    } else {
        std::printf("  [FAIL] map: %s\n", error.c_str());
        ++failures;
    }
    if (selftestGridTrace(map, error)) {
        std::printf("  [ok] gameplay rays: 1200 random rays match a brute-force march\n");
        std::printf("  [ok] axis-aligned rays handle infinite delta distances\n");
    } else {
        std::printf("  [FAIL] gameplay rays: %s\n", error.c_str());
        ++failures;
    }
    if (selftestWeaponsAndZombies(error)) {
        std::printf("  [ok] zombies: hitscan, wall blocking, chase, melee, respawn\n");
    } else {
        std::printf("  [FAIL] zombies: %s\n", error.c_str());
        ++failures;
    }
    if (selftestVehicles(map, error)) {
        std::printf("  [ok] vehicles: enter/exit, acceleration, braking, steering\n");
        std::printf("  [ok] vehicles: wall and vehicle collision, reverse, speed cap\n");
    } else {
        std::printf("  [FAIL] vehicles: %s\n", error.c_str());
        ++failures;
    }
    if (selftestDash(error)) {
        std::printf("  [ok] dash: burst speed, cooldown, invulnerability, kills, score\n");
    } else {
        std::printf("  [FAIL] dash: %s\n", error.c_str());
        ++failures;
    }
    if (selftestVehicleCombat(error)) {
        std::printf("  [ok] vehicle combat: speed damage, knockback, ragdoll, score\n");
    } else {
        std::printf("  [FAIL] vehicle combat: %s\n", error.c_str());
        ++failures;
    }

    TestAssets assets(map);
    if (selftestScene(assets, error)) {
        std::printf("  [ok] rendering: first person, chase camera and overhead 3D map\n");
        std::printf("  [ok] occlusion: depth buffer hides a zombie behind a wall\n");
    } else {
        std::printf("  [FAIL] rendering: %s\n", error.c_str());
        ++failures;
    }
    if (selftestPPM(error)) {
        std::printf("  [ok] PPM output: header and payload size\n");
    } else {
        std::printf("  [FAIL] PPM output: %s\n", error.c_str());
        ++failures;
    }

    if (failures == 0) {
        std::printf("All checks passed.\n");
        return true;
    }
    std::printf("%d check(s) failed.\n", failures);
    return false;
}

}  // namespace g3d
