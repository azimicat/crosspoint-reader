#include <HalDisplay.h>
#include <HalGPIO.h>

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
  einkDisplay.begin();
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  uint8_t* fb = einkDisplay.getFrameBuffer();
  const uint16_t srcBytesPerRow = (w + 7) / 8;
  for (uint16_t row = 0; row < h; row++) {
    if ((y + row) >= DISPLAY_HEIGHT) break;
    for (uint16_t col = 0; col < w; col++) {
      if ((x + col) >= DISPLAY_WIDTH) break;
      uint8_t srcByte = fromProgmem ? pgm_read_byte(&imageData[row * srcBytesPerRow + col / 8])
                                    : imageData[row * srcBytesPerRow + col / 8];
      if (!(srcByte & (0x80 >> (col % 8)))) {
        // Black pixel: write to framebuffer, leave white (transparent) pixels unchanged
        uint32_t fbIdx = static_cast<uint32_t>(y + row) * DISPLAY_WIDTH_BYTES + (x + col) / 8;
        fb[fbIdx] &= ~(0x80 >> ((x + col) % 8));
      }
    }
  }
}

static EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::DARK_REDRIVE:
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (mode == RefreshMode::DARK_REDRIVE) {
    einkDisplay.forceRedRamInverted();
  }
  einkDisplay.displayBuffer(convertRefreshMode(mode));
  if (turnOffScreen) {
    einkDisplay.deepSleep();
  }
}

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
}

void HalDisplay::displayWindowDarkRedrive(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  einkDisplay.displayWindowDarkRedrive(x, y, w, h, turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (mode == RefreshMode::DARK_REDRIVE) {
    einkDisplay.forceRedRamInverted();
  }
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }

uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }

uint16_t HalDisplay::getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }

uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }
