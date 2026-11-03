// Screen-space overlays: health, minimap, crosshair and the driving HUD.
#pragma once

#include "renderer.hpp"
#include "world.hpp"

namespace g3d {

void drawHealthBar(Image& image, int x, int y, int width, int height,
                   double fraction);
void drawPlayerHud(Image& image, const Game& game);
void drawCrosshair(Image& image, bool firing);
void drawMinimap(Image& image, const Game& game);

}  // namespace g3d
