#pragma once
#include "appstate.h"

void dnd_register_sources();                 // register drop targets on list+tree
void dnd_register_list(HWND list);           // a list made later (once the others are registered)
void dnd_unregister_all();
void dnd_begin_list_drag();                  // called from LVN_BEGINDRAG
