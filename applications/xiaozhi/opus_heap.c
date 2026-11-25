/*
 * Opus dynamic memory allocator routed to RT-Thread unified heap (SoCMEM + HyperRAM)
 */
#include <rtthread.h>
#include <stdint.h>

void *opus_heap_malloc(uint32_t size)
{
    return rt_malloc(size);
}

void opus_heap_free(void *p)
{
    if (p != RT_NULL)
    {
        rt_free(p);
    }
}
