#pragma once
#include "raylib.h"

// Initializes the physical SPI and GPIO pins on the Pi.
void InitOled();

// Grabs Raylib's FBO pixels, packs them, and pushes them to the physical screen.
void UpdateOled(RenderTexture2D oledScreen);

// Closes SPI file descriptors and cleans up on exit.
void ShutdownOled();
