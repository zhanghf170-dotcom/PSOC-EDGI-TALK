#include "thing.h"

extern "C" {
#include "../xiaozhi.h"
extern rt_err_t lcd_set_brightness(rt_uint8_t percent);
extern rt_uint8_t lcd_get_brightness(void);
}

namespace iot {

class Screen : public Thing {
public:
    Screen() : Thing("Screen", "屏幕")
    {
        properties_.AddNumberProperty("brightness", "屏幕亮度(15-100)", []() -> int {
            return (int)lcd_get_brightness();
        });

        methods_.AddMethod("SetEmoji", "设置Gemini娘表情", ParameterList({
            Parameter("emoji", "表情名称", kValueTypeString, true)
        }), [](const ParameterList &parameters) {
            xiaozhi_set_emotion_name(parameters["emoji"].string().c_str());
        });

        methods_.AddMethod("SetBrightness", "设置屏幕亮度", ParameterList({
            Parameter("brightness", "15-100", kValueTypeNumber, true)
        }), [](const ParameterList &parameters) {
            int b = parameters["brightness"].number();
            if (b < 15) b = 15;
            if (b > 100) b = 100;
            (void)lcd_set_brightness((rt_uint8_t)b);
        });
    }
};

} /* namespace iot */

DECLARE_THING(Screen);
