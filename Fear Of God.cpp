#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#include "Offsets.hpp"

#pragma comment( lib, "user32.lib" )

#define REBASE( Base, Offset ) ( Base + Offset )

struct Vector2
{

    float X, Y;

};

struct Vector3
{

    float X, Y, Z;

};

struct Matrix4
{

    float Data[16];

};

struct Entity
{

    std::string Name;
    uintptr_t Team;
    Vector3 Position;
    Vector2 ScreenPosition;
    float LastUpdateTime = 0;
    Vector2 SmoothedPosition;
    float Velocity = 0;
    Vector2 LastPosition;

};

HANDLE ProcessHandle = nullptr;
HWND GameWindow = nullptr;
uintptr_t BaseAddress = 0;

std::atomic< bool > IsRunning = true;
std::vector< Entity > PlayerList;
std::mutex PlayerMutex;

template< typename T >
T ReadMemory(uintptr_t Address)
{

    T Value;
    ReadProcessMemory(ProcessHandle, (LPCVOID)Address, &Value, sizeof(T), nullptr);
    return Value;

}

std::string ReadString(uintptr_t Address)
{

    uintptr_t StringPtr = ReadMemory< uintptr_t >(Address);
    if (!StringPtr)
        return "";

    int32_t Size = ReadMemory< int32_t >(StringPtr + Engine::Instance::ClassDescriptor);
    if (Size <= 0 || Size > 256)
        return "";

    if (Size >= 16)
        StringPtr = ReadMemory< uintptr_t >(StringPtr);

    char Buffer[256] = { 0 };
    ReadProcessMemory(ProcessHandle, (LPCVOID)StringPtr, Buffer, (std::min)(Size, 255), nullptr);
    return std::string(Buffer);

}

uintptr_t FindFirstChild(uintptr_t Parent, const std::string& ChildName)
{

    uintptr_t ChildrenPtr = ReadMemory< uintptr_t >(Parent + Engine::Instance::Children);
    if (!ChildrenPtr)
        return 0;

    uintptr_t Start = ReadMemory< uintptr_t >(ChildrenPtr);
    uintptr_t End = ReadMemory< uintptr_t >(ChildrenPtr + sizeof(uintptr_t));

    for (uintptr_t i = Start; i < End; i += (sizeof(uintptr_t) * 2))
    {

        uintptr_t Child = ReadMemory< uintptr_t >(i);
        if (!Child)
            continue;

        std::string Name = ReadString(Child + Engine::Instance::Name);
        if (Name == ChildName)
            return Child;

    }

    return 0;

}

Vector3 GetPosition(uintptr_t Instance)
{

    if (!Instance)
        return Vector3();

    uintptr_t Primitive = ReadMemory< uintptr_t >(Instance + Engine::Instance::BasePartPrimitive);
    if (!Primitive || Primitive < 0x10000 || Primitive >= 0x7FFFFFFFFFFF)
        return Vector3();

    return ReadMemory< Vector3 >(Primitive + Engine::Instance::BasePartPosition);

}

Vector2 WorldToScreen(const Vector3& Position, const Matrix4& ViewMatrix, const Vector2& DisplaySize)
{

    const float W = (Position.X * ViewMatrix.Data[12]) + (Position.Y * ViewMatrix.Data[13]) + (Position.Z * ViewMatrix.Data[14]) + ViewMatrix.Data[15];
    if (W < 0.1f)
        return Vector2(-1, -1);

    const float WInv = 1.0f / W;
    const float Xndc = ((Position.X * ViewMatrix.Data[0]) + (Position.Y * ViewMatrix.Data[1]) + (Position.Z * ViewMatrix.Data[2]) + ViewMatrix.Data[3]) * WInv;
    const float Yndc = ((Position.X * ViewMatrix.Data[4]) + (Position.Y * ViewMatrix.Data[5]) + (Position.Z * ViewMatrix.Data[6]) + ViewMatrix.Data[7]) * WInv;

    return Vector2((DisplaySize.X * 0.5f) + (DisplaySize.X * 0.5f * Xndc), (DisplaySize.Y * 0.5f) - (DisplaySize.Y * 0.5f * Yndc));

}

void UpdatePlayers()
{

    uintptr_t Engine = ReadMemory< uintptr_t >(BaseAddress + Engine::Game::EnginePtr);
    if (!Engine)
        return;

    uintptr_t DataModel = ReadMemory< uintptr_t >(ReadMemory< uintptr_t >(Engine + Engine::Game::Padding) + Engine::Game::Instance);
    if (!DataModel)
        return;

    uintptr_t PlayersService = FindFirstChild(DataModel, "Players");
    if (!PlayersService)
        return;

    uintptr_t LocalPlayer = ReadMemory< uintptr_t >(PlayersService + Engine::Players::LocalPlayer);
    if (!LocalPlayer)
        return;

    Matrix4 ViewMatrix = ReadMemory< Matrix4 >(Engine + Engine::Game::ViewMatrix);
    RECT ClientRect;
    GetClientRect(GameWindow, &ClientRect);
    Vector2 DisplaySize = { static_cast<float>(ClientRect.right), static_cast<float>(ClientRect.bottom) };

    std::vector< Entity > NewPlayers;
    uintptr_t PlayersList = ReadMemory< uintptr_t >(PlayersService + Engine::Instance::Children);
    if (!PlayersList)
        return;

    uintptr_t Start = ReadMemory< uintptr_t >(PlayersList);
    uintptr_t End = ReadMemory< uintptr_t >(PlayersList + sizeof(uintptr_t));

    std::vector<std::string> Parts =
    {

        // R15 Parts
        "Head",
        "UpperTorso",
        "LowerTorso",
        "LeftUpperArm",
        "LeftLowerArm",
        "LeftHand",
        "RightUpperArm",
        "RightLowerArm",
        "RightHand",
        "LeftUpperLeg",
        "LeftLowerLeg",
        "LeftFoot",
        "RightUpperLeg",
        "RightLowerLeg",
        "RightFoot",

        // R6 Parts
        "Torso",
        "Left Arm",
        "Right Arm",
        "Left Leg",
        "Right Leg"

    };

    for (uintptr_t i = Start; i < End; i += (sizeof(uintptr_t) * 2))
    {

        uintptr_t Player = ReadMemory< uintptr_t >(i);
        if (!Player || Player == LocalPlayer)
            continue;

        uintptr_t Character = ReadMemory< uintptr_t >(Player + Engine::Players::Character);
        if (!Character)
            continue;

        Entity Entity;
        Entity.Name = ReadString(Player + Engine::Instance::Name);

        for (const auto& partName : Parts)
        {

            uintptr_t Part = FindFirstChild(Character, partName);
            if (!Part)
                continue;

            Entity.Position = ReadMemory<Vector3>(ReadMemory<uintptr_t>(Part + Engine::Instance::BasePartPrimitive) + Engine::Instance::BasePartPosition);
            Entity.ScreenPosition = WorldToScreen(Entity.Position, ViewMatrix, DisplaySize);

            NewPlayers.push_back(Entity);

        }

    }

    std::lock_guard< std::mutex > Lock(PlayerMutex);
    PlayerList = std::move(NewPlayers);

}

void DataThread()
{

    while (IsRunning)
    {

        if (IsWindow(GameWindow) && GetForegroundWindow() == GameWindow)
            UpdatePlayers();
        else if (!IsWindow(GameWindow))
            IsRunning = false;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    }

}

void AimBot()
{

    while (IsRunning)
    {

        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
        {

            POINT MousePos;
            GetCursorPos(&MousePos);
            ScreenToClient(GameWindow, &MousePos);

            std::lock_guard< std::mutex > Lock(PlayerMutex);
            float ClosestDistance = FLT_MAX;
            Entity* TargetPlayer = nullptr;

            for (auto& Player : PlayerList)
            {

                float DistX = Player.ScreenPosition.X - MousePos.x;
                float DistY = Player.ScreenPosition.Y - MousePos.y;
                float Distance = sqrt(DistX * DistX + DistY * DistY);

                if (Distance < ClosestDistance)
                {

                    ClosestDistance = Distance;
                    TargetPlayer = &Player;

                }

            }

            if (TargetPlayer && ClosestDistance < 300.0f)
            {

                float SmoothX = 3.0f + (ClosestDistance * 0.01f);
                float SmoothY = 3.0f + (ClosestDistance * 0.01f);

                INPUT Input = { 0 };
                Input.type = INPUT_MOUSE;
                Input.mi.dwFlags = MOUSEEVENTF_MOVE;
                Input.mi.dx = static_cast<LONG>((TargetPlayer->ScreenPosition.X - MousePos.x) / SmoothX);
                Input.mi.dy = static_cast<LONG>((TargetPlayer->ScreenPosition.Y - MousePos.y) / SmoothY);
                SendInput(1, &Input, sizeof(INPUT));

            }

        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    }

}

void TriggerBot()
{

    const float Radius = 10.0f;

    while (IsRunning)
    {

        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
        {

            RECT rect;
            GetClientRect(GameWindow, &rect);
            Vector2 Crosshair =
            {

                static_cast<float>(rect.right) / 2.0f,
                static_cast<float>(rect.bottom) / 2.0f

            };

            std::lock_guard<std::mutex> lock(PlayerMutex);

            for (auto& player : PlayerList)
            {

                float dx = player.ScreenPosition.X - Crosshair.X;
                float dy = player.ScreenPosition.Y - Crosshair.Y;
                float dist = sqrt(dx * dx + dy * dy);

                if (dist <= Radius)
                {

                    INPUT input[2] = {};
                    input[0].type = INPUT_MOUSE;
                    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    input[1].type = INPUT_MOUSE;
                    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

                    SendInput(2, input, sizeof(INPUT));
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    break;

                }

            }

        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    }

}

int main()
{

    ShowWindow(GetConsoleWindow(), SW_HIDE);

    GameWindow = FindWindowA(nullptr, "Roblox");
    if (!GameWindow)
        return 1;

    DWORD ProcessId;
    GetWindowThreadProcessId(GameWindow, &ProcessId);
    ProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ProcessId);

    MODULEENTRY32 ModuleEntry = { sizeof(MODULEENTRY32) };
    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ProcessId);
    if (Module32First(Snapshot, &ModuleEntry))
        BaseAddress = (uintptr_t)ModuleEntry.modBaseAddr;
    CloseHandle(Snapshot);

    std::thread(DataThread).detach();
    std::thread(AimBot).detach();
    std::thread(TriggerBot).detach();

    while (IsRunning)
    {

        if (!IsWindow(GameWindow))
            IsRunning = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    }

    CloseHandle(ProcessHandle);
    return 0;

}