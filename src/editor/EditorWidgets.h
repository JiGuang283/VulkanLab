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
                      const char *const *labels, size_t labelCount,
                      float totalWidth = 0.0f);

bool iconButton(const char *id, const char *icon, const char *fallback,
                const char *tooltip, ImVec2 size = {});
bool toggleIconButton(const char *id, const char *icon,
                      const char *fallback, const char *tooltip,
                      bool active, ImVec2 size = {});
void statusChip(const char *label, StatusTone tone);
bool sectionHeader(const char *label, bool *enabled = nullptr,
                   StatusTone tone = StatusTone::Neutral,
                   const char *status = nullptr);
bool resetButton(const char *id, const char *tooltip = "Reset");

} // namespace vkr::editor
