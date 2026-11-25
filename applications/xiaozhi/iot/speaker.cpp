#include "thing.h"

extern "C" {
#include "alert_manager.h"
}

namespace iot {

class Speaker : public Thing {
public:
    Speaker() : Thing("Speaker", "扬声器")
    {
        properties_.AddNumberProperty("volume", "当前音量值(0到100之间)", []() -> int {
            return alert_get_volume();
        });

        methods_.AddMethod("SetVolume", "设置音量", ParameterList({
            Parameter("volume", "0到100之间的整数", kValueTypeNumber, true)
        }), [](const ParameterList &parameters) {
            int vol = parameters["volume"].number();
            if (vol < 0) vol = 0;
            if (vol > 99) vol = 99;
            alert_set_volume(vol);
        });

        methods_.AddMethod("GetVolume", "获取当前音量", ParameterList(),
            [](const ParameterList &) {});
    }
};

} /* namespace iot */

DECLARE_THING(Speaker);
