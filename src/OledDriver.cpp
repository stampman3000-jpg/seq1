#include "OledDriver.hpp"
#include <iostream>

#if defined(__linux__)
// --- RASPBERRY PI ONLY HEADERS ---
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
// Note: We can add GPIO control headers here later!
#endif

// Global handles (only compiled on Linux)
#if defined(__linux__)
static int g_spiFd = -1;
#endif

void InitOled() {
#if defined(__linux__)
    std::cout << "[OLED] Initializing SPI on Raspberry Pi..." << std::endl;
    // Real Pi SPI initialization code will go here soon!
#else
    // Mac does nothing
    std::cout << "[OLED] Running on macOS: Physical OLED simulation active (No-op)." << std::endl;
#endif
}

void UpdateOled(RenderTexture2D oledScreen) {
#if defined(__linux__)
    // Real Pi pixel extraction and SPI transfer code will go here soon!
#endif
}

void ShutdownOled() {
#if defined(__linux__)
    if (g_spiFd >= 0) {
        close(g_spiFd);
        g_spiFd = -1;
        std::cout << "[OLED] SPI interface closed cleanly." << std::endl;
    }
#endif
}
