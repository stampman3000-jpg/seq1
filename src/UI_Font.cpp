// =============================================================================
// BITMAP FONT ENGINE
//
// Three faces share one glyph representation: a width plus a run of '0'/'1'
// rows. Unknown characters fall back to a hollow box rather than silently
// vanishing, so a missing glyph is visible during development instead of
// leaving a hole in the middle of a string.
//
//   3x5  fixed pitch, for dense data rows where columns must line up
//   5x5  proportional, the workhorse for labels and values
//   5x7  proportional, for headline values that need emphasis
//
// Digits are deliberately given a single shared width in every face so that a
// changing numeric readout does not jitter horizontally.
// =============================================================================

#include "UI_Draw.hpp"

namespace {

struct Glyph {
    int width;
    const char* rows; // width * height characters, row major
};

const Glyph kMissing5x5 = { 4, "1111" "1001" "1001" "1001" "1111" };
const Glyph kMissing3x5 = { 3, "111" "101" "101" "101" "111" };
const Glyph kMissing5x7 = { 5, "11111" "10001" "10001" "10001" "10001" "10001" "11111" };

void BlitGlyph(const Glyph& g, int height, int x, int y, Color color) {
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < g.width; ++col) {
            if (g.rows[row * g.width + col] == '1') {
                DrawPixel(x + col, y + row, color);
            }
        }
    }
}

// --- 5x5 proportional -------------------------------------------------------

const Glyph* Lookup5x5(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);

    switch (c) {
        case '!': { static const Glyph g = { 1, "1" "1" "1" "0" "1" }; return &g; }
        case '"': { static const Glyph g = { 3, "101" "101" "000" "000" "000" }; return &g; }
        case '#': { static const Glyph g = { 5, "01010" "11111" "01010" "11111" "01010" }; return &g; }
        case '$': { static const Glyph g = { 5, "01111" "11100" "01110" "00111" "11110" }; return &g; }
        case '%': { static const Glyph g = { 5, "11001" "11010" "00100" "01011" "10011" }; return &g; }
        case '&': { static const Glyph g = { 5, "01100" "10010" "01100" "10011" "01101" }; return &g; }
        case '\'': { static const Glyph g = { 1, "1" "1" "0" "0" "0" }; return &g; }
        case '(': { static const Glyph g = { 2, "01" "10" "10" "10" "01" }; return &g; }
        case ')': { static const Glyph g = { 2, "10" "01" "01" "01" "10" }; return &g; }
        case '*': { static const Glyph g = { 5, "00100" "10101" "01110" "10101" "00100" }; return &g; }
        case '+': { static const Glyph g = { 3, "000" "010" "111" "010" "000" }; return &g; }
        case ',': { static const Glyph g = { 2, "00" "00" "00" "01" "10" }; return &g; }
        case '-': { static const Glyph g = { 3, "000" "000" "111" "000" "000" }; return &g; }
        case '.': { static const Glyph g = { 1, "0" "0" "0" "0" "1" }; return &g; }
        case '/': { static const Glyph g = { 3, "001" "001" "010" "100" "100" }; return &g; }

        // Tabular figures: every digit is 4 wide so values do not shift.
        case '0': { static const Glyph g = { 4, "0110" "1011" "1101" "1001" "0110" }; return &g; }
        case '1': { static const Glyph g = { 4, "0010" "0110" "0010" "0010" "0111" }; return &g; }
        case '2': { static const Glyph g = { 4, "1110" "0001" "0110" "1000" "1111" }; return &g; }
        case '3': { static const Glyph g = { 4, "1110" "0001" "0110" "0001" "1110" }; return &g; }
        case '4': { static const Glyph g = { 4, "1001" "1001" "1111" "0001" "0001" }; return &g; }
        case '5': { static const Glyph g = { 4, "1111" "1000" "1110" "0001" "1110" }; return &g; }
        case '6': { static const Glyph g = { 4, "0110" "1000" "1110" "1001" "0110" }; return &g; }
        case '7': { static const Glyph g = { 4, "1111" "0001" "0010" "0100" "0100" }; return &g; }
        case '8': { static const Glyph g = { 4, "0110" "1001" "0110" "1001" "0110" }; return &g; }
        case '9': { static const Glyph g = { 4, "0110" "1001" "0111" "0001" "0110" }; return &g; }

        case ':': { static const Glyph g = { 1, "0" "1" "0" "1" "0" }; return &g; }
        case ';': { static const Glyph g = { 2, "00" "01" "00" "01" "10" }; return &g; }
        case '<': { static const Glyph g = { 3, "001" "010" "100" "010" "001" }; return &g; }
        case '=': { static const Glyph g = { 3, "000" "111" "000" "111" "000" }; return &g; }
        case '>': { static const Glyph g = { 3, "100" "010" "001" "010" "100" }; return &g; }
        case '?': { static const Glyph g = { 4, "1110" "0001" "0110" "0000" "0100" }; return &g; }
        case '@': { static const Glyph g = { 5, "01110" "10011" "10101" "10110" "01110" }; return &g; }

        case 'A': { static const Glyph g = { 4, "0110" "1001" "1111" "1001" "1001" }; return &g; }
        case 'B': { static const Glyph g = { 4, "1110" "1001" "1110" "1001" "1110" }; return &g; }
        case 'C': { static const Glyph g = { 4, "0111" "1000" "1000" "1000" "0111" }; return &g; }
        case 'D': { static const Glyph g = { 4, "1110" "1001" "1001" "1001" "1110" }; return &g; }
        case 'E': { static const Glyph g = { 4, "1111" "1000" "1110" "1000" "1111" }; return &g; }
        case 'F': { static const Glyph g = { 4, "1111" "1000" "1110" "1000" "1000" }; return &g; }
        case 'G': { static const Glyph g = { 4, "0111" "1000" "1011" "1001" "0111" }; return &g; }
        case 'H': { static const Glyph g = { 4, "1001" "1001" "1111" "1001" "1001" }; return &g; }
        case 'I': { static const Glyph g = { 3, "111" "010" "010" "010" "111" }; return &g; }
        case 'J': { static const Glyph g = { 4, "0011" "0001" "0001" "1001" "0110" }; return &g; }
        case 'K': { static const Glyph g = { 4, "1001" "1010" "1100" "1010" "1001" }; return &g; }
        case 'L': { static const Glyph g = { 4, "1000" "1000" "1000" "1000" "1111" }; return &g; }
        case 'M': { static const Glyph g = { 5, "10001" "11011" "10101" "10001" "10001" }; return &g; }
        case 'N': { static const Glyph g = { 5, "10001" "11001" "10101" "10011" "10001" }; return &g; }
        case 'O': { static const Glyph g = { 4, "0110" "1001" "1001" "1001" "0110" }; return &g; }
        case 'P': { static const Glyph g = { 4, "1110" "1001" "1110" "1000" "1000" }; return &g; }
        case 'Q': { static const Glyph g = { 4, "0110" "1001" "1001" "1011" "0111" }; return &g; }
        case 'R': { static const Glyph g = { 4, "1110" "1001" "1110" "1010" "1001" }; return &g; }
        case 'S': { static const Glyph g = { 4, "0111" "1000" "0110" "0001" "1110" }; return &g; }
        case 'T': { static const Glyph g = { 5, "11111" "00100" "00100" "00100" "00100" }; return &g; }
        case 'U': { static const Glyph g = { 4, "1001" "1001" "1001" "1001" "0110" }; return &g; }
        case 'V': { static const Glyph g = { 5, "10001" "10001" "10001" "01010" "00100" }; return &g; }
        case 'W': { static const Glyph g = { 5, "10001" "10001" "10101" "11011" "10001" }; return &g; }
        case 'X': { static const Glyph g = { 5, "10001" "01010" "00100" "01010" "10001" }; return &g; }
        case 'Y': { static const Glyph g = { 5, "10001" "01010" "00100" "00100" "00100" }; return &g; }
        case 'Z': { static const Glyph g = { 4, "1111" "0001" "0110" "1000" "1111" }; return &g; }

        case '[': { static const Glyph g = { 2, "11" "10" "10" "10" "11" }; return &g; }
        case '\\': { static const Glyph g = { 3, "100" "100" "010" "001" "001" }; return &g; }
        case ']': { static const Glyph g = { 2, "11" "01" "01" "01" "11" }; return &g; }
        case '^': { static const Glyph g = { 3, "010" "101" "000" "000" "000" }; return &g; }
        case '_': { static const Glyph g = { 4, "0000" "0000" "0000" "0000" "1111" }; return &g; }
        case '`': { static const Glyph g = { 2, "10" "01" "00" "00" "00" }; return &g; }
        case '{': { static const Glyph g = { 3, "011" "010" "110" "010" "011" }; return &g; }
        case '|': { static const Glyph g = { 1, "1" "1" "1" "1" "1" }; return &g; }
        case '}': { static const Glyph g = { 3, "110" "010" "011" "010" "110" }; return &g; }
        case '~': { static const Glyph g = { 5, "00000" "01001" "10110" "00000" "00000" }; return &g; }

        default: return nullptr;
    }
}

// --- 5x7 proportional -------------------------------------------------------

const Glyph* Lookup5x7(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);

    switch (c) {
        case '0': { static const Glyph g = { 5, "01110" "10001" "10011" "10101" "11001" "10001" "01110" }; return &g; }
        case '1': { static const Glyph g = { 5, "00100" "01100" "00100" "00100" "00100" "00100" "01110" }; return &g; }
        case '2': { static const Glyph g = { 5, "01110" "10001" "00001" "00010" "00100" "01000" "11111" }; return &g; }
        case '3': { static const Glyph g = { 5, "11111" "00010" "00100" "00010" "00001" "10001" "01110" }; return &g; }
        case '4': { static const Glyph g = { 5, "00010" "00110" "01010" "10010" "11111" "00010" "00010" }; return &g; }
        case '5': { static const Glyph g = { 5, "11111" "10000" "11110" "00001" "00001" "10001" "01110" }; return &g; }
        case '6': { static const Glyph g = { 5, "00110" "01000" "10000" "11110" "10001" "10001" "01110" }; return &g; }
        case '7': { static const Glyph g = { 5, "11111" "00001" "00010" "00100" "01000" "01000" "01000" }; return &g; }
        case '8': { static const Glyph g = { 5, "01110" "10001" "10001" "01110" "10001" "10001" "01110" }; return &g; }
        case '9': { static const Glyph g = { 5, "01110" "10001" "10001" "01111" "00001" "00010" "01100" }; return &g; }

        case 'A': { static const Glyph g = { 5, "01110" "10001" "10001" "11111" "10001" "10001" "10001" }; return &g; }
        case 'B': { static const Glyph g = { 5, "11110" "10001" "10001" "11110" "10001" "10001" "11110" }; return &g; }
        case 'C': { static const Glyph g = { 5, "01110" "10001" "10000" "10000" "10000" "10001" "01110" }; return &g; }
        case 'D': { static const Glyph g = { 5, "11110" "10001" "10001" "10001" "10001" "10001" "11110" }; return &g; }
        case 'E': { static const Glyph g = { 5, "11111" "10000" "10000" "11110" "10000" "10000" "11111" }; return &g; }
        case 'F': { static const Glyph g = { 5, "11111" "10000" "10000" "11110" "10000" "10000" "10000" }; return &g; }
        case 'G': { static const Glyph g = { 5, "01110" "10001" "10000" "10111" "10001" "10001" "01111" }; return &g; }
        case 'H': { static const Glyph g = { 5, "10001" "10001" "10001" "11111" "10001" "10001" "10001" }; return &g; }
        case 'I': { static const Glyph g = { 3, "111" "010" "010" "010" "010" "010" "111" }; return &g; }
        case 'J': { static const Glyph g = { 5, "00111" "00010" "00010" "00010" "00010" "10010" "01100" }; return &g; }
        case 'K': { static const Glyph g = { 5, "10001" "10010" "10100" "11000" "10100" "10010" "10001" }; return &g; }
        case 'L': { static const Glyph g = { 5, "10000" "10000" "10000" "10000" "10000" "10000" "11111" }; return &g; }
        case 'M': { static const Glyph g = { 5, "10001" "11011" "10101" "10101" "10001" "10001" "10001" }; return &g; }
        case 'N': { static const Glyph g = { 5, "10001" "11001" "11001" "10101" "10011" "10011" "10001" }; return &g; }
        case 'O': { static const Glyph g = { 5, "01110" "10001" "10001" "10001" "10001" "10001" "01110" }; return &g; }
        case 'P': { static const Glyph g = { 5, "11110" "10001" "10001" "11110" "10000" "10000" "10000" }; return &g; }
        case 'Q': { static const Glyph g = { 5, "01110" "10001" "10001" "10001" "10101" "10010" "01101" }; return &g; }
        case 'R': { static const Glyph g = { 5, "11110" "10001" "10001" "11110" "10100" "10010" "10001" }; return &g; }
        case 'S': { static const Glyph g = { 5, "01111" "10000" "10000" "01110" "00001" "00001" "11110" }; return &g; }
        case 'T': { static const Glyph g = { 5, "11111" "00100" "00100" "00100" "00100" "00100" "00100" }; return &g; }
        case 'U': { static const Glyph g = { 5, "10001" "10001" "10001" "10001" "10001" "10001" "01110" }; return &g; }
        case 'V': { static const Glyph g = { 5, "10001" "10001" "10001" "10001" "10001" "01010" "00100" }; return &g; }
        case 'W': { static const Glyph g = { 5, "10001" "10001" "10001" "10101" "10101" "11011" "10001" }; return &g; }
        case 'X': { static const Glyph g = { 5, "10001" "10001" "01010" "00100" "01010" "10001" "10001" }; return &g; }
        case 'Y': { static const Glyph g = { 5, "10001" "10001" "01010" "00100" "00100" "00100" "00100" }; return &g; }
        case 'Z': { static const Glyph g = { 5, "11111" "00001" "00010" "00100" "01000" "10000" "11111" }; return &g; }

        case '-': { static const Glyph g = { 3, "000" "000" "000" "111" "000" "000" "000" }; return &g; }
        case '+': { static const Glyph g = { 5, "00000" "00100" "00100" "11111" "00100" "00100" "00000" }; return &g; }
        case '.': { static const Glyph g = { 1, "0" "0" "0" "0" "0" "0" "1" }; return &g; }
        case ':': { static const Glyph g = { 1, "0" "0" "1" "0" "0" "1" "0" }; return &g; }
        case '/': { static const Glyph g = { 3, "001" "001" "010" "010" "010" "100" "100" }; return &g; }
        case '%': { static const Glyph g = { 5, "11001" "11010" "00010" "00100" "01000" "01011" "10011" }; return &g; }
        case '#': { static const Glyph g = { 5, "01010" "01010" "11111" "01010" "11111" "01010" "01010" }; return &g; }

        default: return nullptr;
    }
}

// --- 3x5 fixed pitch --------------------------------------------------------

const Glyph* Lookup3x5(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);

    switch (c) {
        case '0': { static const Glyph g = { 3, "111" "101" "101" "101" "111" }; return &g; }
        case '1': { static const Glyph g = { 3, "010" "110" "010" "010" "111" }; return &g; }
        case '2': { static const Glyph g = { 3, "111" "001" "111" "100" "111" }; return &g; }
        case '3': { static const Glyph g = { 3, "111" "001" "111" "001" "111" }; return &g; }
        case '4': { static const Glyph g = { 3, "101" "101" "111" "001" "001" }; return &g; }
        case '5': { static const Glyph g = { 3, "111" "100" "111" "001" "111" }; return &g; }
        case '6': { static const Glyph g = { 3, "111" "100" "111" "101" "111" }; return &g; }
        case '7': { static const Glyph g = { 3, "111" "001" "001" "010" "010" }; return &g; }
        case '8': { static const Glyph g = { 3, "111" "101" "111" "101" "111" }; return &g; }
        case '9': { static const Glyph g = { 3, "111" "101" "111" "001" "111" }; return &g; }

        case 'A': { static const Glyph g = { 3, "010" "101" "111" "101" "101" }; return &g; }
        case 'B': { static const Glyph g = { 3, "110" "101" "110" "101" "110" }; return &g; }
        case 'C': { static const Glyph g = { 3, "111" "100" "100" "100" "111" }; return &g; }
        case 'D': { static const Glyph g = { 3, "110" "101" "101" "101" "110" }; return &g; }
        case 'E': { static const Glyph g = { 3, "111" "100" "110" "100" "111" }; return &g; }
        case 'F': { static const Glyph g = { 3, "111" "100" "110" "100" "100" }; return &g; }
        case 'G': { static const Glyph g = { 3, "111" "100" "101" "101" "111" }; return &g; }
        case 'H': { static const Glyph g = { 3, "101" "101" "111" "101" "101" }; return &g; }
        case 'I': { static const Glyph g = { 3, "111" "010" "010" "010" "111" }; return &g; }
        case 'J': { static const Glyph g = { 3, "001" "001" "001" "101" "111" }; return &g; }
        case 'K': { static const Glyph g = { 3, "101" "110" "100" "110" "101" }; return &g; }
        case 'L': { static const Glyph g = { 3, "100" "100" "100" "100" "111" }; return &g; }
        case 'M': { static const Glyph g = { 3, "111" "111" "101" "101" "101" }; return &g; }
        case 'N': { static const Glyph g = { 3, "111" "101" "101" "101" "101" }; return &g; }
        case 'O': { static const Glyph g = { 3, "111" "101" "101" "101" "111" }; return &g; }
        case 'P': { static const Glyph g = { 3, "111" "101" "111" "100" "100" }; return &g; }
        case 'Q': { static const Glyph g = { 3, "111" "101" "111" "010" "001" }; return &g; }
        case 'R': { static const Glyph g = { 3, "111" "101" "110" "101" "101" }; return &g; }
        case 'S': { static const Glyph g = { 3, "111" "100" "111" "001" "111" }; return &g; }
        case 'T': { static const Glyph g = { 3, "111" "010" "010" "010" "010" }; return &g; }
        case 'U': { static const Glyph g = { 3, "101" "101" "101" "101" "111" }; return &g; }
        case 'V': { static const Glyph g = { 3, "101" "101" "101" "010" "010" }; return &g; }
        case 'W': { static const Glyph g = { 3, "101" "101" "101" "111" "101" }; return &g; }
        case 'X': { static const Glyph g = { 3, "101" "101" "010" "101" "101" }; return &g; }
        case 'Y': { static const Glyph g = { 3, "101" "101" "010" "010" "010" }; return &g; }
        case 'Z': { static const Glyph g = { 3, "111" "001" "010" "100" "111" }; return &g; }

        case '!': { static const Glyph g = { 3, "010" "010" "010" "000" "010" }; return &g; }
        case '"': { static const Glyph g = { 3, "101" "101" "000" "000" "000" }; return &g; }
        case '#': { static const Glyph g = { 3, "101" "111" "101" "111" "101" }; return &g; }
        case '$': { static const Glyph g = { 3, "011" "110" "010" "011" "110" }; return &g; }
        case '%': { static const Glyph g = { 3, "101" "001" "010" "100" "101" }; return &g; }
        case '&': { static const Glyph g = { 3, "010" "101" "010" "101" "011" }; return &g; }
        case '\'': { static const Glyph g = { 3, "010" "010" "000" "000" "000" }; return &g; }
        case '(': { static const Glyph g = { 3, "001" "010" "010" "010" "001" }; return &g; }
        case ')': { static const Glyph g = { 3, "100" "010" "010" "010" "100" }; return &g; }
        case '*': { static const Glyph g = { 3, "101" "010" "101" "000" "000" }; return &g; }
        case '+': { static const Glyph g = { 3, "010" "010" "111" "010" "010" }; return &g; }
        case ',': { static const Glyph g = { 3, "000" "000" "000" "010" "100" }; return &g; }
        case '-': { static const Glyph g = { 3, "000" "000" "111" "000" "000" }; return &g; }
        case '.': { static const Glyph g = { 3, "000" "000" "000" "000" "010" }; return &g; }
        case '/': { static const Glyph g = { 3, "001" "010" "010" "010" "100" }; return &g; }
        case ':': { static const Glyph g = { 3, "000" "010" "000" "010" "000" }; return &g; }
        case ';': { static const Glyph g = { 3, "000" "010" "000" "010" "100" }; return &g; }
        case '<': { static const Glyph g = { 3, "001" "010" "100" "010" "001" }; return &g; }
        case '=': { static const Glyph g = { 3, "000" "111" "000" "111" "000" }; return &g; }
        case '>': { static const Glyph g = { 3, "100" "010" "001" "010" "100" }; return &g; }
        case '?': { static const Glyph g = { 3, "110" "001" "010" "000" "010" }; return &g; }
        case '@': { static const Glyph g = { 3, "111" "101" "111" "100" "111" }; return &g; }
        case '[': { static const Glyph g = { 3, "011" "010" "010" "010" "011" }; return &g; }
        case '\\': { static const Glyph g = { 3, "100" "010" "010" "010" "001" }; return &g; }
        case ']': { static const Glyph g = { 3, "110" "010" "010" "010" "110" }; return &g; }
        case '^': { static const Glyph g = { 3, "010" "101" "000" "000" "000" }; return &g; }
        case '_': { static const Glyph g = { 3, "000" "000" "000" "000" "111" }; return &g; }
        case '`': { static const Glyph g = { 3, "100" "010" "000" "000" "000" }; return &g; }
        case '{': { static const Glyph g = { 3, "011" "010" "110" "010" "011" }; return &g; }
        case '|': { static const Glyph g = { 3, "010" "010" "010" "010" "010" }; return &g; }
        case '}': { static const Glyph g = { 3, "110" "010" "011" "010" "110" }; return &g; }
        case '~': { static const Glyph g = { 3, "000" "011" "110" "000" "000" }; return &g; }

        default: return nullptr;
    }
}

} // namespace

// =============================================================================
// 5x5 PROPORTIONAL
// =============================================================================

int MeasureChar5x5(char c) {
    if (c == ' ') return FONT_SPACE_5x5;
    const Glyph* g = Lookup5x5(c);
    return (g ? g->width : kMissing5x5.width) + FONT_TRACKING;
}

int MeasureText5x5(const char* str) {
    int w = 0;
    for (int i = 0; str[i] != '\0'; ++i) w += MeasureChar5x5(str[i]);
    return (w > 0) ? w - FONT_TRACKING : 0; // no trailing gap
}

void Draw5x5Char(char c, int x, int y, Color color) {
    if (c == ' ') return;
    const Glyph* g = Lookup5x5(c);
    BlitGlyph(g ? *g : kMissing5x5, 5, x, y, color);
}

void Draw5x5String(const char* str, int x, int y, Color color) {
    int cursor = x;
    for (int i = 0; str[i] != '\0'; ++i) {
        Draw5x5Char(str[i], cursor, y, color);
        cursor += MeasureChar5x5(str[i]);
    }
}

// =============================================================================
// 5x7 PROPORTIONAL (emphasis)
// =============================================================================

int MeasureChar5x7(char c) {
    if (c == ' ') return FONT_SPACE_5x7;
    const Glyph* g = Lookup5x7(c);
    return (g ? g->width : kMissing5x7.width) + FONT_TRACKING;
}

int MeasureText5x7(const char* str) {
    int w = 0;
    for (int i = 0; str[i] != '\0'; ++i) w += MeasureChar5x7(str[i]);
    return (w > 0) ? w - FONT_TRACKING : 0;
}

void Draw5x7Char(char c, int x, int y, Color color) {
    if (c == ' ') return;
    const Glyph* g = Lookup5x7(c);
    BlitGlyph(g ? *g : kMissing5x7, 7, x, y, color);
}

void Draw5x7String(const char* str, int x, int y, Color color) {
    int cursor = x;
    for (int i = 0; str[i] != '\0'; ++i) {
        Draw5x7Char(str[i], cursor, y, color);
        cursor += MeasureChar5x7(str[i]);
    }
}

// =============================================================================
// 3x5 FIXED PITCH
// Kept monospaced: these rows are dense tables where columns must align.
// =============================================================================

int MeasureChar3x5(char c) {
    // Punctuation this narrow reads better tucked in, and existing layouts
    // depend on the tighter advance.
    if (c == ':' || c == '.') return 2;
    return FONT_ADVANCE_3x5;
}

int MeasureText3x5(const char* str) {
    int w = 0;
    for (int i = 0; str[i] != '\0'; ++i) w += MeasureChar3x5(str[i]);
    return w;
}

void Draw3x5Char(char c, int x, int y, Color color) {
    if (c == ' ') return;
    const Glyph* g = Lookup3x5(c);
    BlitGlyph(g ? *g : kMissing3x5, 5, x, y, color);
}

void Draw3x5String(const char* str, int x, int y, Color color) {
    int cursor = x;
    for (int i = 0; str[i] != '\0'; ++i) {
        Draw3x5Char(str[i], cursor, y, color);
        cursor += MeasureChar3x5(str[i]);
    }
}
