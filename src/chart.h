// BenchKrasulya — рисование графиков и таблиц (GDI)
#pragma once
#include <windows.h>
#include <cmath>
#include <string>
#include <vector>

enum XKind { X_SIZE, X_JUMP, X_COUNT };

struct Series {
    std::wstring name;
    COLORREF col = RGB(255, 255, 255);
    bool dash = false;
    std::vector<double> x, y;
    bool band = false;  // линия — скользящее среднее, вокруг неё полупрозрачная полоса «мин–макс»
    int panel = 0;      // номер панели, если график разбит на панели (Chart::rows × cols)
};

struct Mark {
    double v;
    std::wstring label;
    COLORREF col;
};

struct Chart {
    std::wstring header;       // строка сверху справа (процессор, ядро, частота)
    std::wstring legendTitle;  // «Чтение блока:»
    std::wstring xTitle, yUnit;
    XKind xKind = X_SIZE;
    bool yLog = false;
    double xmin = 0, xmax = 1;
    double yminFixed = NAN, ymaxFixed = NAN;
    std::vector<Series> s;
    std::vector<Mark> vmarks, hmarks;
    std::wstring empty;        // текст, если данных нет
    int rows = 1, cols = 1;    // > 1 — отдельная панель на каждую группу линий (Series::panel), панели идут по строкам
    std::vector<std::wstring> panelTitle;
    std::vector<std::wstring> colTitle;  // крупная подпись над каждым столбцом панелей
};

struct Table {
    std::vector<std::wstring> head;
    std::vector<std::vector<std::wstring>> rows;
    std::vector<COLORREF> swatch;  // цвет квадратика в начале строки или CLR_INVALID
    std::vector<bool> swatchDash;
};

struct ChartFonts {
    HFONT normal, bold, table, tableBold;
    int dpi;
};

void DrawChart(HDC dc, const RECT& rc, const Chart& c, const ChartFonts& f, POINT hover);
int DrawTable(HDC dc, int x, int y, const Table& t, const ChartFonts& f, bool measureOnly);
std::wstring FmtGB(double v);
std::wstring FmtJump(double v);
