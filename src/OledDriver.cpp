#include "OledDriver.hpp"
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring> // Required for std::memset

#if defined(__linux__)
// --- RASPBERRY PI / LINUX HEADERS ---
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

// Global handles (only compiled on Linux)
static int g_spiFd = -1;

// --- GPIO PIN CONFIGURATION (Includes modern 512 kernel offset) ---
static const int PIN_DC  = 512 + 25;  // GPIO 25 (Pin 22) -> 537
static const int PIN_RES = 512 + 24;  // GPIO 24 (Pin 18) -> 536

// --- DIRECT SYSFS GPIO HELPERS WITH ERROR CHECKING ---
static void gpioExport(int pin) {
    std::ofstream f("/sys/class/gpio/export");
    if (!f.is_open()) {
        std::cerr << "[OLED] GPIO Error: Failed to open /sys/class/gpio/export" << std::endl;
        return;
    }
    f << pin;
}

static void gpioUnexport(int pin) {
    std::ofstream f("/sys/class/gpio/unexport");
    if (!f.is_open()) return;
    f << pin;
}

static void gpioSetDir(int pin, const std::string& dir) {
    std::string path = "/sys/class/gpio/gpio" + std::to_string(pin) + "/direction";
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[OLED] GPIO Error: Failed to open " << path << std::endl;
        return;
    }
    f << dir;
}

static void gpioWrite(int pin, int val) {
    std::string path = "/sys/class/gpio/gpio" + std::to_string(pin) + "/value";
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[OLED] GPIO Error: Failed to write to " << path << std::endl;
        return;
    }
    f << val;
}

// --- STANDARD LINUX IOCTL SPI TRANSMISSION (Enforces CS and Speed) ---
static void spiWrite(const uint8_t* data, size_t len) {
    if (g_spiFd < 0 || len == 0) return;

    struct spi_ioc_transfer tr;
    std::memset(&tr, 0, sizeof(tr));
    
    tr.tx_buf = (unsigned long)data;
    tr.rx_buf = 0;
    tr.len = len;
    tr.speed_hz = 2000000;      // Explicitly enforce 2 MHz SPI Speed
    tr.bits_per_word = 8;       // Enforce 8-bit words
    tr.delay_usecs = 0;

    // Execute synchronous, hardware-bounded SPI transfer (toggles CS automaticamente)
    if (ioctl(g_spiFd, SPI_IOC_MESSAGE(1), &tr) < 0) {
        std::cerr << "[OLED] SPI Error: SPI_IOC_MESSAGE transaction failed" << std::endl;
    }
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

    // 1. Export GPIO Pins for DC and RST (using 512 offset)
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
    uint8_t mode = SPI_MODE_0; // Mode 0 (matches luma.oled)
    uint8_t bits = 8;
    uint32_t speed = 2000000; // 2 MHz for clean, noise-free signals

    if (ioctl(g_spiFd, SPI_IOC_WR_MODE, &mode) < 0) std::cerr << "[OLED] SPI mode failed" << std::endl;
    if (ioctl(g_spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) std::cerr << "[OLED] SPI bits failed" << std::endl;
    if (ioctl(g_spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) std::cerr << "[OLED] SPI speed failed" << std::endl;

    // 4. Hard Reset the OLED Display via RST pin
    gpioWrite(PIN_RES, 0); // RST Low
    usleep(150000);   // 150ms delay
    gpioWrite(PIN_RES, 1); // RST High
    usleep(150000);   // 150ms delay

    // 5. Send SSD1322 Init Commands (Aligned 100% with luma.oled Python library)
    uint8_t dataVal;
    
    dataVal = 0x12;
    writeCommandWithData(0xFD, &dataVal, 1); // Command Lock: Unlock
    
    writeCommand(0xAE); // Display OFF
    
    dataVal = 0x91;
    writeCommandWithData(0xB3, &dataVal, 1); // Set Front Clock Divider / Oscillator Frequency
    
    dataVal = 0x3F;
    writeCommandWithData(0xCA, &dataVal, 1); // Set MUX Ratio
    
    dataVal = 0x00;
    writeCommandWithData(0xA2, &dataVal, 1); // Set Display Offset
    
    dataVal = 0x00;
    writeCommandWithData(0xA1, &dataVal, 1); // Set Display Start Line
    
    uint8_t remapData[] = {0x14, 0x11};
    writeCommandWithData(0xA0, remapData, 2); // Set Re-map and Dual COM Line
    
    dataVal = 0x00;
    writeCommandWithData(0xB5, &dataVal, 1); // Set GPIO
    
    dataVal = 0x01;
    writeCommandWithData(0xAB, &dataVal, 1); // Function Selection (Internal VDD)
    
    dataVal = 0x32;
    writeCommandWithData(0xB1, &dataVal, 1); // Pre-charge / Phase Length
    
    uint8_t enhanceData[] = {0xA0, 0xB5, 0x55};
    writeCommandWithData(0xB4, enhanceData, 3); // Display Enhancement A (SVS)
    
    dataVal = 0x05;
    writeCommandWithData(0xBE, &dataVal, 1); // Set VCOMH Voltage
    
    dataVal = 0x0F;
    writeCommandWithData(0xC7, &dataVal, 1); // Contrast Master
    
    dataVal = 0x01;
    writeCommandWithData(0xB6, &dataVal, 1); // Precharge2 / Second Pre-charge Period
    
    writeCommand(0xA6); // Normal Display Mode

    // Set Bipolar Contrast Current across Segments A, B, and C (3 bytes)
    uint8_t contrastData[] = {0x7F, 0x7F, 0x7F};
    writeCommandWithData(0xC1, contrastData, 3);
    
    // Clear screen initially by writing zero bytes (Using 128 offset range: 0x20 to 0x5F)
    std::vector<uint8_t> clearBuf(8192, 0);
    uint8_t colData[] = {0x20, 0x5F};
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

    // Set Column Address (0x20 to 0x5F matches Python's 128 offset!)
    uint8_t colData[] = {0x20, 0x5F};
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
