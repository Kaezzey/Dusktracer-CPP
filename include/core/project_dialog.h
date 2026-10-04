#pragma once
#include <filesystem>
#include <optional>

namespace dusk_project {
std::filesystem::path command_line_scene(const char* fallback);

enum class dialog_kind { open_scene, save_scene, open_folder };
std::optional<std::filesystem::path> choose_path(void* owner, dialog_kind kind,
                                                 const std::filesystem::path& initial = {});
} // namespace dusk_project
