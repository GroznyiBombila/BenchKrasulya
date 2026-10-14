// BenchKrasulya — рисование графиков и таблиц (GDI)
#include "chart.h"
#include "sysinfo.h"
#include "theme.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>

static int Sc(const ChartFonts& f, int v) { return MulDiv(v, f.dpi, 96); }

std::wstring FmtGB(double v)
{
    if (!std::isfinite(v)) return L"—";
    if (v >= 100) return Fmt(L"%.0f", v);
    if (v >= 10) return Fmt(L"%.1f", v);
    return Fmt(L"%.2f", v);
}

std::wstring FmtJump(double v)
{
    int i = (int)std::lround(v);
    if (i == 0) return L"0";
    if (i % 1024 == 0) return Fmt(L"%dК", i / 1024);
    return Fmt(L"%d", i);
}

static double NiceStep(double raw)
{
    if (raw <= 0) return 1;
    double e = std::pow(10.0, std::floor(std::log10(raw)));
    double f = raw / e;
    double nf = f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10;
    return nf * e;
}

static std::wstring FmtTick(double v)
{
    if (std::fabs(v) >= 100 || std::fabs(v - std::round(v)) < 1e-9) return Fmt(L"%.0f", v);
    if (std::fabs(v) >= 1) return Fmt(L"%.1f", v);
    return Fmt(L"%g", v);
}

static void TextAt(HDC dc, int x, int y, const std::wstring& s, UINT align)
{
    SetTextAlign(dc, align);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
}

static SIZE TextSize(HDC dc, const std::wstring& s)
{
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    return sz;
}

static HPEN MakePen(COLORREF c, int w, bool dash)
{
    if (!dash) return CreatePen(PS_SOLID, w, c);
    LOGBRUSH lb = {BS_SOLID, c, 0};
    DWORD style[2] = {(DWORD)(w * 4 + 2), (DWORD)(w * 3 + 1)};
    return ExtCreatePen(PS_GEOMETRIC | PS_USERSTYLE | PS_ENDCAP_FLAT, w, &lb, 2, style);
}

static size_t Nearest(const std::vector<double>& xs, double v)
{
    if (xs.empty()) return 0;
    auto it = std::lower_bound(xs.begin(), xs.end(), v);
    if (it == xs.end()) return xs.size() - 1;
    size_t i = it - xs.begin();
    if (i > 0 && std::fabs(xs[i - 1] - v) < std::fabs(xs[i] - v)) i--;
    return i;
}

// скользящие минимум, максимум и среднее по 8 соседним точкам (для Series::band)
static void Envelope(const std::vector<double>& y, std::vector<double>& lo, std::vector<double>& hi, std::vector<double>& mean)
{
    const long n = (long)y.size(), half = 4;
    lo.assign(n, NAN);
    hi.assign(n, NAN);
    mean.assign(n, NAN);
    for (long i = 0; i < n; i++) {
        if (!std::isfinite(y[i])) continue;
        double mn = INFINITY, mx = -INFINITY, sum = 0;
        int k = 0;
        for (long j = std::max(0L, i - half); j < std::min(n, i + half); j++)
            if (std::isfinite(y[j])) {
                mn = std::min(mn, y[j]);
                mx = std::max(mx, y[j]);
                sum += y[j];
                k++;
            }
        lo[i] = mn, hi[i] = mx, mean[i] = sum / k;
    }
}

// полупрозрачная заливка между двумя ломаными (GDI не умеет прозрачность — рисует GDI+)
static void FillBand(HDC dc, const std::vector<POINT>& up, const std::vector<POINT>& dn, COLORREF col, BYTE alpha)
{
    if (up.size() < 2 || up.size() != dn.size()) return;
    std::vector<Gdiplus::Point> pts;
    for (auto& p : up) pts.push_back(Gdiplus::Point(p.x, p.y));
    for (size_t i = dn.size(); i-- > 0;) pts.push_back(Gdiplus::Point(dn[i].x, dn[i].y));
    Gdiplus::Graphics g(dc);
    Gdiplus::SolidBrush b(Gdiplus::Color(alpha, GetRValue(col), GetGValue(col), GetBValue(col)));
    g.FillPolygon(&b, pts.data(), (INT)pts.size());
}

// panel: график — одна из панелей; xLabels: подписывать ось X (только у нижнего ряда панелей)
static void DrawOne(HDC dc, const RECT& rc, const Chart& c0, const ChartFonts& f, POINT hover, bool panel, bool xLabels, const std::wstring& title)
{
    HBRUSH black = CreateSolidBrush(th::chartBg);
    FillRect(dc, &rc, black);
    DeleteObject(black);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, f.normal);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int lh = tm.tmHeight;

    RECT P = {rc.left + Sc(f, 60), rc.top + (panel ? Sc(f, 4) : lh + Sc(f, 12)), rc.right - Sc(f, 16),
              rc.bottom - (xLabels ? lh * 2 + Sc(f, 10) : Sc(f, 4))};
    if (P.right - P.left < 50 || P.bottom - P.top < (panel ? 24 : 50)) {
        SelectObject(dc, oldFont);
        return;
    }

    // ---- линии с полосой: рисуем среднее, а мин/макс храним для заливки
    Chart c = c0;
    std::vector<std::vector<double>> bandLo(c.s.size()), bandHi(c.s.size());
    for (size_t k = 0; k < c.s.size(); k++)
        if (c.s[k].band) {
            std::vector<double> mean;
            Envelope(c0.s[k].y, bandLo[k], bandHi[k], mean);
            c.s[k].y = mean;
        }

    // ---- заголовок справа сверху и единицы слева сверху
    if (!panel) {
        SetTextColor(dc, th::text);
        TextAt(dc, rc.right - Sc(f, 16), rc.top + Sc(f, 5), c.header, TA_RIGHT | TA_TOP);
        SetTextColor(dc, th::muted);
        TextAt(dc, rc.left + Sc(f, 8), rc.top + Sc(f, 5), c.yUnit, TA_LEFT | TA_TOP);
    }

    // ---- некорректный диапазон X (график ещё не настроен) — только надпись
    if (!(c.xmax > c.xmin) || (c.xKind == X_SIZE && c.xmin <= 0) || (c.xKind == X_COUNT && c.xmax - c.xmin > 100000)) {
        SetTextColor(dc, RGB(200, 200, 200));
        TextAt(dc, (P.left + P.right) / 2, (P.top + P.bottom) / 2 - lh / 2, c.empty, TA_CENTER | TA_TOP);
        SelectObject(dc, oldFont);
        return;
    }

    // ---- диапазон Y
    double lo = INFINITY, hi = -INFINITY;
    for (auto& s : c0.s)  // по исходным точкам: полоса доходит до них
        for (double v : s.y)
            if (std::isfinite(v) && (!c.yLog || v > 0)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
    for (auto& m : c.hmarks) hi = std::max(hi, m.v);
    bool hasData = std::isfinite(lo);
    if (!hasData) {
        lo = c.yLog ? 1 : 0;
        hi = c.yLog ? 100 : 10;
    }
    double step = 0;
    if (c.yLog) {
        lo = std::pow(10.0, std::floor(std::log10(lo)));
        hi = std::pow(10.0, std::ceil(std::log10(hi * 1.02)));
        if (hi <= lo) hi = lo * 10;
    } else {
        lo = 0;
        int ticks = std::max(2, std::min(8, (int)((P.bottom - P.top) / (lh * 1.8))));
        step = NiceStep(hi * 1.08 / ticks);
        hi = std::max(step, std::ceil(hi * 1.08 / step) * step);
    }
    if (std::isfinite(c.yminFixed)) lo = c.yminFixed;
    if (std::isfinite(c.ymaxFixed)) hi = c.ymaxFixed;

    const double W = P.right - P.left, H = P.bottom - P.top;
    auto X = [&](double v) -> double {
        if (c.xKind == X_SIZE) return P.left + W * (std::log2(v) - std::log2(c.xmin)) / (std::log2(c.xmax) - std::log2(c.xmin));
        return P.left + W * (v - c.xmin) / (c.xmax - c.xmin);
    };
    auto Xinv = [&](double px) -> double {
        double t = (px - P.left) / W;
        if (c.xKind == X_SIZE) return std::pow(2.0, std::log2(c.xmin) + t * (std::log2(c.xmax) - std::log2(c.xmin)));
        return c.xmin + t * (c.xmax - c.xmin);
    };
    auto Y = [&](double v) -> double {
        if (c.yLog) return P.bottom - H * (std::log10(v) - std::log10(lo)) / (std::log10(hi) - std::log10(lo));
        return P.bottom - H * (v - lo) / (hi - lo);
    };

    HPEN grid = CreatePen(PS_SOLID, 1, th::grid);
    HPEN gridHi = CreatePen(PS_SOLID, 1, th::gridHi);
    HGDIOBJ oldPen = SelectObject(dc, grid);
    SetTextColor(dc, th::muted);

    // ---- сетка Y
    auto hline = [&](double v, HPEN pen) {
        int y = (int)std::lround(Y(v));
        SelectObject(dc, pen);
        MoveToEx(dc, P.left, y, nullptr);
        LineTo(dc, P.right, y);
        return y;
    };
    if (c.yLog) {
        for (double d = lo; d <= hi * 1.0001; d *= 10)
            for (double m : {1.0, 2.0, 5.0}) {
                double v = d * m;
                if (v > hi * 1.0001) break;
                int y = hline(v, m == 1 ? gridHi : grid);
                TextAt(dc, P.left - Sc(f, 5), y - lh / 2, FmtTick(v), TA_RIGHT | TA_TOP);
            }
    } else {
        for (double v = lo; v <= hi + step * 0.01; v += step) {
            int y = hline(v, grid);
            TextAt(dc, P.left - Sc(f, 5), y - lh / 2, FmtTick(v), TA_RIGHT | TA_TOP);
        }
    }

    // ---- сетка X
    auto vline = [&](double v, HPEN pen, bool label, const std::wstring& text) {
        int x = (int)std::lround(X(v));
        SelectObject(dc, pen);
        MoveToEx(dc, x, P.top, nullptr);
        LineTo(dc, x, P.bottom);
        if (label && xLabels) TextAt(dc, x, P.bottom + Sc(f, 4), text, TA_CENTER | TA_TOP);
    };
    if (c.xKind == X_SIZE) {
        int e0 = (int)std::ceil(std::log2(c.xmin) - 1e-9), e1 = (int)std::floor(std::log2(c.xmax) + 1e-9);
        double pxOct = W / (std::log2(c.xmax) - std::log2(c.xmin));
        int k = pxOct < Sc(f, 34) ? 2 : 1;
        for (int e = e0; e <= e1; e++) vline(std::ldexp(1.0, e), (e % 10 == 0) ? gridHi : grid, e % k == 0, FormatSizeAxis(std::ldexp(1.0, e)));
    } else if (c.xKind == X_JUMP) {
        for (double v = std::ceil(c.xmin / 1024) * 1024; v <= c.xmax; v += 1024) vline(v, grid, true, FmtJump(v));
        HPEN z = MakePen(RGB(200, 200, 200), 1, true);
        vline(0, z, false, L"");
        DeleteObject(z);
    } else {
        double px = W / std::max(1.0, c.xmax - c.xmin);
        int k = 1;
        while (px * k < Sc(f, 26)) k = k == 1 ? 2 : k == 2 ? 4 : k == 4 ? 8 : k * 2;
        for (int v = (int)c.xmin; v <= (int)c.xmax; v++)
            if (v == 1 || v % k == 0) vline(v, grid, true, Fmt(L"%d", v));
    }
    SetTextColor(dc, th::muted);
    if (xLabels) TextAt(dc, (P.left + P.right) / 2, P.bottom + Sc(f, 4) + lh, c.xTitle, TA_CENTER | TA_TOP);

    // ---- вертикальные отметки (границы кэшей, кластеры ядер)
    int markRow = 0;
    for (auto& m : c.vmarks) {
        if (m.v < c.xmin || m.v > c.xmax) continue;
        HPEN p = MakePen(m.col, 1, true);
        int x = (int)std::lround(X(m.v));
        SelectObject(dc, p);
        MoveToEx(dc, x, P.top, nullptr);
        LineTo(dc, x, P.bottom);
        SelectObject(dc, grid);
        DeleteObject(p);
        SetTextColor(dc, m.col);
        TextAt(dc, x + Sc(f, 3), P.bottom - lh * (1 + markRow % 2) - Sc(f, 2), m.label, TA_LEFT | TA_TOP);
        markRow++;
    }
    for (auto& m : c.hmarks) {
        HPEN p = MakePen(m.col, Sc(f, 1), true);
        SelectObject(dc, p);
        int y = (int)std::lround(Y(m.v));
        MoveToEx(dc, P.left, y, nullptr);
        LineTo(dc, P.right, y);
        SelectObject(dc, grid);
        DeleteObject(p);
        SetTextColor(dc, m.col);
        TextAt(dc, P.left + Sc(f, 6), y - lh - 1, m.label, TA_LEFT | TA_TOP);
    }

    // ---- линии данных
    HRGN clip = CreateRectRgn(P.left, P.top - 1, P.right + 1, P.bottom + 1);
    SelectClipRgn(dc, clip);
    for (size_t k = 0; k < c.s.size(); k++) {  // сначала все полосы, поверх — линии
        if (!c.s[k].band) continue;
        std::vector<POINT> up, dn;
        auto flush = [&] {
            FillBand(dc, up, dn, c.s[k].col, panel ? 64 : 44);
            up.clear();
            dn.clear();
        };
        for (size_t i = 0; i < c.s[k].x.size() && i < bandLo[k].size(); i++) {
            if (!std::isfinite(bandLo[k][i])) {
                flush();
                continue;
            }
            int x = (int)std::lround(X(c.s[k].x[i]));
            up.push_back({x, (int)std::lround(Y(bandHi[k][i]))});
            dn.push_back({x, (int)std::lround(Y(bandLo[k][i]))});
        }
        flush();
    }
    for (auto& s : c.s) {
        HPEN p = MakePen(s.col, s.dash ? Sc(f, 1) + 1 : Sc(f, 2), s.dash);
        SelectObject(dc, p);
        bool pen = false;
        for (size_t i = 0; i < s.x.size() && i < s.y.size(); i++) {
            double v = s.y[i];
            if (!std::isfinite(v) || (c.yLog && v <= 0)) {
                pen = false;
                continue;
            }
            int x = (int)std::lround(X(s.x[i])), y = (int)std::lround(Y(v));
            if (!pen) MoveToEx(dc, x, y, nullptr);
            else LineTo(dc, x, y);
            pen = true;
        }
        SelectObject(dc, grid);
        DeleteObject(p);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // ---- рамка
    HPEN frame = CreatePen(PS_SOLID, 1, th::frame);
    SelectObject(dc, frame);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, P.left, P.top, P.right + 1, P.bottom + 1);

    // ---- подпись панели слева сверху
    if (panel && !title.empty()) {
        SelectObject(dc, f.bold);
        SetTextColor(dc, c.s.size() == 1 ? c.s[0].col : th::text);
        TextAt(dc, P.left + Sc(f, 8), P.top + Sc(f, 4), title, TA_LEFT | TA_TOP);
        SelectObject(dc, f.normal);
    }

    // ---- легенда в правом верхнем углу графика
    if (!c.s.empty() && !panel) {
        int sample = Sc(f, 18), gap = Sc(f, 10);
        int total = TextSize(dc, c.legendTitle).cx + gap;
        for (auto& s : c.s) total += sample + Sc(f, 4) + TextSize(dc, s.name).cx + gap;
        int x = std::max<int>(P.left + Sc(f, 4), P.right - total - Sc(f, 6)), y = P.top + Sc(f, 4);
        RECT box = {x - Sc(f, 6), y - Sc(f, 2), P.right - Sc(f, 2), y + lh + Sc(f, 2)};
        HBRUSH bb = CreateSolidBrush(th::chartBg);
        FillRect(dc, &box, bb);
        DeleteObject(bb);
        SetTextColor(dc, th::muted);
        TextAt(dc, x, y, c.legendTitle, TA_LEFT | TA_TOP);
        x += TextSize(dc, c.legendTitle).cx + gap;
        for (auto& s : c.s) {
            HPEN p = MakePen(s.col, s.dash ? Sc(f, 1) + 1 : Sc(f, 2), s.dash);
            SelectObject(dc, p);
            MoveToEx(dc, x, y + lh / 2, nullptr);
            LineTo(dc, x + sample, y + lh / 2);
            SelectObject(dc, frame);
            DeleteObject(p);
            x += sample + Sc(f, 4);
            SetTextColor(dc, s.col);
            TextAt(dc, x, y, s.name, TA_LEFT | TA_TOP);
            x += TextSize(dc, s.name).cx + gap;
        }
    }

    if (!hasData && !c.empty.empty()) {
        SetTextColor(dc, RGB(200, 200, 200));
        TextAt(dc, (P.left + P.right) / 2, (P.top + P.bottom) / 2 - lh / 2, c.empty, TA_CENTER | TA_TOP);
    }

    // ---- значения под курсором
    if (hasData && hover.x >= P.left && hover.x <= P.right && hover.y >= P.top && hover.y <= P.bottom) {
        double xv = Xinv(hover.x);
        const Series* base = nullptr;
        for (auto& s : c.s)
            if (!s.x.empty()) { base = &s; break; }
        if (base) {
            size_t bi = Nearest(base->x, xv);
            double xa = base->x[bi];
            int xpx = (int)std::lround(X(xa));
            HPEN cur = MakePen(RGB(160, 160, 160), 1, true);
            SelectObject(dc, cur);
            MoveToEx(dc, xpx, P.top, nullptr);
            LineTo(dc, xpx, P.bottom);
            SelectObject(dc, frame);
            DeleteObject(cur);
            std::vector<std::pair<std::wstring, COLORREF>> lines;
            if (c.xKind == X_SIZE) lines.push_back({L"Размер: " + FormatSize((uint64_t)xa), RGB(255, 255, 255)});
            else if (c.xKind == X_JUMP) lines.push_back({Fmt(L"Скачок: %d Б", (int)xa), RGB(255, 255, 255)});
            else lines.push_back({Fmt(L"Потоков: %d", (int)xa), RGB(255, 255, 255)});
            for (auto& s : c.s) {
                if (s.x.empty()) continue;
                size_t i = Nearest(s.x, xa);
                double v = i < s.y.size() ? s.y[i] : NAN;
                if (!std::isfinite(v)) continue;
                std::wstring val = c.yLog ? Fmt(L"%.2f", v) : FmtGB(v);
                size_t k = &s - &c.s[0];
                if (s.band && std::isfinite(bandLo[k][i]))  // среднее и разброс вокруг точки
                    val += L" (" + FmtGB(bandLo[k][i]) + L"…" + FmtGB(bandHi[k][i]) + L")";
                lines.push_back({s.name + L": " + val + L" " + c.yUnit, s.col});
                int px = (int)std::lround(X(s.x[i])), py = (int)std::lround(Y(v));
                HBRUSH db = CreateSolidBrush(s.col);
                RECT dot = {px - Sc(f, 3), py - Sc(f, 3), px + Sc(f, 3) + 1, py + Sc(f, 3) + 1};
                FillRect(dc, &dot, db);
                DeleteObject(db);
            }
            int bw = 0;
            for (auto& l : lines) bw = std::max<int>(bw, TextSize(dc, l.first).cx);
            bw += Sc(f, 14);
            int bh = (int)lines.size() * lh + Sc(f, 10);
            int bx = hover.x + Sc(f, 14), by = std::max<int>(P.top + lh + Sc(f, 10), hover.y - bh / 2);
            if (bx + bw > P.right) bx = hover.x - Sc(f, 14) - bw;
            if (by + bh > P.bottom) by = P.bottom - bh;
            RECT b = {bx, by, bx + bw, by + bh};
            HBRUSH bg = CreateSolidBrush(th::panel);
            FillRect(dc, &b, bg);
            DeleteObject(bg);
            SelectObject(dc, frame);
            Rectangle(dc, b.left, b.top, b.right, b.bottom);
            int ty = by + Sc(f, 5);
            for (auto& l : lines) {
                SetTextColor(dc, l.second);
                TextAt(dc, bx + Sc(f, 7), ty, l.first, TA_LEFT | TA_TOP);
                ty += lh;
            }
        }
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldFont);
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    DeleteObject(grid);
    DeleteObject(gridHi);
    DeleteObject(frame);
}

void DrawChart(HDC dc, const RECT& rc, const Chart& c, const ChartFonts& f, POINT hover)
{
    int n = c.rows * c.cols;
    if (n <= 1) {
        DrawOne(dc, rc, c, f, hover, false, true, L"");
        return;
    }
    HBRUSH black = CreateSolidBrush(th::chartBg);
    FillRect(dc, &rc, black);
    DeleteObject(black);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, f.normal);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int lh = tm.tmHeight;
    SetTextColor(dc, th::text);
    TextAt(dc, rc.right - Sc(f, 16), rc.top + Sc(f, 5), c.header, TA_RIGHT | TA_TOP);
    SetTextColor(dc, th::muted);
    TextAt(dc, rc.left + Sc(f, 8), rc.top + Sc(f, 5), c.yUnit, TA_LEFT | TA_TOP);
    SetTextAlign(dc, TA_LEFT | TA_TOP);

    // подписи столбцов — жирным по центру столбца, с линией цвета акцента снизу
    int top = rc.top + lh + Sc(f, 10);
    if (!c.colTitle.empty()) {
        SelectObject(dc, f.bold);
        TEXTMETRICW tb;
        GetTextMetricsW(dc, &tb);
        const int cw = (rc.right - rc.left) / c.cols;
        for (int i = 0; i < c.cols && i < (int)c.colTitle.size(); i++) {
            int cx = rc.left + Sc(f, 60) / 2 + i * cw + cw / 2;
            SetTextColor(dc, th::text);
            TextAt(dc, cx, top, c.colTitle[i], TA_CENTER | TA_TOP);
            SIZE sz = TextSize(dc, c.colTitle[i]);
            RECT u = {cx - sz.cx / 2, top + tb.tmHeight + Sc(f, 2), cx + sz.cx / 2, top + tb.tmHeight + Sc(f, 4)};
            HBRUSH ub = CreateSolidBrush(th::accent);
            FillRect(dc, &u, ub);
            DeleteObject(ub);
        }
        SelectObject(dc, f.normal);
        SetTextAlign(dc, TA_LEFT | TA_TOP);
        top += tb.tmHeight + Sc(f, 10);
    }
    SelectObject(dc, oldFont);

    // панели одинаковой высоты; нижний ряд ниже на подписи оси X
    const int xl = lh * 2 + Sc(f, 6), gap = Sc(f, 6);
    const int ph = (rc.bottom - top - xl - gap * (c.rows - 1)) / c.rows;
    const int pw = (rc.right - rc.left) / c.cols;
    for (int p = 0; p < n; p++) {
        int row = p / c.cols, col = p % c.cols;
        bool last = row == c.rows - 1;
        RECT pr = {rc.left + col * pw, top + row * (ph + gap), col == c.cols - 1 ? rc.right : rc.left + (col + 1) * pw,
                   top + row * (ph + gap) + ph + (last ? xl : 0)};
        Chart sub = c;
        sub.rows = sub.cols = 1;
        sub.s.clear();
        for (auto& s : c.s)
            if (s.panel == p) sub.s.push_back(s);
        DrawOne(dc, pr, sub, f, hover, true, last, p < (int)c.panelTitle.size() ? c.panelTitle[p] : L"");
    }
}

int DrawTable(HDC dc, int x0, int y0, const Table& t, const ChartFonts& f, bool measureOnly)
{
    size_t nc = t.head.size();
    if (!nc) return 0;
    bool sw = !t.swatch.empty();
    std::vector<int> w(nc, 0);
    HGDIOBJ old = SelectObject(dc, f.tableBold);
    for (size_t i = 0; i < nc; i++) w[i] = TextSize(dc, t.head[i]).cx;
    SelectObject(dc, f.table);
    for (auto& r : t.rows)
        for (size_t i = 0; i < nc && i < r.size(); i++) w[i] = std::max<int>(w[i], TextSize(dc, r[i]).cx);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int rh = tm.tmHeight + Sc(f, 5);
    int pad = Sc(f, 16), swW = sw ? Sc(f, 22) : 0;
    int total = swW;
    for (int v : w) total += v + pad;
    int height = rh * (int)(t.rows.size() + 1) + Sc(f, 2);
    if (measureOnly) {
        SelectObject(dc, old);
        return height;
    }
    SetBkMode(dc, TRANSPARENT);
    // шапка
    RECT hr = {x0, y0, x0 + total, y0 + rh};
    HBRUSH hb = CreateSolidBrush(th::tableHead);
    FillRect(dc, &hr, hb);
    DeleteObject(hb);
    SelectObject(dc, f.tableBold);
    SetTextColor(dc, th::muted);
    int x = x0 + swW;
    for (size_t i = 0; i < nc; i++) {
        if (i == 0) TextAt(dc, x + Sc(f, 4), y0 + Sc(f, 2), t.head[i], TA_LEFT | TA_TOP);
        else TextAt(dc, x + w[i] + pad - Sc(f, 6), y0 + Sc(f, 2), t.head[i], TA_RIGHT | TA_TOP);
        x += w[i] + pad;
    }
    SelectObject(dc, f.table);
    for (size_t r = 0; r < t.rows.size(); r++) {
        int y = y0 + rh * (int)(r + 1);
        if (r % 2) {
            RECT rr = {x0, y, x0 + total, y + rh};
            HBRUSH b = CreateSolidBrush(th::tableAlt);
            FillRect(dc, &rr, b);
            DeleteObject(b);
        }
        if (sw && r < t.swatch.size() && t.swatch[r] != CLR_INVALID) {
            bool dash = r < t.swatchDash.size() && t.swatchDash[r];
            HPEN p = MakePen(t.swatch[r], Sc(f, 3), dash);
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, x0 + Sc(f, 4), y + rh / 2, nullptr);
            LineTo(dc, x0 + swW - Sc(f, 2), y + rh / 2);
            SelectObject(dc, op);
            DeleteObject(p);
        }
        SetTextColor(dc, th::text);
        x = x0 + swW;
        for (size_t i = 0; i < nc && i < t.rows[r].size(); i++) {
            if (i == 0) TextAt(dc, x + Sc(f, 4), y + Sc(f, 2), t.rows[r][i], TA_LEFT | TA_TOP);
            else TextAt(dc, x + w[i] + pad - Sc(f, 6), y + Sc(f, 2), t.rows[r][i], TA_RIGHT | TA_TOP);
            x += w[i] + pad;
        }
    }
    HPEN line = CreatePen(PS_SOLID, 1, th::border);
    HGDIOBJ op = SelectObject(dc, line);
    MoveToEx(dc, x0, y0 + rh, nullptr);
    LineTo(dc, x0 + total, y0 + rh);
    SelectObject(dc, op);
    DeleteObject(line);
    SelectObject(dc, old);
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    return height;
}
