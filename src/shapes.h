#pragma once
#include <string>
#include <vector>

// The picture editor's objects (rectangles, ellipses, text) in the picture's own pixels, and
// what the mouse does with them: hit, moved, resized by a handle. Pure logic (unit tested).

enum class ShapeKind { Rect, Ellipse, Text };

struct Shape {
    ShapeKind kind = ShapeKind::Rect;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // its box, x0 <= x1, y0 <= y1 (text: as drawn)
    unsigned color = 0;                         // COLORREF
    bool fill = false;                          // a rectangle or ellipse filled with its colour
    double width = 4;                           // outline
    double size = 32;                           // text: the font's height
    std::wstring text;
};

// the handles of a rectangle or ellipse (text has none: its size makes its box):
// 0 top-left, 1 top, 2 top-right, 3 right, 4 bottom-right, 5 bottom, 6 bottom-left, 7 left
constexpr int kShapeHandles = 8;
void shape_handle_pos(const Shape& s, int h, double& x, double& y);
// the handle within `tol` of (x, y), -1: none
int shape_handle_at(const Shape& s, double x, double y, double tol);
// (x, y) on the shape: inside it (the rectangle, the ellipse, the text's box) or within
// `tol` of its edge
bool shape_hit(const Shape& s, double x, double y, double tol);
int shapes_hit(const std::vector<Shape>& v, double x, double y, double tol);     // the topmost, -1: none
void shape_move(Shape& s, double dx, double dy);
// the box spanned by a drag from (ax, ay) to (x, y); `square`: the shorter side made as
// long as the other (Shift), towards the drag
void shape_span(Shape& s, double ax, double ay, double x, double y, bool square);
// handle `h` dragged to (x, y): the opposite edges stay; dragged past them the shape turns
// over, so the handle held may become another: the one held now is returned
int shape_resize(Shape& s, int h, double x, double y, bool square);
