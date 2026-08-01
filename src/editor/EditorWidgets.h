#pragma once

#include <imgui.h>

#include <cstddef>
#include <string_view>

namespace vkr::editor {

enum class StatusTone { Neutral, Success, Warning, Error, Info };

ImVec4 statusColor(StatusTone tone);
void statusIndicator(const char *label, StatusTone tone,
                     const char *tooltip = nullptr);

bool beginPropertyGrid(const char *id, float labelFraction = 0.42f);
void propertyLabel(const char *label, const char *tooltip = nullptr);
void endPropertyGrid();

void pathValue(std::string_view value);
void emptyState(const char *message);

bool segmentedControl(const char *id, int &selected,
                      const char *const *labels, size_t labelCount);

} // namespace vkr::editor
