/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
}  // namespace

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::loop() {
  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
      startActivityForResult(
          std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasPressed(MappedInputManager::Button::Left))
                                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered = usePressForPageTurn
                                 ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasPressed(MappedInputManager::Button::Right))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  const bool skipPages = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    requestUpdate();
  } else if (nextTriggered) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    requestUpdate();
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

void XtcReaderActivity::renderPage() {
  // XTC pages are pre-rendered images; disable dark mode to preserve
  // original appearance (clearScreen fills white, pixels not inverted).
  const bool wasDarkMode = renderer.isDarkMode();
  renderer.setDarkMode(false);

  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  if (bitDepth == 2) {
    // 2-bit XTCH: column-major format requires the full page in RAM.
    // Two bit planes: ((width * height + 7) / 8) * 2 bytes.
    const size_t pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
    uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
    if (!pageBuffer) {
      LOG_ERR("XTR", "Failed to allocate 2-bit page buffer (%lu bytes)", pageBufferSize);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      renderer.setDarkMode(wasDarkMode);
      return;
    }

    size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
    if (bytesRead == 0) {
      LOG_ERR("XTR", "Failed to load page %lu", currentPage);
      free(pageBuffer);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      renderer.setDarkMode(wasDarkMode);
      return;
    }

    renderer.clearScreen();

    // Two bit planes, column-major order (columns right-to-left, 8 vertical pixels/byte)
    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;
    const size_t colBytes = (pageHeight + 7) / 8;

    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    LOG_DBG("XTR", "Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu", pixelCounts[0],
            pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW — all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    ReaderRuntime::RefreshContext refreshContext{};
    refreshContext.readerKind = ReaderRuntime::ReaderKind::Xtc;
    refreshContext.textAntiAliasing = true;
    refreshContext.grayscaleRequested = true;
    refreshContext.lowMemory =
        ReaderRuntime::classifyReaderMemory(ESP.getFreeHeap()) != ReaderRuntime::MemoryDecision::Proceed;
    refreshContext.cadenceRemaining = pagesUntilFullRefresh;
    refreshContext.refreshFrequency = SETTINGS.getRefreshFrequency();

    const auto decision = ReaderRuntime::chooseReaderRefresh(refreshContext);
    ReaderUtils::displayWithRefreshDecision(renderer, decision);
    pagesUntilFullRefresh = decision.nextCadenceRemaining;

    if (!decision.runGrayscalePass) {
      free(pageBuffer);
      LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit BW fallback)", currentPage + 1, xtc->getPageCount());
      renderer.setDarkMode(wasDarkMode);
      return;
    }

    // Pass 2: LSB — dark grey pixels only (XTH value 1)
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB — dark and light grey pixels (XTH value 1 or 2)
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();
    renderer.displayGrayBuffer();

    // Pass 4: Restore BW framebuffer for the next page turn
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);
    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());

  } else {
    // 1-bit XTC: stream row-by-row directly into the framebuffer.
    // Avoids a 48KB heap allocation; uses only a small chunk buffer (srcRowBytes).
    // Format: row-major, MSB-first, (width+7)/8 bytes per row.
    renderer.clearScreen();
    const size_t srcRowBytes = (pageWidth + 7) / 8;

    const xtc::XtcError streamErr = xtc->loadPageStreaming(
        currentPage,
        [&](const uint8_t* data, size_t size, size_t offset) {
          for (size_t i = 0; i < size; i++) {
            const size_t byteOffset = offset + i;
            const uint16_t srcY = static_cast<uint16_t>(byteOffset / srcRowBytes);
            const uint16_t baseX = static_cast<uint16_t>((byteOffset % srcRowBytes) * 8);
            const uint8_t byte = data[i];
            for (int bit = 0; bit < 8; bit++) {
              const uint16_t srcX = baseX + bit;
              if (srcX >= pageWidth) break;
              if (!((byte >> (7 - bit)) & 1)) {  // XTC polarity: 0=black, 1=white
                renderer.drawPixel(srcX, srcY, true);
              }
            }
          }
        },
        srcRowBytes);

    if (streamErr != xtc::XtcError::OK) {
      LOG_ERR("XTR", "Failed to stream page %lu", currentPage);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      renderer.setDarkMode(wasDarkMode);
      return;
    }

    ReaderRuntime::RefreshContext refreshContext{};
    refreshContext.readerKind = ReaderRuntime::ReaderKind::Xtc;
    refreshContext.lowMemory =
        ReaderRuntime::classifyReaderMemory(ESP.getFreeHeap()) != ReaderRuntime::MemoryDecision::Proceed;
    refreshContext.cadenceRemaining = pagesUntilFullRefresh;
    refreshContext.refreshFrequency = SETTINGS.getRefreshFrequency();

    const auto decision = ReaderRuntime::chooseReaderRefresh(refreshContext);
    ReaderUtils::displayWithRefreshDecision(renderer, decision);
    pagesUntilFullRefresh = decision.nextCadenceRemaining;

    LOG_DBG("XTR", "Rendered page %lu/%lu (1-bit streaming)", currentPage + 1, xtc->getPageCount());
  }

  renderer.setDarkMode(wasDarkMode);
}

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}
