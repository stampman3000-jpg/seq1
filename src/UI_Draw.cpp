#include "UI_Draw.hpp"
#include "Globals.hpp"
#include <cmath>
#include <algorithm>
// Instantiate the global CPU-side framebuffer
Color g_oledCPUPixels[256 * 64];

void CpuClearBackground(Color color) {
    for (int i = 0; i < 256 * 64; ++i) {
        g_oledCPUPixels[i] = color;
    }
}

void CpuDrawPixel(int x, int y, Color color) {
    if (x >= 0 && x < 256 && y >= 0 && y < 64) {
        g_oledCPUPixels[y * 256 + x] = color;
    }
}

void CpuDrawRectangle(int x, int y, int w, int h, Color color) {
    for (int dy = 0; dy < h; ++dy) {
        int py = y + dy;
        if (py < 0 || py >= 64) continue;
        for (int dx = 0; dx < w; ++dx) {
            int px = x + dx;
            if (px < 0 || px >= 256) continue;
            g_oledCPUPixels[py * 256 + px] = color;
        }
    }
}

void CpuDrawLine(int x0, int y0, int x1, int y1, Color color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        CpuDrawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void CpuDrawCircleLines(int cx, int cy, int r, Color color) {
    int x = r, y = 0;
    int P = 1 - r;
    while (x >= y) {
        CpuDrawPixel(cx + x, cy + y, color);
        CpuDrawPixel(cx - x, cy + y, color);
        CpuDrawPixel(cx + x, cy - y, color);
        CpuDrawPixel(cx - x, cy - y, color);
        CpuDrawPixel(cx + y, cy + x, color);
        CpuDrawPixel(cx - y, cy + x, color);
        CpuDrawPixel(cx + y, cy - x, color);
        CpuDrawPixel(cx - y, cy - x, color);
        y++;
        if (P <= 0) {
            P = P + 2 * y + 1;
        } else {
            x--;
            P = P + 2 * y - 2 * x + 1;
        }
    }
}

void CpuDrawRectangleLines(int x, int y, int w, int h, Color color) {
    for (int dx = 0; dx < w; ++dx) {
        CpuDrawPixel(x + dx, y, color);
        CpuDrawPixel(x + dx, y + h - 1, color);
    }
    for (int dy = 0; dy < h; ++dy) {
        CpuDrawPixel(x, y + dy, color);
        CpuDrawPixel(x + w - 1, y + dy, color);
    }
}
struct DeterministicRand {
    unsigned int seed;
    DeterministicRand(unsigned int s) : seed(s) {}
    int next(int minVal, int maxVal) {
        seed = seed * 1103515245 + 12345;
        unsigned int val = (seed / 65536) % 32768;
        return minVal + (int)(val % (maxVal - minVal + 1));
    }
};

// =============================================================================
// I. LOW-LEVEL SHAPE DRAWING TOOLS
// =============================================================================

void DrawPixelLine(int x0, int y0, int x1, int y1, Color color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        DrawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void DrawPixelRectLines(int x, int y, int w, int h, Color color) {
    for (int dx = 0; dx < w; ++dx) {
        DrawPixel(x + dx, y, color);
        DrawPixel(x + dx, y + h - 1, color);
    }
    for (int dy = 0; dy < h; ++dy) {
        DrawPixel(x, y + dy, color);
        DrawPixel(x + w - 1, y + dy, color);
    }
}

static void DrawPixelSquare(int cx, int cy, int r, Color color) {
    for (int dx = -r; dx <= r; ++dx) {
        DrawPixel(cx + dx, cy - r, color);
        DrawPixel(cx + dx, cy + r, color);
    }
    for (int dy = -r; dy <= r; ++dy) {
        DrawPixel(cx - r, cy + dy, color);
        DrawPixel(cx + r, cy + dy, color);
    }
}

// =============================================================================
// II. CUSTOM FONT ENGINE
// =============================================================================

void Draw5x5Char(char c, int x, int y, Color color) {
    const char* grid = nullptr;
    switch (c) {
        case '1': grid = "00100" "01100" "00100" "00100" "01110"; break;
        case '2': grid = "11111" "00001" "11111" "10000" "11111"; break;
        case '3': grid = "11111" "00001" "11111" "00001" "11111"; break;
        case '4': grid = "10001" "10001" "11111" "00001" "00001"; break;
        case '5': grid = "11111" "10000" "11111" "00001" "11111"; break;
        case '6': grid = "11111" "10000" "11111" "10001" "11111"; break;
        case '7': grid = "11111" "00001" "00010" "00100" "00100"; break;
        case '8': grid = "11111" "10001" "11111" "10001" "11111"; break;
        case '9': grid = "11111" "10001" "11111" "00001" "11111"; break;
        case '0': grid = "11111" "10001" "10001" "10001" "11111"; break;
        case 'A': grid = "01110" "10001" "11111" "10001" "10001"; break;
        case 'B': grid = "11110" "10001" "11110" "10001" "11110"; break;
        case 'C': grid = "11111" "10000" "10000" "10000" "11111"; break;
        case 'D': grid = "11110" "10001" "10001" "10001" "11110"; break;
        case 'E': grid = "11111" "10000" "11110" "10000" "11111"; break;
        case 'F': grid = "11111" "10000" "11110" "10000" "10000"; break;
        case 'G': grid = "11111" "10000" "10111" "10001" "11111"; break;
        case 'H': grid = "10001" "10001" "11111" "10001" "10001"; break;
        case 'I': grid = "11111" "00100" "00100" "00100" "11111"; break;
        case 'J': grid = "00001" "00001" "00001" "10001" "01110"; break;
        case 'K': grid = "10001" "10010" "11100" "10010" "10001"; break;
        case 'L': grid = "10000" "10000" "10000" "10000" "11111"; break;
        case 'M': grid = "10001" "11011" "10101" "10001" "10001"; break;
        case 'N': grid = "10001" "11001" "10101" "10011" "10001"; break;
        case 'O': grid = "01110" "10001" "10001" "10001" "01110"; break;
        case 'P': grid = "11110" "10001" "11110" "10000" "10000"; break;
        case 'Q': grid = "01110" "10001" "10001" "10101" "01111"; break;
        case 'R': grid = "11110" "10001" "11110" "10010" "10001"; break;
        case 'S': grid = "01111" "10000" "01110" "00001" "11110"; break;
        case 'T': grid = "11111" "00100" "00100" "00100" "00100"; break;
        case 'U': grid = "10001" "10001" "10001" "10001" "11111"; break;
        case 'V': grid = "10001" "10001" "10001" "01010" "00100"; break;
        case 'W': grid = "10001" "10001" "10101" "11011" "10001"; break;
        case 'X': grid = "10001" "01010" "00100" "01010" "10001"; break;
        case 'Y': grid = "10001" "01010" "00100" "00100" "00100"; break;
        case 'Z': grid = "11111" "00010" "00100" "01000" "11111"; break;
        case '[': grid = "11100" "10000" "10000" "10000" "11100"; break;
        case ']': grid = "00111" "00001" "00001" "00001" "00111"; break;
        case ':': grid = "00000" "00100" "00000" "00100" "00000"; break;
        case '-': grid = "00000" "00000" "11111" "00000" "00000"; break;
        case '+': grid = "00100" "00100" "11111" "00100" "00100"; break;
        default:  return;
    }
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (grid[row * 5 + col] == '1') {
                DrawPixel(x + col, y + row, color);
            }
        }
    }
}

void Draw5x5String(const char* str, int x, int y, Color color) {
    int i = 0;
    int currentX = x;
    while (str[i] != '\0') {
        if (str[i] == ' ') {
            currentX += 4;
        } else {
            Draw5x5Char(str[i], currentX, y, color);
            currentX += 6;
        }
        i++;
    }
}

void Draw3x5Char(char c, int x, int y, Color color) {
    const char* grid = nullptr;
    switch (c) {
        case '1': grid = "010" "110" "010" "010" "111"; break;
        case '2': grid = "111" "001" "111" "100" "111"; break;
        case '3': grid = "111" "001" "111" "001" "111"; break;
        case '4': grid = "101" "101" "111" "001" "001"; break;
        case '5': grid = "111" "100" "111" "001" "111"; break;
        case '6': grid = "111" "100" "111" "101" "111"; break;
        case '7': grid = "111" "001" "001" "010" "010"; break;
        case '8': grid = "111" "101" "111" "101" "111"; break;
        case '9': grid = "111" "101" "111" "001" "111"; break;
        case '0': grid = "111" "101" "101" "101" "111"; break;
        case ':': grid = "000" "010" "000" "010" "000"; break;
        case '/': grid = "001" "010" "010" "010" "100"; break;
        case '.': grid = "000" "000" "000" "000" "010"; break;
        case 'R': grid = "111" "101" "110" "101" "101"; break;
        case 'A': grid = "010" "101" "111" "101" "101"; break;
        case 'F': grid = "111" "100" "110" "100" "100"; break;
        case 'T': grid = "111" "010" "010" "010" "010"; break;
        case 'W': grid = "101" "101" "101" "111" "101"; break;
        case 'V': grid = "101" "101" "101" "010" "010"; break;
        case 'C': grid = "111" "100" "100" "100" "111"; break;
        case 'S': grid = "111" "100" "111" "001" "111"; break;
        case 'D': grid = "110" "101" "101" "101" "110"; break;
        case 'P': grid = "111" "101" "111" "100" "100"; break;
        case 'E': grid = "111" "100" "110" "100" "111"; break;
        case 'N': grid = "111" "101" "101" "101" "101"; break;
        case 'H': grid = "101" "101" "111" "101" "101"; break;
        case 'B': grid = "110" "101" "110" "101" "110"; break;
        case 'L': grid = "100" "100" "100" "100" "111"; break;
        case 'O': grid = "111" "101" "101" "101" "111"; break;
        case 'Y': grid = "101" "101" "010" "010" "010"; break;
        case 'Z': grid = "111" "001" "010" "100" "111"; break;
        case 'G': grid = "111" "100" "101" "101" "111"; break;
        case 'M': grid = "111" "111" "101" "101" "101"; break;
        case 'I': grid = "111" "010" "010" "010" "111"; break;
        case 'J': grid = "001" "001" "001" "101" "111"; break;
                case 'K': grid = "101" "110" "100" "110" "101"; break;
                case 'Q': grid = "111" "101" "111" "010" "001"; break;
                case 'U': grid = "101" "101" "101" "101" "111"; break;
        case 'X': grid = "101" "101" "010" "101" "101"; break;
                case '-': grid = "000" "000" "111" "000" "000"; break; // Mid-row horizontal line
                case '+': grid = "010" "010" "111" "010" "010"; break; // Symmetrical cross
                default:  return;
            }
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (grid[row * 3 + col] == '1') {
                DrawPixel(x + col, y + row, color);
            }
        }
    }
}

void Draw3x5String(const char* str, int x, int y, Color color) {
    int i = 0;
    int currentX = x;
    while (str[i] != '\0') {
        Draw3x5Char(str[i], currentX, y, color);
        if (str[i] == ':' || str[i] == '.') {
            currentX += 2;
        } else {
            currentX += 4;
        }
        i++;
    }
}

// =============================================================================
// III. INTERMEDIATE GRAPHICS & ANIMATED ICONS
// =============================================================================

void ParseAndDrawNote(const char* note, int vel, bool forceVelocity, int x, int y, Color color) {
    if (note[0] == '\0') return;
    char baseNote = note[0], accidental = '\0', octave = '\0';
    if (note[1] != '\0') {
        if (note[1] == '#' || note[1] == 'b' || note[1] == '-') {
            accidental = note[1];
            if (note[2] != '\0') octave = note[2];
        } else {
            octave = note[1];
        }
    }
    if (octave != '\0') {
        Draw5x5Char(baseNote, x + 1, y + 3, color);
        Draw5x5Char(octave, x + 7, y + 3, color);
    } else {
        Draw5x5Char(baseNote, x + 4, y + 3, color);
    }
    if (accidental != '\0' && !forceVelocity) {
        if (accidental == '#') {
            DrawPixel(x + 1, y + 9, color); DrawPixel(x + 3, y + 9, color);
            DrawPixel(x + 1, y + 10, color); DrawPixel(x + 2, y + 10, color); DrawPixel(x + 3, y + 10, color);
            DrawPixel(x + 1, y + 11, color); DrawPixel(x + 3, y + 11, color);
        } else if (accidental == 'b' || accidental == '-') {
            DrawPixel(x + 1, y + 9, color);
            DrawPixel(x + 1, y + 10, color); DrawPixel(x + 2, y + 10, color);
            DrawPixel(x + 1, y + 11, color); DrawPixel(x + 2, y + 11, color); DrawPixel(x + 3, y + 11, color);
        }
    }
    int fillWidth = 0;
    if (vel == 1)      fillWidth = 1;
    else if (vel == 2) fillWidth = 3;
    else if (vel == 3) fillWidth = 5;
    else               fillWidth = 0;
    if (fillWidth > 0) DrawRectangle(x + 6, y + 10, fillWidth, 2, color);
}

void DrawTrigInBox(const char* trig, int retrig, int x, int y, Color color) {
    if (trig[0] != '\0') {
        int len = 0; while (trig[len] != '\0') len++;
        if (len == 1)      Draw3x5Char(trig[0], x + 5, y + 2, color);
        else if (len == 2) Draw3x5String(trig, x + 3, y + 2, color);
        else if (len == 3) {
            Draw3x5Char(trig[0], x + 2, y + 2, color);
            Draw3x5Char(trig[1], x + 5, y + 2, color);
            Draw3x5Char(trig[2], x + 8, y + 2, color);
        }
    }
    if (retrig > 0) {
        int limit = std::min(retrig, 16);
        for (int p = 0; p < limit; ++p) {
            DrawPixel(x + retrigPattern[p].dx, y + retrigPattern[p].dy, color);
        }
    }
}

void DrawSmoothWaveform(int morph, int x, int y, int w, int h, Color color) {
    int midY = y + h / 2, prevPixelY = midY;
    for (int dx = 0; dx < w; ++dx) {
        int sineIndex = (dx * 32) / w;
        sineIndex = std::clamp(sineIndex, 0, 31);
        float sine = sineTable[sineIndex];
        
        double tri = 0.0, normPhase = ((double)dx / w);
        if (normPhase < 0.25)      tri = normPhase * 4.0;
        else if (normPhase < 0.75) tri = 2.0 - (normPhase * 4.0);
        else                       tri = (normPhase * 4.0) - 4.0;
        
        double saw = 1.0 - (normPhase * 2.0);
        double sqr = (normPhase < 0.5) ? 1.0 : -1.0;
        
        double finalAmp = 0.0;
        if (morph < 33) {
            double t = morph / 33.0; finalAmp = (1.0 - t) * sine + t * tri;
        } else if (morph < 66) {
            double t = (morph - 33) / 33.0; finalAmp = (1.0 - t) * tri + t * saw;
        } else {
            double t = (morph - 66) / 33.0; finalAmp = (1.0 - t) * saw + t * sqr;
        }
        int ampOffset = (int)(finalAmp * (h / 2 - 2));
        int pixelY = midY - ampOffset;
        if (dx > 0) DrawPixelLine(x + dx - 1, prevPixelY, x + dx, pixelY, color);
        prevPixelY = pixelY;
    }
}

void DrawSourceWaveform(int morph, int x, int y, int w, int h, bool isSelected, Color color) {
    for (int dx = 2; dx < w - 2; dx += 2) {
        DrawPixel(x + dx, y + h - 2, color);
    }
    for (int dy = 0; dy < h; dy += 3) {
        DrawPixel(x, y + dy, color);
        DrawPixel(x + w - 1, y + dy, color);
    }
    if (isSelected) {
        DrawPixel(x - 2, y + h / 2, color);
        DrawPixel(x + w + 1, y + h / 2, color);
    }
    int midY = y + h / 2, prevPixelY = midY;
    for (int dx = 2; dx < w - 2; ++dx) {
        int sineIndex = ((dx - 2) * 32) / (w - 4);
        sineIndex = std::clamp(sineIndex, 0, 31);
        float sine = sineTable[sineIndex];
        
        double tri = 0.0, normPhase = ((double)(dx - 2) / (w - 4));
        if (normPhase < 0.25)      tri = normPhase * 4.0;
        else if (normPhase < 0.75) tri = 2.0 - (normPhase * 4.0);
        else                       tri = (normPhase * 4.0) - 4.0;
        
        double saw = 1.0 - (normPhase * 2.0);
        double sqr = (normPhase < 0.5) ? 1.0 : -1.0;
        
        double finalAmp = 0.0;
        if (morph < 33) {
            double t = morph / 33.0; finalAmp = (1.0 - t) * sine + t * tri;
        } else if (morph < 66) {
            double t = (morph - 33) / 33.0; finalAmp = (1.0 - t) * tri + t * saw;
        } else {
            double t = (morph - 66) / 33.0; finalAmp = (1.0 - t) * saw + t * sqr;
        }
        int ampOffset = (int)(finalAmp * (h / 2 - 2));
        int pixelY = midY - ampOffset;
        if (dx > 2) DrawPixelLine(x + dx - 1, prevPixelY, x + dx, pixelY, color);
        prevPixelY = pixelY;
    }
}

void DrawSlider(int val, int minVal, int maxVal, int x, int y, int w, bool isSelected, bool showValue, Color color) {
    DrawPixelLine(x + 1, y + 2, x + w - 2, y + 2, color);
    DrawPixelLine(x, y, x, y + 4, color);
    DrawPixelLine(x + w - 1, y, x + w - 1, y + 4, color);
    DrawPixelLine(x + w / 2, y + 1, x + w / 2, y + 3, color);
    
    float norm = (float)(val - minVal) / (maxVal - minVal);
    int ptrX = x + 1 + (int)(norm * (w - 4));
    DrawRectangle(ptrX - 1, y + 1, 3, 3, color);
    
    if (showValue) {
        std::string valStr = std::to_string(val);
        if (val > 0 && minVal < 0) valStr = "+" + valStr;
        int valX = x + w / 2 - (int)(valStr.length() * 3);
        Draw5x5String(valStr.c_str(), valX, y - 6, color);
    }
    
    if (isSelected) {
        DrawPixel(x - 2, y + 2, color);
        DrawPixel(x + w + 1, y + 2, color);
    }
}

void DrawLevelBars(int val, int x, int y, int w, int h, bool isSelected, Color color) {
    int numBars = 4, barWidth = 2, gap = 1;
    float norm = val / 99.0f;
    int activeBars = (int)(norm * numBars + 0.5f);
    if (val > 0 && activeBars == 0) activeBars = 1;

    for (int i = 0; i < numBars; ++i) {
        int bx = x + i * (barWidth + gap);
        int bh = 3 + i * 4;
        int by = y + h - bh;
        if (i < activeBars) DrawRectangle(bx, by, barWidth, bh, color);
        else               {}
    }
    if (isSelected) {
        DrawPixel(x - 2, y + h / 2, color);
        DrawPixel(x + w + 1, y + h / 2, color);
    }
}

void DrawConcentricSquares(int val, int x, int y, int w, int h, bool isSelected, Color color) {
    float norm = val / 99.0f;
    int numLayers = 1 + (int)(norm * 3.0f); // 1 to 4 layers
    int cx = x + w / 2;
    int cy = y + h / 2 - 1;

    for (int i = 0; i < numLayers; ++i) {
        int r = 1;
        if (numLayers > 1) {
            r = 1 + (i * 5) / (numLayers - 1); // Max radius to 6 pixels
        }
        DrawPixelSquare(cx, cy, r, color);
    }
    if (isSelected) {
        DrawPixel(x - 2, y + h / 2 - 1, color);
        DrawPixel(x + w + 1, y + h / 2 - 1, color);
    }
}

void DrawPitchCurve(int depth, int time, int x, int y, int w, int h, bool isSelected, Color color) {
    DrawPixelRectLines(x, y, w, h, color);

    int baseline = y + h - 2;
    int maxUsableHeight = h - 3;
    float normPD = depth / 99.0f;
    float normST = time / 99.0f;

    int prevY = baseline - (int)(normPD * maxUsableHeight);
    float decayRate = (1.0f - normST) * 8.0f;

    for (int dx = 1; dx < w - 1; ++dx) {
        float progress = (float)(dx - 1) / (w - 3);
        float factor = exp(-progress * decayRate);
        
        if (time == 0) {
            factor = 0.0f;
        }

        int py = baseline - (int)(normPD * factor * maxUsableHeight);
        DrawPixelLine(x + dx - 1, prevY, x + dx, py, color);
        prevY = py;
    }
    if (isSelected) {
        DrawPixel(x - 2, y + h / 2, color);
        DrawPixel(x + w + 1, y + h / 2, color);
    }
}

void DrawOctagonKnob(int cx, int cy, float valNorm, bool isSelected, Color color) {
    DrawCircleLines(cx, cy, 4, color);
    if (isSelected) DrawCircleLines(cx, cy, 6, color);
    float angle = -140.0f + (valNorm * 280.0f);
    float rad = angle * DEG2RAD;
    int pointerX = cx + (int)(sin(rad) * 4.0f);
    int pointerY = cy - (int)(cos(rad) * 4.0f);
    DrawPixelLine(cx, cy, pointerX, pointerY, color);
}

void DrawAdsrEnvelope(int attack, int decay, int sustain, int release, int x, int y, int w, int h, Color color) {
    int atkWidth = (int)((attack / 99.0f) * (w * 0.25f));
    int decWidth = (int)((decay / 99.0f) * (w * 0.25f));
    int relWidth = (int)((release / 99.0f) * (w * 0.25f));
    int susHeight = (int)(((99 - sustain) / 99.0f) * (h - 2));

    Vector2 p0 = { (float)x, (float)(y + h - 1) };
    Vector2 p1 = { (float)(x + atkWidth), (float)y };
    Vector2 p2 = { (float)(x + atkWidth + decWidth), (float)(y + susHeight) };
    Vector2 p3 = { (float)(x + w - 1 - relWidth), (float)(y + susHeight) };
    Vector2 p4 = { (float)(x + w - 1), (float)(y + h - 1) };

    DrawPixelLine((int)p0.x, (int)p0.y, (int)p1.x, (int)p1.y, color);
    DrawPixelLine((int)p1.x, (int)p1.y, (int)p2.x, (int)p2.y, color);
    DrawPixelLine((int)p2.x, (int)p2.y, (int)p3.x, (int)p3.y, color);
    DrawPixelLine((int)p3.x, (int)p3.y, (int)p4.x, (int)p4.y, color);

    auto DrawDashedVLine = [](int lx, int lyMin, int lyMax, Color c) {
        for (int ly = lyMin; ly <= lyMax; ly += 2) DrawPixel(lx, ly, c);
    };
    if (atkWidth > 0) DrawDashedVLine((int)p1.x, y, y + h - 1, color);
    if (decWidth > 0) DrawDashedVLine((int)p2.x, y, y + h - 1, color);
    if (relWidth > 0) DrawDashedVLine((int)p3.x, y, y + h - 1, color);
}

void DrawAhdEnvelope(int attack, int hold, int decay, int x, int y, int w, int h, Color color) {
    int atkWidth = (int)((attack / 99.0f) * (w * 0.33f));
    int holdWidth = (int)((hold / 99.0f) * (w * 0.33f));
    int decWidth = (int)((decay / 99.0f) * (w * 0.33f));

    Vector2 p0 = { (float)x, (float)(y + h - 1) };
    Vector2 p1 = { (float)(x + atkWidth), (float)y };
    Vector2 p2 = { (float)(x + atkWidth + holdWidth), (float)y };
    Vector2 p3 = { (float)(x + atkWidth + holdWidth + decWidth), (float)(y + h - 1) };

    DrawPixelLine((int)p0.x, (int)p0.y, (int)p1.x, (int)p1.y, color);
    DrawPixelLine((int)p1.x, (int)p1.y, (int)p2.x, (int)p2.y, color);
    DrawPixelLine((int)p2.x, (int)p2.y, (int)p3.x, (int)p3.y, color);

    if (x + w - 1 > (int)p3.x) {
        DrawPixelLine((int)p3.x, (int)p3.y, x + w - 1, y + h - 1, color);
    }

    auto DrawDashedVLine = [](int lx, int lyMin, int lyMax, Color c) {
        for (int ly = lyMin; ly <= lyMax; ly += 2) DrawPixel(lx, ly, c);
    };
    if (atkWidth > 0) DrawDashedVLine((int)p1.x, y, y + h - 1, color);
    if (holdWidth > 0) DrawDashedVLine((int)p2.x, y, y + h - 1, color);
}

void DrawGranularRings(int size, int dens, int scat, int cx, int cy, Color color) {
    if (size == 0) {
        DrawPixel(cx, cy, color);
        return;
    }

    float normSize = size / 99.0f;
    float normDens = dens / 99.0f;
    float normScat = scat / 99.0f;

    float maxRy = normSize * 6.0f;
    float maxRx = normSize * 18.0f;

    if (maxRy < 1.0f) maxRy = 1.0f;
    if (maxRx < 2.0f) maxRx = 2.0f;

    int baseRings = 1 + (int)(normSize * 2.0f);
    int extraRings = (int)(normDens * 5.0f);
    int totalRings = baseRings + extraRings;

    for (int ring = 0; ring < totalRings; ++ring) {
        float t = (totalRings > 1) ? (float)ring / (totalRings - 1) : 1.0f;
        float ry = 1.0f + t * (maxRy - 1.0f);
        float rx = 2.0f + t * (maxRx - 2.0f);

        float angleStep = 15.0f - (normDens * 10.0f);
        if (angleStep < 5.0f) angleStep = 5.0f;

        int pixelIndex = 0;
        for (float angle = 0; angle < 360.0f; angle += angleStep) {
            DeterministicRand drand((unsigned int)(angle + ring * 23 + size * 37 + dens * 7));

            if ((drand.next(0, 99)) > (normDens * 80 + 20)) {
                if (dens > 5 || drand.next(0, 9) != 0) {
                    continue;
                }
            }

            float rad = angle * (3.14159265f / 180.0f);
            int px = cx + (int)(cos(rad) * rx);
            int py = cy + (int)(sin(rad) * ry);

            if (scat > 0) {
                float scatterMultiplier = (pixelIndex % 2 == 0) ? 1.5f : 0.5f;
                int maxDispX = (int)(normScat * 12.0f * scatterMultiplier);
                int maxDispY = (int)(normScat * 3.5f * scatterMultiplier);
                if (maxDispX > 0) px += drand.next(-maxDispX, maxDispX);
                if (maxDispY > 0) py += drand.next(-maxDispY, maxDispY);
            }

            DrawPixel(px, py, color);
            pixelIndex++;
        }
    }
}

void DrawCleanText(const char* text, int x, int y, Color color) {
    Vector2 position = { (float)x, (float)y };
    DrawTextEx(GetFontDefault(), text, position, 10.0f, 1.0f, color);
}

// Waves Icon (Reverb Send): Exact jagged coils matching your hand-drawn layout
void DrawWavesIcon(int x, int y, int val, Color color) {
    struct Pixel { int dx, dy; };
    const std::vector<Pixel> wavePoints = {
        // Helix Loop 1 (Jagged stairs)
        {2, 9}, {1, 9}, {0, 8}, {0, 7}, {1, 6}, {2, 5}, {3, 4}, {3, 3}, {2, 2}, {3, 1}, {4, 1}, {5, 2}, {5, 3}, {4, 4}, {3, 5}, {2, 6}, {2, 7}, {3, 8}, {4, 8}, {5, 8},
        // Helix Loop 2 (Jagged stairs)
        {7, 9}, {6, 9}, {5, 8}, {5, 7}, {6, 6}, {7, 5}, {8, 4}, {8, 3}, {7, 2}, {8, 1}, {9, 1}, {10, 2}, {10, 3}, {9, 4}, {8, 5}, {7, 6}, {7, 7}, {8, 8}, {9, 8}, {10, 8},
        // Helix Loop 3 (Jagged stairs)
        {12, 9}, {11, 9}, {10, 8}, {10, 7}, {11, 6}, {12, 5}, {13, 4}, {13, 3}, {12, 2}, {13, 1}, {14, 1}, {15, 2}, {15, 3}, {14, 4}, {13, 5}, {12, 6}, {12, 7}, {13, 8}, {14, 8}, {15, 8}
    };

    float scale = val / 99.0f;
    const int baselineY = 9;

    for (const auto& p : wavePoints) {
        // Dynamic scaling collapsing to horizontal line at y=9
        int drawY = y + baselineY - (int)((baselineY - p.dy) * scale);
        DrawPixel(x + p.dx, drawY, color);
    }

    // Dynamic splashing water droplets above the three jagged crests
    if (val > 40) {
        int sprayScale = (val - 40);
        DeterministicRand drand((unsigned int)(val * 31));
        int numDots = 1 + (int)(sprayScale / 12);
        for (int d = 0; d < numDots; ++d) {
            int peakIndex = drand.next(0, 2);
            int peakX = (peakIndex == 0) ? 3 : ((peakIndex == 1) ? 8 : 13);
            int ox = peakX + drand.next(-2, 2);
            int oy = 1 - drand.next(1, 3);
            
            if (ox >= 0 && ox <= 18 && oy >= 0) {
                DrawPixel(x + ox, y + oy, color);
            }
        }
    }
}

// Rain Icon (Delay Send): Deterministic rain pixels that multiply continuously around canopy
void DrawRainIcon(int x, int y, int val, Color color) {
    // Exact outline of your cute umbrella canopy & J-hook handle
    struct Pixel { int dx, dy; };
    const std::vector<Pixel> umbrella = {
        {10, 1},
        {8, 2}, {9, 2}, {10, 2}, {11, 2}, {12, 2},
        {6, 3}, {7, 3}, {13, 3}, {14, 3},
        {4, 4}, {5, 4}, {15, 4}, {16, 4},
        {3, 5}, {17, 5},
        {10, 3}, {10, 4}, {10, 5}, {10, 6}, {10, 7}, {10, 8},
        {9, 9}, {8, 9}, {7, 8}
    };

    for (const auto& p : umbrella) {
        DrawPixel(x + p.dx, y + p.dy, color);
    }

    if (val > 0) {
        // Continuous, sequential raindrop pixels (8 vertical channels)
        const Pixel rainPath[] = {
            // Column 1
            {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6}, {1, 7}, {1, 8}, {1, 9}, {1, 10},
            // Column 19
            {19, 1}, {19, 2}, {19, 3}, {19, 4}, {19, 5}, {19, 6}, {19, 7}, {19, 8}, {19, 9}, {19, 10},
            // Column 5
            {5, 1}, {5, 2}, {5, 3}, {5, 4}, {5, 5}, {5, 6}, {5, 7}, {5, 8}, {5, 9}, {5, 10},
            // Column 15
            {15, 1}, {15, 2}, {15, 3}, {15, 4}, {15, 5}, {15, 6}, {15, 7}, {15, 8}, {15, 9}, {15, 10},
            // Column 3
            {3, 1}, {3, 2}, {3, 3}, {3, 4}, {3, 5}, {3, 6}, {3, 7}, {3, 8}, {3, 9}, {3, 10},
            // Column 17
            {17, 1}, {17, 2}, {17, 3}, {17, 4}, {17, 5}, {17, 6}, {17, 7}, {17, 8}, {17, 9}, {17, 10},
            // Column 8
            {8, 1}, {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 6}, {8, 7}, {8, 8}, {8, 9}, {8, 10},
            // Column 12
            {12, 1}, {12, 2}, {12, 3}, {12, 4}, {12, 5}, {12, 6}, {12, 7}, {12, 8}, {12, 9}, {12, 10}
        };

        int drawn = 0;
        int targetPixels = (int)(val * (80.0f / 99.0f));

        for (int i = 0; i < 80 && drawn < targetPixels; ++i) {
            const auto& p = rainPath[i];
            
            // Mask and protect the canopy structure and J-hook handle from being overwritten
            bool overlapsCanopy = (p.dx >= 3 && p.dx <= 17 && p.dy >= 2 && p.dy <= 5);
            bool overlapsHandle = (p.dx == 10 && p.dy >= 3 && p.dy <= 8) || (p.dx >= 7 && p.dx <= 9 && p.dy == 9);
            
            if (overlapsCanopy || overlapsHandle) {
                continue;
            }
            
            DrawPixel(x + p.dx, y + p.dy, color);
            drawn++;
        }
    }
}

// Sunshine Icon (Saturation Send): Core centered at (cx, cy) to stay strictly in the box
void DrawSunIcon(int x, int y, int val, Color color) {
    int cx = x + 8;
    int cy = y + 5; // Lifted centerY coordinate prevents box leak on bottom

    // Central circular solar core
    DrawPixel(cx - 1, cy - 2, color); DrawPixel(cx, cy - 2, color); DrawPixel(cx + 1, cy - 2, color);
    DrawPixel(cx - 2, cy - 1, color);                               DrawPixel(cx + 2, cy - 1, color);
    DrawPixel(cx - 2, cy,     color);                               DrawPixel(cx + 2, cy,     color);
    DrawPixel(cx - 2, cy + 1, color);                               DrawPixel(cx + 2, cy + 1, color);
    DrawPixel(cx - 1, cy + 2, color); DrawPixel(cx, cy + 2, color); DrawPixel(cx + 1, cy + 2, color);

    if (val >= 20) {
        // Step 1: Cardinal Inner Rays
        DrawPixel(cx, cy - 4, color);  // North
        DrawPixel(cx, cy + 4, color);  // South
        DrawPixel(cx - 4, cy, color);  // West
        DrawPixel(cx + 4, cy, color);  // East
    }
    if (val >= 40) {
        // Step 2: Cardinal Outer Rays
        DrawPixel(cx, cy - 5, color);
        DrawPixel(cx, cy + 5, color);
        DrawPixel(cx - 5, cy, color);
        DrawPixel(cx + 5, cy, color);
    }
    if (val >= 60) {
        // Step 3: Diagonal Inner Rays
        DrawPixel(cx - 3, cy - 3, color); // NW
        DrawPixel(cx + 3, cy - 3, color); // NE
        DrawPixel(cx - 3, cy + 3, color); // SW
        DrawPixel(cx + 3, cy + 3, color); // SE
    }
    if (val >= 80) {
        // Step 4: Diagonal Outer Rays
        DrawPixel(cx - 4, cy - 4, color);
        DrawPixel(cx + 4, cy - 4, color);
        DrawPixel(cx - 4, cy + 4, color);
        DrawPixel(cx + 4, cy + 4, color);
    }
}

// Tornado Icon (Chorus Send): Procedural horizontal squished ellipses
void DrawTornadoIcon(int x, int y, int val, Color color) {
    float scale = val / 99.0f;
    int centerX = x + 8;
    int centerY = y + 5;

    int widths[] = { 7, 5, 3, 2, 1 };
    int rows[]   = { -4, -2, 0, 2, 4 };

    if (val == 0) {
        DrawPixelLine(centerX, centerY - 4, centerX, centerY + 5, color);
    } else {
        // Draw squished circular ellipses tapering down
        for (int i = 0; i < 5; ++i) {
            int rx = (int)(widths[i] * scale);
            int ry = std::clamp((int)(rx / 3), 1, 2);
            
            for (int dx = -rx; dx <= rx; ++dx) {
                if (dx == -rx || dx == rx) {
                    DrawPixel(centerX + dx, centerY + rows[i], color);
                } else {
                    DrawPixel(centerX + dx, centerY + rows[i] - ry, color);
                    DrawPixel(centerX + dx, centerY + rows[i] + ry, color);
                }
            }
        }
        
        // Draw tail curving out
        struct Pixel { int dx, dy; };
        const std::vector<Pixel> tail = {
            {8, 5}, {9, 6}, {10, 7}
        };

        for (const auto& p : tail) {
            int targetX = centerX + (int)((p.dx - 8) * scale);
            DrawPixel(targetX, centerY + p.dy, color);
        }
    }
}

// Reusable Depth Wedge Drawing Routine (Populates columns left-to-right, bottom-to-top)
void DrawDepthWedgeGraphic(int val, int x, int y, int w, int h, bool isSelected, Color color) {
    if (isSelected) {
        DrawPixelRectLines(x - 1, y - 1, w + 2, h + 2, color);
    }
    
    // Generate coordinate pairs below the diagonal slope
    std::vector<std::pair<int, int>> pixels;
    for (int col = 0; col < w; ++col) {
        // Height of this column ranges linearly from 1 up to the total height h
        int colH = 1 + (col * (h - 1)) / (w - 1);
        for (int row = 0; row < colH; ++row) {
            pixels.push_back({col, (h - 1) - row});
        }
    }
    
    // Draw a subset of the pixels corresponding to the current value
    int total = (int)pixels.size();
    int limit = (int)(val * (total / 99.0f));
    
    for (int i = 0; i < limit; ++i) {
        DrawPixel(x + pixels[i].first, y + pixels[i].second, color);
    }
}

// Custom Filter Response curve drawing routine
void DrawFilterCurve(int cutoff, int resonance, int type, int x, int y, int w, int h, Color color) {
    DrawPixelRectLines(x, y, w, h, color);

    int baseline = y + h - 2;
    float normCut = cutoff / 99.0f;
    float normRes = resonance / 99.0f;

    int prevY = baseline;
    int cutoffX = 2 + (int)(normCut * (w - 4));

    for (int dx = 1; dx < w - 1; ++dx) {
        int py = baseline;
        if (type == 0) {
            if (dx < cutoffX - 4) {
                py = y + 3;
            } else if (dx < cutoffX) {
                float t = (dx - (cutoffX - 4)) / 4.0f;
                int peak = (int)(normRes * 6.0f);
                py = y + 3 - (int)(t * peak);
            } else {
                float t = std::min((dx - cutoffX) / 8.0f, 1.0f);
                int peak = (int)(normRes * 6.0f);
                py = (y + 3 - peak) + (int)(t * (baseline - (y + 3 - peak)));
            }
        } else if (type == 1) {
            if (dx > cutoffX + 4) {
                py = y + 3;
            } else if (dx > cutoffX) {
                float t = ((cutoffX + 4) - dx) / 4.0f;
                int peak = (int)(normRes * 6.0f);
                py = y + 3 - (int)(t * peak);
            } else {
                float t = std::min((cutoffX - dx) / 8.0f, 1.0f);
                int peak = (int)(normRes * 6.0f);
                py = (y + 3 - peak) + (int)(t * (baseline - (y + 3 - peak)));
            }
        } else {
            int dist = abs(dx - cutoffX);
            if (dist < 4) {
                float t = (4 - dist) / 4.0f;
                py = y + 3 - (int)(t * normRes * 6.0f);
            } else {
                float t = std::min((dist - 4) / 8.0f, 1.0f);
                py = (y + 6) + (int)(t * (baseline - (y + 6)));
            }
        }

        py = std::clamp(py, y + 1, baseline);
        if (dx > 1) {
            DrawPixelLine(x + dx - 1, prevY, x + dx, py, color);
        }
        prevY = py;
    }
}
