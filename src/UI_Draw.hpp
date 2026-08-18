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

// =============================================================================
// MOTION
// Wall-clock driven so animation runs at the same rate on the 60fps desktop
// preview and on the panel, whatever frame rate the Pi happens to hit.
// =============================================================================

extern float g_uiTime; // seconds since startup

void UiTickClock(float deltaSeconds);

// Triangle/sine oscillators in 0..1 over the given period, for cursors and idle motion.
float UiPulse(float periodSeconds);
float UiPulsePhase(float periodSeconds, float phase01);
// Hard on/off at 50% duty, for anything that should read as a blink rather than a fade.
bool  UiBlink(float periodSeconds);

// Frame-rate independent exponential approach. halfLife is the time to close
// half the remaining distance to the target.
float UiApproach(float current, float target, float halfLifeSeconds, float deltaSeconds);
float UiEaseOut(float t01);

// A value that chases a target with separate rise and fall rates. Meters use a
// fast attack and slow release so transients read clearly.
struct UiSmoothed {
    float value = 0.0f;
    float attackHalfLife = 0.02f;
    float releaseHalfLife = 0.25f;

    void Set(float target, float deltaSeconds);
    void Snap(float target) { value = target; }
};

// Utility Drawing Declarations
void DrawPixelLine(int x0, int y0, int x1, int y1, Color color);
void DrawPixelRectLines(int x, int y, int w, int h, Color color);
// =============================================================================
// FONTS
// 3x5 is fixed pitch for dense tables; 5x5 and 5x7 are proportional. Digits are
// a single shared width in every face so numeric readouts do not jitter.
// Unknown characters render as a hollow box rather than disappearing.
// =============================================================================

constexpr int FONT_TRACKING    = 1; // gap between glyphs
constexpr int FONT_SPACE_5x5   = 3; // full advance of a space
constexpr int FONT_SPACE_5x7   = 4;
constexpr int FONT_ADVANCE_3x5 = 4;

constexpr int FONT_HEIGHT_3x5 = 5;
constexpr int FONT_HEIGHT_5x5 = 5;
constexpr int FONT_HEIGHT_5x7 = 7;

void Draw5x5Char(char c, int x, int y, Color color);
void Draw5x5String(const char* str, int x, int y, Color color);
int  MeasureChar5x5(char c);
int  MeasureText5x5(const char* str);

void Draw5x7Char(char c, int x, int y, Color color);
void Draw5x7String(const char* str, int x, int y, Color color);
int  MeasureChar5x7(char c);
int  MeasureText5x7(const char* str);

void Draw3x5Char(char c, int x, int y, Color color);
void Draw3x5String(const char* str, int x, int y, Color color);
int  MeasureChar3x5(char c);
int  MeasureText3x5(const char* str);

// =============================================================================
// SHARED CHROME
// One definition each for the header rule, column dividers and the selection
// highlight, so every screen frames its content the same way.
// =============================================================================

void DrawHeaderRule();
void DrawVDivider(int x);

// Focus indicator for graphic widgets, which cannot be inverted like text.
// Drawn just outside the given bounds.
void DrawFocusFrame(int x, int y, int w, int h);

// --- TRANSIENT EDIT READOUT ---
// While a parameter is being turned, its name and value are echoed in large
// type over the middle of the screen, then dim out. The focused control's
// label and value are captured as the screen draws, so no screen code has to
// know the overlay exists; it only needs to report that an edit happened via
// UiNoteParamEdit().
//
// Captures are valid for one frame only, and the overlay refuses to draw
// unless a label and a value both arrived from the same frame. That way a
// control that reports only half a readout goes silent rather than pairing its
// label with a leftover number. The focus id ties the readout to the control
// that was actually edited, so moving to another encoder clears it at once.
void UiBeginFocusCapture();
void UiNoteParamEdit(int focusId, bool editAll = false);
void UiCaptureFocusLabel(const char* label);
void UiCaptureFocusValue(int value);
void UiCaptureFocusText(const char* label, const char* value);
void DrawEditOverlay(int focusId);

// The single selection idiom: an inverted fill sized from the text itself.
// Returns the advance width so callers can lay out the next item.
int DrawSelectableLabel(const char* text, int x, int y, bool selected, Color color);
int DrawSelectableLabel3x5(const char* text, int x, int y, bool selected, Color color);

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

// Dynamic Custom Pixel Art Icon Generators.
// Only the focused pictogram animates; the rest freeze at phase zero so the
// screen has one moving element at a time rather than four competing ones.
void DrawWavesIcon(int x, int y, int val, Color color, bool animate = false);
void DrawRainIcon(int x, int y, int val, Color color, bool animate = false);
void DrawSunIcon(int x, int y, int val, Color color, bool animate = false);
void DrawTornadoIcon(int x, int y, int val, Color color, bool animate = false);

// Screen layouts
void DrawDepthWedgeGraphic(int val, int x, int y, int w, int h, bool isSelected, Color color);
void DrawFilterCurve(int cutoff, int resonance, int type, int x, int y, int w, int h, Color color);
