#include "Screen.h"

Screen screen = {};

Screen::Screen() {}

void Screen::drawTopBar() {
    if (!shouldDraw()) return;

    char buffer[64];
    auto time = M5.Rtc.getDateTime();
    // battery level
    char batteryStatus[2];
    batteryStatus[1] = 0;
    auto batteryLevel = M5.Power.getBatteryLevel();
    if (batteryLevel >= 100) {
        batteryStatus[0] = 'F';
    } else {
        batteryStatus[0] = '0' + (batteryLevel / 10);
    }
    // print UTC time + battery status + current
    snprintf(buffer, sizeof(buffer), "%4d-%02d-%02dZ%02d:%02d:%02d %s%+dmA     ",  //
             time.date.year, time.date.month, time.date.date,                      //
             time.time.hours, time.time.minutes, time.time.seconds,                //
             batteryStatus, M5.Power.getBatteryCurrent()                           //
    );

    drawString(buffer,                                 //
               2,                                      // font size
               touchSuppressed ? 0xFFFF00 : 0xFFFFFF,  // text color: yellow if touch is suppressed
               0x003264,                               // custom background color
               top_left, 0, 0);
}

void Screen::loopBeforeApp() {
    bool hasTouch = M5.Touch.getCount() > 0;
    bool hasBtn = M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed();

    if (!isOn() && (hasTouch || hasBtn)) {
        turnOnBacklight();
    }
    if (touchSuppressed) {  // release suppress if no touch
        if (!hasTouch && !hasBtn) {
            touchSuppressed = false;
        }
    }
    if (hasTouch || hasBtn) {
        // mark active if user has touch or button input
        markActive();
    }
    // update frame timer, target FPS: 20, frame interval 50ms
    if (millis() - frameTimer >= 50) {
        shouldDrawFrame = true;
        frameTimer += 50;
    } else {
        shouldDrawFrame = false;
    }
}

void Screen::loopAfterApp() {
    // turn off screen when timeout
    if (isOn() && millis() - lastActive >= SCREEN_TIMEOUT_MS) {
        turnOffBacklight();
    }
    if (isOn()) {
        drawTopBar();
    }
}

bool Screen::isOn() const { return backlight; }

const m5::Touch_Class* Screen::touch() const {
    if (isOn() && !touchSuppressed) {
        return &M5.Touch;
    } else {
        return nullptr;
    }
}

const m5::Button_Class* Screen::btnA() const {
    if (isOn() && !touchSuppressed) {
        return &M5.BtnA;
    } else {
        return nullptr;
    }
}

const m5::Button_Class* Screen::btnB() const {
    if (isOn() && !touchSuppressed) {
        return &M5.BtnB;
    } else {
        return nullptr;
    }
}

const m5::Button_Class* Screen::btnC() const {
    if (isOn() && !touchSuppressed) {
        return &M5.BtnC;
    } else {
        return nullptr;
    }
}

void Screen::markActive() { lastActive = millis(); }

bool Screen::shouldDraw() const { return shouldDrawFrame; }

int32_t Screen::width() const { return M5.Display.width(); }

int32_t Screen::height() const { return M5.Display.height(); }

int32_t Screen::topBarHeight() const { return 20; }

void Screen::clearScreen() { M5.Display.fillScreen(0x000000); }

void Screen::drawString(const char* buffer, float textSize, uint32_t fgRGB, uint32_t bgRGB, textdatum_t datum, int32_t x, int32_t y) {
    M5.Display.setTextSize(textSize);
    auto textStyle = M5.Display.getTextStyle();
    textStyle.fore_rgb888 = fgRGB;
    textStyle.back_rgb888 = bgRGB;
    M5.Display.setTextStyle(textStyle);
    M5.Display.setTextDatum(datum);
    M5.Display.drawString(buffer, x, y);
}

void Screen::drawStringMiddleCenter(const char* buffer, float textSize, uint32_t fgRGB, uint32_t bgRGB, int32_t y) {
    drawString(buffer, textSize, fgRGB, bgRGB, middle_center, width() / 2, y);
}

void Screen::turnOnBacklight() {
    M5.Display.setBrightness(255);
    backlight = true;
    markActive();
    // check touch and button status, suppress them if they are touched while backlight is off
    bool hasTouch = M5.Touch.getCount() > 0;
    bool hasBtn = M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed();
    if (hasTouch || hasBtn) {
        touchSuppressed = true;
    }
}

void Screen::turnOffBacklight() {
    backlight = false;
    M5.Display.setBrightness(0);
}
