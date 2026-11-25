#include "iot_c_api.h"
#include "thing_manager.h"
#include "cJSON.h"
#include <rtthread.h>
#include <string.h>
#include "../mcp/mcp_server.h"

static std::string descriptors_json;
static std::string states_json;

extern "C" {

void iot_initialize()
{
    auto &manager = iot::ThingManager::GetInstance();
    if (auto *t = iot::CreateThing("Speaker")) manager.AddThing(t);
    if (auto *t = iot::CreateThing("Screen"))  manager.AddThing(t);
    if (auto *t = iot::CreateThing("Led"))     manager.AddThing(t);

    McpServer::GetInstance().AddCommonTools();
}

void iot_invoke(const uint8_t *data, uint16_t len)
{
    char *json_str = (char *)rt_malloc(len + 1U);
    if (json_str == RT_NULL) return;
    rt_memcpy(json_str, data, len);
    json_str[len] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    if (root == nullptr)
    {
        rt_free(json_str);
        return;
    }

    iot::ThingManager::GetInstance().Invoke(root);
    cJSON_Delete(root);
    rt_free(json_str);
}

const char *iot_get_descriptors_json()
{
    descriptors_json = iot::ThingManager::GetInstance().GetDescriptorsJson();
    return descriptors_json.c_str();
}

const char *iot_get_states_json()
{
    states_json.clear();
    iot::ThingManager::GetInstance().GetStatesJson(states_json, false);
    return states_json.c_str();
}

} /* extern "C" */
