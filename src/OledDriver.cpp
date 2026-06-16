#include "OledDriver.hpp"
#include <iostream>
#include <vector>

#if defined(__linux__)
// --- RASPBERRY PI / LINUX HEADERS ---
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

// Global handles (only compiled on Linux)
static int g_spiFd = -1;

// --- GPIO PIN CONFIGURATION (Matches your wiring) ---
static const int PIN_DC  = 25;  // GPIO 25 (Pin 22)
static const int PIN_RES = 24;  // GPIO 24 (Pin 18)

// --- DIRECT SYSFS GPIO HELPERS ---
static void gpioExport(int pin) {
    std::ofstream f("/sys/class/gpio/export");
    if (f.is_open()) f << pin;
}

static void gpioUnexport(int pin) {
    std::ofstream f("/sys/class/gpio/unexport");
    if (f.is_open()) f << pin;
}

static void gpioSetDir(int pin, const std::string& dir) {
    std::ofstream f("/sys/class/gpio/gpio" + std::to_string(pin) + "/direction");
    if (f.is_open()) f << dir;
}

static void gpioWrite(int pin, int val) {
    std::ofstream f("/sys/class/gpio/gpio" + std::to_string(pin) + "/value");
    if (f.is_open()) f << val;
}

// --- SPI TRANSMISSION HELPERS ---
static void spiWrite(const uint8_t* data, size_t len) {
    if (g_spiFd < 0) return;
    write(g_spiFd, data, len);
}

static void writeCommand(uint8_t cmd) {
    gpioWrite(PIN_DC, 0); // DC Low = Command
    spiWrite(&cmd, 1);
}

static void writeCommandWithData(uint8_t cmd, const uint8_t* data, size_t len) {
    writeCommand(cmd);
    if (len > 0) {
        gpioWrite(PIN_DC, 1); // DC High = Data
        spiWrite(data, len);
    }
}
#endif

void InitOled() {
#if defined(__linux__)
    std::cout << "[OLED] Initializing SPI on Raspberry Pi..." << std::endl;

    // 1. Export GPIO Pins for DC and RST
    gpioExport(PIN_DC);
    gpioExport(PIN_RES);
    usleep(50000); // 50ms safety delay

    gpioSetDir(PIN_DC, "out");
    gpioSetDir(PIN_RES, "out");

    // 2. Open SPI Device (SPI0.0)
    g_spiFd = open("/dev/spidev0.0", O_RDWR);
    if (g_spiFd < 0) {
        std::cerr << "[OLED] Error: Could not open SPI device /dev/spidev0.0" << std::endl;
        return;
    }

    // 3. Configure SPI speed, word bits, and mode
    uint8_t mode = SPI_MODE_3; // Mode 3 or Mode 0 is standard for SSD1322
    uint8_t bits = 8;
    uint32_t speed = 10000000; // 10 MHz SPI Clock

    if (ioctl(g_spiFd, SPI_IOC_WR_MODE, &mode) < 0) std::cerr << "[OLED] SPI mode failed" << std::endl;
    if (ioctl(g_spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) std::cerr << "[OLED] SPI bits failed" << std::endl;
    if (ioctl(g_spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) std::cerr << "[OLED] SPI speed failed" << std::endl;

    // 4. Hard Reset the OLED Display via RST pin
    gpioWrite(PIN_RES, 0); // RST Low
    usleep(150000);   // 150ms delay
    gpioWrite(PIN_RES, 1); // RST High
    usleep(150000);   // 150ms delay

    // 5. Send SSD1322 Init Commands (C++ Standard compliant)
    uint8_t dataVal;
    
    dataVal = 0x12;
    writeCommandWithData(0xFD, &dataVal, 1); // Command Lock: Unlock
    
    writeCommand(0xAE); // Display OFF
    
    dataVal = 0x91;
    writeCommandWithData(0xB3, &dataVal, 1); // Display Clock
    
    dataVal = 0x3F;
    writeCommandWithData(0xCA, &dataVal, 1); // MUX Ratio (64)
    
    dataVal = 0x00;
    writeCommandWithData(0xA2, &dataVal, 1); // Display Offset (0)
    
    dataVal = 0x00;
    writeCommandWithData(0xA1, &dataVal, 1); // Start Line (0)
    
    uint8_t remapData[] = {0x14, 0x11};
    writeCommandWithData(0xA0, remapData, 2); // Set Re-map
    
    dataVal = 0x01;
    writeCommandWithData(0xAB, &dataVal, 1); // Function Selection (Internal VDD)
    
    uint8_t enhanceData[] = {0xA0, 0xFD};
    writeCommandWithData(0xB4, enhanceData, 2); // Display Enhancement A
    
    dataVal = 0x7F;
    writeCommandWithData(0xC1, &dataVal, 1); // Contrast Current
    
    dataVal = 0x0F;
    writeCommandWithData(0xC7, &dataVal, 1); // Master Contrast (Max)
    
    dataVal = 0xE2;
    writeCommandWithData(0xB1, &dataVal, 1); // Phase Length
    
    dataVal = 0x1F;
    writeCommandWithData(0xBB, &dataVal, 1); // Pre-charge Voltage
    
    dataVal = 0x07;
    writeCommandWithData(0xBE, &dataVal, 1); // VCOMH Voltage
    
    writeCommand(0xA6); // Normal Display mode
    
    // Clear screen initially by writing zero bytes
    std::vector<uint8_t> clearBuf(8192, 0);
    uint8_t colData[] = {0x1C, 0x5B};
    writeCommandWithData(0x15, colData, 2);
    uint8_t rowData[] = {0x00, 0x3F};
    writeCommandWithData(0x75, rowData, 2);
    writeCommand(0x5C);
    gpioWrite(PIN_DC, 1); // DC High
    spiWrite(clearBuf.data(), clearBuf.size());

    writeCommand(0xAF); // Display ON!
    std::cout << "[OLED] Display ON! SSD1322 Initialized successfully." << std::endl;
#else
    // Mac does nothing
    std::cout << "[OLED] Running on macOS: Physical OLED simulation active (No-op)." << std::endl;
#endif
}

void UpdateOled(RenderTexture2D oledScreen) {
#if defined(__linux__)
    if (g_spiFd < 0) return;

    // Set Column Address (0x1C to 0x5B is the centered 256 pixels of SSD1322 RAM)
    uint8_t colData[] = {0x1C, 0x5B};
    writeCommandWithData(0x15, colData, 2);

    // Set Row Address (0x00 to 0x3F)
    uint8_t rowData[] = {0x00, 0x3F};
    writeCommandWithData(0x75, rowData, 2);

    // Start writing data to GDDRAM
    writeCommand(0x5C);

    // Extract screen colors from Raylib
    Image img = LoadImageFromTexture(oledScreen.texture);
    Color* pixels = LoadImageColors(img);

    // SSD1322 expects 4-bits per pixel (2 pixels per byte)
    // Buffer size: 256 * 64 / 2 = 8,192 bytes
    static std::vector<uint8_t> oledBuffer(8192, 0);

    // Because texture coordinates are vertically inverted in OpenGL,
    // we read rows from bottom to top to draw right-side-up!
    int outIndex = 0;
    for (int y = 63; y >= 0; --y) {
        for (int x = 0; x < 256; x += 2) {
            Color p1 = pixels[y * 256 + x];
            Color p2 = pixels[y * 256 + x + 1];

            // Convert to 4-bit grayscale (0 to 15)
            // If any channel is active, we turn the pixel fully white
            uint8_t gray1 = (p1.r > 127 || p1.g > 127 || p1.b > 127) ? 15 : 0;
            uint8_t gray2 = (p2.r > 127 || p2.g > 127 || p2.b > 127) ? 15 : 0;

            oledBuffer[outIndex++] = (gray1 << 4) | (gray2 & 0x0F);
        }
    }

    UnloadImageColors(pixels);
    UnloadImage(img);

    // Send the entire packed frame buffer via SPI
    gpioWrite(PIN_DC, 1); // DC High = Data
    spiWrite(oledBuffer.data(), oledBuffer.size());
#endif
}

void ShutdownOled() {
#if defined(__linux__)
    if (g_spiFd >= 0) {
        close(g_spiFd);
        g_spiFd = -1;
        std::cout << "[OLED] SPI interface closed cleanly." << std::endl;
    }
    // Clean up GPIO sysfs exports
    gpioUnexport(PIN_DC);
    gpioUnexport(PIN_RES);
#endif
}
