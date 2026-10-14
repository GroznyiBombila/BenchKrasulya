// BenchKrasulya — эмуляция чужого железа для проверки (только тестовая сборка, -DBK_EMU).
// Подменяет CPUID, топологию Windows (GetLogicalProcessorInformationEx) и таблицу SMBIOS
// данными профиля. Тесты при этом реально выполняются на ядрах этого ПК.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct LogicalCpu;

bool EmuParseArgs(int argc, wchar_t** argv);  // --emu <профиль>, --emu-list <файл>, --emu-fuzz <N> <файл>
bool EmuActive();
std::wstring EmuTitle();                      // «ЭМУЛЯЦИЯ: …» или пусто
bool EmuCpuid(unsigned leaf, unsigned sub, unsigned r[4]);
bool EmuGlpi(std::vector<BYTE>& buf, bool& ok);
bool EmuSmbios(std::vector<BYTE>& buf, bool& ok);
bool EmuPin(const LogicalCpu& c, bool& ok);
uint64_t EmuTotalMB();
