#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace CemuUwp::TitleInstaller
{
enum class ContentType
{
	BaseGame,
	Demo,
	Update,
	Dlc,
	SystemTitle,
	SystemData,
	Unknown,
};

enum class ExistingState
{
	None,
	Upgrade,
	SameVersion,
	Downgrade,
	DifferentType,
	Invalid,
};

enum class State
{
	Idle,
	Installing,
	Cancelling,
	Succeeded,
	Cancelled,
	Failed,
};

struct InstallPlan
{
	std::filesystem::path sourceRoot;
	std::filesystem::path targetPath;
	std::string titleName;
	ContentType type{ ContentType::Unknown };
	uint64_t titleId{};
	uint16_t version{};
	uint64_t requiredBytes{};
	uint64_t availableBytes{};
	uint64_t fileCount{};
	ExistingState existingState{ ExistingState::None };
	ContentType existingType{ ContentType::Unknown };
	uint64_t existingTitleId{};
	uint16_t existingVersion{};
};

struct PlanResult
{
	std::optional<InstallPlan> plan;
	std::string error;
};

struct Snapshot
{
	State state{ State::Idle };
	uint64_t copiedBytes{};
	uint64_t requiredBytes{};
	uint64_t copiedFiles{};
	uint64_t fileCount{};
	std::string currentFile;
	std::string message;
};

std::string_view ContentTypeName(ContentType type);
PlanResult BuildPlan(const std::filesystem::path& selectedFolder, const std::filesystem::path& mlcRoot);
bool Start(const InstallPlan& plan);
void RequestCancel();
Snapshot GetSnapshot();
bool IsBusy();
void Reset();
void Shutdown();
}
