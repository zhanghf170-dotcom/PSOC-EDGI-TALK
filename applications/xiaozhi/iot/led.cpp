#include "thing.h"
#include <rtthread.h>
#include <rtdevice.h>
#include "drv_gpio.h"

namespace iot {

#define XZ_LED_PIN_B GET_PIN(16, 5)
#define XZ_LED_PIN_G GET_PIN(16, 6)
#define XZ_LED_PIN_R GET_PIN(16, 7)

class Led : public Thing {
public:
    Led() : Thing("Led", "板载RGB指示灯")
    {
        rt_pin_mode(XZ_LED_PIN_R, PIN_MODE_OUTPUT);
        rt_pin_mode(XZ_LED_PIN_G, PIN_MODE_OUTPUT);

        methods_.AddMethod("SetLed", "控制板载指示灯", ParameterList({
            Parameter("color", "red/green/blue/all/off/red_off/green_off/blue_off", kValueTypeString, true)
        }), [](const ParameterList &p) {
            std::string color = p["color"].string();
            if (color == "red")            rt_pin_write(XZ_LED_PIN_R, PIN_HIGH);
            else if (color == "red_off")   rt_pin_write(XZ_LED_PIN_R, PIN_LOW);
            else if (color == "green")     rt_pin_write(XZ_LED_PIN_G, PIN_HIGH);
            else if (color == "green_off") rt_pin_write(XZ_LED_PIN_G, PIN_LOW);
            else if (color == "blue")      { rt_pin_mode(XZ_LED_PIN_B, PIN_MODE_OUTPUT); rt_pin_write(XZ_LED_PIN_B, PIN_HIGH); }
            else if (color == "blue_off")  { rt_pin_mode(XZ_LED_PIN_B, PIN_MODE_OUTPUT); rt_pin_write(XZ_LED_PIN_B, PIN_LOW); }
            else if (color == "all")
            {
                rt_pin_write(XZ_LED_PIN_R, PIN_HIGH);
                rt_pin_write(XZ_LED_PIN_G, PIN_HIGH);
            }
            else
            {
                rt_pin_write(XZ_LED_PIN_R, PIN_LOW);
                rt_pin_write(XZ_LED_PIN_G, PIN_LOW);
            }
        });
    }
};

} /* namespace iot */

DECLARE_THING(Led);
