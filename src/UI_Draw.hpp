#pragma once
#include "Common.hpp"

// High-speed CPU-side pixel buffer
extern Color g_oledCPUPixels[256 * 64];

// Fast CPU-side drawing primitives
void CpuClearBackground(Color color);
void CpuDrawPixel(int x, int y, Color color);
void CpuDrawRectangle(int x, int y, int w, int h, Color color);
void CpuDrawLine(int x0, int y0, int x1, int y1, Color color);
void CpuDrawCircleLines(int cx, int cy, int r, Color color);
void CpuDrawRectangleLines(int x, int y, int w, int h, Color color);

// Seamlessly redirect standard Raylib GPU calls to our CPU Framebuffer
#define DrawPixel CpuDrawPixel
#define DrawRectangle CpuDrawRectangle
#define DrawLine CpuDrawLine
#define DrawCircleLines CpuDrawCircleLines
#define DrawRectangleLines CpuDrawRectangleLines
#define ClearBackground CpuClearBackground

// Utility Drawing Declarations
void DrawPixelLine(int x0, int y0, int x1, int y1, Color color);
void DrawPixelRectLines(int x, int y, int w, int h, Color color);
void Draw5x5Char(char c, int x, int y, Color color);
void Draw5x5String(const char* str, int x, int y, Color color);
void Draw3x5Char(char c, int x, int y, Color color);
void Draw3x5String(const char* str, int x, int y, Color color);

// Component Visualization Declarations
void ParseAndDrawNote(const char* note, int vel, bool forceVelocity, int x, int y, Color color);
void DrawTrigInBox(const char* trig, int retrig, int x, int y, Color color);
void DrawSmoothWaveform(int morph, int x, int y, int w, int h, Color color);
void DrawSourceWaveform(int morph, int x, int y, int w, int h, bool isSelected, Color color);
void DrawSlider(int val, int minVal, int maxVal, int x, int y, int w, bool isSelected, bool showValue, Color color);
void DrawLevelBars(int val, int x, int y, int w, int h, bool isSelected, Color color);
void DrawConcentricSquares(int val, int x, int y, int w, int h, bool isSelected, Color color);
void DrawPitchCurve(int depth, int time, int x, int y, int w, int h, bool isSelected, Color color);
void DrawOctagonKnob(int cx, int cy, float valNorm, bool isSelected, Color color);
void DrawAdsrEnvelope(int attack, int decay, int sustain, int release, int x, int y, int w, int h, Color color);
void DrawAhdEnvelope(int attack, int hold, int decay, int x, int y, int w, int h, Color color);
void DrawGranularRings(int size, int dens, int scat, int cx, int cy, Color color);
void DrawFilterResponse(int cutoff, int resonance, int type, int x, int y, int w, int h, Color color);
void DrawCleanText(const char* text, int x, int y, Color color);

// Dynamic Custom Pixel Art Icon Generators
void DrawWavesIcon(int x, int y, int val, Color color);
void DrawRainIcon(int x, int y, int val, Color color);
void DrawSunIcon(int x, int y, int val, Color color);
void DrawTornadoIcon(int x, int y, int val, Color color);

// Screen layouts
void DrawDepthWedgeGraphic(int val, int x, int y, int w, int h, bool isSelected, Color color);
void DrawFilterCurve(int cutoff, int resonance, int type, int x, int y, int w, int h, Color color);
