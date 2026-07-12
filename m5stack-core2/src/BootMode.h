#ifndef BOOT_MODE_H
#define BOOT_MODE_H

class BootMode {
   public:
    virtual ~BootMode() = default;
    virtual void setup() = 0;
    virtual void loop() = 0;
};

#endif  // BOOT_MODE_H
