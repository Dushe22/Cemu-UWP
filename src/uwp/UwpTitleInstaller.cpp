#include "UwpTitleInstaller.h"

#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cemu/Logging/CemuLogging.h"
#include "util/helpers/helpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using namespace CemuUwp::TitleInstaller;

constexpr uint64_t kMinimumFreeSpaceReserve = 4ull * 1024ull * 1024ull;
constexpr uint64_t kMaximumFreeSpaceReserve = 64ull * 1024ull * 1024ull;
constexpr size_t kCopyBufferSize = 1024 * 1024;

std::mutex s_stateMutex;
std::thread s_worker;
std::atomic_bool s_cancelRequested{ false };
Snapshot s_snapshot{};

class InstallCancelled final
{
};

bool IsAsciiEqualInsensitive(std::wstring_view lhs, std::wstring_view rhs)
{
	if (lhs.size() != rhs.size())
		return false;
	for (size_t i = 0; i < lhs.size(); ++i)
	{
		wchar_t left = lhs[i];
		wchar_t right = rhs[i];
		if (left >= L'A' && left <= L'Z')
			left += L'a' - L'A';
		if (right >= L'A' && right <= L'Z')
			right += L'a' - L'A';
		if (left != right)
			return false;
	}
	return true;
}

fs::path NormalizePath(const fs::path& path)
{
	std::error_code ec;
	fs::path normalized = fs::weakly_canonical(path, ec);
	if (!ec)
		return normalized;
	normalized = fs::absolute(path, ec);
	return ec ? path.lexically_normal() : normalized.lexically_normal();
}

// Windows refuses paths past MAX_PATH unless they use the extended-length form; Mario Kart 8's update has ghost files
// whose path under LocalState reaches about 270 characters. Expects an absolute, normalized path.
fs::path ExtendedLengthPath(const fs::path& path)
{
	std::wstring text = path.wstring();
	if (text.size() < 3 || text[1] != L':' || text.starts_with(L"\\\\?\\"))
		return path;
	std::replace(text.begin(), text.end(), L'/', L'\\');
	return fs::path(L"\\\\?\\" + text);
}

fs::path NormalizeSourceRoot(const fs::path& selectedFolder)
{
	fs::path sourceRoot = NormalizePath(selectedFolder);
	const std::wstring filename = sourceRoot.filename().wstring();
	if (IsAsciiEqualInsensitive(filename, L"code") ||
		IsAsciiEqualInsensitive(filename, L"content") ||
		IsAsciiEqualInsensitive(filename, L"meta"))
	{
		sourceRoot = sourceRoot.parent_path();
	}
	return sourceRoot;
}

constexpr std::array<const char*, 3> kTitleDirectories{ "code", "content", "meta" };
constexpr int kTitleSearchDepth = 3;
constexpr size_t kTitleSearchFolderLimit = 4096;

bool IsTitleRoot(const fs::path& folder)
{
	std::error_code ec;
	for (const char* directoryName : kTitleDirectories)
	{
		if (!fs::is_directory(folder / directoryName, ec) || ec)
			return false;
	}
	return true;
}

// lists the first entries of a folder for error messages
std::string DescribeFolderContents(const fs::path& folder)
{
	std::error_code ec;
	fs::directory_iterator it(folder, ec);
	if (ec)
		return fmt::format("it could not be listed ({})", ec.message());
	std::string listing;
	size_t count = 0;
	for (; it != fs::directory_iterator(); it.increment(ec))
	{
		if (ec)
			break;
		if (count < 8)
		{
			if (!listing.empty())
				listing += ", ";
			listing += _pathToUtf8(it->path().filename());
			std::error_code typeError;
			if (it->is_directory(typeError))
				listing += "/";
		}
		count++;
	}
	if (count == 0)
		return "it is empty";
	if (count > 8)
		listing += fmt::format(" and {} more", count - 8);
	return "it contains " + listing;
}

// the picked folder may be the title itself or a folder that holds it
fs::path FindTitleRoot(const fs::path& selectedRoot)
{
	if (IsTitleRoot(selectedRoot))
		return selectedRoot;

	std::vector<fs::path> titleRoots;
	std::vector<std::pair<fs::path, int>> pending{ { selectedRoot, 0 } };
	size_t visitedFolders = 0;
	while (!pending.empty() && visitedFolders < kTitleSearchFolderLimit)
	{
		const auto [folder, depth] = pending.back();
		pending.pop_back();
		std::error_code ec;
		for (fs::directory_iterator it(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			std::error_code statusError;
			const fs::file_status status = it->symlink_status(statusError);
			if (statusError || !fs::is_directory(status))
				continue;
			visitedFolders++;
			if (IsTitleRoot(it->path()))
				titleRoots.push_back(it->path());
			else if (depth + 1 < kTitleSearchDepth)
				pending.emplace_back(it->path(), depth + 1);
		}
	}

	if (titleRoots.size() == 1)
		return titleRoots.front();
	if (titleRoots.size() > 1)
	{
		std::string names;
		for (size_t i = 0; i < titleRoots.size() && i < 4; ++i)
		{
			if (!names.empty())
				names += ", ";
			names += _pathToUtf8(titleRoots[i].filename());
		}
		throw std::runtime_error(fmt::format("The selected folder '{}' contains {} titles ({}). Select one of them.",
			_pathToUtf8(selectedRoot), titleRoots.size(), names));
	}
	for (const char* directoryName : kTitleDirectories)
	{
		std::error_code ec;
		if (!fs::is_directory(selectedRoot / directoryName, ec) || ec)
		{
			throw std::runtime_error(fmt::format("The selected folder '{}' is missing its '{}' directory{}; {}",
				_pathToUtf8(selectedRoot), directoryName, ec ? fmt::format(" ({})", ec.message()) : std::string{},
				DescribeFolderContents(selectedRoot)));
		}
	}
	return selectedRoot;
}

bool IsSafeRelativePath(const fs::path& relative)
{
	if (relative.empty() || relative.is_absolute())
		return false;
	for (const fs::path& component : relative)
	{
		if (component == "..")
			return false;
	}
	return true;
}

bool IsPathWithin(const fs::path& candidate, const fs::path& root)
{
	const fs::path relative = candidate.lexically_relative(root);
	return IsSafeRelativePath(relative);
}

fs::path AddSuffix(fs::path path, std::wstring_view suffix)
{
	path += std::wstring(suffix);
	return path;
}

ContentType ToContentType(TitleIdParser::TITLE_TYPE type)
{
	switch (type)
	{
	case TitleIdParser::TITLE_TYPE::BASE_TITLE:
		return ContentType::BaseGame;
	case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO:
		return ContentType::Demo;
	case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE:
		return ContentType::Update;
	case TitleIdParser::TITLE_TYPE::AOC:
		return ContentType::Dlc;
	case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
	case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
		return ContentType::SystemTitle;
	case TitleIdParser::TITLE_TYPE::SYSTEM_DATA:
		return ContentType::SystemData;
	case TitleIdParser::TITLE_TYPE::HOMEBREW:
	case TitleIdParser::TITLE_TYPE::UNKNOWN:
	default:
		return ContentType::Unknown;
	}
}

void ThrowFilesystemError(std::string_view operation, const fs::path& path, const std::error_code& ec)
{
	throw std::runtime_error(fmt::format("{} '{}': {}", operation, _pathToUtf8(path), ec.message()));
}

void RemoveTreeChecked(const fs::path& path)
{
	std::error_code ec;
	if (!fs::exists(path, ec))
	{
		if (ec)
			ThrowFilesystemError("Could not inspect", path, ec);
		return;
	}
	fs::remove_all(path, ec);
	if (ec)
		ThrowFilesystemError("Could not remove", path, ec);
}

void RenameChecked(const fs::path& source, const fs::path& target)
{
	std::error_code ec;
	fs::rename(source, target, ec);
	if (ec)
		throw std::runtime_error(fmt::format("Could not rename '{}' to '{}': {}",
			_pathToUtf8(source), _pathToUtf8(target), ec.message()));
}

void CheckCancelled()
{
	if (s_cancelRequested.load(std::memory_order_relaxed))
		throw InstallCancelled{};
}

void SetCurrentFile(const fs::path& relative)
{
	std::lock_guard lock(s_stateMutex);
	s_snapshot.currentFile = _pathToUtf8(relative);
}

void AddCopiedBytes(uint64_t bytes)
{
	std::lock_guard lock(s_stateMutex);
	s_snapshot.copiedBytes += bytes;
}

void CompleteFile()
{
	std::lock_guard lock(s_stateMutex);
	s_snapshot.copiedFiles++;
}

void SetTerminalState(State state, std::string message)
{
	std::lock_guard lock(s_stateMutex);
	s_snapshot.state = state;
	s_snapshot.currentFile.clear();
	s_snapshot.message = std::move(message);
}

bool ValidateInstalledTitle(const fs::path& path, const InstallPlan& plan)
{
	TitleInfo title(path);
	return title.IsValid() && title.HasValidXmlInfo() &&
		title.GetAppTitleId() == plan.titleId && title.GetAppTitleVersion() == plan.version;
}

void RecoverPreviousTransaction(const InstallPlan& plan, const fs::path& backupPath)
{
	std::error_code ec;
	if (!fs::exists(backupPath, ec))
	{
		if (ec)
			ThrowFilesystemError("Could not inspect installer backup", backupPath, ec);
		return;
	}

	const bool targetExists = fs::exists(plan.targetPath, ec);
	if (ec)
		ThrowFilesystemError("Could not inspect install target", plan.targetPath, ec);
	if (!targetExists)
	{
		RenameChecked(backupPath, plan.targetPath);
		return;
	}

	if (!ValidateInstalledTitle(plan.targetPath, plan))
	{
		throw std::runtime_error(fmt::format(
			"An unfinished installation left both '{}' and '{}'. Neither was removed so the existing content can be recovered safely.",
			_pathToUtf8(plan.targetPath), _pathToUtf8(backupPath)));
	}
	RemoveTreeChecked(backupPath);
}

void CopyFileCancellable(const fs::path& source, const fs::path& target, std::vector<char>& buffer)
{
	std::ifstream input(source, std::ios::binary);
	if (!input)
		throw std::runtime_error(fmt::format("Could not open '{}' for reading", _pathToUtf8(source)));
	std::ofstream output(target, std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error(fmt::format("Could not open '{}' for writing", _pathToUtf8(target)));

	while (input)
	{
		CheckCancelled();
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize count = input.gcount();
		if (count <= 0)
			break;
		output.write(buffer.data(), count);
		if (!output)
			throw std::runtime_error(fmt::format("Could not write '{}'", _pathToUtf8(target)));
		AddCopiedBytes(static_cast<uint64_t>(count));
	}
	if (input.bad())
		throw std::runtime_error(fmt::format("Could not finish reading '{}'", _pathToUtf8(source)));
	output.flush();
	if (!output)
		throw std::runtime_error(fmt::format("Could not finish writing '{}'", _pathToUtf8(target)));
}

void CopyTitleTree(const InstallPlan& plan, const fs::path& stagingPath)
{
	std::vector<char> buffer(kCopyBufferSize);
	for (const char* directoryName : { "code", "content", "meta" })
	{
		const fs::path sourceDirectory = plan.sourceRoot / directoryName;
		std::error_code ec;
		fs::create_directories(stagingPath / directoryName, ec);
		if (ec)
			ThrowFilesystemError("Could not create", stagingPath / directoryName, ec);
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(sourceDirectory))
		{
			CheckCancelled();
			const fs::path relative = entry.path().lexically_relative(plan.sourceRoot);
			if (!IsSafeRelativePath(relative))
				throw std::runtime_error(fmt::format("Unsafe source path '{}'", _pathToUtf8(entry.path())));
			const fs::path target = stagingPath / relative;
			const fs::file_status status = entry.symlink_status(ec);
			if (ec)
				ThrowFilesystemError("Could not inspect", entry.path(), ec);
			if (fs::is_symlink(status))
				throw std::runtime_error(fmt::format("Symbolic links are not supported in install content: '{}'", _pathToUtf8(entry.path())));
			if (fs::is_directory(status))
			{
				fs::create_directories(target, ec);
				if (ec)
					ThrowFilesystemError("Could not create", target, ec);
				continue;
			}
			if (!fs::is_regular_file(status))
				throw std::runtime_error(fmt::format("Unsupported source item '{}'", _pathToUtf8(entry.path())));

			fs::create_directories(target.parent_path(), ec);
			if (ec)
				ThrowFilesystemError("Could not create", target.parent_path(), ec);
			SetCurrentFile(relative);
			CopyFileCancellable(entry.path(), target, buffer);
			CompleteFile();
		}
	}
}

void RestoreBackupAfterFailure(const fs::path& targetPath, const fs::path& backupPath, bool targetWasMoved)
{
	if (!targetWasMoved)
		return;
	std::error_code ec;
	const bool targetExists = fs::exists(targetPath, ec);
	if (ec)
	{
		cemuLog_log(LogType::Force, "UWP title installer could not inspect failed target '{}': {}", _pathToUtf8(targetPath), ec.message());
		return;
	}
	if (targetExists)
	{
		fs::remove_all(targetPath, ec);
		if (ec)
		{
			cemuLog_log(LogType::Force, "UWP title installer could not remove failed target '{}': {}", _pathToUtf8(targetPath), ec.message());
			return;
		}
	}
	ec.clear();
	const bool backupExists = fs::exists(backupPath, ec);
	if (ec)
	{
		cemuLog_log(LogType::Force, "UWP title installer could not inspect backup '{}': {}", _pathToUtf8(backupPath), ec.message());
		return;
	}
	if (!backupExists)
	{
		cemuLog_log(LogType::Force, "UWP title installer backup '{}' is missing", _pathToUtf8(backupPath));
		return;
	}
	fs::rename(backupPath, targetPath, ec);
	if (ec)
		cemuLog_log(LogType::Force, "UWP title installer could not restore backup '{}': {}", _pathToUtf8(backupPath), ec.message());
}

void RunInstall(InstallPlan plan)
{
	// every path below derives from these two, so long content paths work for copy, validation and cleanup
	plan.sourceRoot = ExtendedLengthPath(plan.sourceRoot);
	plan.targetPath = ExtendedLengthPath(plan.targetPath);
	const fs::path stagingPath = AddSuffix(plan.targetPath, L".installing");
	const fs::path backupPath = AddSuffix(plan.targetPath, L".backup");
	bool targetWasMoved = false;
	bool stagingWasPromoted = false;
	try
	{
		CheckCancelled();
		std::error_code ec;
		fs::create_directories(plan.targetPath.parent_path(), ec);
		if (ec)
			ThrowFilesystemError("Could not create MLC title directory", plan.targetPath.parent_path(), ec);

		RecoverPreviousTransaction(plan, backupPath);
		RemoveTreeChecked(stagingPath);
		fs::create_directories(stagingPath, ec);
		if (ec)
			ThrowFilesystemError("Could not create staging directory", stagingPath, ec);

		CopyTitleTree(plan, stagingPath);
		CheckCancelled();
		{
			std::lock_guard lock(s_stateMutex);
			if (s_snapshot.copiedBytes != plan.requiredBytes || s_snapshot.copiedFiles != plan.fileCount)
				throw std::runtime_error("The source title changed while it was being installed");
		}
		if (!ValidateInstalledTitle(stagingPath, plan))
			throw std::runtime_error("The copied title failed metadata validation");

		const bool targetExists = fs::exists(plan.targetPath, ec);
		if (ec)
			ThrowFilesystemError("Could not inspect install target", plan.targetPath, ec);
		if (targetExists)
		{
			RenameChecked(plan.targetPath, backupPath);
			targetWasMoved = true;
		}
		RenameChecked(stagingPath, plan.targetPath);
		stagingWasPromoted = true;
		if (!ValidateInstalledTitle(plan.targetPath, plan))
			throw std::runtime_error("The committed title failed final metadata validation");

		std::string message = fmt::format("{} installed", ContentTypeName(plan.type));
		if (targetWasMoved)
		{
			fs::remove_all(backupPath, ec);
			if (ec)
			{
				message += "; the previous-version backup could not be removed";
				cemuLog_log(LogType::Force, "UWP title installer could not remove backup '{}': {}", _pathToUtf8(backupPath), ec.message());
			}
		}
		SetTerminalState(State::Succeeded, std::move(message));
	}
	catch (const InstallCancelled&)
	{
		if (stagingWasPromoted)
		{
			std::error_code ec;
			fs::remove_all(plan.targetPath, ec);
		}
		else
		{
			std::error_code ec;
			fs::remove_all(stagingPath, ec);
		}
		RestoreBackupAfterFailure(plan.targetPath, backupPath, targetWasMoved);
		SetTerminalState(State::Cancelled, "Installation cancelled");
	}
	catch (const std::exception& ex)
	{
		if (stagingWasPromoted)
		{
			std::error_code ec;
			fs::remove_all(plan.targetPath, ec);
		}
		else
		{
			std::error_code ec;
			fs::remove_all(stagingPath, ec);
		}
		RestoreBackupAfterFailure(plan.targetPath, backupPath, targetWasMoved);
		cemuLog_log(LogType::Force, "UWP title installation failed: {}", ex.what());
		SetTerminalState(State::Failed, ex.what());
	}
}

void JoinFinishedWorker()
{
	if (s_worker.joinable())
		s_worker.join();
}
}

namespace CemuUwp::TitleInstaller
{
std::string_view ContentTypeName(ContentType type)
{
	switch (type)
	{
	case ContentType::BaseGame: return "Base game";
	case ContentType::Demo: return "Demo";
	case ContentType::Update: return "Update";
	case ContentType::Dlc: return "DLC";
	case ContentType::SystemTitle: return "System title";
	case ContentType::SystemData: return "System data";
	case ContentType::Unknown:
	default: return "Title";
	}
}

PlanResult BuildPlan(const fs::path& selectedFolder, const fs::path& mlcRoot)
{
	PlanResult result;
	try
	{
		InstallPlan plan;
		const fs::path selectedRoot = NormalizeSourceRoot(selectedFolder);
		std::error_code ec;
		if (!fs::is_directory(selectedRoot, ec) || ec)
		{
			throw std::runtime_error(fmt::format("The selected folder '{}' is not accessible{}",
				_pathToUtf8(selectedRoot), ec ? fmt::format(" ({})", ec.message()) : std::string{}));
		}
		plan.sourceRoot = FindTitleRoot(selectedRoot);
		cemuLog_log(LogType::Force, "UWP title installer: selected '{}', title folder '{}'",
			_pathToUtf8(selectedFolder), _pathToUtf8(plan.sourceRoot));
		if (!fs::is_regular_file(plan.sourceRoot / "meta" / "meta.xml", ec) || ec)
			throw std::runtime_error("The selected title is missing meta/meta.xml");

		TitleInfo title(plan.sourceRoot);
		if (!title.IsValid() || !title.HasValidXmlInfo())
			throw std::runtime_error("Cemu could not parse the selected title metadata");
		plan.titleId = title.GetAppTitleId();
		plan.version = title.GetAppTitleVersion();
		plan.type = ToContentType(TitleIdParser(plan.titleId).GetType());
		if (plan.type == ContentType::Unknown)
			throw std::runtime_error("This title type cannot be installed into the Wii U MLC");
		plan.titleName = title.GetMetaTitleName();
		std::replace(plan.titleName.begin(), plan.titleName.end(), '\r', ' ');
		std::replace(plan.titleName.begin(), plan.titleName.end(), '\n', ' ');
		if (plan.titleName.empty())
			plan.titleName = fmt::format("{:016x}", plan.titleId);

		const fs::path normalizedMlc = NormalizePath(mlcRoot);
		if (!fs::is_directory(normalizedMlc, ec) || ec)
			throw std::runtime_error("The configured MLC folder is not accessible");
		plan.targetPath = NormalizePath(normalizedMlc / title.GetInstallPath());
		if (!IsPathWithin(plan.targetPath, normalizedMlc))
			throw std::runtime_error("The title metadata resolved outside the configured MLC folder");
		if (IsPathWithin(plan.sourceRoot, plan.targetPath) || IsPathWithin(plan.targetPath, plan.sourceRoot))
			throw std::runtime_error("The selected source overlaps its MLC install destination");

		for (const char* directoryName : { "code", "content", "meta" })
		{
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(plan.sourceRoot / directoryName))
			{
				const fs::path relative = entry.path().lexically_relative(plan.sourceRoot);
				if (!IsSafeRelativePath(relative))
					throw std::runtime_error(fmt::format("Unsafe source path '{}'", _pathToUtf8(entry.path())));
				const fs::file_status status = entry.symlink_status(ec);
				if (ec)
					ThrowFilesystemError("Could not inspect", entry.path(), ec);
				if (fs::is_symlink(status))
					throw std::runtime_error(fmt::format("Symbolic links are not supported in install content: '{}'", _pathToUtf8(entry.path())));
				if (fs::is_regular_file(status))
				{
					const uint64_t size = entry.file_size(ec);
					if (ec)
						ThrowFilesystemError("Could not read the size of", entry.path(), ec);
					if (size > std::numeric_limits<uint64_t>::max() - plan.requiredBytes)
						throw std::runtime_error("The selected title is too large to install");
					plan.requiredBytes += size;
					plan.fileCount++;
				}
				else if (!fs::is_directory(status))
				{
					throw std::runtime_error(fmt::format("Unsupported source item '{}'", _pathToUtf8(entry.path())));
				}
			}
		}
		if (plan.fileCount == 0)
			throw std::runtime_error("The selected title does not contain any installable files");

		const fs::space_info space = fs::space(normalizedMlc, ec);
		if (ec)
			ThrowFilesystemError("Could not read free space for", normalizedMlc, ec);
		plan.availableBytes = space.available;
		const uint64_t reserve = std::clamp(plan.requiredBytes / 100, kMinimumFreeSpaceReserve, kMaximumFreeSpaceReserve);
		if (plan.requiredBytes > plan.availableBytes || reserve > plan.availableBytes - plan.requiredBytes)
			throw std::runtime_error(fmt::format("Not enough free space: {} bytes required and {} bytes available",
				plan.requiredBytes + reserve, plan.availableBytes));

		const bool targetExists = fs::exists(plan.targetPath, ec);
		if (ec)
			ThrowFilesystemError("Could not inspect existing title", plan.targetPath, ec);
		if (targetExists)
		{
			TitleInfo existing(plan.targetPath);
			if (!existing.IsValid() || !existing.HasValidXmlInfo())
			{
				plan.existingState = ExistingState::Invalid;
			}
			else
			{
				plan.existingTitleId = existing.GetAppTitleId();
				plan.existingVersion = existing.GetAppTitleVersion();
				plan.existingType = ToContentType(TitleIdParser(plan.existingTitleId).GetType());
				if (plan.existingType != plan.type)
					plan.existingState = ExistingState::DifferentType;
				else if (plan.existingVersion == plan.version)
					plan.existingState = ExistingState::SameVersion;
				else if (plan.existingVersion > plan.version)
					plan.existingState = ExistingState::Downgrade;
				else
					plan.existingState = ExistingState::Upgrade;
			}
		}

		result.plan = std::move(plan);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "UWP title install check failed: {}", ex.what());
		result.error = ex.what();
	}
	return result;
}

bool Start(const InstallPlan& plan)
{
	{
		std::lock_guard lock(s_stateMutex);
		if (s_snapshot.state == State::Installing || s_snapshot.state == State::Cancelling)
			return false;
	}
	JoinFinishedWorker();
	s_cancelRequested.store(false, std::memory_order_relaxed);
	{
		std::lock_guard lock(s_stateMutex);
		s_snapshot = {};
		s_snapshot.state = State::Installing;
		s_snapshot.requiredBytes = plan.requiredBytes;
		s_snapshot.fileCount = plan.fileCount;
	}
	try
	{
		s_worker = std::thread(RunInstall, plan);
		return true;
	}
	catch (const std::exception& ex)
	{
		SetTerminalState(State::Failed, fmt::format("Could not start the installer: {}", ex.what()));
		return false;
	}
}

void RequestCancel()
{
	s_cancelRequested.store(true, std::memory_order_relaxed);
	std::lock_guard lock(s_stateMutex);
	if (s_snapshot.state == State::Installing)
		s_snapshot.state = State::Cancelling;
}

Snapshot GetSnapshot()
{
	std::lock_guard lock(s_stateMutex);
	return s_snapshot;
}

bool IsBusy()
{
	std::lock_guard lock(s_stateMutex);
	return s_snapshot.state == State::Installing || s_snapshot.state == State::Cancelling;
}

void Reset()
{
	if (IsBusy())
		return;
	JoinFinishedWorker();
	std::lock_guard lock(s_stateMutex);
	s_snapshot = {};
	s_cancelRequested.store(false, std::memory_order_relaxed);
}

void Shutdown()
{
	RequestCancel();
	JoinFinishedWorker();
	std::lock_guard lock(s_stateMutex);
	s_snapshot = {};
	s_cancelRequested.store(false, std::memory_order_relaxed);
}
}
