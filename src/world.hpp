// The museum map, game rules, vehicles and the 3D meshes built from them.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.hpp"
#include "renderer.hpp"

namespace g3d {

constexpr int kMapWidth = 24;
constexpr int kMapHeight = 24;
constexpr double kCellSize = 1.0;
constexpr double kWallHeight = 2.8;
constexpr double kFountainHeight = 0.95;
constexpr double kEyeHeight = 1.62;
constexpr double kPlayerRadius = 0.22;
constexpr double kVehicleRadius = 0.3;

constexpr int kPlayerMaxHealth = 100;
constexpr int kZombieMaxHealth = 100;
constexpr double kZombieRadius = 0.3;
constexpr double kZombieAggroRange = 9.0;
constexpr double kZombieAttackRange = 0.95;
constexpr double kZombieSpeed = 0.85;
constexpr int kZombieAttackDamage = 10;
constexpr double kZombieAttackCooldown = 1.1;
constexpr double kZombieRespawnSeconds = 8.0;
constexpr int kShotDamage = 34;
constexpr double kShotCooldown = 0.24;

constexpr double kMaxVehicleSpeed = 6.2;
constexpr double kMaxReverseSpeed = 2.6;
constexpr double kVehicleAcceleration = 6.4;
constexpr double kVehicleBraking = 11.0;
constexpr double kVehicleReverseAcceleration = 4.2;
constexpr double kVehicleDrag = 1.9;
constexpr double kVehicleMaxSteer = 0.62;
constexpr double kVehicleWheelBase = 1.05;

inline constexpr Vec2 kSpawn{6.5, 14.5};
inline constexpr double kSpawnYaw = 330.0;  // map degrees: 0 = east, 90 = south
inline constexpr double kDefaultFov = 75.0;

inline constexpr std::array<Vec2, 6> kZombieSpawns{{
    {8.5, 13.5}, {10.5, 9.5}, {14.5, 14.5}, {7.5, 8.5}, {16.5, 15.5}, {2.5, 6.5},
}};
inline constexpr std::array<int, 6> kZombieStartHealth{{100, 55, 30, 100, 80, 100}};

inline constexpr std::array<Vec2, 2> kVehicleSpawns{{{8.5, 12.5}, {15.5, 12.5}}};
inline constexpr std::array<double, 2> kVehicleSpawnHeadings{{0.0, 180.0}};

using Grid = std::array<std::array<std::uint8_t, kMapWidth>, kMapHeight>;

Grid loadMap(std::string& error);
bool isSolidAt(const Grid& map, double x, double y);
bool isWalkableCentered(const Grid& map, Vec2 position, double radius = kPlayerRadius);
bool circleTouchesSolid(const Grid& map, Vec2 center, double radius);

// Grid ray used by gameplay (line of sight, hitscan and third-person camera
// clearance).  This is simulation code, not the renderer.
struct GridHit {
    double distance = 1e30;
    int mapX = -1;
    int mapY = -1;
    std::uint8_t material = 0;
    bool side = false;
    double wallU = 0.0;
};

GridHit traceGrid(const Grid& map, Vec2 origin, Vec2 direction);
bool hasLineOfSight(const Grid& map, Vec2 from, Vec2 to);

// Map coordinates (x east, y south) to world coordinates (x east, y north,
// z up).  The map is mirrored on y so the 3D pipeline stays right-handed.
Vec3 mapToWorld(Vec2 position, double z);
Vec3 mapDirectionToWorld(double mapHeading);
double mapYawToWorldYaw(double mapYaw);

struct Player {
    Vec2 position = kSpawn;
    double yawDegrees = kSpawnYaw;
    int health = kPlayerMaxHealth;
    bool inVehicle = false;
    int vehicleIndex = -1;
};

struct Vehicle {
    int id = 0;
    Vec2 spawn;
    double spawnHeading = 0.0;
    Vec2 position;
    double heading = 0.0;
    double speed = 0.0;
    double steer = 0.0;
    double wheelSpin = 0.0;
};

struct Zombie {
    Vec2 spawn;
    Vec2 position;
    double heading = 0.0;
    int health = kZombieMaxHealth;
    int maxHealth = kZombieMaxHealth;
    bool alive = true;
    double respawnTimer = 0.0;
    double attackTimer = 0.0;
    double hitFlash = 0.0;
    double walkPhase = 0.0;
};

struct Game {
    Grid map{};
    Player player;
    std::vector<Vehicle> vehicles;
    std::vector<Zombie> zombies;
    Vec2 startPosition = kSpawn;
    double startYawDegrees = kSpawnYaw;
    int kills = 0;
    bool playerDead = false;
    double shotCooldown = 0.0;
    double muzzleFlash = 0.0;
    double damageFlash = 0.0;
};

struct Input {
    double forward = 0.0;   // [-1, 1], W/S or Up/Down
    double strafe = 0.0;    // [-1, 1], A/D
    double turn = 0.0;      // [-1, 1], Left/Right arrows
    bool shoot = false;
    bool enterExit = false;
};

Game createGame();
void resetGame(Game& game);
void updateGame(Game& game, const Input& input, double deltaSeconds);

int nearbyVehicle(const Game& game, double maxDistance = 2.2);
bool enterVehicle(Game& game, int vehicleIndex);
bool exitVehicle(Game& game);
int findShotTarget(const Game& game, Vec2 origin, Vec2 direction);
bool damageZombie(Zombie& zombie, int damage);

// Cameras.  horizontalFov/aspect describe the output image.
Camera firstPersonCamera(const Game& game, double horizontalFovDegrees, double aspect);
Camera chaseCamera(const Game& game, double horizontalFovDegrees, double aspect);
Camera overheadCamera(double aspect);

// Procedural textures and static triangles for the map.
struct WorldAssets {
    std::array<Texture, 6> wallTextures;
    Texture floorTexture;
    Texture ceilingTexture;
    Texture waterTexture;
    std::array<Mesh, 6> wallMeshes;
    std::array<Mesh, 6> wallTopMeshes;
    Mesh floorMesh;
    Mesh ceilingMesh;
    Mesh waterMesh;

    void build(const Grid& map);
};

struct ZombieAssets {
    Mesh torso;
    Mesh head;
    Mesh leftLeg;
    Mesh rightLeg;
    Mesh leftArm;
    Mesh rightArm;
    Mesh leftEye;
    Mesh rightEye;
    Mesh shadow;
    Mesh healthQuad;

    ZombieAssets();
};

struct VehicleAssets {
    Mesh body;
    Mesh cabin;
    Mesh roof;
    Mesh wheel;
    Mesh hub;
    Mesh headlight;
    Mesh taillight;
    Mesh shadow;

    VehicleAssets();
};

Color mapMaterialColor(std::uint8_t material);

void appendWorldGeometry(Scene& scene, const WorldAssets& assets, bool includeCeiling);
void appendVehicles(Scene& scene, const Game& game, const VehicleAssets& assets);
void appendZombies(Scene& scene, const Game& game, const ZombieAssets& assets,
                   bool showHealthBars);

}  // namespace g3d
