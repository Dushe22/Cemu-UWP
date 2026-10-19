#include "Fiber.h"

#include <Windows.h>

#include <system_error>

thread_local Fiber* sCurrentFiber{};

namespace
{
constexpr SIZE_T kFiberStackReserveSize = 2 * 1024 * 1024;
constexpr DWORD kFiberFlags = FIBER_FLAG_FLOAT_SWITCH;

[[noreturn]] void ThrowLastError(const char* operation)
{
	throw std::system_error((int)GetLastError(), std::system_category(), operation);
}
}

Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	LPVOID fiberHandle = CreateFiberEx(0, kFiberStackReserveSize, kFiberFlags, (LPFIBER_START_ROUTINE)FiberEntryPoint, userParam);
	if (!fiberHandle)
		ThrowLastError("CreateFiberEx");
	m_implData = fiberHandle;
	m_stackPtr = nullptr;
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
	LPVOID fiberHandle = ConvertThreadToFiberEx(nullptr, kFiberFlags);
	if (!fiberHandle)
		ThrowLastError("ConvertThreadToFiberEx");
	m_implData = fiberHandle;
	m_stackPtr = nullptr;
}

Fiber::~Fiber()
{
	DeleteFiber((LPVOID)m_implData);
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	cemu_assert_debug(sCurrentFiber == nullptr);
	Fiber* currentFiber = new Fiber(privateData);
	sCurrentFiber = currentFiber;
	return currentFiber;
}

void Fiber::Switch(Fiber& targetFiber)
{
	sCurrentFiber = &targetFiber;
	SwitchToFiber((LPVOID)targetFiber.m_implData);
}

void* Fiber::GetFiberPrivateData()
{
	return sCurrentFiber ? sCurrentFiber->m_privateData : nullptr;
}
