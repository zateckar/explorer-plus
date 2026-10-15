#include "shapes.h"
#include <algorithm>
#include <cmath>

void shape_handle_pos(const Shape& s, int h, double& x, double& y) {
    double mx = (s.x0 + s.x1) / 2, my = (s.y0 + s.y1) / 2;
    static const int kx[kShapeHandles] = { 0, 1, 2, 2, 2, 1, 0, 0 };    // 0 left, 1 middle, 2 right
    static const int ky[kShapeHandles] = { 0, 0, 0, 1, 2, 2, 2, 1 };
    x = kx[h] == 0 ? s.x0 : kx[h] == 1 ? mx : s.x1;
    y = ky[h] == 0 ? s.y0 : ky[h] == 1 ? my : s.y1;
}

int shape_handle_at(const Shape& s, double x, double y, double tol) {
    if (s.kind == ShapeKind::Text) return -1;
    int best = -1;
    double bestD = tol * tol;
    for (int h = 0; h < kShapeHandles; ++h) {
        double hx, hy;
        shape_handle_pos(s, h, hx, hy);
        double d = (hx - x) * (hx - x) + (hy - y) * (hy - y);
        if (d <= bestD) { bestD = d; best = h; }
    }
    return best;
}

bool shape_hit(const Shape& s, double x, double y, double tol) {
    double t = tol + (s.kind == ShapeKind::Text ? 0 : s.width / 2);
    if (x < s.x0 - t || x > s.x1 + t || y < s.y0 - t || y > s.y1 + t) return false;
    if (s.kind != ShapeKind::Ellipse) return true;
    // inside the ellipse grown by t
    double rx = (s.x1 - s.x0) / 2 + t, ry = (s.y1 - s.y0) / 2 + t;
    double dx = x - (s.x0 + s.x1) / 2, dy = y - (s.y0 + s.y1) / 2;
    return rx > 0 && ry > 0 && (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) <= 1.0;
}

int shapes_hit(const std::vector<Shape>& v, double x, double y, double tol) {
    for (int i = (int)v.size() - 1; i >= 0; --i)
        if (shape_hit(v[(size_t)i], x, y, tol)) return i;
    return -1;
}

void shape_move(Shape& s, double dx, double dy) {
    s.x0 += dx; s.x1 += dx;
    s.y0 += dy; s.y1 += dy;
}

void shape_span(Shape& s, double ax, double ay, double x, double y, bool square) {
    if (square) {
        double side = std::max(std::fabs(x - ax), std::fabs(y - ay));
        x = ax + (x < ax ? -side : side);
        y = ay + (y < ay ? -side : side);
    }
    s.x0 = std::min(ax, x); s.x1 = std::max(ax, x);
    s.y0 = std::min(ay, y); s.y1 = std::max(ay, y);
}

int shape_resize(Shape& s, int h, double x, double y, bool square) {
    bool left = h == 0 || h == 6 || h == 7, right = h >= 2 && h <= 4;
    bool top = h <= 2, bottom = h >= 4 && h <= 6;
    if ((left || right) && (top || bottom)) {
        // a corner: the opposite one stays
        double ax = left ? s.x1 : s.x0, ay = top ? s.y1 : s.y0;
        shape_span(s, ax, ay, x, y, square);
        bool l = x < ax, t = y < ay;
        return t ? (l ? 0 : 2) : (l ? 6 : 4);
    }
    if (left || right) {
        double ax = left ? s.x1 : s.x0;
        s.x0 = std::min(ax, x); s.x1 = std::max(ax, x);
        return x < ax ? 7 : 3;
    }
    double ay = top ? s.y1 : s.y0;
    s.y0 = std::min(ay, y); s.y1 = std::max(ay, y);
    return y < ay ? 1 : 5;
}
