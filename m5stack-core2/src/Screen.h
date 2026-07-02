#ifndef SCREEN_H
#define SCREEN_H

#include <M5Unified.h>

#ifndef SCREEN_TIMEOUT_MS
#define SCREEN_TIMEOUT_MS 30000
#endif

class Screen {
   private:
    bool backlight = true;
    uint32_t lastActive = 0;
    bool touchSuppressed = false;
    uint32_t frameTimer = 0;
    bool shouldDrawFrame = false;

    void markActive();
    void drawTopBar();

   public:
    Screen();
    // should call before app loop
    void loopBeforeApp();
    // should call after app loop
    void loopAfterApp();
    bool isOn() const;
    bool shouldDraw() const;
    int32_t width() const;
    int32_t height() const;
    int32_t topBarHeight() const;

    void clearScreen();
    void drawString(const char* buffer, float textSize, uint32_t fgRGB, uint32_t bgRGB, textdatum_t datum, int32_t x, int32_t y);
    void drawStringMiddleCenter(const char* buffer, float textSize, uint32_t fgRGB, uint32_t bgRGB, int32_t y);

    void turnOnBacklight();
    void turnOffBacklight();
    // return M5.Touch if backlight is on, otherwise nullptr
    const m5::Touch_Class* touch() const;
    // return M5.BtnA if backlight is on, otherwise nullptr
    const m5::Button_Class* btnA() const;
    // return M5.BtnB if backlight is on, otherwise nullptr
    const m5::Button_Class* btnB() const;
    // return M5.BtnC if backlight is on, otherwise nullptr
    const m5::Button_Class* btnC() const;
};

// global instance
extern Screen screen;

#endif
