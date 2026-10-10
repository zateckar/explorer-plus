#pragma once
#include "appstate.h"

// drop queued work + cached images (new listing or thumbnail size change)
void thumb_reset_for_new_folder();
// LVN_GETDISPINFO helper, called with g_itemsMx held: returns the thumbnail image
// index for `it`, or -1 while it is being extracted (a request is queued).
void thumb_get_or_request(Item* it, int row, int* outImageIndex);

// the thumbnail imagelist for the current view (nullptr when the view uses icons)
HIMAGELIST thumb_imagelist_for_current_view();
