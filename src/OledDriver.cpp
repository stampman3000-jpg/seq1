#include "OledDriver.hpp"
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <fstream>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

// --- UDP SOCKET HEADERS ---
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern Color g_oledCPUPixels[256 * 64];

// Global handles
static int g_spiFd = -1;
static std::ofstream g_dcFile;
static std::ofstream g_resFile;

// Global UDP sockets
static int g_udpFd = -1;
static struct sockaddr_in g_macAddr;

// --- REPLACE WITH YOUR MAC'S ACTUAL LOCAL IP ADDRESS ---
static const char* MAC_IP_ADDRESS = "192.168.0.94"; // <--- CHANGE THIS!

static const int PIN_DC  = 512 + 25;
static const int PIN_RES = 512 + 24;

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
    if (pin == PIN_DC && g_dcFile.is_open()) {
        g_dcFile << val;
        g_dcFile.flush();
    } else if (pin == PIN_RES && g_resFile.is_open()) {
        g_resFile << val;
        g_resFile.flush();
    }
}

static void spiWrite(const uint8_t* data, size_t len) {
    if (g_spiFd < 0 || len == 0) return;

    const size_t max_chunk = 4096;
    size_t bytes_sent = 0;

    while (bytes_sent < len) {
        size_t chunk_size = len - bytes_sent;
        if (chunk_size > max_chunk) {
            chunk_size = max_chunk;
        }

        struct spi_ioc_transfer tr;
        std::memset(&tr, 0, sizeof(tr));
        
        tr.tx_buf = (uintptr_t)(data + bytes_sent);
        tr.rx_buf = 0;
        tr.len = chunk_size;
        tr.speed_hz = 2000000;
        tr.bits_per_word = 8;
        tr.delay_usecs = 0;

        if (ioctl(g_spiFd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            std::cerr << "[OLED] SPI Error: SPI_IOC_MESSAGE transaction failed: "
                      << std::strerror(errno) << " (chunk size: " << chunk_size << ")" << std::endl;
            return;
        }

        bytes_sent += chunk_size;
    }
}

static void writeCommand(uint8_t cmd) {
    gpioWrite(PIN_DC, 0);
    spiWrite(&cmd, 1);
}

static void writeCommandWithData(uint8_t cmd, const uint8_t* data, size_t len) {
    writeCommand(cmd);
    if (len > 0) {
        gpioWrite(PIN_DC, 1);
        spiWrite(data, len);
    }
}
#endif

void InitOled() {
#if defined(__linux__)
    std::cout << "[OLED] Initializing SPI on Raspberry Pi..." << std::endl;

    gpioExport(PIN_DC);
    gpioExport(PIN_RES);
    usleep(50000);

    gpioSetDir(PIN_DC, "out");
    gpioSetDir(PIN_RES, "out");

    g_dcFile.open("/sys/class/gpio/gpio" + std::to_string(PIN_DC) + "/value");
    g_resFile.open("/sys/class/gpio/gpio" + std::to_string(PIN_RES) + "/value");

    if (!g_dcFile.is_open()) std::cerr << "[OLED] GPIO Error: Failed to open DC persistent stream" << std::endl;
    if (!g_resFile.is_open()) std::cerr << "[OLED] GPIO Error: Failed to open RES persistent stream" << std::endl;

    g_spiFd = open("/dev/spidev0.0", O_RDWR);
    if (g_spiFd < 0) {
        std::cerr << "[OLED] Error: Could not open SPI device /dev/spidev0.0" << std::endl;
        return;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = 2000000;

    if (ioctl(g_spiFd, SPI_IOC_WR_MODE, &mode) < 0) std::cerr << "[OLED] SPI mode failed" << std::endl;
    if (ioctl(g_spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) std::cerr << "[OLED] SPI bits failed" << std::endl;
    if (ioctl(g_spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) std::cerr << "[OLED] SPI speed failed" << std::endl;

    gpioWrite(PIN_RES, 0);
    usleep(150000);
    gpioWrite(PIN_RES, 1);
    usleep(150000);

    uint8_t dataVal;
    
    dataVal = 0x12;
    writeCommandWithData(0xFD, &dataVal, 1);
    
    writeCommand(0xAE);
    
    dataVal = 0x91;
    writeCommandWithData(0xB3, &dataVal, 1);
    
    dataVal = 0x3F;
    writeCommandWithData(0xCA, &dataVal, 1);
    
    dataVal = 0x00;
    writeCommandWithData(0xA2, &dataVal, 1);
    
    dataVal = 0x00;
    writeCommandWithData(0xA1, &dataVal, 1);
    
    uint8_t remapData[] = {0x14, 0x11};
    writeCommandWithData(0xA0, remapData, 2);
    
    dataVal = 0x00;
    writeCommandWithData(0xB5, &dataVal, 1);
    
    dataVal = 0x01;
    writeCommandWithData(0xAB, &dataVal, 1);
    
    dataVal = 0x32;
    writeCommandWithData(0xB1, &dataVal, 1);
    
    uint8_t enhanceData[] = {0xA0, 0xB5, 0x55};
    writeCommandWithData(0xB4, enhanceData, 3);
    
    dataVal = 0x05;
    writeCommandWithData(0xBE, &dataVal, 1);
    
    dataVal = 0x0F;
    writeCommandWithData(0xC7, &dataVal, 1);
    
    dataVal = 0x01;
    writeCommandWithData(0xB6, &dataVal, 1);
    
    writeCommand(0xA6);

    uint8_t contrastData[] = {0x20, 0x20, 0x20};
    writeCommandWithData(0xC1, contrastData, 3);
    
    std::vector<uint8_t> clearBuf(8192, 0);
    uint8_t colData[] = {0x20, 0x5F};
    writeCommandWithData(0x15, colData, 2);
    uint8_t rowData[] = {0x00, 0x3F};
    writeCommandWithData(0x75, rowData, 2);
    writeCommand(0x5C);
    gpioWrite(PIN_DC, 1);
    spiWrite(clearBuf.data(), clearBuf.size());

    writeCommand(0xAF);
    std::cout << "[OLED] Display ON! SSD1322 Initialized successfully." << std::endl;
    sleep(2);

    // --- INITIALIZE NON-BLOCKING UDP SOCKET ---
    g_udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udpFd >= 0) {
        // Set socket to non-blocking so the C++ thread never blocks if Mac is offline
        int flags = fcntl(g_udpFd, F_GETFL, 0);
        fcntl(g_udpFd, F_SETFL, flags | O_NONBLOCK);

        g_macAddr.sin_family = AF_INET;
        g_macAddr.sin_port = htons(5006); // Port 5005
        g_macAddr.sin_addr.s_addr = inet_addr(MAC_IP_ADDRESS);
        std::cout << "[OLED] Screen streaming active to: " << MAC_IP_ADDRESS << ":5006" << std::endl;
    } else {
        std::cerr << "[OLED] Failed to initialize UDP streaming socket!" << std::endl;
    }
#else
    std::cout << "[OLED] Running on macOS: Physical OLED simulation active (No-op)." << std::endl;
#endif
}

void UpdateOled(RenderTexture2D oledScreen) {
#if defined(__linux__)
    if (g_spiFd < 0) return;

    uint8_t colData[] = {0x20, 0x5F};
    writeCommandWithData(0x15, colData, 2);

    uint8_t rowData[] = {0x00, 0x3F};
    writeCommandWithData(0x75, rowData, 2);

    writeCommand(0x5C);

    static std::vector<uint8_t> oledBuffer(8192, 0);

    int outIndex = 0;
    for (int y = 0; y < 64; ++y) {
        int targetY = 63 - y;
        for (int x = 0; x < 256; x += 2) {
            Color p1 = g_oledCPUPixels[targetY * 256 + x];
            Color p2 = g_oledCPUPixels[targetY * 256 + (x + 1)];

            uint8_t gray1 = (p1.r > 127 || p1.g > 127 || p1.b > 127) ? 15 : 0;
            uint8_t gray2 = (p2.r > 127 || p2.g > 127 || p2.b > 127) ? 15 : 0;

            oledBuffer[outIndex++] = (gray1 << 4) | (gray2 & 0x0F);
        }
    }

    // 1. Push data to physical hardware SPI OLED
    gpioWrite(PIN_DC, 1);
    spiWrite(oledBuffer.data(), oledBuffer.size());

    // 2. Broadcast the packed buffer to your Mac's screen mirror
    if (g_udpFd >= 0) {
        sendto(g_udpFd, oledBuffer.data(), oledBuffer.size(), 0, (struct sockaddr*)&g_macAddr, sizeof(g_macAddr));
    }
#endif
}

void ShutdownOled() {
#if defined(__linux__)
    if (g_dcFile.is_open()) g_dcFile.close();
    if (g_resFile.is_open()) g_resFile.close();

    if (g_spiFd >= 0) {
        close(g_spiFd);
        g_spiFd = -1;
        std::cout << "[OLED] SPI interface closed cleanly." << std::endl;
    }
    
    if (g_udpFd >= 0) {
        close(g_udpFd);
        g_udpFd = -1;
        std::cout << "[OLED] UDP streaming socket closed cleanly." << std::endl;
    }

    gpioUnexport(PIN_DC);
    gpioUnexport(PIN_RES);
#endif
}
