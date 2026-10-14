#pragma once
#include <windows.h>

// Тёмная тема: все цвета окна, графиков и таблиц (они же — в сохранённых PNG)
namespace th {
constexpr COLORREF bg = RGB(24, 26, 32);          // фон окна
constexpr COLORREF panel = RGB(34, 37, 45);       // кнопки, списки, полоса состояния
constexpr COLORREF panelHot = RGB(43, 47, 57);    // под курсором
constexpr COLORREF panelDown = RGB(52, 57, 69);   // нажата
constexpr COLORREF border = RGB(54, 59, 72);
constexpr COLORREF text = RGB(228, 231, 237);
constexpr COLORREF muted = RGB(148, 155, 169);
constexpr COLORREF disabled = RGB(88, 94, 106);
constexpr COLORREF accent = RGB(77, 140, 255);
constexpr COLORREF accentHot = RGB(104, 159, 255);
constexpr COLORREF danger = RGB(226, 85, 78);
constexpr COLORREF warn = RGB(255, 170, 84);
constexpr COLORREF chartBg = RGB(17, 19, 24);
constexpr COLORREF grid = RGB(35, 39, 48);
constexpr COLORREF gridHi = RGB(52, 58, 71);
constexpr COLORREF frame = RGB(60, 66, 80);
constexpr COLORREF mark = RGB(118, 126, 142);     // границы кэшей на графике
constexpr COLORREF tableHead = RGB(36, 40, 49);
constexpr COLORREF tableAlt = RGB(29, 32, 39);
}
