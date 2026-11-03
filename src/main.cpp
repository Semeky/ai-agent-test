#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hud.hpp"
#include "renderer.hpp"
#include "selftest.hpp"
#include "world.hpp"

#if defined(__unix__) || defined(__APPLE__)
#define GAME3D_POSIX 1
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace g3d {
namespace {

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
    int driveVehicle = -1;
    std::string renderPath;
};

struct RenderAssets {
    WorldAssets world;
    ZombieAssets zombies;
    VehicleAssets vehicles;

    void build(const Grid& map) { world.build(map); }
};

struct FrameOptions {
    bool mapView = false;
    bool minimap = false;
    bool firing = false;
    double fovDegrees = kDefaultFov;
};

void printUsage(std::FILE* stream) {
    std::fprintf(
        stream,
        "Museum 3D - a software 3D engine with drivable vehicles (C++17)\n"
        "\n"
        "Usage:\n"
        "  game3d                      interactive demo (needs a terminal)\n"
        "  game3d --render frame.ppm   render one PPM frame\n"
        "  game3d --selftest           run built-in checks\n"
        "\n"
        "Options:\n"
        "  --render <file.ppm>  write a binary PPM frame and exit\n"
        "  --map                overhead 3D map view\n"
        "  --minimap            start with the minimap enabled\n"
        "  --drive <n>          start inside vehicle n (0-based)\n"
        "  --width <px>         render width (default 960)\n"
        "  --height <px>        render height (default 540)\n"
        "  --fov <deg>          horizontal field of view (default 75)\n"
        "  --pos <x>,<y>        start position (default 6.5,14.5)\n"
        "  --angle <deg>        start angle, 0=east 90=south (default 330)\n"
        "  --fps <n>            interactive frame cap (default 30)\n"
        "  -h, --help           show this help\n"
        "\n"
        "Interactive controls:\n"
        "  W/S or Up/Down  move / accelerate / brake\n"
        "  A/D             strafe on foot / steer a vehicle\n"
        "  Left/Right      turn on foot / steer a vehicle\n"
        "  E               enter or exit a vehicle\n"
        "  Space           fire on foot\n"
        "  Tab             overhead 3D map view\n"
        "  M               minimap\n"
        "  R               restart\n"
        "  Q/Esc           quit\n");
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
        } else if (argument == "--drive") {
            const char* value = nextValue("--drive");
            if (value == nullptr) {
                return false;
            }
            if (!parseInt(value, options.driveVehicle)) {
                error = "invalid vehicle index: " + std::string(value);
                return false;
            }
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
    if (options.driveVehicle < -1) {
        error = "--drive must be a non-negative vehicle index";
        return false;
    }
    return true;
}

void renderGameFrame(Image& image, const Game& game, const RenderAssets& assets,
                     const FrameOptions& options) {
    Scene scene;
    const double aspect =
        static_cast<double>(image.width()) / static_cast<double>(image.height());
    if (options.mapView) {
        scene.camera = overheadCamera(aspect);
    } else if (game.player.inVehicle) {
        scene.camera = chaseCamera(game, options.fovDegrees, aspect);
    } else {
        scene.camera = firstPersonCamera(game, options.fovDegrees, aspect);
    }
    appendWorldGeometry(scene, assets.world, !options.mapView);
    appendVehicles(scene, game, assets.vehicles);
    appendZombies(scene, game, assets.zombies, !options.mapView);
    renderScene(image, scene);

    if (options.mapView) {
        return;
    }
    if (game.damageFlash > 0.0) {
        image.fillRect(0, 0, image.width(), image.height(), makeColor(150, 16, 16),
                       std::min(0.28, game.damageFlash * 0.4));
    }
    if (game.playerDead) {
        image.fillRect(0, 0, image.width(), image.height(), makeColor(48, 0, 0), 0.35);
    }
    drawPlayerHud(image, game);
    if (options.minimap) {
        drawMinimap(image, game);
    }
    if (!game.player.inVehicle) {
        drawCrosshair(image, options.firing);
    }
}

bool startInVehicle(Game& game, int index) {
    if (index < 0 || index >= static_cast<int>(game.vehicles.size())) {
        return false;
    }
    game.player.position = game.vehicles[static_cast<std::size_t>(index)].position;
    return enterVehicle(game, index);
}

#if defined(GAME3D_POSIX)

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

int runInteractive(const Options& options, const Grid& map, const RenderAssets& assets) {
    (void)map;
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

    Game game = createGame();
    game.startPosition = options.position;
    game.startYawDegrees = options.yawDegrees;
    resetGame(game);
    if (options.driveVehicle >= 0 && !startInVehicle(game, options.driveVehicle)) {
        terminal.restore();
        std::fprintf(stderr, "error: vehicle %d cannot be entered\n", options.driveVehicle);
        return 1;
    }

    bool mapView = options.mapView;
    bool minimap = options.minimap;
    bool running = true;

    double forwardIntent = 0.0;
    double strafeIntent = 0.0;
    double turnIntent = 0.0;
    bool shootRequested = false;
    bool enterRequested = false;
    double displayedFps = 0.0;
    auto previousFrame = std::chrono::steady_clock::now();

    const auto restart = [&]() {
        resetGame(game);
        if (options.driveVehicle >= 0) {
            startInVehicle(game, options.driveVehicle);
        }
        forwardIntent = 0.0;
        strafeIntent = 0.0;
        turnIntent = 0.0;
        shootRequested = false;
        enterRequested = false;
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
                case 'e':
                case 'E':
                    enterRequested = true;
                    break;
                case ' ':
                    shootRequested = true;
                    break;
                case '\t':
                    mapView = !mapView;
                    break;
                case 'm':
                case 'M':
                    minimap = !minimap;
                    break;
                case 'r':
                case 'R':
                    restart();
                    break;
                case 'q':
                case 'Q':
                case 27:
                case 3:
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

        const double safeDelta = std::max(deltaSeconds, 1e-6);
        const double forwardStep = clampd(forwardIntent, -deltaSeconds, deltaSeconds);
        forwardIntent -= forwardStep;
        const double strafeStep = clampd(strafeIntent, -deltaSeconds, deltaSeconds);
        strafeIntent -= strafeStep;
        const double turnStep = clampd(turnIntent, -deltaSeconds, deltaSeconds);
        turnIntent -= turnStep;

        Input input;
        input.forward = clampd(forwardStep / safeDelta, -1.0, 1.0);
        input.strafe = clampd(strafeStep / safeDelta, -1.0, 1.0);
        input.turn = clampd(turnStep / safeDelta, -1.0, 1.0);
        input.shoot = shootRequested;
        input.enterExit = enterRequested;
        shootRequested = false;
        enterRequested = false;

        updateGame(game, input, deltaSeconds);

        const auto [columns, rows] = terminalSize();
        const int width = std::clamp(columns, 20, 400);
        const int sceneRows = std::clamp(rows - 1, 4, 200);
        const int height = sceneRows * 2;
        Image image(width, height);
        FrameOptions frameOptions;
        frameOptions.mapView = mapView;
        frameOptions.minimap = minimap;
        frameOptions.firing = game.muzzleFlash > 0.0;
        frameOptions.fovDegrees = options.fovDegrees;
        renderGameFrame(image, game, assets, frameOptions);

        std::string output = renderTerminalFrame(image);
        char status[512];
        if (game.playerDead) {
            std::snprintf(status, sizeof(status),
                          "  museum3d | YOU DIED - press R to restart | kills=%d | Q quit",
                          game.kills);
        } else if (mapView) {
            std::snprintf(status, sizeof(status),
                          "  museum3d | overhead 3D map | Tab 3D view | M minimap | "
                          "R restart | Q quit");
        } else if (game.player.inVehicle) {
            const Vehicle& vehicle =
                game.vehicles[static_cast<std::size_t>(game.player.vehicleIndex)];
            std::snprintf(status, sizeof(status),
                          "  museum3d | DRIVING | HP %d | speed %4.1f | kills %d | "
                          "fps=%.0f | W/S throttle, A/D steer, E exit, Tab map, Q quit",
                          game.player.health, vehicle.speed, game.kills, displayedFps);
        } else {
            std::snprintf(status, sizeof(status),
                          "  museum3d | HP %d | kills %d | fps=%.0f | WASD move, E enter "
                          "vehicle, Space shoot, Tab map, M minimap, R restart, Q quit",
                          game.player.health, game.kills, displayedFps);
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
    std::printf("3D demo closed (x=%.1f, y=%.1f, yaw=%.0f).\n", game.player.position.x,
                game.player.position.y, normalizeDegrees(game.player.yawDegrees));
    return 0;
}

#else  // !GAME3D_POSIX

int runInteractive(const Options&, const Grid&, const RenderAssets&) {
    std::fprintf(stderr,
                 "Interactive mode is only available on POSIX terminals.\n"
                 "Use --render <file.ppm> or --selftest instead.\n");
    return 1;
}

#endif  // GAME3D_POSIX

}  // namespace
}  // namespace g3d

int main(int argc, char** argv) {
    using namespace g3d;
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

    Game game = createGame();
    game.startPosition = options.position;
    game.startYawDegrees = options.yawDegrees;
    resetGame(game);

    if (options.driveVehicle >= static_cast<int>(game.vehicles.size())) {
        std::fprintf(stderr, "error: vehicle %d does not exist (have %d)\n",
                     options.driveVehicle, static_cast<int>(game.vehicles.size()));
        return 1;
    }
    if (options.driveVehicle >= 0 && !startInVehicle(game, options.driveVehicle)) {
        std::fprintf(stderr, "error: vehicle %d cannot be entered\n", options.driveVehicle);
        return 1;
    }
    if (options.driveVehicle < 0 &&
        !isWalkableCentered(map, options.position, kPlayerRadius)) {
        std::fprintf(stderr, "error: start position %.2f,%.2f is not walkable\n",
                     options.position.x, options.position.y);
        return 1;
    }

    RenderAssets assets;
    assets.build(map);

    if (!options.renderPath.empty()) {
        Image image(options.width, options.height);
        FrameOptions frameOptions;
        frameOptions.mapView = options.mapView;
        frameOptions.minimap = options.minimap;
        frameOptions.firing = false;
        frameOptions.fovDegrees = options.fovDegrees;
        renderGameFrame(image, game, assets, frameOptions);
        if (!writePPM(options.renderPath, image)) {
            std::fprintf(stderr, "error: could not write %s\n",
                         options.renderPath.c_str());
            return 1;
        }
        std::printf("Rendered %dx%d frame to %s\n", options.width, options.height,
                    options.renderPath.c_str());
        return 0;
    }

    return runInteractive(options, map, assets);
}
