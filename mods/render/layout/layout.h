#ifndef LAYOUT_H
#define LAYOUT_H

#include "parser.h"

extern int layout_content_width;
extern int layout_content_height;
extern int layout_selection_active;
extern int layout_selection_x1;
extern int layout_selection_y1;
extern int layout_selection_x2;
extern int layout_selection_y2;
extern DOMNode* focused_node;
extern int cursor_visible;

void layout_and_render(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y);

#endif
