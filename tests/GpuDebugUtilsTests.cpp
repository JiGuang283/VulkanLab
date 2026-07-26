#include "core/GpuDebugUtils.h"

#include <stdexcept>
#include <utility>

namespace {

uint32_t gNames = 0;
uint32_t gBegins = 0;
uint32_t gInserts = 0;
uint32_t gEnds = 0;

VKAPI_ATTR VkResult VKAPI_CALL
fakeSetName(VkDevice, const VkDebugUtilsObjectNameInfoEXT *) {
    ++gNames;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
fakeBeginLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {
    ++gBegins;
}

VKAPI_ATTR void VKAPI_CALL
fakeInsertLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {
    ++gInserts;
}

VKAPI_ATTR void VKAPI_CALL fakeEndLabel(VkCommandBuffer) { ++gEnds; }

void requireDebugUtils(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testNoOpDebugUtils() {
    const vkr::GpuDebugUtils debugUtils;
    debugUtils.setObjectName(VK_OBJECT_TYPE_BUFFER, uint64_t{1}, "Buffer");
    debugUtils.insertLabel(reinterpret_cast<VkCommandBuffer>(1), "Insert");
    {
        vkr::ScopedGpuLabel label(
            debugUtils, reinterpret_cast<VkCommandBuffer>(1), "Scope");
    }
    requireDebugUtils(!debugUtils.available(),
                      "empty debug utils unexpectedly became available");
}

void testDebugUtilsCallsAndScope() {
    gNames = gBegins = gInserts = gEnds = 0;
    vkr::GpuDebugFunctionTable functions;
    functions.setObjectName = fakeSetName;
    functions.beginLabel = fakeBeginLabel;
    functions.insertLabel = fakeInsertLabel;
    functions.endLabel = fakeEndLabel;
    const auto device = reinterpret_cast<VkDevice>(1);
    const auto commandBuffer = reinterpret_cast<VkCommandBuffer>(2);
    const vkr::GpuDebugUtils debugUtils(device, functions);

    debugUtils.setObjectName(VK_OBJECT_TYPE_BUFFER, uint64_t{3}, "Buffer");
    debugUtils.insertLabel(commandBuffer, "Insert");
    {
        vkr::ScopedGpuLabel first(debugUtils, commandBuffer, "First");
        vkr::ScopedGpuLabel second(std::move(first));
    }

    requireDebugUtils(gNames == 1, "object name call was not forwarded");
    requireDebugUtils(gInserts == 1, "insert label call was not forwarded");
    requireDebugUtils(gBegins == 1 && gEnds == 1,
                      "scoped label was not balanced");
}

} // namespace

void runGpuDebugUtilsTests() {
    testNoOpDebugUtils();
    testDebugUtilsCallsAndScope();
}
