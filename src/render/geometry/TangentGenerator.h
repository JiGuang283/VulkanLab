#pragma once

#include "Vertex.h"

#include <cstdint>
#include <vector>

namespace vkr {

void generateTangents(std::vector<Vertex> &vertices,
                      const std::vector<uint32_t> &indices);

} // namespace vkr
