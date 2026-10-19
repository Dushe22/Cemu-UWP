#pragma once

#include <string>
#include <string_view>

namespace UwpImGuiFrontend
{
	struct TextureHandle;
}

namespace CemuUwp::Frontend
{
	enum class TextEditingCommand
	{
		Backspace,
		Delete,
		MoveLeft,
		MoveRight,
		MoveHome,
		MoveEnd,
		Submit,
	};

	void Initialize();
	void SetActive(bool active);
	void BeginFrame();
	void Draw();
	void TickGameplayInput();
	bool WantsGameplayOverlay();
	bool IsGameplayInputCaptured();
	bool IsInputMappingReadActive();
	bool IsInputMapperOpen();

	class ScopedInputMappingRead final
	{
	public:
		ScopedInputMappingRead() noexcept;
		~ScopedInputMappingRead();
		ScopedInputMappingRead(const ScopedInputMappingRead&) = delete;
		ScopedInputMappingRead& operator=(const ScopedInputMappingRead&) = delete;
	};

	void DrawGameplayOverlay();
	void PushGameplayDialogStyle();
	void PopGameplayDialogStyle();
	void Shutdown();
	void RequestBack();
	void SetStatus(std::string message);
	void PushNotification(std::string message);
	[[nodiscard]] UwpImGuiFrontend::TextureHandle GetControllerPreviewTexture(
		std::string_view assetName);
	void HandleTextEditingCommand(TextEditingCommand command, bool extendSelection);
};
