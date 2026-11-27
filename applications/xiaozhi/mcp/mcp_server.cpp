#include "mcp_server.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "../iot/thing_manager.h"
#include "../xiaozhi.h"

extern "C" {
#include "alert_manager.h"
#include "dualcore_bridge.h"
#include "wifi_stream.h"
#include "uvc_ai_app.h"
#include "usbh_uvc_display_hook.h"
#include "ui_manager.h"
extern float g_aht_temp;
extern float g_aht_humi;
extern rt_err_t lcd_set_brightness(rt_uint8_t percent);
extern rt_uint8_t lcd_get_brightness(void);
}

#define BOARD_NAME "edgi-talk-e84"

McpServer::McpServer() {}

McpServer::~McpServer()
{
    for (auto *tool : tools_) delete tool;
    tools_.clear();
}

void McpServer::AddCommonTools()
{
    auto original_tools = std::move(tools_);

    /* Speaker volume */
    AddTool("self.audio_speaker.set_volume",
            "Set speaker volume (0-100).",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [](const PropertyList &props) -> ReturnValue {
                alert_set_volume(props["volume"].value<int>());
                alert_save_volume_cfg();
                return true;
            });

    AddTool("self.audio_speaker.get_volume",
            "Get current speaker volume.",
            PropertyList(),
            [](const PropertyList &) -> ReturnValue {
                return alert_get_volume();
            });

    /* Screen brightness & Gemini-chan expression */
    AddTool("self.screen.set_brightness",
            "Set cockpit LCD brightness (15-100).",
            PropertyList({Property("brightness", kPropertyTypeInteger, 15, 100)}),
            [](const PropertyList &props) -> ReturnValue {
                int b = props["brightness"].value<int>();
                (void)lcd_set_brightness((rt_uint8_t)b);
                return true;
            });

    AddTool("self.screen.set_emoji",
            "Set Gemini-chan avatar emotion: neutral/happy/listening/thinking/winking/surprised/sad/sleepy.",
            PropertyList({Property("emoji", kPropertyTypeString)}),
            [](const PropertyList &props) -> ReturnValue {
                xiaozhi_set_emotion_name(props["emoji"].value<std::string>().c_str());
                return true;
            });

    /* Onboard RGB LED */
    auto led = iot::ThingManager::GetInstance().GetThing("Led");
    if (led)
    {
        AddTool("self.led.set",
                "Control onboard LED: red/green/blue/all/off/red_off/green_off/blue_off.",
                PropertyList({Property("color", kPropertyTypeString)}),
                [led](const PropertyList &props) -> ReturnValue {
                    char buf[80];
                    rt_snprintf(buf, sizeof(buf),
                                "{\"method\":\"SetLed\",\"parameters\":{\"color\":\"%s\"}}",
                                props["color"].value<std::string>().c_str());
                    cJSON *cmd = cJSON_Parse(buf);
                    if (cmd)
                    {
                        led->Invoke(cmd);
                        cJSON_Delete(cmd);
                    }
                    return true;
                });
    }

    /* Cockpit telemetry, navigation & fatigue monitor control */
    AddTool("self.cockpit.get_status",
            "Get cabin temperature, humidity, fatigue alert count, dual-core status and Wi-Fi IP.",
            PropertyList(),
            [](const PropertyList &) -> ReturnValue {
                rt_bool_t dc = RT_FALSE;
                uint32_t hb = 0, rtt = 0;
                wifi_status_t wst;
                uvc_ai_telemetry_t tel;
                dualcore_get_brief(&dc, &hb, &rtt);
                wifi_mgr_get_status(&wst);
                uvc_ai_app_get_telemetry(&tel);

                char buf[256];
                float t_disp = std::isfinite(g_aht_temp) ? g_aht_temp : 0.0f;
                float h_disp = std::isfinite(g_aht_humi) ? g_aht_humi : 0.0f;
                rt_snprintf(buf, sizeof(buf),
                            "Cabin %.1fC, Humidity %.1f%%, FatigueMonitor=%s, Alerts=%lu, DualCore=%s(RTT %lums), IP=%s, Brightness=%u%%",
                            (double)t_disp, (double)h_disp,
                            tel.running ? "ON" : "OFF",
                            (unsigned long)alert_get_danger_count(),
                            dc ? "ONLINE" : "SOLO", (unsigned long)rtt,
                            wst.ip_str, (unsigned)lcd_get_brightness());
                return std::string(buf);
            });

    AddTool("self.cockpit.open_page",
            "Switch cockpit display page: face/apps/xiaozhi/monitor/weather/wifi/settings/brightness/gallery/music/camera.",
            PropertyList({Property("page", kPropertyTypeString)}),
            [](const PropertyList &props) -> ReturnValue {
                return ui_request_page_by_name(props["page"].value<std::string>().c_str()) == RT_EOK;
            });

    AddTool("self.cockpit.set_fatigue_monitor",
            "Start or stop driver fatigue AI monitoring.",
            PropertyList({Property("enable", kPropertyTypeBoolean)}),
            [](const PropertyList &props) -> ReturnValue {
                bool en = props["enable"].value<bool>();
                if (en)
                {
                    (void)ui_request_page_by_name("monitor");
                }
                else
                {
                    (void)uvc_app_stop();
                    (void)ui_request_page_by_name("xiaozhi");
                }
                return true;
            });

    AddTool("self.cockpit.take_snapshot",
            "Capture a camera snapshot and push via Wi-Fi relay.",
            PropertyList(),
            [](const PropertyList &) -> ReturnValue {
                return wifi_stream_push_snapshot("voice_snap.jpg") == RT_EOK;
            });

    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::SendText(const std::string &text)
{
    (void)xz_ws_send_text(text.c_str(), text.length());
}

void McpServer::SendmcpMessage(const std::string &payload)
{
    std::string message = "{\"session_id\":\"" + std::string(xz_ws_get_session_id()) +
                          "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

void McpServer::AddTool(McpTool *tool)
{
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool *t) {
            return t->name() == tool->name();
        }) != tools_.end())
    {
        delete tool;
        return;
    }
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string &name, const std::string &description,
                        const PropertyList &properties,
                        std::function<ReturnValue(const PropertyList &)> callback)
{
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::ParseMessage(const std::string &message)
{
    cJSON *json = cJSON_Parse(message.c_str());
    if (json == nullptr) return;
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseMessage(const cJSON *json)
{
    auto *version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0)
        return;

    auto *method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) return;
    std::string method_str(method->valuestring);
    if (method_str.find("notifications") == 0) return;

    auto *params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) return;

    auto *id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) return;
    int id_int = id->valueint;

    if (method_str == "initialize")
    {
        std::string msg = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
                          "\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"1.2.0\"}}";
        ReplyResult(id_int, msg);
    }
    else if (method_str == "tools/list")
    {
        std::string cursor_str;
        if (params != nullptr)
        {
            auto *cursor = cJSON_GetObjectItem(params, "cursor");
            if (cursor && cJSON_IsString(cursor)) cursor_str = cursor->valuestring;
        }
        GetToolsList(id_int, cursor_str);
    }
    else if (method_str == "tools/call")
    {
        if (!cJSON_IsObject(params))
        {
            ReplyError(id_int, "Missing params");
            return;
        }
        auto *tool_name = cJSON_GetObjectItem(params, "name");
        if (!tool_name || !cJSON_IsString(tool_name))
        {
            ReplyError(id_int, "Missing name");
            return;
        }
        auto *tool_args = cJSON_GetObjectItem(params, "arguments");
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_args);
    }
    else
    {
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string &result)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                          ",\"result\":" + result + "}";
    SendmcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string &message)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                          ",\"error\":{\"message\":\"" + message + "\"}}";
    SendmcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string &cursor)
{
    const size_t max_payload_size = 7500U;
    std::string json = "{\"tools\":[";
    bool found_cursor = cursor.empty();
    std::string next_cursor;

    for (auto *tool : tools_)
    {
        if (!found_cursor)
        {
            if (tool->name() == cursor) found_cursor = true;
            else continue;
        }
        std::string tj = tool->to_json() + ",";
        if (json.length() + tj.length() + 30U > max_payload_size)
        {
            next_cursor = tool->name();
            break;
        }
        json += tj;
    }

    if (json.back() == ',') json.pop_back();
    if (next_cursor.empty()) json += "]}";
    else json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string &tool_name, const cJSON *tool_arguments)
{
    auto it = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const McpTool *t) {
        return t->name() == tool_name;
    });
    if (it == tools_.end())
    {
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*it)->properties();
    for (auto &arg : arguments)
    {
        bool found = false;
        if (tool_arguments && cJSON_IsObject(tool_arguments))
        {
            auto *val = cJSON_GetObjectItem(tool_arguments, arg.name().c_str());
            if (arg.type() == kPropertyTypeBoolean && cJSON_IsBool(val))
            {
                arg.set_value<bool>(cJSON_IsTrue(val) != 0);
                found = true;
            }
            else if (arg.type() == kPropertyTypeInteger && cJSON_IsNumber(val))
            {
                int v = val->valueint;
                if (arg.has_range()) v = std::clamp(v, arg.min_value(), arg.max_value());
                arg.set_value<int>(v);
                found = true;
            }
            else if (arg.type() == kPropertyTypeString && cJSON_IsString(val))
            {
                arg.set_value<std::string>(val->valuestring);
                found = true;
            }
        }
        if (!arg.has_default_value() && !found)
        {
            ReplyError(id, "Missing valid argument: " + arg.name());
            return;
        }
    }

    ReplyResult(id, (*it)->Call(arguments));
}
