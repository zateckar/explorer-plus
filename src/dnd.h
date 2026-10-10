#pragma once
#include "appstate.h"

void dnd_register_sources();                 // register drop targets on list+tree
void dnd_unregister_all();
void dnd_begin_list_drag();                  // called from LVN_BEGINDRAG
